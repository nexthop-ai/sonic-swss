#include "exec.h"
#include "teammgr.h"
#include "logger.h"
#include "shellcmd.h"
#include "tokenize.h"
#include "warm_restart.h"
#include "portmgr.h"
#include <swss/redisutility.h>

#include <algorithm>
#include <iostream>
#include <fstream>
#include <sstream>
#include <thread>

#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <signal.h>
#include <netlink/route/link.h>


using namespace std;
using namespace swss;


TeamMgr::TeamMgr(DBConnector *confDb, DBConnector *applDb, DBConnector *statDb,
        const vector<TableConnector> &tables) :
    Orch(tables),
    m_cfgMetadataTable(confDb, CFG_DEVICE_METADATA_TABLE_NAME),
    m_cfgPortTable(confDb, CFG_PORT_TABLE_NAME),
    m_cfgLagTable(confDb, CFG_LAG_TABLE_NAME),
    m_cfgLagMemberTable(confDb, CFG_LAG_MEMBER_TABLE_NAME),
    m_appPortTable(applDb, APP_PORT_TABLE_NAME),
    m_appLagTable(applDb, APP_LAG_TABLE_NAME),
    m_statePortTable(statDb, STATE_PORT_TABLE_NAME),
    m_stateLagTable(statDb, STATE_LAG_TABLE_NAME),
    m_stateMACsecIngressSATable(statDb, STATE_MACSEC_INGRESS_SA_TABLE_NAME)
{
    SWSS_LOG_ENTER();

    // Clean up state database LAG entries
    vector<string> keys;
    m_stateLagTable.getKeys(keys);

    for (auto alias : keys)
    {
        m_stateLagTable.del(alias);
    }

    // Get the MAC address from configuration database
    vector<FieldValueTuple> fvs;
    m_cfgMetadataTable.get("localhost", fvs);
    auto it = find_if(fvs.begin(), fvs.end(), [](const FieldValueTuple &fv) {
            return fv.first == "mac";
            });

    if (it == fvs.end())
    {
        throw runtime_error("Failed to get MAC address from configuration database");
    }

    m_mac = MacAddress(it->second);
}

bool TeamMgr::isPortStateOk(const string &alias)
{
    SWSS_LOG_ENTER();

    vector<FieldValueTuple> temp;

    if (!m_statePortTable.get(alias, temp))
    {
        SWSS_LOG_INFO("Port %s is not ready", alias.c_str());
        return false;
    }

    auto state_opt = swss::fvsGetValue(temp, "state", true);
    if (!state_opt)
    {
        SWSS_LOG_INFO("Port %s is not ready", alias.c_str());
        return false;
    }

    return true;
}

bool TeamMgr::isLagStateOk(const string &alias)
{
    SWSS_LOG_ENTER();

    vector<FieldValueTuple> temp;

    if (!m_stateLagTable.get(alias, temp))
    {
        SWSS_LOG_INFO("Lag %s is not ready", alias.c_str());
        return false;
    }

    return true;
}

bool TeamMgr::isMACsecAttached(const std::string &port)
{
    SWSS_LOG_ENTER();

    vector<FieldValueTuple> temp;

    if (!m_cfgPortTable.get(port, temp))
    {
        SWSS_LOG_INFO("Port %s is not ready", port.c_str());
        return false;
    }

    auto macsec_opt = swss::fvsGetValue(temp, "macsec", true);
    if (!macsec_opt || macsec_opt->empty())
    {
        SWSS_LOG_INFO("MACsec isn't setted on the port %s", port.c_str());
        return false;
    }

    return true;
}

bool TeamMgr::isMACsecIngressSAOk(const std::string &port)
{
    SWSS_LOG_ENTER();

    vector<string> keys;
    m_stateMACsecIngressSATable.getKeys(keys);

    for (auto key: keys)
    {
        auto tokens = tokenize(key, state_db_key_delimiter);
        auto interface = tokens[0];

        if (port == interface)
        {
            SWSS_LOG_NOTICE(" MACsec is ready on the port %s", port.c_str());
            return true;
        }
    }

    SWSS_LOG_INFO("MACsec is NOT ready on the port %s", port.c_str());
    return false;
}

void TeamMgr::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto table = consumer.getTableName();

    SWSS_LOG_INFO("Get task from table %s", table.c_str());

    if (table == CFG_LAG_TABLE_NAME)
    {
        doLagTask(consumer);
    }
    else if (table == CFG_LAG_MEMBER_TABLE_NAME)
    {
        doLagMemberTask(consumer);
    }
    else if (table == CFG_DEVICE_METADATA_TABLE_NAME)
    {
        doDeviceMetadataTask(consumer);
    }
    else if (table == STATE_PORT_TABLE_NAME)
    {
        doPortUpdateTask(consumer);
    }
}

void TeamMgr::cleanTeamProcesses()
{
    SWSS_LOG_ENTER();
    SWSS_LOG_NOTICE("Cleaning up LAGs during shutdown...");

    std::unordered_map<std::string, int> aliasPidMap;

    for (const auto& alias: m_lagList)
    {
        pid_t pid;
        // Sleep for 10 milliseconds so as to not overwhelm the netlink
        // socket buffers with events about interfaces going down
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        try
        {
            ifstream pidFile("/var/run/teamd/" + alias + ".pid");
            if (pidFile.is_open())
            {
                pidFile >> pid;
                aliasPidMap[alias] = pid;
                SWSS_LOG_INFO("Read port channel %s pid %d", alias.c_str(), pid);
            }
            else
            {
                SWSS_LOG_NOTICE("Unable to read pid file for %s, skipping...", alias.c_str());
                continue;
            }
        }
        catch (const std::exception &e)
        {
            // Handle Warm/Fast reboot scenario
            SWSS_LOG_NOTICE("Skipping non-existent port channel %s pid...", alias.c_str());
            continue;
        }

        if (kill(pid, SIGTERM))
        {
            SWSS_LOG_ERROR("Failed to send SIGTERM to port channel %s pid %d: %s", alias.c_str(), pid, strerror(errno));
            aliasPidMap.erase(alias);
        }
        else
        {
            SWSS_LOG_NOTICE("Sent SIGTERM to port channel %s pid %d", alias.c_str(), pid);
        }
    }

    for (const auto& cit: aliasPidMap)
    {
        const auto &alias = cit.first;
        const auto &pid = cit.second;

        SWSS_LOG_NOTICE("Waiting for port channel %s pid %d to stop...", alias.c_str(), pid);

        while (!kill(pid, 0))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    SWSS_LOG_NOTICE("LAGs cleanup is done");
}

void TeamMgr::doLagTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    // Resolve the system-wide default LACP mode once per pass, not per LAG.
    const string default_lacp_mode = getDefaultLacpMode();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string alias = kfvKey(t);
        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            int min_links = 0;
            bool fallback = false;
            bool fast_rate = false;
            string lacp_mode = default_lacp_mode;
            string admin_status = DEFAULT_ADMIN_STATUS_STR;
            string mtu = DEFAULT_MTU_STR;
            string learn_mode;
            string tpid;
            string sys_mac;

            for (auto i : kfvFieldsValues(t))
            {
                // min_links and fallback attributes cannot be changed
                // after the LAG is created.
                if (fvField(i) == "min_links")
                {
                    min_links = stoi(fvValue(i));
                    SWSS_LOG_INFO("Get min_links value %d", min_links);
                }
                else if (fvField(i) == "fallback")
                {
                    fallback = fvValue(i) == "true";
                    SWSS_LOG_INFO("Get fallback option %s",
                            fallback ? "true" : "false");
                }
                else if (fvField(i) == "admin_status")
                {
                    admin_status = fvValue(i);;
                    SWSS_LOG_INFO("Get admin_status %s",
                            admin_status.c_str());
                }
                else if (fvField(i) == "mtu")
                {
                    mtu = fvValue(i);
                    SWSS_LOG_INFO("Get MTU %s", mtu.c_str());
                }
                else if (fvField(i) == "learn_mode")
                {
                    learn_mode = fvValue(i);
                    SWSS_LOG_INFO("Get learn_mode %s",
                            learn_mode.c_str());
                }
                else if (fvField(i) == "tpid")
                {
                    tpid = fvValue(i);
                    SWSS_LOG_INFO("Get TPID %s", tpid.c_str());
                }
                else if (fvField(i) == "fast_rate")
                {
                    fast_rate = fvValue(i) == "true";
                    SWSS_LOG_INFO("Get fast_rate `%s`",
                                  fast_rate ? "true" : "false");
                }
                else if (fvField(i) == "lacp_mode")
                {
                    lacp_mode = fvValue(i);
                    // "couple" is a legacy alias for "coupled" (pre-rename
                    // CONFIG_DB rows written by older sonic-utilities).
                    // Normalize so stored state and intrinsic-change detection
                    // use the canonical value and don't spuriously restart the
                    // LAG when a "couple" row is later rewritten as "coupled".
                    if (lacp_mode == "couple")
                    {
                        lacp_mode = "coupled";
                    }
                    SWSS_LOG_INFO("Get lacp_mode %s", lacp_mode.c_str());
                }
                else if (fvField(i) == "system_mac")
                {
                    sys_mac = fvValue(i);
                    SWSS_LOG_INFO("Get sys_mac %s.", sys_mac.c_str());
                }
            }

            if (m_lagList.find(alias) == m_lagList.end())
            {
                if (addLag(alias, min_links, fallback, fast_rate, lacp_mode) == task_need_retry)
                {
                    // If LAG creation fails, we need to clean up any potentially orphaned teamd processes
                    removeLag(alias);
                    it++;
                    continue;
                }

                m_lagList.insert(alias);
<<<<<<< HEAD
=======
            } else {
                if (isIntrinsicParamsChanged(alias, min_links, fallback, fast_rate, lacp_mode)) {
                    if (!restartLag(alias, min_links, fallback, fast_rate, lacp_mode)) {
                        SWSS_LOG_ERROR("Failed to restart port channel %s with new attributes", alias.c_str());
                        it++;
                        continue;
                   }
                }
>>>>>>> a295efcc (NOS-10884: teammgrd: start teamd with configured LACP mode (independent/coupled) (#756))
            }

            setLagAdminStatus(alias, admin_status);
            setLagMtu(alias, mtu);
            if (!learn_mode.empty())
            {
                setLagLearnMode(alias, learn_mode);
                SWSS_LOG_NOTICE("Configure %s MAC learn mode to %s", alias.c_str(), learn_mode.c_str());
            }
            if (!tpid.empty())
            {
                setLagTpid(alias, tpid);
                SWSS_LOG_NOTICE("Configure %s TPID to %s", alias.c_str(), tpid.c_str());
            }
            if (!sys_mac.empty())
            {
                if (setLagSysmac(alias, sys_mac))
                {
                    SWSS_LOG_NOTICE("Successfully configured %s sys_mac to %s", alias.c_str(), sys_mac.c_str());
                }
                else
                {
                    SWSS_LOG_ERROR("Failed to configure %s sys_mac to %s", alias.c_str(), sys_mac.c_str());
                }
            }
        }
        else if (op == DEL_COMMAND)
        {
            if (m_lagList.find(alias) != m_lagList.end())
            {
                removeLag(alias);
                m_lagList.erase(alias);
            }
        }

        it = consumer.m_toSync.erase(it);
    }
}

void TeamMgr::doLagMemberTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        auto tokens = tokenize(kfvKey(t), config_db_key_delimiter);
        auto lag = tokens[0];
        auto member = tokens[1];

        auto op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            if (!isPortStateOk(member) || !isLagStateOk(lag))
            {
                it++;
                continue;
            }
            if (isMACsecAttached(member) && !isMACsecIngressSAOk(member))
            {
                it++;
                continue;
            }
            if (addLagMember(lag, member) == task_need_retry)
            {
                it++;
                continue;
            }
        }
        else if (op == DEL_COMMAND)
        {
            removeLagMember(lag, member);
        }

        it = consumer.m_toSync.erase(it);
    }
}

bool TeamMgr::checkPortIffUp(const string &port)
{
    SWSS_LOG_ENTER();

    struct ifreq ifr;
    memcpy(ifr.ifr_name, port.c_str(), strlen(port.c_str()));
    ifr.ifr_name[strlen(port.c_str())] = 0;

    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd == -1 || ioctl(fd, SIOCGIFFLAGS, &ifr) == -1)
    {
        SWSS_LOG_ERROR("Failed to get port %s flags", port.c_str());
        if (fd != -1)
        {
            close(fd);
        }
        return false;
    }

    SWSS_LOG_INFO("Get port %s flags %i", port.c_str(), ifr.ifr_flags);
    close(fd);
    return ifr.ifr_flags & IFF_UP;
}

bool TeamMgr::isPortEnslaved(const string &port)
{
    SWSS_LOG_ENTER();

    struct stat buf;
    string path = "/sys/class/net/" + port + "/master";

    return lstat(path.c_str(), &buf) == 0;
}

bool TeamMgr::findPortMaster(string &master, const string &port)
{
    SWSS_LOG_ENTER();

    vector<string> keys;
    m_cfgLagMemberTable.getKeys(keys);

    for (auto key: keys)
    {
        auto tokens = tokenize(key, config_db_key_delimiter);
        auto lag = tokens[0];
        auto member = tokens[1];

        if (port == member)
        {
            master = lag;
            return true;
        }
    }

    return false;
}

// When a port gets removed and created again, notification is triggered
// when state dabatabase gets updated. In this situation, the port needs
// to be enslaved into the LAG again.
void TeamMgr::doPortUpdateTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        auto alias = kfvKey(t);
        auto op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            SWSS_LOG_INFO("Received port %s state update", alias.c_str());

            string lag;
            if (findPortMaster(lag, alias))
            {
                if (isMACsecAttached(alias) && !isMACsecIngressSAOk(alias))
                {
                    it++;
                    SWSS_LOG_INFO("MACsec is NOT ready on the port %s", alias.c_str());
                    continue;
                }

                if (addLagMember(lag, alias) == task_need_retry)
                {
                    it++;
                    continue;
                }
            }
        }
        else if (op == DEL_COMMAND)
        {
            SWSS_LOG_INFO("Received port %s state removal", alias.c_str());
        }

        it = consumer.m_toSync.erase(it);
    }
}

bool TeamMgr::setLagAdminStatus(const string &alias, const string &admin_status)
{
    SWSS_LOG_ENTER();

    stringstream cmd;
    string res;

    // ip link set dev <port_channel_name> [up|down]
    cmd << IP_CMD << " link set dev " << shellquote(alias) << " " << shellquote(admin_status);
    EXEC_WITH_ERROR_THROW(cmd.str(), res);

    SWSS_LOG_NOTICE("Set port channel %s admin status to %s",
            alias.c_str(), admin_status.c_str());

    return true;
}

bool TeamMgr::setLagMtu(const string &alias, const string &mtu)
{
    SWSS_LOG_ENTER();

    stringstream cmd;
    string res;

    // ip link set dev <port_channel_name> mtu <mtu_value>
    cmd << IP_CMD << " link set dev " << shellquote(alias) << " mtu " << shellquote(mtu);
    EXEC_WITH_ERROR_THROW(cmd.str(), res);

    vector<FieldValueTuple> fvs;
    FieldValueTuple fv("mtu", mtu);
    fvs.push_back(fv);
    m_appLagTable.set(alias, fvs);

    vector<string> keys;
    m_cfgLagMemberTable.getKeys(keys);

    for (auto key : keys)
    {
        auto tokens = tokenize(key, config_db_key_delimiter);
        auto lag = tokens[0];
        auto member = tokens[1];

        if (alias == lag)
        {
            m_appPortTable.set(member, fvs);
        }
    }

    SWSS_LOG_NOTICE("Set port channel %s MTU to %s",
            alias.c_str(), mtu.c_str());

    return true;
}

bool TeamMgr::setLagTpid(const string &alias, const string &tpid)
{
    SWSS_LOG_ENTER();

    vector<FieldValueTuple> fvs;
    FieldValueTuple fv("tpid", tpid);
    fvs.push_back(fv);
    m_appLagTable.set(alias, fvs);

    SWSS_LOG_NOTICE("Set port channel %s TPID to %s", alias.c_str(), tpid.c_str());

    return true;
}


bool TeamMgr::setLagLearnMode(const string &alias, const string &learn_mode)
{
    // Set the port MAC learn mode in application database
    vector<FieldValueTuple> fvs;
    FieldValueTuple fv("learn_mode", learn_mode);
    fvs.push_back(fv);
    m_appLagTable.set(alias, fvs);

    return true;
}

int TeamMgr::update_kernel(const string &alias, const string &system_mac)
{
    struct rtnl_link *link;
    struct rtnl_link *orig_link;
    int err = 0;
    struct nl_addr *nl_addr;
    MacAddress sys_mac(system_mac);
    struct nl_sock * sockk = nl_socket_alloc();
    uint32_t ifindex = if_nametoindex(alias.c_str());
    uint8_t *addr = const_cast<uint8_t *>(sys_mac.getMac());

    if (sockk == NULL) {
        SWSS_LOG_ERROR("Failed to allocate netlink socket.\n");
        return -1;
    }
    if (nl_connect(sockk, NETLINK_ROUTE) < 0) {
        SWSS_LOG_ERROR("Failed to connect to netlink.\n");
        nl_socket_free(sockk);
        return -1;
    }

    link = rtnl_link_alloc();
    if (!link) {
        SWSS_LOG_ERROR("Unable to create link");
        nl_close(sockk);
        nl_socket_free(sockk);
        return -ENOMEM;
    }
    SWSS_LOG_NOTICE("ifindex %d, mac %02x:%02x:%02x:%02x:%02x:%02x, err %d",
                ifindex, addr[0], addr[1], addr[2], addr[3], addr[4], addr[5], err);
    void *mac = (void *)addr;
    nl_addr = nl_addr_build(AF_UNSPEC, mac, ETHER_ADDR_LEN);
    if (!nl_addr)
    {
        SWSS_LOG_ERROR("Error in update_kernel");
        err = -ENOMEM;
        rtnl_link_put(link);
        nl_close(sockk);
        nl_socket_free(sockk);
        return err;
    }

    rtnl_link_set_addr(link, nl_addr);
    nl_addr_put(nl_addr); // Release our reference; link now owns the addr

    if (rtnl_link_get_kernel(sockk, 0, alias.c_str(), &orig_link) < 0) {
        SWSS_LOG_ERROR("Failed to get link for interface %d.\n", ifindex);
        rtnl_link_put(link);
        nl_close(sockk);
        nl_socket_free(sockk);
        return -1;
    }
    /* The 4th arg of rtnl_link_change() is a netlink flags bitmask, not the
     * interface index. The target interface is already identified by
     * orig_link, so pass 0 here. */
    if (rtnl_link_change(sockk, orig_link, link, 0) < 0) {
        SWSS_LOG_ERROR("Failed to change the MAC address.\n");
        rtnl_link_put(orig_link);
        rtnl_link_put(link);
        nl_close(sockk);
        nl_socket_free(sockk);
        return -1;
    }

    SWSS_LOG_NOTICE("Successfully changed the MAC address of the interface %d.\n", ifindex);

    rtnl_link_put(orig_link);
    rtnl_link_put(link);
    nl_close(sockk);
    nl_socket_free(sockk);
    return err;
}

bool TeamMgr::setLagSysmac(const string &alias, string &sys_mac)
{
    vector<FieldValueTuple> fvs;
    stringstream    cmd;
    if (sys_mac == "None") {
        sys_mac = m_mac.to_string();
    }
    FieldValueTuple fv("system_mac", sys_mac);
    fvs.push_back(fv);

    //update in kernel first
    int err = this->update_kernel(alias, sys_mac);
    if (err != 0)
    {
        SWSS_LOG_ERROR("Failed to update kernel for %s with system_mac %s, error %d",
                       alias.c_str(), sys_mac.c_str(), err);
        return false;
    }

    SWSS_LOG_NOTICE("Successfully updated kernel for %s with system_mac %s",
                    alias.c_str(), sys_mac.c_str());

    //Update in APP_DB only after successful kernel update
    m_appLagTable.set(alias, fvs);
    //Update in State_DB only after successful kernel update
    m_stateLagTable.set(alias, fvs);
    return true;
}

task_process_status TeamMgr::addLag(const string &alias, int min_links, bool fallback, bool fast_rate, const string &lacp_mode)
{
    SWSS_LOG_ENTER();

    stringstream cmd;
    string res;

    stringstream conf;

    const string dump_path = "/var/warmboot/teamd/";
    MacAddress mac_boot = m_mac;

    // set portchannel mac same with mac before warmStart, when warmStart and there
    // is a file written by teamd.
    ifstream aliasfile(dump_path + alias);
    if (WarmStart::isWarmStart() && aliasfile.is_open())
    {
        const int partner_system_id_offset = 40;
        string line;

        while (getline(aliasfile, line))
        {
            ifstream memberfile(dump_path + line, ios::binary);
            uint8_t mac_temp[ETHER_ADDR_LEN] = {0};
            uint8_t null_mac[ETHER_ADDR_LEN] = {0};

            if (!memberfile.is_open())
                continue;

            memberfile.seekg(partner_system_id_offset, std::ios::beg);
            memberfile.read(reinterpret_cast<char*>(mac_temp), ETHER_ADDR_LEN);

            /* During negotiation stage partner info of pdu is empty , skip it */
            if (memcmp(mac_temp, null_mac, ETHER_ADDR_LEN) == 0)
                continue;

            mac_boot = MacAddress(mac_temp);
            break;
        }
    }

    conf << "'{\"device\":\"" << alias << "\","
         << "\"hwaddr\":\"" << mac_boot.to_string() << "\","
         << "\"runner\":{"
         << "\"active\":true,"
         << "\"name\":\"lacp\"";

    if (min_links != 0)
    {
        conf << ",\"min_ports\":" << min_links;
    }

    if (fallback)
    {
        conf << ",\"fallback\":true";
    }

    if (fast_rate)
    {
        conf << ",\"fast_rate\":true";
    }

    if (lacp_mode == "independent")
    {
        conf << ",\"independent_mode\":true";
    }

    conf << "}}'";

    SWSS_LOG_INFO("Port channel %s teamd configuration: %s",
            alias.c_str(), conf.str().c_str());

    string warmstart_flag = WarmStart::isWarmStart() ? " -w -o" : " -r";

    cmd << TEAMD_CMD
        << warmstart_flag
        << " -t " << alias
        << " -c " << conf.str()
        << " -L " << dump_path
        << " -g -d";

    if (exec(cmd.str(), res) != 0)
    {
        SWSS_LOG_INFO("Failed to start port channel %s with teamd, retry...",
                alias.c_str());
        return task_need_retry;
    }

<<<<<<< HEAD
=======
    addLagParams(alias, min_links, fallback, fast_rate, lacp_mode);

>>>>>>> a295efcc (NOS-10884: teammgrd: start teamd with configured LACP mode (independent/coupled) (#756))
    SWSS_LOG_NOTICE("Start port channel %s with teamd", alias.c_str());

    return task_success;
}

bool TeamMgr::removeLag(const string &alias)
{
    SWSS_LOG_ENTER();

    pid_t pid;

    {
        ifstream pidfile("/var/run/teamd/" + alias + ".pid");
        if (pidfile.is_open())
        {
            pidfile >> pid;
            SWSS_LOG_INFO("Read port channel %s pid %d", alias.c_str(), pid);
        }
        else
        {
            SWSS_LOG_NOTICE("Failed to remove non-existent port channel %s pid...", alias.c_str());
            return false;
        }
    }

    if (kill(pid, SIGTERM))
    {
        SWSS_LOG_ERROR("Failed to send SIGTERM to port channel %s pid %d: %s", alias.c_str(), pid, strerror(errno));
        return false;
    }

    SWSS_LOG_NOTICE("Stop port channel %s", alias.c_str());

    return true;
}

// Port-channel names are in the pattern of "PortChannel####"
//
// The LACP key could be generated in 3 ways based on the value in config DB:
//      1. "auto" - LACP key is extracted from the port-channel name and is set to be the number at the end of the port-channel name
//                  We are adding 1 at the beginning to avoid LACP key collisions between similar LACP keys e.g. PortChannel10 and PortChannel010.
//      2. n -      LACP key will be n.
//      3. "" -     LACP key will be 0 - exists for backward compatibility.
uint16_t TeamMgr::generateLacpKey(const string& lag)
{
    vector <FieldValueTuple> fvs;
    m_cfgLagTable.get(lag, fvs);

    auto it = find_if(fvs.begin(), fvs.end(), [](const FieldValueTuple& fv)
    {
        return fv.first == "lacp_key";
    });
    string lacp_key;
    if (it != fvs.end())
    {
        lacp_key = it->second;
        if (!lacp_key.empty())
        {
            try
            {
                if (lacp_key == "auto")
                {
                    return static_cast<uint16_t>(std::stoul("1" + lag.substr(lag.find_first_of("0123456789"))));
                }
                else
                {
                    return static_cast<uint16_t>(std::stoul(lacp_key));
                }
            }
            catch (const std::exception& e)
            {
                SWSS_LOG_THROW("Failed to parse LACP key %s for port channel %s", lacp_key.c_str(), lag.c_str());
            }
        }
        else
        {
            return 0;
        }
    }
    return 0;
}

// React to a default_lacp_mode change: restart every port channel that inherits
// the default (no explicit lacp_mode) and whose effective mode changed.
void TeamMgr::doDeviceMetadataTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        if (kfvKey(t) == "localhost" && kfvOp(t) == SET_COMMAND)
        {
            string default_lacp_mode = getDefaultLacpMode();

            for (const auto &alias : m_lagList)
            {
                vector<FieldValueTuple> fvs;
                m_cfgLagTable.get(alias, fvs);
                if (swss::fvsGetValue(fvs, "lacp_mode", true))
                {
                    continue;
                }

                LagInfo &lagInfo = getOrCreateLagInfo(alias);
                if (lagInfo.params.lacp_mode == default_lacp_mode)
                {
                    continue;
                }

                SWSS_LOG_NOTICE("Default LACP mode changed to %s, restarting port channel %s",
                                default_lacp_mode.c_str(), alias.c_str());

                if (!restartLag(alias, lagInfo.params.min_links, lagInfo.params.fall_back,
                                lagInfo.params.fast_rate, default_lacp_mode))
                {
                    SWSS_LOG_ERROR("Failed to restart port channel %s for default LACP mode change", alias.c_str());
                }
            }
        }

        it = consumer.m_toSync.erase(it);
    }
}

// Read the system-wide default LACP mode from DEVICE_METADATA|localhost:default_lacp_mode.
string TeamMgr::getDefaultLacpMode()
{
    vector<FieldValueTuple> fvs;
    m_cfgMetadataTable.get("localhost", fvs);

    auto default_mode = swss::fvsGetValue(fvs, "default_lacp_mode", true);
    string lacp_mode = default_mode ? *default_mode : "coupled";

    // "couple" is a legacy alias for "coupled" written by older tooling;
    // accept and normalize it instead of warning and coercing.
    if (lacp_mode == "couple")
    {
        lacp_mode = "coupled";
    }

    if (lacp_mode != "independent" && lacp_mode != "coupled")
    {
        SWSS_LOG_WARN("Unrecognized default_lacp_mode '%s', defaulting to coupled", lacp_mode.c_str());
        lacp_mode = "coupled";
    }

    return lacp_mode;
}

// Once a port is enslaved into a port channel, the port's MTU will
// be inherited from the master's MTU while the port's admin status
// will still be controlled separately.
task_process_status TeamMgr::addLagMember(const string &lag, const string &member)
{
    SWSS_LOG_ENTER();

    stringstream cmd;
    string res;

    // If port was already deleted, ignore this operation
    cmd << IP_CMD << " link show " << shellquote(member);
    if (exec(cmd.str(), res) != 0)
    {
	SWSS_LOG_WARN("Unable to find port %s", member.c_str());
	return task_ignore;
    }

    // If port is already enslaved, ignore this operation
    // TODO: check the current master if it is the same as to be configured
    if (isPortEnslaved(member))
    {
        return task_ignore;
    }

    uint16_t keyId = generateLacpKey(lag);
    cmd.str("");
    cmd.clear();

    // Set admin down LAG member (required by teamd) and enslave it
    // ip link set dev <member> down;
    // teamdctl <port_channel_name> port config update <member> { "lacp_key": <lacp_key>, "link_watch": { "name": "ethtool" } };
    // teamdctl <port_channel_name> port add <member>;
    cmd << IP_CMD << " link set dev " << shellquote(member) << " down; ";
    cmd << TEAMDCTL_CMD << " " << shellquote(lag) << " port config update " << shellquote(member)
        << " '{\"lacp_key\":"
        << keyId
        << ",\"link_watch\": {\"name\": \"ethtool\"} }'; ";
    cmd << TEAMDCTL_CMD << " " << shellquote(lag) << " port add " << shellquote(member);

    if (exec(cmd.str(), res) != 0)
    {
        // teamdctl port add command will fail when the member port is not
        // set to admin status down; it is possible that some other processes
        // or users (e.g. portmgrd) are executing the command to bring up the
        // member port while adding this port into the port channel. This piece
        // of code will check if the port is set to admin status up. If yes,
        // it will retry to add the port into the port channel.
        if (checkPortIffUp(member))
        {
            SWSS_LOG_INFO("Failed to add %s to port channel %s, retry...",
                    member.c_str(), lag.c_str());
            return task_need_retry;
        }
        else
        {
            SWSS_LOG_ERROR("Failed to add %s to port channel %s",
                    member.c_str(), lag.c_str());
            return task_failed;
        }
    }

    vector<FieldValueTuple> fvs;
    m_cfgPortTable.get(member, fvs);

    // Get the member admin status
    auto it = find_if(fvs.begin(), fvs.end(), [](const FieldValueTuple &fv) {
            return fv.first == "admin_status";
            });

    string admin_status = DEFAULT_ADMIN_STATUS_STR;
    if (it != fvs.end())
    {
        admin_status = it->second;
    }

    // Get the LAG MTU (by default 9100)
    // Member port will inherit master's MTU attribute
    m_cfgLagTable.get(lag, fvs);
    it = find_if(fvs.begin(), fvs.end(), [](const FieldValueTuple &fv) {
            return fv.first == "mtu";
            });

    string mtu = DEFAULT_MTU_STR;
    if (it != fvs.end())
    {
        mtu = it->second;
    }

    // ip link set dev <member> [up|down]
    cmd.str(string());
    cmd << IP_CMD << " link set dev " << shellquote(member) << " " << shellquote(admin_status);
    EXEC_WITH_ERROR_THROW(cmd.str(), res);

    fvs.clear();
    FieldValueTuple fv("mtu", mtu);
    fvs.push_back(fv);
    m_appPortTable.set(member, fvs);

    SWSS_LOG_NOTICE("Add %s to port channel %s", member.c_str(), lag.c_str());

    return task_success;
}

// Once a port is removed from from the master, both the admin status and the
// MTU will be re-set to its original value.
bool TeamMgr::removeLagMember(const string &lag, const string &member)
{
    SWSS_LOG_ENTER();

    stringstream cmd;
    string res;

    // teamdctl <port_channel_name> port remove <member>;
    cmd << TEAMDCTL_CMD << " " << lag << " port remove " << member << "; ";

    vector<FieldValueTuple> fvs;
    m_cfgPortTable.get(member, fvs);

    // Re-configure port MTU and admin status (by default 9100 and up)
    string admin_status = DEFAULT_ADMIN_STATUS_STR;
    string mtu = DEFAULT_MTU_STR;
    for (auto i : fvs)
    {
        if (fvField(i) == "admin_status")
        {
            admin_status = fvValue(i);
        }
        else if (fvField(i) == "mtu")
        {
            mtu = fvValue(i);
        }
    }

    // ip link set dev <port_name> [up|down];
    // ip link set dev <port_name> mtu
    cmd << IP_CMD << " link set dev " << shellquote(member) << " " << shellquote(admin_status) << "; ";
    cmd << IP_CMD << " link set dev " << shellquote(member) << " mtu " << shellquote(mtu);

    EXEC_WITH_ERROR_THROW(cmd.str(), res);
    fvs.clear();
    FieldValueTuple fv("admin_status", admin_status);
    fvs.push_back(fv);
    fv = FieldValueTuple("mtu", mtu);
    fvs.push_back(fv);
    m_appPortTable.set(member, fvs);

    SWSS_LOG_NOTICE("Remove %s from port channel %s", member.c_str(), lag.c_str());

    return true;
}
<<<<<<< HEAD
=======


void TeamMgr::doMonitorLinkGroupMemberTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string interface_name = kfvKey(t);
        string op = kfvOp(t);
        auto data = kfvFieldsValues(t);

        // Only process PortChannel interfaces in teammgr
        if (interface_name.find("PortChannel") != 0)
        {
            it = consumer.m_toSync.erase(it);
            continue;
        }

        if (op == SET_COMMAND)
        {
            SWSS_LOG_INFO("TeamMgr: Processing monitor link group member SET for %s", interface_name.c_str());

            // Parse state and down_due_to from STATE_DB
            string monitor_link_state = "";
            string down_due_to = "";

            for (const auto &idx : data)
            {
                const auto &field = fvField(idx);
                const auto &value = fvValue(idx);

                if (field == "state")
                {
                    monitor_link_state = value;
                }
                else if (field == "down_due_to")
                {
                    down_due_to = value;
                }
            }

            // Early exit if no state provided
            if (monitor_link_state.empty())
            {
                SWSS_LOG_WARN("TeamMgr: No state provided for interface %s, skipping", interface_name.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            // Get current configuration admin status
            vector<FieldValueTuple> lag_data;
            if (m_cfgLagTable.get(interface_name, lag_data))
            {
                bool config_admin_up = true; // Default to up if not specified
                for (const auto &fv : lag_data)
                {
                    if (fvField(fv) == "admin_status")
                    {
                        config_admin_up = (fvValue(fv) == "up");
                        break;
                    }
                }

                // Combine monitor link state with configuration state
                bool should_be_up = (monitor_link_state == "allow_up") && config_admin_up;

                SWSS_LOG_INFO("TeamMgr: Interface %s - monitor_link_state: %s, config_admin_up: %s, final_state: %s, down_due_to: %s",
                             interface_name.c_str(), monitor_link_state.c_str(),
                             config_admin_up ? "up" : "down", should_be_up ? "up" : "down", down_due_to.c_str());

                // Apply the combined state to the LAG interface using setLagAdminStatus
                // which uses ip link set to actually bring the interface up/down
                setLagAdminStatus(interface_name, should_be_up ? "up" : "down");
            }
            else
            {
                SWSS_LOG_WARN("TeamMgr: Could not get configuration for interface %s", interface_name.c_str());
            }
        }
        else if (op == DEL_COMMAND)
        {
            SWSS_LOG_INFO("TeamMgr: Processing monitor link group member DEL for %s", interface_name.c_str());

            // When monitor link control is removed, restore interface to its configuration state
            vector<FieldValueTuple> lag_data;
            if (m_cfgLagTable.get(interface_name, lag_data))
            {
                bool config_admin_up = true; // Default to up if not specified
                for (const auto &fv : lag_data)
                {
                    if (fvField(fv) == "admin_status")
                    {
                        config_admin_up = (fvValue(fv) == "up");
                        break;
                    }
                }

                SWSS_LOG_INFO("TeamMgr: Restoring interface %s to configuration state: %s",
                             interface_name.c_str(), config_admin_up ? "up" : "down");

                // Use setLagAdminStatus to actually bring the interface up/down
                setLagAdminStatus(interface_name, config_admin_up ? "up" : "down");
            }
        }

        it = consumer.m_toSync.erase(it);
    }
}

// v0 PortChannel micro-BFD: derive the APPL_DB BFD_SESSION_TABLE parent row(s) from
// PORTCHANNEL config and publish them. bfdorch detects a LAG-aliased parent and fans
// out per-member offloaded sessions. teamd is intentionally untouched in v0 (the
// min_links / forwarding gate is deferred to v1).
void TeamMgr::updateMicroBfdParent(const string &alias, bool enable,
                                   const string &peer_v4, const string &peer_v6,
                                   uint32_t tx_interval, uint32_t rx_interval,
                                   uint8_t multiplier)
{
    SWSS_LOG_ENTER();

    // Idempotent: withdraw whatever we previously published for this LAG, then
    // (re)publish from the current config.
    removeMicroBfdParent(alias);

    if (!enable)
    {
        return;
    }

    vector<pair<string, bool>> afs = { { peer_v4, false }, { peer_v6, true } };

    for (const auto &af : afs)
    {
        const string &peer = af.first;
        bool v6 = af.second;

        if (peer.empty())
        {
            continue;
        }

        string local_ip;
        if (!getLagLocalIp(alias, v6, local_ip))
        {
            SWSS_LOG_WARN("micro-BFD: %s has no %s address configured; skipping parent "
                          "(re-apply micro_bfd after configuring the PortChannel IP)",
                          alias.c_str(), v6 ? "IPv6" : "IPv4");
            continue;
        }

        // bfdorch key: <vrf>:<alias>:<peer>. Default VRF for v0.
        string key = string("default:") + alias + ":" + peer;

        vector<FieldValueTuple> fvs = {
            { "local_addr",  local_ip },
            { "type",        "async_active" },
            { "dst_mac",     MICRO_BFD_DST_MAC },
            { "multihop",    "false" },
            { "tx_interval", to_string(tx_interval) },
            { "rx_interval", to_string(rx_interval) },
            { "multiplier",  to_string(static_cast<unsigned>(multiplier)) },
        };

        m_appBfdSessionTable.set(key, fvs);
        m_microBfdParentKeys[alias].push_back(key);

        SWSS_LOG_NOTICE("micro-BFD: published parent %s (local %s peer %s tx/rx/mult %u/%u/%u)",
                        alias.c_str(), local_ip.c_str(), peer.c_str(),
                        tx_interval, rx_interval, static_cast<unsigned>(multiplier));
    }
}

void TeamMgr::removeMicroBfdParent(const string &alias)
{
    SWSS_LOG_ENTER();

    auto it = m_microBfdParentKeys.find(alias);
    if (it == m_microBfdParentKeys.end())
    {
        return;
    }

    for (const auto &key : it->second)
    {
        m_appBfdSessionTable.del(key);
        SWSS_LOG_NOTICE("micro-BFD: withdrew parent %s", key.c_str());
    }
    m_microBfdParentKeys.erase(it);
}

// Look up the PortChannel's own IP (from PORTCHANNEL_INTERFACE) to use as the BFD
// source address. Returns the first configured address of the requested family.
bool TeamMgr::getLagLocalIp(const string &alias, bool v6, string &ip_out)
{
    SWSS_LOG_ENTER();

    vector<string> keys;
    m_cfgLagIntfTable.getKeys(keys);

    for (const auto &k : keys)
    {
        auto tokens = tokenize(k, config_db_key_delimiter);
        if (tokens.size() != 2 || tokens[0] != alias)
        {
            continue;   // skip the plain "PortChannelX" row and other LAGs
        }

        string ip = tokens[1];
        auto slash = ip.find('/');
        if (slash != string::npos)
        {
            ip = ip.substr(0, slash);
        }

        bool is_v6 = ip.find(':') != string::npos;
        if (is_v6 == v6)
        {
            ip_out = ip;
            return true;
        }
    }

    return false;
}

void TeamMgr::addLagParams(const std::string &alias, const int min_links, const bool fall_back, const bool fast_rate, const std::string &lacp_mode)
{
    LagInfo& lagInfo = getOrCreateLagInfo(alias);
    lagInfo.params.min_links = min_links;
    lagInfo.params.fall_back = fall_back;
    lagInfo.params.fast_rate = fast_rate;
    lagInfo.params.lacp_mode = lacp_mode;
}

void TeamMgr::removeLagParams(const std::string &alias)
{
    auto it = m_lagInfo.find(alias);
    if (it != m_lagInfo.end()) {
        it->second.params = LagParams();
    }
}

LagInfo& TeamMgr::getOrCreateLagInfo(const std::string &alias)
{
    auto it = m_lagInfo.find(alias);
    if (it == m_lagInfo.end()) {
        m_lagInfo[alias] = LagInfo();
    }
    return m_lagInfo[alias];
}

void TeamMgr::removeLagInfo(const std::string &alias)
{
    m_lagInfo.erase(alias);
}

void TeamMgr::addLagMemberInfo(const std::string &lag, const std::string &member)
{
    LagInfo& lagInfo = getOrCreateLagInfo(lag);
    lagInfo.members.insert(member);
}

void TeamMgr::removeLagMemberInfo(const std::string &lag, const std::string &member)
{
    auto it = m_lagInfo.find(lag);
    if (it != m_lagInfo.end()) {
        it->second.members.erase(member);
    }
}

bool TeamMgr::isIntrinsicParamsChanged(const std::string &alias, const int min_links,
    const bool fall_back, const bool fast_rate, const std::string &lacp_mode)
{
    LagInfo& lagInfo = getOrCreateLagInfo(alias);
    std::string changedStr = "";
    bool changed = false;

    auto updateChangedStr = [&changedStr](const std::string& paramName, const auto& oldValue, const auto& newValue) {
        std::ostringstream oss;
        oss << paramName << " changed from " << oldValue << " to " << newValue << "; ";
        changedStr += oss.str();
    };

    if (lagInfo.params.min_links != min_links) {
        updateChangedStr("min_links", lagInfo.params.min_links, min_links);
        changed = true;
    }

    if (lagInfo.params.fall_back != fall_back) {
        updateChangedStr("fall_back", lagInfo.params.fall_back, fall_back);
        changed = true;
    }

    if (lagInfo.params.fast_rate != fast_rate) {
        updateChangedStr("fast_rate", lagInfo.params.fast_rate, fast_rate);
        changed = true;
    }

    if (lagInfo.params.lacp_mode != lacp_mode) {
        updateChangedStr("lacp_mode", lagInfo.params.lacp_mode, lacp_mode);
        changed = true;
    }

    if (changed) {
        SWSS_LOG_INFO("Intrinsic params of LAG %s has changed: %s", alias.c_str(), changedStr.c_str());
    }

    return changed;
}

bool TeamMgr::restartLag(const std::string &alias, const int min_links, const bool fall_back, const bool fast_rate, const std::string &lacp_mode)
{
    // Save member list and parameters before removing LAG
    LagInfo& lagInfo = getOrCreateLagInfo(alias);
    const std::set<std::string> savedMembers = lagInfo.members;
    LagParams savedParams = lagInfo.params;

    bool removed = removeLag(alias);
    if (removed) {
        task_process_status status = addLag(alias, min_links, fall_back, fast_rate, lacp_mode);

        auto restoreMembers = [this, alias, &savedMembers]() {
            for (const auto& member : savedMembers) {
                addLagMember(alias, member);
            }
        }; 

        if (status == task_need_retry) {
            // addLag failed, need to restore the LAG with old parameters and members
            // so it can be retried in the next iteration
            SWSS_LOG_WARN("Port channel %s restart failed (retry needed), restoring with old parameters and members", alias.c_str());

            if (addLag(alias, savedParams.min_links, savedParams.fall_back, savedParams.fast_rate, savedParams.lacp_mode) == task_success) {
                restoreMembers();
                SWSS_LOG_NOTICE("Restored port channel %s with old parameters after restart failure", alias.c_str());
            } else {
                SWSS_LOG_ERROR("Failed to restore port channel %s after restart failure; LAG is now missing and will not be retried until its config changes", alias.c_str());
            }
            return false;
        }

        restoreMembers();

        SWSS_LOG_NOTICE("Restart port channel %s with new attributes", alias.c_str());
    }

    return removed;
}
>>>>>>> a295efcc (NOS-10884: teammgrd: start teamd with configured LACP mode (independent/coupled) (#756))
