// Tests for the PFCWD ACL create-failure handling in PfcWdAclHandler and the
// m_rolledBack/isValid() consumption in PfcWdSwOrch::startWdActionOnQueue.
//
// Expose the internals under test (m_rolledBack, m_aclTables, PfcWdSwOrch
// entry map) the same way portal.h exposes AclOrch internals.
// Pull in std/system headers before the access-override block below; defining
// private/public around libstdc++ headers breaks them (e.g. <sstream>).
#include <thread>
#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_table.h"

#define private public
#define protected public
#include "pfcactionhandler.h"
#include "pfcwdsworch.h"
#undef protected
#undef private

extern sai_object_id_t gSwitchId;
extern sai_acl_api_t *sai_acl_api;
extern sai_switch_api_t *sai_switch_api;

extern SwitchOrch *gSwitchOrch;
extern CrmOrch *gCrmOrch;
extern PortsOrch *gPortsOrch;
extern RouteOrch *gRouteOrch;
extern FlowCounterRouteOrch *gFlowCounterRouteOrch;
extern IntfsOrch *gIntfsOrch;
extern NeighOrch *gNeighOrch;
extern FgNhgOrch *gFgNhgOrch;
extern Srv6Orch *gSrv6Orch;
extern FdbOrch *gFdbOrch;
extern MirrorOrch *gMirrorOrch;
extern PolicerOrch *gPolicerOrch;
extern VRFOrch *gVrfOrch;
extern QosOrch *gQosOrch;
extern BufferOrch *gBufferOrch;
extern AclOrch *gAclOrch;
extern Directory<Orch*> gDirectory;

namespace pfcactionhandler_test
{
    using namespace std;

    // Restores an environment variable on scope exit.
    struct EnvGuard
    {
        string m_name;
        string m_oldValue;
        bool m_wasSet;

        EnvGuard(const string &name, const string &value) : m_name(name)
        {
            const char *old = getenv(name.c_str());
            m_wasSet = (old != nullptr);
            if (m_wasSet)
            {
                m_oldValue = old;
            }
            setenv(name.c_str(), value.c_str(), 1);
        }

        ~EnvGuard()
        {
            if (m_wasSet)
            {
                setenv(m_name.c_str(), m_oldValue.c_str(), 1);
            }
            else
            {
                unsetenv(m_name.c_str());
            }
        }
    };

    // Injects SAI create failures for ACL tables/entries. failTableAt/failEntryAt
    // are 1-based call indexes (0 = never fail). All other calls go to the real
    // (virtual switch) implementation.
    struct AclCreateFailureInjector
    {
        using create_fn = sai_status_t (*)(sai_object_id_t *, sai_object_id_t,
                                           uint32_t, const sai_attribute_t *);

        create_fn m_origCreateTable;
        create_fn m_origCreateEntry;
        int m_tableCalls = 0;
        int m_entryCalls = 0;
        int m_failTableAt = 0;
        int m_failEntryAt = 0;

        shared_ptr<SaiSpyFunctor<SAI_API_ACL, SAI_OBJECT_TYPE_ACL_TABLE, sai_status_t,
            sai_object_id_t *, sai_object_id_t, uint32_t, const sai_attribute_t *>> m_tableSpy;
        shared_ptr<SaiSpyFunctor<SAI_API_ACL, SAI_OBJECT_TYPE_ACL_ENTRY, sai_status_t,
            sai_object_id_t *, sai_object_id_t, uint32_t, const sai_attribute_t *>> m_entrySpy;

        AclCreateFailureInjector()
        {
            m_origCreateTable = sai_acl_api->create_acl_table;
            m_origCreateEntry = sai_acl_api->create_acl_entry;

            m_tableSpy = SpyOn<SAI_API_ACL, SAI_OBJECT_TYPE_ACL_TABLE>(&sai_acl_api->create_acl_table);
            m_tableSpy->callFake([this](sai_object_id_t *oid, sai_object_id_t sw,
                                        uint32_t count, const sai_attribute_t *attrs) -> sai_status_t {
                ++m_tableCalls;
                if (m_failTableAt != 0 && m_tableCalls == m_failTableAt)
                {
                    return SAI_STATUS_INSUFFICIENT_RESOURCES;
                }
                return m_origCreateTable(oid, sw, count, attrs);
            });

            m_entrySpy = SpyOn<SAI_API_ACL, SAI_OBJECT_TYPE_ACL_ENTRY>(&sai_acl_api->create_acl_entry);
            m_entrySpy->callFake([this](sai_object_id_t *oid, sai_object_id_t sw,
                                        uint32_t count, const sai_attribute_t *attrs) -> sai_status_t {
                ++m_entryCalls;
                if (m_failEntryAt != 0 && m_entryCalls == m_failEntryAt)
                {
                    return SAI_STATUS_INSUFFICIENT_RESOURCES;
                }
                return m_origCreateEntry(oid, sw, count, attrs);
            });
        }
    };

    struct PfcActionHandlerTest : public ::testing::Test
    {
        shared_ptr<swss::DBConnector> m_app_db;
        shared_ptr<swss::DBConnector> m_config_db;
        shared_ptr<swss::DBConnector> m_state_db;
        shared_ptr<swss::DBConnector> m_counters_db;
        shared_ptr<swss::DBConnector> m_chassis_app_db;
        shared_ptr<Table> m_countersTable;

        void SetUp() override
        {
            ::testing_db::reset();

            m_app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
            m_config_db = make_shared<swss::DBConnector>("CONFIG_DB", 0);
            m_state_db = make_shared<swss::DBConnector>("STATE_DB", 0);
            m_counters_db = make_shared<swss::DBConnector>("COUNTERS_DB", 0);
            m_countersTable = make_shared<Table>(m_counters_db.get(), "COUNTERS");

            map<string, string> profile = {
                { "SAI_VS_SWITCH_TYPE", "SAI_VS_SWITCH_TYPE_BCM56850" },
                { "KV_DEVICE_MAC_ADDRESS", "20:03:04:05:06:00" }
            };

            auto status = ut_helper::initSaiApi(profile);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);

            sai_attribute_t attr;
            attr.id = SAI_SWITCH_ATTR_INIT_SWITCH;
            attr.value.booldata = true;
            status = sai_switch_api->create_switch(&gSwitchId, 1, &attr);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);

            attr.id = SAI_SWITCH_ATTR_SRC_MAC_ADDRESS;
            status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);
            gMacAddress = attr.value.mac;

            attr.id = SAI_SWITCH_ATTR_DEFAULT_VIRTUAL_ROUTER_ID;
            status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);
            gVirtualRouterId = attr.value.oid;

            // Orch dependency chain (same as aclorch_ut plus the pieces
            // portsorch_ut/qosorch_ut need for real port creation).
            ASSERT_EQ(gCrmOrch, nullptr);
            gCrmOrch = new CrmOrch(m_config_db.get(), CFG_CRM_TABLE_NAME);

            TableConnector stateDbSwitchTable(m_state_db.get(), "SWITCH_CAPABILITY");
            TableConnector conf_asic_sensors(m_config_db.get(), CFG_ASIC_SENSORS_TABLE_NAME);
            TableConnector app_switch_table(m_app_db.get(), APP_SWITCH_TABLE_NAME);
            vector<TableConnector> switch_tables = { conf_asic_sensors, app_switch_table };

            ASSERT_EQ(gSwitchOrch, nullptr);
            gSwitchOrch = new SwitchOrch(m_app_db.get(), switch_tables, stateDbSwitchTable);

            vector<string> flex_counter_tables = { CFG_FLEX_COUNTER_TABLE_NAME };
            auto* flexCounterOrch = new FlexCounterOrch(m_config_db.get(), flex_counter_tables);
            gDirectory.set(flexCounterOrch);

            const int portsorch_base_pri = 40;
            vector<table_name_with_pri_t> ports_tables = {
                { APP_PORT_TABLE_NAME, portsorch_base_pri + 5 },
                { APP_VLAN_TABLE_NAME, portsorch_base_pri + 2 },
                { APP_VLAN_MEMBER_TABLE_NAME, portsorch_base_pri },
                { APP_LAG_TABLE_NAME, portsorch_base_pri + 4 },
                { APP_LAG_MEMBER_TABLE_NAME, portsorch_base_pri }
            };

            ASSERT_EQ(gPortsOrch, nullptr);
            gPortsOrch = new PortsOrch(m_app_db.get(), m_state_db.get(), ports_tables, m_chassis_app_db.get());

            vector<string> buffer_tables = { APP_BUFFER_POOL_TABLE_NAME,
                                             APP_BUFFER_PROFILE_TABLE_NAME,
                                             APP_BUFFER_QUEUE_TABLE_NAME,
                                             APP_BUFFER_PG_TABLE_NAME,
                                             APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME,
                                             APP_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME };
            ASSERT_EQ(gBufferOrch, nullptr);
            gBufferOrch = new BufferOrch(m_app_db.get(), m_config_db.get(), m_state_db.get(), buffer_tables);

            vector<string> qos_tables = {
                CFG_TC_TO_QUEUE_MAP_TABLE_NAME,
                CFG_SCHEDULER_TABLE_NAME,
                CFG_DSCP_TO_TC_MAP_TABLE_NAME,
                CFG_MPLS_TC_TO_TC_MAP_TABLE_NAME,
                CFG_DOT1P_TO_TC_MAP_TABLE_NAME,
                CFG_QUEUE_TABLE_NAME,
                CFG_PORT_QOS_MAP_TABLE_NAME,
                CFG_WRED_PROFILE_TABLE_NAME,
                CFG_TC_TO_PRIORITY_GROUP_MAP_TABLE_NAME,
                CFG_PFC_PRIORITY_TO_PRIORITY_GROUP_MAP_TABLE_NAME,
                CFG_PFC_PRIORITY_TO_QUEUE_MAP_TABLE_NAME,
                CFG_DSCP_TO_FC_MAP_TABLE_NAME,
                CFG_EXP_TO_FC_MAP_TABLE_NAME,
                CFG_TC_TO_DSCP_MAP_TABLE_NAME
            };
            ASSERT_EQ(gQosOrch, nullptr);
            gQosOrch = new QosOrch(m_config_db.get(), qos_tables);

            static const vector<string> route_pattern_tables = {
                CFG_FLOW_COUNTER_ROUTE_PATTERN_TABLE_NAME,
            };
            gFlowCounterRouteOrch = new FlowCounterRouteOrch(m_config_db.get(), route_pattern_tables);

            ASSERT_EQ(gVrfOrch, nullptr);
            gVrfOrch = new VRFOrch(m_app_db.get(), APP_VRF_TABLE_NAME, m_state_db.get(), STATE_VRF_OBJECT_TABLE_NAME);

            vector<table_name_with_pri_t> intf_tables = {
                { APP_INTF_TABLE_NAME, IntfsOrch::intfsorch_pri },
                { APP_SAG_TABLE_NAME,  IntfsOrch::intfsorch_pri }
            };
            ASSERT_EQ(gIntfsOrch, nullptr);
            gIntfsOrch = new IntfsOrch(m_app_db.get(), intf_tables, gVrfOrch, m_chassis_app_db.get());

            const int fdborch_pri = 20;
            vector<table_name_with_pri_t> app_fdb_tables = {
                { APP_FDB_TABLE_NAME,       FdbOrch::fdborch_pri },
                { APP_VXLAN_FDB_TABLE_NAME, FdbOrch::fdborch_pri },
                { APP_MCLAG_FDB_TABLE_NAME, fdborch_pri }
            };
            TableConnector stateDbFdb(m_state_db.get(), STATE_FDB_TABLE_NAME);
            TableConnector stateMclagDbFdb(m_state_db.get(), STATE_MCLAG_REMOTE_FDB_TABLE_NAME);
            ASSERT_EQ(gFdbOrch, nullptr);
            gFdbOrch = new FdbOrch(m_app_db.get(), app_fdb_tables, stateDbFdb, stateMclagDbFdb, gPortsOrch,
                                   m_config_db.get());

            ASSERT_EQ(gNeighOrch, nullptr);
            gNeighOrch = new NeighOrch(m_app_db.get(), APP_NEIGH_TABLE_NAME, gIntfsOrch, gFdbOrch, gPortsOrch, m_chassis_app_db.get());

            ASSERT_EQ(gFgNhgOrch, nullptr);
            const int fgnhgorch_pri = 15;
            vector<table_name_with_pri_t> fgnhg_tables = {
                { CFG_FG_NHG,        fgnhgorch_pri },
                { CFG_FG_NHG_PREFIX, fgnhgorch_pri },
                { CFG_FG_NHG_MEMBER, fgnhgorch_pri }
            };
            gFgNhgOrch = new FgNhgOrch(m_config_db.get(), m_app_db.get(), m_state_db.get(), fgnhg_tables, gNeighOrch, gIntfsOrch, gVrfOrch);

            ASSERT_EQ(gSrv6Orch, nullptr);
            TableConnector srv6_sid_list_table(m_app_db.get(), APP_SRV6_SID_LIST_TABLE_NAME);
            TableConnector srv6_my_sid_table(m_app_db.get(), APP_SRV6_MY_SID_TABLE_NAME);
            TableConnector srv6_my_sid_cfg_table(m_config_db.get(), CFG_SRV6_MY_SID_TABLE_NAME);
            vector<TableConnector> srv6_tables = {
                srv6_sid_list_table,
                srv6_my_sid_table,
                srv6_my_sid_cfg_table
            };
            gSrv6Orch = new Srv6Orch(m_config_db.get(), m_app_db.get(), srv6_tables, gSwitchOrch, gVrfOrch, gNeighOrch);

            ASSERT_EQ(gRouteOrch, nullptr);
            const int routeorch_pri = 5;
            vector<table_name_with_pri_t> route_tables = {
                { APP_ROUTE_TABLE_NAME,       routeorch_pri },
                { APP_LABEL_ROUTE_TABLE_NAME, routeorch_pri }
            };
            gRouteOrch = new RouteOrch(m_app_db.get(), route_tables, gSwitchOrch, gNeighOrch, gIntfsOrch, gVrfOrch, gFgNhgOrch, gSrv6Orch);

            vector<TableConnector> policer_tables = {
                TableConnector(m_config_db.get(), CFG_POLICER_TABLE_NAME),
                TableConnector(m_config_db.get(), CFG_PORT_STORM_CONTROL_TABLE_NAME)
            };
            ASSERT_EQ(gPolicerOrch, nullptr);
            gPolicerOrch = new PolicerOrch(policer_tables, gPortsOrch);

            TableConnector stateDbMirrorSession(m_state_db.get(), STATE_MIRROR_SESSION_TABLE_NAME);
            TableConnector confDbMirrorSession(m_config_db.get(), CFG_MIRROR_SESSION_TABLE_NAME);
            ASSERT_EQ(gMirrorOrch, nullptr);
            gMirrorOrch = new MirrorOrch(stateDbMirrorSession, confDbMirrorSession,
                                         gPortsOrch, gRouteOrch, gNeighOrch, gFdbOrch, gPolicerOrch, gSwitchOrch);

            // Populate real ports so PfcWdAclHandler can resolve aliases and
            // the ingress DROP rule can match on IN_PORTS.
            Table portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);
            auto ports = ut_helper::getInitialSaiPorts();
            ASSERT_FALSE(ports.empty());
            for (const auto &it : ports)
            {
                portTable.set(it.first, it.second);
            }
            portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });
            portTable.set("PortInitDone", { { "lanes", "0" } });
            gPortsOrch->addExistingData(&portTable);
            static_cast<Orch *>(gPortsOrch)->doTask();

            // AclOrch last: it queries switch ACL capabilities.
            TableConnector confDbAclTable(m_config_db.get(), CFG_ACL_TABLE_TABLE_NAME);
            TableConnector confDbAclRuleTable(m_config_db.get(), CFG_ACL_RULE_TABLE_NAME);
            vector<TableConnector> acl_table_connectors = { confDbAclTable, confDbAclRuleTable };
            ASSERT_EQ(gAclOrch, nullptr);
            gAclOrch = new AclOrch(acl_table_connectors, m_state_db.get(), gSwitchOrch, gPortsOrch,
                                   gMirrorOrch, gNeighOrch, gRouteOrch);

            // The handler's table dict is class-static; start each test clean.
            PfcWdAclHandler::m_aclTables.clear();
        }

        void TearDown() override
        {
            PfcWdAclHandler::m_aclTables.clear();
<<<<<<< HEAD
=======
            PfcWdAclHandler::m_desiredInPorts.clear();
            PfcWdAclHandler::m_dirtyQueueIds.clear();
            PfcWdAclHandler::m_failedFlushes.clear();
            PfcWdAclHandler::m_deferredEgressRules.clear();
>>>>>>> cb949ebc (NOS-20722: [pfcwd] count egress ACL drops in the Broadcom software watchdog's TX drop stats (#1627))

            delete gAclOrch;
            gAclOrch = nullptr;
            delete gMirrorOrch;
            gMirrorOrch = nullptr;
            delete gPolicerOrch;
            gPolicerOrch = nullptr;
            delete gRouteOrch;
            gRouteOrch = nullptr;
            delete gSrv6Orch;
            gSrv6Orch = nullptr;
            delete gFgNhgOrch;
            gFgNhgOrch = nullptr;
            delete gNeighOrch;
            gNeighOrch = nullptr;
            delete gFdbOrch;
            gFdbOrch = nullptr;
            delete gIntfsOrch;
            gIntfsOrch = nullptr;
            delete gVrfOrch;
            gVrfOrch = nullptr;
            delete gFlowCounterRouteOrch;
            gFlowCounterRouteOrch = nullptr;
            delete gQosOrch;
            gQosOrch = nullptr;
            delete gBufferOrch;
            gBufferOrch = nullptr;
            delete gPortsOrch;
            gPortsOrch = nullptr;
            delete gSwitchOrch;
            gSwitchOrch = nullptr;
            delete gCrmOrch;
            gCrmOrch = nullptr;
            gDirectory.m_values.clear();

            auto status = sai_switch_api->remove_switch(gSwitchId);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);
            gSwitchId = 0;

            ut_helper::uninitSaiApi();
        }

        sai_object_id_t getPortOid(const string &alias)
        {
            Port p;
            EXPECT_TRUE(gPortsOrch->getPort(alias, p));
            return p.m_port_id;
        }

        sai_object_id_t getQueueOid(const string &alias, size_t queueIdx)
        {
            Port p;
            EXPECT_TRUE(gPortsOrch->getPort(alias, p));
            EXPECT_GT(p.m_queue_ids.size(), queueIdx);
            return p.m_queue_ids[queueIdx];
        }
    };

<<<<<<< HEAD
=======
    // Broadcom tests. AclOrch registers its table types for the platform when
    // it is constructed, so the platform is set before SetUp() builds it.
    struct PfcActionHandlerBrcmTest : public PfcActionHandlerTest
    {
        unique_ptr<EnvGuard> m_platformGuard;

        void SetUp() override
        {
            m_platformGuard = make_unique<EnvGuard>("platform", BRCM_PLATFORM_SUBSTRING);
            PfcActionHandlerTest::SetUp();

            // A storm has a baseline only once the queue's flex counters are in
            // COUNTERS_DB, so give every queue a row, as a running switch has.
            for (const auto &it : gPortsOrch->getAllPorts())
            {
                for (const auto &queue : it.second.m_queue_ids)
                {
                    m_countersTable->set(sai_serialize_object_id(queue), {
                        { "SAI_QUEUE_STAT_PACKETS", "0" },
                        { "SAI_QUEUE_STAT_DROPPED_PACKETS", "0" } });
                }
            }
        }

        void TearDown() override
        {
            PfcActionHandlerTest::TearDown();
            m_platformGuard.reset();
        }
    };

    // Nothing left for flush() to program: no port is asked for on any queue
    // index and no queue index is waiting to be reconciled.
    static bool noPendingInPorts()
    {
        for (const auto &kv : PfcWdAclHandler::m_desiredInPorts)
        {
            if (!kv.second.empty())
            {
                return false;
            }
        }
        return PfcWdAclHandler::m_dirtyQueueIds.empty();
    }

>>>>>>> cb949ebc (NOS-20722: [pfcwd] count egress ACL drops in the Broadcom software watchdog's TX drop stats (#1627))
    static string ingressRuleName(uint8_t queueId)
    {
        return "Rule_PfcWdAclHandler_" + to_string(queueId);
    }

<<<<<<< HEAD
    // Ingress DROP ACL table creation fails: the handler must mark itself
    // rolled back, leave no half-built state and destroy cleanly.
=======
    static set<sai_object_id_t> ingressRuleInPorts(uint8_t queueId)
    {
        AclRule *rule = gAclOrch->getAclRule(INGRESS_TABLE_DROP, ingressRuleName(queueId));
        if (rule == nullptr)
        {
            return {};
        }
        auto v = rule->getInPorts();
        return set<sai_object_id_t>(v.begin(), v.end());
    }

    // The handler constructor only queues its ingress change; flush() is what
    // PfcWdSwOrch calls at the end of a batch to program it.
    static void makeDeferredDue()
    {
        for (auto &it : PfcWdAclHandler::m_deferredEgressRules)
        {
            it.second.due = std::chrono::steady_clock::now();
        }
    }

    static set<sai_object_id_t> flushHandlers()
    {
        return PfcWdAclHandler::flush();
    }

    // Ingress DROP ACL table creation fails at flush time: the flush reports
    // the queue, leaves no half-built ingress state and drops the pending add.
>>>>>>> cb949ebc (NOS-20722: [pfcwd] count egress ACL drops in the Broadcom software watchdog's TX drop stats (#1627))
    TEST_F(PfcActionHandlerTest, IngressAclTableCreateFailure)
    {
        AclCreateFailureInjector inject;
        inject.m_failTableAt = 1;

        {
            PfcWdAclHandler handler(getPortOid("Ethernet0"), getQueueOid("Ethernet0", 3), 3, m_countersTable);

            EXPECT_TRUE(handler.m_rolledBack);
            EXPECT_FALSE(handler.isValid());

            // No table left behind, neither in AclOrch nor in the class dict.
            EXPECT_EQ(gAclOrch->getTableById(INGRESS_TABLE_DROP), SAI_NULL_OBJECT_ID);
            EXPECT_TRUE(PfcWdAclHandler::m_aclTables.empty());
        }
        // Destructor of a rolled-back handler must not throw (it used to
        // SWSS_LOG_THROW on the missing ingress rule).
    }

    // Ingress table creation succeeds but the ingress rule fails (first-time
    // path): rolled back, table kept, rule absent.
    TEST_F(PfcActionHandlerTest, IngressAclRuleCreateFailureFirstTime)
    {
        AclCreateFailureInjector inject;
        inject.m_failEntryAt = 1;

        {
            PfcWdAclHandler handler(getPortOid("Ethernet0"), getQueueOid("Ethernet0", 3), 3, m_countersTable);

            EXPECT_TRUE(handler.m_rolledBack);
            EXPECT_FALSE(handler.isValid());

            EXPECT_NE(gAclOrch->getTableById(INGRESS_TABLE_DROP), SAI_NULL_OBJECT_ID);
            EXPECT_EQ(gAclOrch->getAclRule(INGRESS_TABLE_DROP, ingressRuleName(3)), nullptr);
        }
    }

    // Ingress rule creation fails when the ingress table already exists
    // (second handler, different queue).
    TEST_F(PfcActionHandlerTest, IngressAclRuleCreateFailureExistingTable)
    {
        AclCreateFailureInjector inject;

        // First handler is fully successful.
        PfcWdAclHandler good(getPortOid("Ethernet0"), getQueueOid("Ethernet0", 3), 3, m_countersTable);
        ASSERT_TRUE(good.isValid());
        ASSERT_NE(gAclOrch->getAclRule(INGRESS_TABLE_DROP, ingressRuleName(3)), nullptr);

        // Second handler for another queue: fail its ingress rule creation.
        inject.m_failEntryAt = inject.m_entryCalls + 1;
        {
            PfcWdAclHandler bad(getPortOid("Ethernet0"), getQueueOid("Ethernet0", 4), 4, m_countersTable);

            EXPECT_TRUE(bad.m_rolledBack);
            EXPECT_FALSE(bad.isValid());
            EXPECT_EQ(gAclOrch->getAclRule(INGRESS_TABLE_DROP, ingressRuleName(4)), nullptr);
        }

        // The first handler's state is untouched.
        EXPECT_NE(gAclOrch->getAclRule(INGRESS_TABLE_DROP, ingressRuleName(3)), nullptr);
    }

    // Egress (per-queue, non-shared) ACL table creation fails: the ingress
    // drop rule installed just before must be rolled back.
    TEST_F(PfcActionHandlerTest, EgressAclTableCreateFailureRollsBackIngress)
    {
        AclCreateFailureInjector inject;
        inject.m_failTableAt = 2; // 1st = ingress table, 2nd = egress table

        {
            PfcWdAclHandler handler(getPortOid("Ethernet0"), getQueueOid("Ethernet0", 3), 3, m_countersTable);

            EXPECT_TRUE(handler.m_rolledBack);
            EXPECT_FALSE(handler.isValid());

            // Ingress table remains, but the ingress rule was rolled back.
            EXPECT_NE(gAclOrch->getTableById(INGRESS_TABLE_DROP), SAI_NULL_OBJECT_ID);
            EXPECT_EQ(gAclOrch->getAclRule(INGRESS_TABLE_DROP, ingressRuleName(3)), nullptr);

            // No egress table was left behind.
            EXPECT_EQ(gAclOrch->getTableById("EgressTable_PfcWdAclHandler_3"), SAI_NULL_OBJECT_ID);
            EXPECT_EQ(PfcWdAclHandler::m_aclTables.count("EgressTable_PfcWdAclHandler_3"), 0U);
        }
    }

    // Egress (per-queue, non-shared) rule creation fails after the egress
    // table was created: the fresh egress table and the ingress rule are both
    // rolled back.
    TEST_F(PfcActionHandlerTest, EgressAclRuleCreateFailureRollsBackTableAndIngress)
    {
        AclCreateFailureInjector inject;
        inject.m_failEntryAt = 2; // 1st = ingress rule, 2nd = egress rule

        {
            PfcWdAclHandler handler(getPortOid("Ethernet0"), getQueueOid("Ethernet0", 3), 3, m_countersTable);

            EXPECT_TRUE(handler.m_rolledBack);
            EXPECT_FALSE(handler.isValid());

            EXPECT_EQ(gAclOrch->getAclRule(INGRESS_TABLE_DROP, ingressRuleName(3)), nullptr);
            EXPECT_EQ(gAclOrch->getTableById("EgressTable_PfcWdAclHandler_3"), SAI_NULL_OBJECT_ID);
            EXPECT_EQ(PfcWdAclHandler::m_aclTables.count("EgressTable_PfcWdAclHandler_3"), 0U);
        }
    }

<<<<<<< HEAD
    // Shared egress table mode (BRCM DNX): shared egress table creation fails
    // on the first handler; ingress rule is rolled back.
    TEST_F(PfcActionHandlerTest, SharedEgressAclTableCreateFailure)
=======
    // Shared egress table mode (Broadcom): shared egress table creation fails
    // on the first handler; nothing is queued for the ingress rule.
    TEST_F(PfcActionHandlerBrcmTest, SharedEgressAclTableCreateFailure)
>>>>>>> cb949ebc (NOS-20722: [pfcwd] count egress ACL drops in the Broadcom software watchdog's TX drop stats (#1627))
    {
        EnvGuard subPlatformGuard("sub_platform", BRCM_DNX_PLATFORM_SUBSTRING);

        AclCreateFailureInjector inject;
        inject.m_failTableAt = 2; // 1st = ingress table, 2nd = shared egress table

        {
            PfcWdAclHandler handler(getPortOid("Ethernet0"), getQueueOid("Ethernet0", 3), 3, m_countersTable);

            EXPECT_TRUE(handler.m_rolledBack);
            EXPECT_FALSE(handler.isValid());

            EXPECT_EQ(gAclOrch->getAclRule(INGRESS_TABLE_DROP, ingressRuleName(3)), nullptr);
            EXPECT_EQ(gAclOrch->getTableById("EgressTable_PfcWdAclHandler"), SAI_NULL_OBJECT_ID);
        }
        // Rolled-back destructor in shared mode also exercises the
        // missing-egress-rule notice path instead of throwing.
    }

    // Shared egress table mode: the shared table already exists (created by a
    // successful handler); a later handler fails its egress rule creation. The
    // shared table must be left in place, only that handler is rolled back.
    TEST_F(PfcActionHandlerBrcmTest, SharedEgressAclRuleCreateFailureKeepsSharedTable)
    {
        EnvGuard subPlatformGuard("sub_platform", BRCM_DNX_PLATFORM_SUBSTRING);

        AclCreateFailureInjector inject;

        PfcWdAclHandler good(getPortOid("Ethernet0"), getQueueOid("Ethernet0", 3), 3, m_countersTable);
        ASSERT_TRUE(good.isValid());
        ASSERT_NE(gAclOrch->getTableById("EgressTable_PfcWdAclHandler"), SAI_NULL_OBJECT_ID);
        ASSERT_NE(gAclOrch->getAclRule("EgressTable_PfcWdAclHandler", "Egress_Rule_PfcWdAclHandler_Ethernet0_3"), nullptr);

        // Second handler on another port/queue: ingress rule (1st entry
        // create) succeeds, egress rule (2nd) fails.
        inject.m_failEntryAt = inject.m_entryCalls + 2;
        {
            PfcWdAclHandler bad(getPortOid("Ethernet4"), getQueueOid("Ethernet4", 4), 4, m_countersTable);

            EXPECT_TRUE(bad.m_rolledBack);
            EXPECT_FALSE(bad.isValid());

            // Shared egress table survives, along with the good handler's rule.
            EXPECT_NE(gAclOrch->getTableById("EgressTable_PfcWdAclHandler"), SAI_NULL_OBJECT_ID);
            EXPECT_NE(gAclOrch->getAclRule("EgressTable_PfcWdAclHandler", "Egress_Rule_PfcWdAclHandler_Ethernet0_3"), nullptr);

            // The failed handler's rules were rolled back.
            EXPECT_EQ(gAclOrch->getAclRule("EgressTable_PfcWdAclHandler", "Egress_Rule_PfcWdAclHandler_Ethernet4_4"), nullptr);
            EXPECT_EQ(gAclOrch->getAclRule(INGRESS_TABLE_DROP, ingressRuleName(4)), nullptr);
        }
    }

    // PfcWdSwOrch consumes isValid(): a storm whose drop handler failed to
    // install its ACLs must be marked "failed" in the PFC_WD_SW_STATE_TABLE
    // and must not record INSTORM state; recovery and a later successful
    // storm go back to "configured".
    TEST_F(PfcActionHandlerTest, PfcWdSwOrchHandlesRolledBackDropHandler)
    {
        vector<string> pfc_wd_tables = { CFG_PFC_WD_TABLE_NAME };
        static const vector<sai_port_stat_t> portStatIds = {
            SAI_PORT_STAT_PFC_3_RX_PKTS,
        };
        static const vector<sai_queue_stat_t> queueStatIds = {
            SAI_QUEUE_STAT_PACKETS,
        };
        static const vector<sai_queue_attr_t> queueAttrIds = {
            SAI_QUEUE_ATTR_PAUSE_STATUS,
        };

        auto orch = make_unique<PfcWdSwOrch<PfcWdAclHandler, PfcWdLossyHandler>>(
            m_config_db.get(), pfc_wd_tables, portStatIds, queueStatIds, queueAttrIds, 100);

        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));
        ASSERT_GT(port.m_queue_ids.size(), 3U);
        sai_object_id_t queueOid = port.m_queue_ids[3];

        // Register the queue directly (bypasses the flex counter plumbing that
        // registerInWdDb would need).
        orch->m_entryMap.emplace(queueOid,
            PfcWdSwOrch<PfcWdAclHandler, PfcWdLossyHandler>::PfcWdQueueEntry(
                PfcWdAction::PFC_WD_ACTION_DROP, port.m_port_id, 3, port.m_alias));

        Table stateTable(m_state_db.get(), "PFC_WD_SW_STATE_TABLE");
        string status;

        {
            // Storm hits while ACL table creation fails: no abort, queue marked failed.
            AclCreateFailureInjector inject;
            inject.m_failTableAt = 1;

            EXPECT_TRUE(orch->startWdActionOnQueue("storm", queueOid));

            auto &entry = orch->m_entryMap.at(queueOid);
            ASSERT_NE(entry.handler, nullptr);
            EXPECT_FALSE(entry.handler->isValid());

            EXPECT_TRUE(stateTable.hget("Ethernet0:3", "status", status));
            EXPECT_EQ(status, "failed");
        }

        // Storm restores: handler released, state back to configured.
        EXPECT_TRUE(orch->startWdActionOnQueue("restore", queueOid));
        EXPECT_EQ(orch->m_entryMap.at(queueOid).handler, nullptr);
        EXPECT_TRUE(stateTable.hget("Ethernet0:3", "status", status));
        EXPECT_EQ(status, "configured");

        // A storm with healthy SAI installs the drop action and records INSTORM.
        EXPECT_TRUE(orch->startWdActionOnQueue("storm", queueOid));
        {
            auto &entry = orch->m_entryMap.at(queueOid);
            ASSERT_NE(entry.handler, nullptr);
            EXPECT_TRUE(entry.handler->isValid());
        }
        EXPECT_TRUE(stateTable.hget("Ethernet0:3", "status", status));
        EXPECT_EQ(status, "configured");

        // Restore before the orch (and gAclOrch) go away.
        EXPECT_TRUE(orch->startWdActionOnQueue("restore", queueOid));
        EXPECT_EQ(orch->m_entryMap.at(queueOid).handler, nullptr);

        // An unknown event is rejected without throwing.
        EXPECT_FALSE(orch->startWdActionOnQueue("bogus_event", queueOid));
    }
<<<<<<< HEAD
=======

    // The ingress rule is created carrying every port of the batch, a later
    // port arrives with a single IN_PORTS write, and the rule is removed
    // rather than written empty when the last port recovers.
    TEST_F(PfcActionHandlerTest, StormBatchWritesRuleOnce)
    {
        AclCreateFailureInjector inject;
        auto orch = makeOrch({ "Ethernet0", "Ethernet4" });
        sai_object_id_t portA = getPortOid("Ethernet0"), portB = getPortOid("Ethernet4");
        sai_object_id_t queueA = getQueueOid("Ethernet0", 3), queueB = getQueueOid("Ethernet4", 3);

        batch(*orch, { { "storm", queueA } });
        EXPECT_EQ(ingressRuleInPorts(3), set<sai_object_id_t>({ portA }));
        EXPECT_TRUE(hasHandler(*orch, queueA));
        EXPECT_EQ(queueStatus(queueA), "stormed");
        EXPECT_EQ(inject.m_inPortsSetCalls, 0);

        batch(*orch, { { "storm", queueB } });
        EXPECT_EQ(ingressRuleInPorts(3), set<sai_object_id_t>({ portA, portB }));
        EXPECT_EQ(inject.m_inPortsSetCalls, 1);

        batch(*orch, { { "restore", queueA }, { "restore", queueB } });
        EXPECT_EQ(gAclOrch->getAclRule(INGRESS_TABLE_DROP, ingressRuleName(3)), nullptr);
        EXPECT_EQ(inject.m_inPortsSetCalls, 1);
        EXPECT_FALSE(hasHandler(*orch, queueA));
        EXPECT_FALSE(hasHandler(*orch, queueB));
        EXPECT_EQ(queueStatus(queueA), "operational");
        EXPECT_EQ(queueStatus(queueB), "operational");
    }

    // A recovers and B storms in one batch whose single write fails. B's
    // handler is kept, marked invalid, and B is no longer asked for; A's
    // removal stays pending and the next flush empties the rule.
    TEST_F(PfcActionHandlerTest, FailedInPortsWriteKeepsHandlerAndRetriesRemoval)
    {
        AclCreateFailureInjector inject;
        auto orch = makeOrch({ "Ethernet0", "Ethernet4" });
        sai_object_id_t portA = getPortOid("Ethernet0");
        sai_object_id_t queueA = getQueueOid("Ethernet0", 3), queueB = getQueueOid("Ethernet4", 3);

        batch(*orch, { { "storm", queueA } });
        ASSERT_EQ(ingressRuleInPorts(3), set<sai_object_id_t>({ portA }));

        inject.m_failInPortsSetAt = 1;
        batch(*orch, { { "restore", queueA }, { "storm", queueB } });

        EXPECT_EQ(inject.m_inPortsSetCalls, 1);
        EXPECT_FALSE(hasHandler(*orch, queueA));
        ASSERT_TRUE(hasHandler(*orch, queueB));
        EXPECT_FALSE(orch->m_entryMap.at(queueB).handler->isValid());
        EXPECT_EQ(ingressRuleInPorts(3), set<sai_object_id_t>({ portA }));

        // The timer's flush retries the removal.
        orch->flushPendingActions();
        EXPECT_EQ(gAclOrch->getAclRule(INGRESS_TABLE_DROP, ingressRuleName(3)), nullptr);
        EXPECT_TRUE(PfcWdAclHandler::m_dirtyQueueIds.empty());

        batch(*orch, { { "restore", queueB } });
        EXPECT_FALSE(hasHandler(*orch, queueB));
        EXPECT_EQ(gAclOrch->getAclRule(INGRESS_TABLE_DROP, ingressRuleName(3)), nullptr);
    }

    // A queue whose add failed stays "stormed" with its (invalid) handler, as
    // the queue is still lossy: re-arming detection would re-detect the same
    // storm every poll and toggle PFC on the port. The restore releases it,
    // and a later storm goes through the normal path.
    TEST_F(PfcActionHandlerTest, FailedInPortsWriteKeepsQueueStormed)
    {
        AclCreateFailureInjector inject;
        auto orch = makeOrch({ "Ethernet0", "Ethernet4" });
        sai_object_id_t portA = getPortOid("Ethernet0"), portB = getPortOid("Ethernet4");
        sai_object_id_t queueA = getQueueOid("Ethernet0", 3), queueB = getQueueOid("Ethernet4", 3);

        batch(*orch, { { "storm", queueA } });
        ASSERT_EQ(ingressRuleInPorts(3), set<sai_object_id_t>({ portA }));

        inject.m_failInPortsSetAt = 1;
        batch(*orch, { { "storm", queueB } });

        ASSERT_TRUE(hasHandler(*orch, queueB));
        EXPECT_FALSE(orch->m_entryMap.at(queueB).handler->isValid());
        EXPECT_EQ(queueStatus(queueB), "stormed");
        EXPECT_EQ(ingressRuleInPorts(3), set<sai_object_id_t>({ portA }));

        // Another storm event for the same queue does not rebuild the handler.
        batch(*orch, { { "storm", queueB } });
        EXPECT_EQ(inject.m_inPortsSetCalls, 1);

        batch(*orch, { { "restore", queueB } });
        EXPECT_FALSE(hasHandler(*orch, queueB));
        EXPECT_EQ(queueStatus(queueB), "operational");
        EXPECT_EQ(ingressRuleInPorts(3), set<sai_object_id_t>({ portA }));

        batch(*orch, { { "storm", queueB } });
        EXPECT_TRUE(orch->m_entryMap.at(queueB).handler->isValid());
        EXPECT_EQ(ingressRuleInPorts(3), set<sai_object_id_t>({ portA, portB }));

        batch(*orch, { { "restore", queueA }, { "restore", queueB } });
        EXPECT_EQ(gAclOrch->getAclRule(INGRESS_TABLE_DROP, ingressRuleName(3)), nullptr);
    }

    // A storm and its restore inside one batch cancel out: the ingress rule is
    // never created, written or removed, and an existing rule is not touched.
    TEST_F(PfcActionHandlerTest, StormAndRestoreInOneBatchCancelOut)
    {
        AclCreateFailureInjector inject;
        auto orch = makeOrch({ "Ethernet0", "Ethernet4" });
        sai_object_id_t portA = getPortOid("Ethernet0");
        sai_object_id_t queueA = getQueueOid("Ethernet0", 3), queueB = getQueueOid("Ethernet4", 3);

        batch(*orch, { { "storm", queueB }, { "restore", queueB } });
        EXPECT_EQ(gAclOrch->getTableById(INGRESS_TABLE_DROP), SAI_NULL_OBJECT_ID);
        EXPECT_EQ(gAclOrch->getAclRule(INGRESS_TABLE_DROP, ingressRuleName(3)), nullptr);
        EXPECT_TRUE(noPendingInPorts());

        batch(*orch, { { "storm", queueA } });
        ASSERT_EQ(ingressRuleInPorts(3), set<sai_object_id_t>({ portA }));
        int sets = inject.m_inPortsSetCalls;

        batch(*orch, { { "storm", queueB }, { "restore", queueB } });
        EXPECT_EQ(inject.m_inPortsSetCalls, sets);
        EXPECT_EQ(ingressRuleInPorts(3), set<sai_object_id_t>({ portA }));

        batch(*orch, { { "restore", queueA } });
        EXPECT_EQ(gAclOrch->getAclRule(INGRESS_TABLE_DROP, ingressRuleName(3)), nullptr);
    }

    // P's removal fails, P storms again before the retry, and then a write for
    // another port fails. P must neither be lost from the rule while its new
    // handler mitigates it, nor be left in the rule once that handler goes.
    TEST_F(PfcActionHandlerTest, PortRestormingDuringFailedRemovalIsNotOrphaned)
    {
        AclCreateFailureInjector inject;
        auto orch = makeOrch({ "Ethernet0", "Ethernet4", "Ethernet8" });
        sai_object_id_t portP = getPortOid("Ethernet0"), portQ = getPortOid("Ethernet4");
        sai_object_id_t queueP = getQueueOid("Ethernet0", 3), queueQ = getQueueOid("Ethernet4", 3);
        sai_object_id_t queueR = getQueueOid("Ethernet8", 3);

        batch(*orch, { { "storm", queueP }, { "storm", queueQ } });
        ASSERT_EQ(ingressRuleInPorts(3), set<sai_object_id_t>({ portP, portQ }));

        // P recovers; the write that takes it out fails.
        inject.m_failInPortsSetAt = inject.m_inPortsSetCalls + 1;
        batch(*orch, { { "restore", queueP } });
        EXPECT_EQ(ingressRuleInPorts(3), set<sai_object_id_t>({ portP, portQ }));

        // P storms again before any retry, and R storms with a failing write.
        inject.m_failInPortsSetAt = inject.m_inPortsSetCalls + 1;
        batch(*orch, { { "storm", queueP }, { "storm", queueR } });
        EXPECT_TRUE(orch->m_entryMap.at(queueP).handler->isValid());
        EXPECT_FALSE(orch->m_entryMap.at(queueR).handler->isValid());
        EXPECT_EQ(ingressRuleInPorts(3), set<sai_object_id_t>({ portP, portQ }));

        // Once every handler is gone, nothing is left in the rule.
        batch(*orch, { { "restore", queueP }, { "restore", queueQ }, { "restore", queueR } });
        EXPECT_EQ(gAclOrch->getAclRule(INGRESS_TABLE_DROP, ingressRuleName(3)), nullptr);
        EXPECT_TRUE(PfcWdAclHandler::m_dirtyQueueIds.empty());
    }

    // If IngressTableDrop disappears from AclOrch while PFCWD still caches it,
    // building the rule throws. The flush must contain that and fail only the
    // queues it was adding, not abort orchagent.
    TEST_F(PfcActionHandlerTest, VanishedIngressTableFailsQueueWithoutThrowing)
    {
        AclCreateFailureInjector inject;
        auto orch = makeOrch({ "Ethernet0" });
        sai_object_id_t queueA = getQueueOid("Ethernet0", 3);

        batch(*orch, { { "storm", queueA } });
        batch(*orch, { { "restore", queueA } });
        ASSERT_EQ(PfcWdAclHandler::m_aclTables.count(INGRESS_TABLE_DROP), 1U);
        ASSERT_TRUE(gAclOrch->removeAclTable(INGRESS_TABLE_DROP));

        EXPECT_NO_THROW(batch(*orch, { { "storm", queueA } }));
        ASSERT_TRUE(hasHandler(*orch, queueA));
        EXPECT_FALSE(orch->m_entryMap.at(queueA).handler->isValid());

        batch(*orch, { { "restore", queueA } });
        EXPECT_FALSE(hasHandler(*orch, queueA));
        PfcWdAclHandler::m_aclTables.erase(INGRESS_TABLE_DROP);
        PfcWdAclHandler::m_ownedAclTables.erase(INGRESS_TABLE_DROP);
    }

    // Under BIG_RED_SWITCH the registered entry keeps its slot with no
    // handler and the live handler sits in m_brsEntryMap. A failed batched
    // write must reach that handler and invalidate it, leaving no
    // half-programmed ingress state.
    TEST_F(PfcActionHandlerTest, FailedWriteInvalidatesBigRedSwitchHandler)
    {
        AclCreateFailureInjector inject;
        auto orch = makeOrch({ "Ethernet0" });
        sai_object_id_t portA = getPortOid("Ethernet0");
        sai_object_id_t queueA = getQueueOid("Ethernet0", 3);

        // What enableBigRedSwitchMode() leaves behind for a registered queue.
        orch->m_bigRedSwitchFlag = true;
        orch->m_entryMap.at(queueA).handler = nullptr;
        auto brs = orch->m_brsEntryMap.emplace(queueA,
            WdOrch::PfcWdQueueEntry(PfcWdAction::PFC_WD_ACTION_DROP, portA, 3, "Ethernet0")).first;
        brs->second.handler = make_shared<PfcWdAclHandler>(portA, queueA, 3, m_countersTable);
        brs->second.handler->initCounters();
        ASSERT_TRUE(brs->second.handler->isValid());
        ASSERT_EQ(queueStatus(queueA), "stormed");

        // The constructor created the egress rule; fail the ingress rule that
        // the flush creates.
        inject.m_failEntryAt = inject.m_entryCalls + 1;
        orch->flushPendingActions();

        ASSERT_NE(orch->m_brsEntryMap.at(queueA).handler, nullptr);
        EXPECT_FALSE(orch->m_brsEntryMap.at(queueA).handler->isValid());
        EXPECT_EQ(gAclOrch->getAclRule(INGRESS_TABLE_DROP, ingressRuleName(3)), nullptr);
        EXPECT_TRUE(PfcWdAclHandler::m_desiredInPorts[3].empty());
        EXPECT_TRUE(PfcWdAclHandler::m_dirtyQueueIds.empty());

        orch->m_brsEntryMap.clear();
        orch->m_bigRedSwitchFlag = false;
    }

    // A port that PortsOrch no longer knows at flush time, removed between
    // detection and the flush, fails only its own queue: the other ports of
    // the batch are still written, and nothing is left queued for it.
    TEST_F(PfcActionHandlerTest, UnknownPortFailsOnlyItsQueue)
    {
        AclCreateFailureInjector inject;
        auto orch = makeOrch({ "Ethernet0", "Ethernet4", "Ethernet8" });
        sai_object_id_t portA = getPortOid("Ethernet0"), portB = getPortOid("Ethernet4"), portC = getPortOid("Ethernet8");
        sai_object_id_t queueA = getQueueOid("Ethernet0", 3), queueB = getQueueOid("Ethernet4", 3), queueC = getQueueOid("Ethernet8", 3);

        batch(*orch, { { "storm", queueA } });
        ASSERT_EQ(ingressRuleInPorts(3), set<sai_object_id_t>({ portA }));

        // B and C storm in one batch; B's port is gone by the time it is flushed.
        EXPECT_TRUE(orch->startWdActionOnQueue("storm", queueB));
        EXPECT_TRUE(orch->startWdActionOnQueue("storm", queueC));
        string aliasB = gPortsOrch->saiOidToAlias.at(portB);
        gPortsOrch->saiOidToAlias.erase(portB);
        auto failed = flushHandlers();
        gPortsOrch->saiOidToAlias[portB] = aliasB;

        EXPECT_EQ(failed, set<sai_object_id_t>({ queueB }));
        EXPECT_EQ(ingressRuleInPorts(3), set<sai_object_id_t>({ portA, portC }));
        EXPECT_EQ(inject.m_inPortsSetCalls, 1);
        EXPECT_EQ(PfcWdAclHandler::m_desiredInPorts[3].count(portB), 0U);
        EXPECT_TRUE(PfcWdAclHandler::m_dirtyQueueIds.empty());

        // Everything the orch still holds comes down cleanly.
        batch(*orch, { { "restore", queueA }, { "restore", queueB }, { "restore", queueC } });
        EXPECT_EQ(gAclOrch->getAclRule(INGRESS_TABLE_DROP, ingressRuleName(3)), nullptr);
    }

    // Broadcom XGS uses the same switch-bound egress table as DNX: one rule
    // per (port, queue) matching OUT_PORT, removed once its final count is taken.
    TEST_F(PfcActionHandlerBrcmTest, XgsUsesSharedEgressTableWithPerPortRules)
    {
        auto counterSpy = SpyOn<SAI_API_ACL, SAI_OBJECT_TYPE_ACL_COUNTER>(&sai_acl_api->get_acl_counter_attribute);
        counterSpy->callFake([&](sai_object_id_t, uint32_t, sai_attribute_t *attrs) -> sai_status_t {
            attrs[0].value.u64 = 0;
            return SAI_STATUS_SUCCESS;
        });

        {
            PfcWdAclHandler a(getPortOid("Ethernet0"), getQueueOid("Ethernet0", 3), 3, m_countersTable);
            PfcWdAclHandler b(getPortOid("Ethernet4"), getQueueOid("Ethernet4", 3), 3, m_countersTable);
            ASSERT_TRUE(a.isValid());
            ASSERT_TRUE(b.isValid());
            ASSERT_TRUE(flushHandlers().empty());

            EXPECT_EQ(gAclOrch->getTableById("EgressTable_PfcWdAclHandler_3"), SAI_NULL_OBJECT_ID);
            const AclTable *table = gAclOrch->getTableByOid(gAclOrch->getTableById("EgressTable_PfcWdAclHandler"));
            ASSERT_NE(table, nullptr);
            EXPECT_TRUE(table->bindToSwitch);
            EXPECT_EQ(table->type.getBindPointTypes(), set<sai_acl_bind_point_type_t>({ SAI_ACL_BIND_POINT_TYPE_SWITCH }));
            EXPECT_EQ(table->type.getMatches().count(SAI_ACL_TABLE_ATTR_FIELD_TC), 1U);
            EXPECT_EQ(table->type.getMatches().count(SAI_ACL_TABLE_ATTR_FIELD_OUT_PORT), 1U);
            EXPECT_NE(gAclOrch->getAclRule("EgressTable_PfcWdAclHandler", "Egress_Rule_PfcWdAclHandler_Ethernet0_3"), nullptr);
            EXPECT_NE(gAclOrch->getAclRule("EgressTable_PfcWdAclHandler", "Egress_Rule_PfcWdAclHandler_Ethernet4_3"), nullptr);
        }
        EXPECT_TRUE(flushHandlers().empty());
        EXPECT_EQ(PfcWdAclHandler::m_deferredEgressRules.size(), 2U);
        makeDeferredDue();
        PfcWdAclHandler::finishDeferred();
        EXPECT_TRUE(PfcWdAclHandler::m_deferredEgressRules.empty());
        EXPECT_EQ(gAclOrch->getAclRule("EgressTable_PfcWdAclHandler", "Egress_Rule_PfcWdAclHandler_Ethernet0_3"), nullptr);
        EXPECT_EQ(gAclOrch->getAclRule("EgressTable_PfcWdAclHandler", "Egress_Rule_PfcWdAclHandler_Ethernet4_3"), nullptr);
    }

    // The storm's TX drops are the queue's own drops plus the packets the
    // handler's egress rule dropped. The rule's counter is read only when the
    // storm's counters are committed for the last time, not on the periodic tick.
    TEST_F(PfcActionHandlerBrcmTest, TxDropsIncludeEgressRuleCounterAtRestore)
    {
        const string queueIdStr = sai_serialize_object_id(getQueueOid("Ethernet0", 3));

        uint64_t aclDrops = 1000;
        int counterReads = 0;
        sai_object_id_t readCounter = SAI_NULL_OBJECT_ID;
        auto counterSpy = SpyOn<SAI_API_ACL, SAI_OBJECT_TYPE_ACL_COUNTER>(&sai_acl_api->get_acl_counter_attribute);
        counterSpy->callFake([&](sai_object_id_t oid, uint32_t count, sai_attribute_t *attrs) -> sai_status_t {
            ++counterReads;
            readCounter = oid;
            EXPECT_EQ(count, 1U);
            EXPECT_EQ(attrs[0].id, SAI_ACL_COUNTER_ATTR_PACKETS);
            attrs[0].value.u64 = aclDrops;
            return SAI_STATUS_SUCCESS;
        });

        {
            PfcWdAclHandler handler(getPortOid("Ethernet0"), getQueueOid("Ethernet0", 3), 3, m_countersTable);
            ASSERT_TRUE(handler.isValid());
            ASSERT_TRUE(flushHandlers().empty());
            AclRule *rule = gAclOrch->getAclRule("EgressTable_PfcWdAclHandler", "Egress_Rule_PfcWdAclHandler_Ethernet0_3");
            ASSERT_NE(rule, nullptr);
            ASSERT_TRUE(rule->hasCounter());

            handler.initCounters();
            const auto atStart = PfcWdBaseOrch::getQueueStats(m_countersTable, queueIdStr);

            handler.commitCounters(true);
            EXPECT_EQ(counterReads, 0);
            EXPECT_EQ(PfcWdBaseOrch::getQueueStats(m_countersTable, queueIdStr).txDropPkt, atStart.txDropPkt);

            handler.commitCounters();
            EXPECT_EQ(counterReads, 1);
            EXPECT_EQ(readCounter, rule->getCounterOid());
            auto stats = PfcWdBaseOrch::getQueueStats(m_countersTable, queueIdStr);
            EXPECT_EQ(stats.txDropPkt, atStart.txDropPkt + aclDrops);
            EXPECT_EQ(stats.txDropPktLast, aclDrops);
        }
    }

    // A storm that never had a baseline (no flex counter row yet) is charged
    // nothing at restore, so the rule's counter is not read then; the deferred
    // final count credits all of its drops.
    TEST_F(PfcActionHandlerBrcmTest, StormWithoutBaselineLeavesRuleDropsToFinalCount)
    {
        const string queueIdStr = sai_serialize_object_id(getQueueOid("Ethernet0", 3));
        m_countersTable->del(queueIdStr);

        uint64_t aclDrops = 0;
        int counterReads = 0;
        auto counterSpy = SpyOn<SAI_API_ACL, SAI_OBJECT_TYPE_ACL_COUNTER>(&sai_acl_api->get_acl_counter_attribute);
        counterSpy->callFake([&](sai_object_id_t, uint32_t, sai_attribute_t *attrs) -> sai_status_t {
            ++counterReads;
            attrs[0].value.u64 = aclDrops;
            return SAI_STATUS_SUCCESS;
        });

        {
            PfcWdAclHandler handler(getPortOid("Ethernet0"), getQueueOid("Ethernet0", 3), 3, m_countersTable);
            ASSERT_TRUE(handler.isValid());
            ASSERT_TRUE(flushHandlers().empty());
            handler.initCounters();
            aclDrops = 70;
            handler.commitCounters();
            EXPECT_EQ(counterReads, 0);
        }
        EXPECT_EQ(PfcWdBaseOrch::getQueueStats(m_countersTable, queueIdStr).txDropPkt, 0U);

        makeDeferredDue();
        PfcWdAclHandler::finishDeferred();
        auto stats = PfcWdBaseOrch::getQueueStats(m_countersTable, queueIdStr);
        EXPECT_EQ(stats.txDropPkt, 70U);
        EXPECT_EQ(stats.txDropPktLast, 70U);
        EXPECT_EQ(gAclOrch->getAclRule("EgressTable_PfcWdAclHandler", "Egress_Rule_PfcWdAclHandler_Ethernet0_3"), nullptr);
    }

    // A failed read of the rule's counter at restore still writes the queue's
    // own stats, and the deferred final count credits the rule's drops.
    TEST_F(PfcActionHandlerBrcmTest, FailedRestoreReadOfRuleCounterIsCreditedLater)
    {
        const string queueIdStr = sai_serialize_object_id(getQueueOid("Ethernet0", 3));

        uint64_t aclDrops = 1000;
        sai_status_t readStatus = SAI_STATUS_FAILURE;
        auto counterSpy = SpyOn<SAI_API_ACL, SAI_OBJECT_TYPE_ACL_COUNTER>(&sai_acl_api->get_acl_counter_attribute);
        counterSpy->callFake([&](sai_object_id_t, uint32_t, sai_attribute_t *attrs) -> sai_status_t {
            attrs[0].value.u64 = aclDrops;
            return readStatus;
        });

        PfcWdQueueStats atStart;
        {
            PfcWdAclHandler handler(getPortOid("Ethernet0"), getQueueOid("Ethernet0", 3), 3, m_countersTable);
            ASSERT_TRUE(handler.isValid());
            ASSERT_TRUE(flushHandlers().empty());
            handler.initCounters();
            atStart = PfcWdBaseOrch::getQueueStats(m_countersTable, queueIdStr);
            handler.commitCounters();
        }

        auto stats = PfcWdBaseOrch::getQueueStats(m_countersTable, queueIdStr);
        EXPECT_EQ(stats.restoreCount, atStart.restoreCount + 1);
        EXPECT_EQ(stats.txDropPkt, atStart.txDropPkt);

        readStatus = SAI_STATUS_SUCCESS;
        makeDeferredDue();
        PfcWdAclHandler::finishDeferred();
        stats = PfcWdBaseOrch::getQueueStats(m_countersTable, queueIdStr);
        EXPECT_EQ(stats.txDropPkt, atStart.txDropPkt + aclDrops);
        EXPECT_EQ(stats.txDropPktLast, aclDrops);
    }

    // Without the shared egress table the egress rule is shared by every
    // bound port, so its counter is not read and TX drops stay the queue's.
    TEST_F(PfcActionHandlerTest, TxDropsUnchangedWithoutSharedEgressTable)
    {
        const string queueIdStr = sai_serialize_object_id(getQueueOid("Ethernet0", 3));

        int counterReads = 0;
        auto counterSpy = SpyOn<SAI_API_ACL, SAI_OBJECT_TYPE_ACL_COUNTER>(&sai_acl_api->get_acl_counter_attribute);
        counterSpy->callFake([&](sai_object_id_t, uint32_t, sai_attribute_t *attrs) -> sai_status_t {
            ++counterReads;
            attrs[0].value.u64 = 1000;
            return SAI_STATUS_SUCCESS;
        });

        PfcWdAclHandler handler(getPortOid("Ethernet0"), getQueueOid("Ethernet0", 3), 3, m_countersTable);
        ASSERT_TRUE(handler.isValid());
        ASSERT_TRUE(flushHandlers().empty());

        handler.initCounters();
        const auto atStart = PfcWdBaseOrch::getQueueStats(m_countersTable, queueIdStr);
        handler.commitCounters(true);
        handler.commitCounters();
        EXPECT_EQ(PfcWdBaseOrch::getQueueStats(m_countersTable, queueIdStr).txDropPkt, atStart.txDropPkt);
        EXPECT_EQ(counterReads, 0);
    }

    // The restore-time read can miss the storm's last drops while the counter
    // cache lags. The egress rule is disabled, not removed, and the drops it
    // counted after that read are credited to the storm when the final count
    // is taken.
    TEST_F(PfcActionHandlerBrcmTest, DeferredFinalCountCreditsLateDrops)
    {
        const string rule = "Egress_Rule_PfcWdAclHandler_Ethernet0_3";
        const string queueIdStr = sai_serialize_object_id(getQueueOid("Ethernet0", 3));

        uint64_t aclDrops = 0;
        auto counterSpy = SpyOn<SAI_API_ACL, SAI_OBJECT_TYPE_ACL_COUNTER>(&sai_acl_api->get_acl_counter_attribute);
        counterSpy->callFake([&](sai_object_id_t, uint32_t, sai_attribute_t *attrs) -> sai_status_t {
            attrs[0].value.u64 = aclDrops;
            return SAI_STATUS_SUCCESS;
        });
        vector<bool> adminStates;
        auto origSet = sai_acl_api->set_acl_entry_attribute;
        auto setSpy = SpyOn<SAI_API_ACL, SAI_OBJECT_TYPE_ACL_ENTRY>(&sai_acl_api->set_acl_entry_attribute);
        setSpy->callFake([&](sai_object_id_t oid, const sai_attribute_t *attr) -> sai_status_t {
            if (attr->id == SAI_ACL_ENTRY_ATTR_ADMIN_STATE)
            {
                adminStates.push_back(attr->value.booldata);
            }
            return origSet(oid, attr);
        });

        {
            PfcWdAclHandler handler(getPortOid("Ethernet0"), getQueueOid("Ethernet0", 3), 3, m_countersTable);
            ASSERT_TRUE(handler.isValid());
            ASSERT_TRUE(flushHandlers().empty());
            handler.initCounters();
            aclDrops = 100;
            handler.commitCounters();
        }

        auto stats = PfcWdBaseOrch::getQueueStats(m_countersTable, queueIdStr);
        const uint64_t atRestore = stats.txDropPkt;
        EXPECT_EQ(stats.txDropPktLast, 100U);
        EXPECT_EQ(adminStates, vector<bool>({ false }));
        ASSERT_NE(gAclOrch->getAclRule("EgressTable_PfcWdAclHandler", rule), nullptr);

        // Not due yet: nothing changes.
        aclDrops = 250;
        PfcWdAclHandler::finishDeferred();
        EXPECT_NE(gAclOrch->getAclRule("EgressTable_PfcWdAclHandler", rule), nullptr);
        EXPECT_EQ(PfcWdBaseOrch::getQueueStats(m_countersTable, queueIdStr).txDropPkt, atRestore);

        makeDeferredDue();
        PfcWdAclHandler::finishDeferred();
        stats = PfcWdBaseOrch::getQueueStats(m_countersTable, queueIdStr);
        EXPECT_EQ(stats.txDropPkt, atRestore + 150);
        EXPECT_EQ(stats.txDropPktLast, 250U);
        EXPECT_EQ(gAclOrch->getAclRule("EgressTable_PfcWdAclHandler", rule), nullptr);
        EXPECT_TRUE(PfcWdAclHandler::m_deferredEgressRules.empty());
    }

    // A queue that storms again before its final count was taken: the count is
    // taken and credited to the earlier storm, and the new storm gets a fresh
    // rule. Stopping the watchdog takes every pending count too.
    TEST_F(PfcActionHandlerBrcmTest, DeferredFinalCountTakenOnRestormAndClear)
    {
        const string rule = "Egress_Rule_PfcWdAclHandler_Ethernet0_3";
        const string queueIdStr = sai_serialize_object_id(getQueueOid("Ethernet0", 3));

        uint64_t aclDrops = 0;
        auto counterSpy = SpyOn<SAI_API_ACL, SAI_OBJECT_TYPE_ACL_COUNTER>(&sai_acl_api->get_acl_counter_attribute);
        counterSpy->callFake([&](sai_object_id_t, uint32_t, sai_attribute_t *attrs) -> sai_status_t {
            attrs[0].value.u64 = aclDrops;
            return SAI_STATUS_SUCCESS;
        });

        {
            PfcWdAclHandler first(getPortOid("Ethernet0"), getQueueOid("Ethernet0", 3), 3, m_countersTable);
            ASSERT_TRUE(flushHandlers().empty());
            first.initCounters();
            aclDrops = 40;
            first.commitCounters();
        }
        AclRule *oldRule = gAclOrch->getAclRule("EgressTable_PfcWdAclHandler", rule);
        ASSERT_NE(oldRule, nullptr);
        const sai_object_id_t oldOid = oldRule->getOid();

        aclDrops = 60;
        {
            PfcWdAclHandler second(getPortOid("Ethernet0"), getQueueOid("Ethernet0", 3), 3, m_countersTable);
            ASSERT_TRUE(second.isValid());
            ASSERT_TRUE(flushHandlers().empty());
            EXPECT_EQ(PfcWdBaseOrch::getQueueStats(m_countersTable, queueIdStr).txDropPktLast, 60U);
            AclRule *newRule = gAclOrch->getAclRule("EgressTable_PfcWdAclHandler", rule);
            ASSERT_NE(newRule, nullptr);
            EXPECT_NE(newRule->getOid(), oldOid);
            EXPECT_TRUE(PfcWdAclHandler::m_deferredEgressRules.empty());
        }

        EXPECT_EQ(PfcWdAclHandler::m_deferredEgressRules.size(), 1U);
        PfcWdAclHandler::clear();
        EXPECT_TRUE(PfcWdAclHandler::m_deferredEgressRules.empty());
        EXPECT_EQ(gAclOrch->getTableById("EgressTable_PfcWdAclHandler"), SAI_NULL_OBJECT_ID);
    }

    // An egress rule whose removal failed is never reused by the queue's next
    // storm: its counter holds the earlier storm's drops, which were already
    // credited. The next storm removes it first, and fails the queue if it
    // cannot, leaving the rule alone.
    TEST_F(PfcActionHandlerBrcmTest, StaleEgressRuleIsNeverReused)
    {
        const string rule = "Egress_Rule_PfcWdAclHandler_Ethernet0_3";
        const string queueIdStr = sai_serialize_object_id(getQueueOid("Ethernet0", 3));

        uint64_t aclDrops = 0;
        auto counterSpy = SpyOn<SAI_API_ACL, SAI_OBJECT_TYPE_ACL_COUNTER>(&sai_acl_api->get_acl_counter_attribute);
        counterSpy->callFake([&](sai_object_id_t, uint32_t, sai_attribute_t *attrs) -> sai_status_t {
            attrs[0].value.u64 = aclDrops;
            return SAI_STATUS_SUCCESS;
        });
        bool failRemove = false;
        auto origRemove = sai_acl_api->remove_acl_entry;
        auto removeSpy = SpyOn<SAI_API_ACL, SAI_OBJECT_TYPE_ACL_ENTRY>(&sai_acl_api->remove_acl_entry);
        removeSpy->callFake([&](sai_object_id_t oid) -> sai_status_t {
            return failRemove ? SAI_STATUS_FAILURE : origRemove(oid);
        });

        {
            PfcWdAclHandler first(getPortOid("Ethernet0"), getQueueOid("Ethernet0", 3), 3, m_countersTable);
            ASSERT_TRUE(flushHandlers().empty());
            first.initCounters();
            aclDrops = 100;
            first.commitCounters();
        }
        failRemove = true;
        makeDeferredDue();
        PfcWdAclHandler::finishDeferred();
        AclRule *stale = gAclOrch->getAclRule("EgressTable_PfcWdAclHandler", rule);
        ASSERT_NE(stale, nullptr);
        const sai_object_id_t staleOid = stale->getOid();
        const uint64_t afterFirst = PfcWdBaseOrch::getQueueStats(m_countersTable, queueIdStr).txDropPkt;

        {
            PfcWdAclHandler second(getPortOid("Ethernet0"), getQueueOid("Ethernet0", 3), 3, m_countersTable);
            EXPECT_FALSE(second.isValid());
        }
        stale = gAclOrch->getAclRule("EgressTable_PfcWdAclHandler", rule);
        ASSERT_NE(stale, nullptr);
        EXPECT_EQ(stale->getOid(), staleOid);
        EXPECT_TRUE(PfcWdAclHandler::m_deferredEgressRules.empty());
        EXPECT_EQ(PfcWdBaseOrch::getQueueStats(m_countersTable, queueIdStr).txDropPkt, afterFirst);

        failRemove = false;
        {
            PfcWdAclHandler third(getPortOid("Ethernet0"), getQueueOid("Ethernet0", 3), 3, m_countersTable);
            ASSERT_TRUE(third.isValid());
            ASSERT_TRUE(flushHandlers().empty());
            AclRule *fresh = gAclOrch->getAclRule("EgressTable_PfcWdAclHandler", rule);
            ASSERT_NE(fresh, nullptr);
            EXPECT_NE(fresh->getOid(), staleOid);
            third.initCounters();
            aclDrops = 30;
            third.commitCounters();
        }
        EXPECT_EQ(PfcWdBaseOrch::getQueueStats(m_countersTable, queueIdStr).txDropPkt, afterFirst + 30);
    }

    // A final count whose rule removal fails is retried on later ticks, with
    // the drops already credited not credited again; after the last attempt
    // the entry is dropped and the failure logged.
    TEST_F(PfcActionHandlerBrcmTest, DeferredRemovalFailureIsRetried)
    {
        const string rule = "Egress_Rule_PfcWdAclHandler_Ethernet0_3";
        const string queueIdStr = sai_serialize_object_id(getQueueOid("Ethernet0", 3));

        uint64_t aclDrops = 0;
        auto counterSpy = SpyOn<SAI_API_ACL, SAI_OBJECT_TYPE_ACL_COUNTER>(&sai_acl_api->get_acl_counter_attribute);
        counterSpy->callFake([&](sai_object_id_t, uint32_t, sai_attribute_t *attrs) -> sai_status_t {
            attrs[0].value.u64 = aclDrops;
            return SAI_STATUS_SUCCESS;
        });
        bool failRemove = false;
        auto origRemove = sai_acl_api->remove_acl_entry;
        auto removeSpy = SpyOn<SAI_API_ACL, SAI_OBJECT_TYPE_ACL_ENTRY>(&sai_acl_api->remove_acl_entry);
        removeSpy->callFake([&](sai_object_id_t oid) -> sai_status_t {
            return failRemove ? SAI_STATUS_FAILURE : origRemove(oid);
        });

        {
            PfcWdAclHandler handler(getPortOid("Ethernet0"), getQueueOid("Ethernet0", 3), 3, m_countersTable);
            ASSERT_TRUE(flushHandlers().empty());
            handler.initCounters();
            aclDrops = 100;
            handler.commitCounters();
        }
        const uint64_t atRestore = PfcWdBaseOrch::getQueueStats(m_countersTable, queueIdStr).txDropPkt;

        failRemove = true;
        aclDrops = 150;
        makeDeferredDue();
        PfcWdAclHandler::finishDeferred();
        EXPECT_EQ(PfcWdBaseOrch::getQueueStats(m_countersTable, queueIdStr).txDropPkt, atRestore + 50);
        EXPECT_NE(gAclOrch->getAclRule("EgressTable_PfcWdAclHandler", rule), nullptr);
        ASSERT_EQ(PfcWdAclHandler::m_deferredEgressRules.count(rule), 1U);

        // Not due again until the next tick.
        PfcWdAclHandler::finishDeferred();
        EXPECT_EQ(PfcWdAclHandler::m_deferredEgressRules.count(rule), 1U);

        failRemove = false;
        aclDrops = 160;
        makeDeferredDue();
        PfcWdAclHandler::finishDeferred();
        auto stats = PfcWdBaseOrch::getQueueStats(m_countersTable, queueIdStr);
        EXPECT_EQ(stats.txDropPkt, atRestore + 60);
        EXPECT_EQ(stats.txDropPktLast, 160U);
        EXPECT_EQ(gAclOrch->getAclRule("EgressTable_PfcWdAclHandler", rule), nullptr);
        EXPECT_TRUE(PfcWdAclHandler::m_deferredEgressRules.empty());

        // A removal that keeps failing is given up after three attempts.
        aclDrops = 0;
        {
            PfcWdAclHandler handler(getPortOid("Ethernet0"), getQueueOid("Ethernet0", 3), 3, m_countersTable);
            ASSERT_TRUE(handler.isValid());
            ASSERT_TRUE(flushHandlers().empty());
        }
        failRemove = true;
        for (int attempt = 1; attempt <= 3; attempt++)
        {
            EXPECT_EQ(PfcWdAclHandler::m_deferredEgressRules.count(rule), 1U);
            makeDeferredDue();
            PfcWdAclHandler::finishDeferred();
        }
        EXPECT_TRUE(PfcWdAclHandler::m_deferredEgressRules.empty());
        EXPECT_NE(gAclOrch->getAclRule("EgressTable_PfcWdAclHandler", rule), nullptr);
        failRemove = false;
    }

    // A final count whose counter read fails keeps the rule and is retried on
    // a later tick, so the storm's late drops are still credited.
    TEST_F(PfcActionHandlerBrcmTest, DeferredReadFailureIsRetried)
    {
        const string rule = "Egress_Rule_PfcWdAclHandler_Ethernet0_3";
        const string queueIdStr = sai_serialize_object_id(getQueueOid("Ethernet0", 3));

        uint64_t aclDrops = 0;
        sai_status_t readStatus = SAI_STATUS_SUCCESS;
        auto counterSpy = SpyOn<SAI_API_ACL, SAI_OBJECT_TYPE_ACL_COUNTER>(&sai_acl_api->get_acl_counter_attribute);
        counterSpy->callFake([&](sai_object_id_t, uint32_t, sai_attribute_t *attrs) -> sai_status_t {
            attrs[0].value.u64 = aclDrops;
            return readStatus;
        });

        {
            PfcWdAclHandler handler(getPortOid("Ethernet0"), getQueueOid("Ethernet0", 3), 3, m_countersTable);
            ASSERT_TRUE(flushHandlers().empty());
            handler.initCounters();
            aclDrops = 100;
            handler.commitCounters();
        }
        const uint64_t atRestore = PfcWdBaseOrch::getQueueStats(m_countersTable, queueIdStr).txDropPkt;

        readStatus = SAI_STATUS_FAILURE;
        makeDeferredDue();
        PfcWdAclHandler::finishDeferred();
        EXPECT_EQ(PfcWdBaseOrch::getQueueStats(m_countersTable, queueIdStr).txDropPkt, atRestore);
        EXPECT_NE(gAclOrch->getAclRule("EgressTable_PfcWdAclHandler", rule), nullptr);
        EXPECT_EQ(PfcWdAclHandler::m_deferredEgressRules.count(rule), 1U);

        readStatus = SAI_STATUS_SUCCESS;
        aclDrops = 130;
        makeDeferredDue();
        PfcWdAclHandler::finishDeferred();
        EXPECT_EQ(PfcWdBaseOrch::getQueueStats(m_countersTable, queueIdStr).txDropPkt, atRestore + 30);
        EXPECT_EQ(gAclOrch->getAclRule("EgressTable_PfcWdAclHandler", rule), nullptr);
        EXPECT_TRUE(PfcWdAclHandler::m_deferredEgressRules.empty());
    }

    // Stopping the watchdog on one port takes that port's final counts at
    // once, so nothing writes to its queues' counters after the stop. Other
    // ports' final counts still wait for the timer.
    TEST_F(PfcActionHandlerBrcmTest, SinglePortStopTakesFinalCountAtOnce)
    {
        auto orch = makeOrch({ "Ethernet0", "Ethernet4" });
        const sai_object_id_t q0 = getQueueOid("Ethernet0", 3);
        const sai_object_id_t q4 = getQueueOid("Ethernet4", 3);
        const string q0Str = sai_serialize_object_id(q0);

        uint64_t aclDrops = 0;
        auto counterSpy = SpyOn<SAI_API_ACL, SAI_OBJECT_TYPE_ACL_COUNTER>(&sai_acl_api->get_acl_counter_attribute);
        counterSpy->callFake([&](sai_object_id_t, uint32_t, sai_attribute_t *attrs) -> sai_status_t {
            attrs[0].value.u64 = aclDrops;
            return SAI_STATUS_SUCCESS;
        });

        batch(*orch, { { "storm", q0 }, { "storm", q4 } });
        aclDrops = 100;
        batch(*orch, { { "restore", q0 }, { "restore", q4 } });
        ASSERT_EQ(PfcWdAclHandler::m_deferredEgressRules.size(), 2U);
        const uint64_t atRestore = PfcWdBaseOrch::getQueueStats(m_countersTable, q0Str).txDropPkt;

        aclDrops = 140;
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));
        orch->unregisterFromWdDb(port);

        EXPECT_EQ(PfcWdBaseOrch::getQueueStats(m_countersTable, q0Str).txDropPkt, atRestore + 40);
        EXPECT_EQ(gAclOrch->getAclRule("EgressTable_PfcWdAclHandler", "Egress_Rule_PfcWdAclHandler_Ethernet0_3"), nullptr);
        ASSERT_EQ(PfcWdAclHandler::m_deferredEgressRules.size(), 1U);
        EXPECT_EQ(PfcWdAclHandler::m_deferredEgressRules.count("Egress_Rule_PfcWdAclHandler_Ethernet4_3"), 1U);

        aclDrops = 200;
        makeDeferredDue();
        PfcWdAclHandler::finishDeferred();
        EXPECT_EQ(PfcWdBaseOrch::getQueueStats(m_countersTable, q0Str).txDropPkt, atRestore + 40);
        EXPECT_TRUE(PfcWdAclHandler::m_deferredEgressRules.empty());
    }

    // A mass restore is finished over several ticks. With cheap removals a call
    // stops at 128 rules, oldest first.
    TEST_F(PfcActionHandlerTest, DeferredFinishIsBoundedPerCall)
    {
        const auto now = std::chrono::steady_clock::now();
        for (int i = 0; i < 200; i++)
        {
            PfcWdAclHandler::m_deferredEgressRules["rule_" + to_string(1000 + i)] =
                { "NoSuchTable", "oid:0x0", m_countersTable, 0, now - std::chrono::milliseconds(300 - i) };
        }

        PfcWdAclHandler::finishDeferred();
        ASSERT_EQ(PfcWdAclHandler::m_deferredEgressRules.size(), 72U);
        for (int i = 128; i < 200; i++)
        {
            EXPECT_EQ(PfcWdAclHandler::m_deferredEgressRules.count("rule_" + to_string(1000 + i)), 1U);
        }

        PfcWdAclHandler::finishDeferred();
        EXPECT_TRUE(PfcWdAclHandler::m_deferredEgressRules.empty());
    }

    // With slow removals a call stops once its time budget is spent, but always
    // finishes at least one rule, so the backlog keeps draining.
    TEST_F(PfcActionHandlerBrcmTest, DeferredFinishStopsAtTimeBudget)
    {
        const vector<string> ports = { "Ethernet0", "Ethernet4", "Ethernet8", "Ethernet12",
                                       "Ethernet16", "Ethernet20", "Ethernet24", "Ethernet28" };
        auto counterSpy = SpyOn<SAI_API_ACL, SAI_OBJECT_TYPE_ACL_COUNTER>(&sai_acl_api->get_acl_counter_attribute);
        counterSpy->callFake([&](sai_object_id_t, uint32_t, sai_attribute_t *attrs) -> sai_status_t {
            attrs[0].value.u64 = 0;
            return SAI_STATUS_SUCCESS;
        });

        for (const auto &p : ports)
        {
            PfcWdAclHandler handler(getPortOid(p), getQueueOid(p, 3), 3, m_countersTable);
            ASSERT_TRUE(handler.isValid());
            ASSERT_TRUE(flushHandlers().empty());
        }
        ASSERT_EQ(PfcWdAclHandler::m_deferredEgressRules.size(), ports.size());
        makeDeferredDue();

        auto origRemove = sai_acl_api->remove_acl_entry;
        auto removeSpy = SpyOn<SAI_API_ACL, SAI_OBJECT_TYPE_ACL_ENTRY>(&sai_acl_api->remove_acl_entry);
        removeSpy->callFake([&](sai_object_id_t oid) -> sai_status_t {
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            return origRemove(oid);
        });

        size_t calls = 0;
        size_t left = ports.size();
        while (!PfcWdAclHandler::m_deferredEgressRules.empty() && calls < ports.size())
        {
            PfcWdAclHandler::finishDeferred();
            size_t now = PfcWdAclHandler::m_deferredEgressRules.size();
            EXPECT_GE(left - now, 1U);
            EXPECT_LE(left - now, 2U);
            left = now;
            calls++;
        }
        EXPECT_TRUE(PfcWdAclHandler::m_deferredEgressRules.empty());
        EXPECT_GE(calls, 4U);
    }
>>>>>>> cb949ebc (NOS-20722: [pfcwd] count egress ACL drops in the Broadcom software watchdog's TX drop stats (#1627))
}
