#include <csignal>
#include <iostream>
#include <deque>

#include <logger.h>
#include <select.h>
#include <dbconnector.h>
#include <subscriberstatetable.h>
#include <schema.h>

#include "teamdctl_mgr.h"
#include "values_store.h"
#include "lacp_sync.h"
#include "subintf.h"


bool g_run = true;


/// This function extract all available updates from the table
/// and add or remove LAG interfaces from the TeamdCtlMgr
///
/// @param table reference to the SubscriberStateTable
/// @param mgr   reference to the TeamdCtlMgr
///
void update_interfaces(swss::SubscriberStateTable & table, TeamdCtlMgr & mgr)
{
    std::deque<swss::KeyOpFieldsValuesTuple> entries;

    table.pops(entries);
    for (const auto & entry: entries)
    {
        const auto & lag_name = kfvKey(entry);
        const auto & op = kfvOp(entry);

        if (lag_name.find(VLAN_SUB_INTERFACE_SEPARATOR) != std::string::npos)
        {
            SWSS_LOG_INFO("Skip subintf %s statedb event", lag_name.c_str());
            continue;
        }
        if (op == "SET")
        {
            mgr.add_lag(lag_name);
        }
        else if (op == "DEL")
        {
            mgr.remove_lag(lag_name);
        }
        else
        {
            SWSS_LOG_WARN("Got invalid operation: '%s' with key '%s'", op.c_str(), lag_name.c_str());
        }
    }
}

/// Drain orchagent's collecting-confirm updates from STATE_DB and relay each
/// one back to teamd via LacpSync.
///
/// @param table reference to the confirm SubscriberStateTable
/// @param lacp  reference to the LacpSync
///
void process_member_collecting_confirmations(swss::SubscriberStateTable & table, LacpSync & lacp)
{
    std::deque<swss::KeyOpFieldsValuesTuple> entries;

    table.pops(entries);
    for (const auto & entry: entries)
    {
        lacp.process_member_collecting_confirmation(entry);
    }
}

///
/// Signal handler
///
void sig_handler(int signo)
{
    (void)signo;
    g_run = false;
}

///
/// main function
///
int main()
{
    const int ms_select_timeout = 1000;

    sighandler_t sig_res;

    sig_res = signal(SIGTERM, sig_handler);
    if (sig_res == SIG_ERR)
    {
        std::cerr << "Can't set signal handler for SIGTERM\n";
        return -1;
    }

    sig_res = signal(SIGINT, sig_handler);
    if (sig_res == SIG_ERR)
    {
        std::cerr << "Can't set signal handler for SIGINT\n";
        return -1;
    }

    int rc = 0;
    try
    {
        swss::Logger::linkToDbNative("tlm_teamd");
        SWSS_LOG_NOTICE("Starting");
        swss::DBConnector db("STATE_DB", 0);
<<<<<<< HEAD
=======
        swss::DBConnector counters_db("COUNTERS_DB", 0);
        swss::DBConnector appl_db("APPL_DB", 0);
>>>>>>> cf9d2c9b (NOS-11514: tlm_teamd: relay independent-mode LACP collecting handshake (#786))

        ValuesStore values_store(&db);
        TeamdCtlMgr teamdctl_mgr;
        LacpSync lacp_sync(&appl_db, teamdctl_mgr);

        swss::Select s;
        swss::Selectable * event;
        swss::SubscriberStateTable sst_lag(&db, STATE_LAG_TABLE_NAME);
        // orchagent's per-member ingress-programmed confirmation.
        swss::SubscriberStateTable sst_member_collecting_confirmation(&db, STATE_LAG_MEMBER_LACP_TABLE_NAME);
        s.addSelectable(&sst_lag);
        s.addSelectable(&sst_member_collecting_confirmation);

        while (g_run && rc == 0)
        {
            int res = s.select(&event, ms_select_timeout);
            if (res == swss::Select::OBJECT)
            {
                if (event == &sst_lag)
                {
                    update_interfaces(sst_lag, teamdctl_mgr);
                }
                else if (event == &sst_member_collecting_confirmation)
                {
                    process_member_collecting_confirmations(sst_member_collecting_confirmation, lacp_sync);
                }
                // Re-poll teamd and drive both consumers on every event
                // (including confirm ACKs) so concurrent state changes are
                // caught; the m_published dedup keeps the publish idempotent.
                const auto & dumps = teamdctl_mgr.get_dumps(false);
                values_store.update(dumps);
                lacp_sync.publish_all_members_collecting_requests_db(dumps);
            }
            else if (res == swss::Select::ERROR)
            {
                SWSS_LOG_ERROR("Select returned ERROR");
                rc = -2;
            }
            else if (res == swss::Select::TIMEOUT)
            {
                teamdctl_mgr.process_add_queue();
                // In the case of lag removal, there is a scenario where the select::TIMEOUT
                // occurs, it triggers get_dumps incorrectly for resource which was in process of
                // getting deleted. The fix here is to retry and check if this is a real failure.
                const auto & dumps = teamdctl_mgr.get_dumps(true);
                values_store.update(dumps);
                lacp_sync.publish_all_members_collecting_requests_db(dumps);
            }
            else
            {
                SWSS_LOG_ERROR("Select returned unknown value");
                rc = -3;
            }
	    }
        SWSS_LOG_NOTICE("Exiting");
    }
    catch (const std::exception & e)
    {
        std::cerr << "Exception \"" << e.what() << "\" had been thrown" << std::endl;
        SWSS_LOG_ERROR("Exception '%s' had been thrown", e.what());
        rc = -1;
    }

    return rc;
}
