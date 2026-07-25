#include <jansson.h>

#include <logger.h>
#include <schema.h>
#include <table.h>
#include "lacp_sync.h"

// teamd runner state items accessed over teamdctl (tlm_teamd-internal; the DB
// contract constants come from swss-common schema.h).
#define TEAMD_ITEM_COLLECTING_REQUESTED  "collecting_requested"
#define TEAMD_ITEM_COLLECTING_REQUEST_ID "collecting_request_id"
#define TEAMD_ITEM_COLLECTING_CONFIRM    "collecting_confirm"
#define TEAMD_COLLECTING_CONFIRM_WITHDRAW "0"

LacpSync::LacpSync(swss::DBConnector * appl_db, TeamdCtlMgr & teamdctl_mgr) :
    m_app_table(appl_db, APP_LAG_MEMBER_LACP_TABLE_NAME),
    m_teamdctl_mgr(teamdctl_mgr)
{
    // Seed from rows a previous incarnation left in APP_DB so the first cycle
    // prunes restart orphans. Include staging ("_"+table): pre-orchagent, rows
    // live there until a ConsumerStateTable drains them.
    const std::string staging_table = std::string("_") + APP_LAG_MEMBER_LACP_TABLE_NAME;
    for (const auto & table_name: { std::string(APP_LAG_MEMBER_LACP_TABLE_NAME), staging_table })
    {
        swss::Table app_table(appl_db, table_name);
        std::vector<std::string> keys;
        app_table.getKeys(keys);
        for (const auto & key: keys)
        {
            m_published[key] = { "", "" };  // sentinel: never equals a real request
        }
    }
}

/// Drop rows for members that genuinely went away: either their LAG reported
/// this cycle without them (left the LAG), or the LAG is no longer tracked at
/// all (removed).
///
/// @param present_lags LAGs that produced a dump this cycle
/// @param seen_keys    member keys published this cycle
///
void LacpSync::prune_member_db(const std::unordered_set<std::string> & present_lags,
                     const std::unordered_set<std::string> & seen_keys)
{
    const auto known_lags = m_teamdctl_mgr.get_lags();
    std::vector<std::string> to_withdraw;
    for (const auto & p: m_published)
    {
        const auto & app_key = p.first;
        if (seen_keys.count(app_key) != 0)
        {
            continue;
        }
        const auto lag = app_key.substr(0, app_key.find(':'));
        const bool lag_removed = known_lags.count(lag) == 0;
        const bool member_left = present_lags.count(lag) != 0;
        if (lag_removed || member_left)
        {
            to_withdraw.push_back(app_key);
        }
    }
    for (const auto & app_key: to_withdraw)
    {
        m_app_table.del(app_key);
        m_published.erase(app_key);          // remove from local cache
        m_incomplete_logged.erase(app_key);  // reset the log-once gate for a future rejoin
        SWSS_LOG_INFO("Withdrew collecting request. key='%s'", app_key.c_str());
    }
}

/// Publish one member's APP_DB row
///
/// @param app_key    the "<lag>:<member>" table key
/// @param requested  whether teamd wants this member to collect
/// @param request_id the collecting request epoch
///
void LacpSync::publish_member_db(const std::string & app_key, bool requested, const std::string & request_id)
{
    const std::string requested_str = requested ? LACP_VALUE_TRUE : LACP_VALUE_FALSE;

    auto it = m_published.find(app_key);
    if (it != m_published.end() && it->second == std::make_pair(requested_str, request_id))
    {  // prevent duplicate writes
        return;
    }

    std::vector<swss::FieldValueTuple> fvs = {
        { LACP_FIELD_COLLECTING_REQUESTED,  requested_str },
        { LACP_FIELD_COLLECTING_REQUEST_ID, request_id },
    };
    m_app_table.set(app_key, fvs);
    m_published[app_key] = { requested_str, request_id }; // add to local cache to prevent duplicate writes
    SWSS_LOG_INFO("Published collecting request. key='%s' requested='%s' id='%s'",
                  app_key.c_str(), requested_str.c_str(), request_id.c_str());
}

/// Publish every independent-mode member's collecting request from the teamd
/// dumps into APP_DB, then prune rows for members that went away.
///
/// @param dumps per-LAG raw teamd state dumps
///
void LacpSync::publish_all_members_collecting_requests_db(const TeamdCtlDumps & dumps)
{
    std::unordered_set<std::string> present_lags;
    std::unordered_set<std::string> seen_keys;

    for (const auto & dump: dumps)
    {
        const auto & lag_name = dump.first;

        json_t * root = json_loads(dump.second.c_str(), 0, nullptr);
        if (!root)
        {
            // A parse failure (e.g. a truncated dump) is treated like a dump
            // failure: the LAG is left out of present_lags so prune keeps its
            // rows, rather than withdrawing a healthy member's request.
            SWSS_LOG_WARN("Can't parse dump for LAG='%s'. Skipping", lag_name.c_str());
            continue;
        }
        present_lags.insert(lag_name);

        json_t * runner = json_object_get(root, "runner");
        if (!json_is_true(json_object_get(runner, "independent_mode")))
        {
            json_decref(root);
            continue;
        }

        json_t * ports = json_object_get(root, "ports");
        if (!json_is_object(ports))
        {
            json_decref(root);
            continue;
        }

        const char * member;
        json_t * member_obj;
        json_object_foreach(ports, member, member_obj)
        {
            const std::string app_key = lag_name + ":" + std::string(member);

            json_t * member_runner = json_object_get(member_obj, "runner");
            json_t * member_req = json_object_get(member_runner, TEAMD_ITEM_COLLECTING_REQUESTED);
            json_t * member_id  = json_object_get(member_runner, TEAMD_ITEM_COLLECTING_REQUEST_ID);

            if (!json_is_boolean(member_req) || !json_is_integer(member_id))
            {
                // Expected mid-transition; log once per member (not every poll)
                // until it recovers, mirroring ValuesStore's tolerance.
                if (m_incomplete_logged.insert(app_key).second)
                {
                    SWSS_LOG_INFO("LAG '%s' member '%s' in independent mode is missing "
                                  "collecting request fields; skipping", lag_name.c_str(), member);
                }
                continue;
            }
            m_incomplete_logged.erase(app_key);

            const bool requested = json_is_true(member_req);
            const std::string request_id = std::to_string(json_integer_value(member_id));

            seen_keys.insert(app_key);
            publish_member_db(app_key, requested, request_id);
        }

        json_decref(root);
    }

    prune_member_db(present_lags, seen_keys);
}

/// Relay one orchagent confirm from STATE_DB to teamd's collecting_confirm:
/// echo the epoch to confirm collecting, or write 0 to withdraw.
///
/// @param event a single STATE_DB LAG_MEMBER_LACP_TABLE update
///
void LacpSync::process_member_collecting_confirmation(const swss::KeyOpFieldsValuesTuple & event)
{
    const auto & key = kfvKey(event);
    const auto & op = kfvOp(event);

    // STATE_DB keys are "<lag>|<member>"; tolerate ':' too for safety.
    auto sep = key.find_first_of("|:");
    if (sep == std::string::npos)
    {
        SWSS_LOG_WARN("Malformed confirm key '%s'. Skipping", key.c_str());
        return;
    }
    const auto lag = key.substr(0, sep);
    const auto member = key.substr(sep + 1);

    // A row delete withdraws the latch outright.
    if (op == DEL_COMMAND)
    {
        m_teamdctl_mgr.set_member_state(lag, member, TEAMD_ITEM_COLLECTING_CONFIRM,
                                        TEAMD_COLLECTING_CONFIRM_WITHDRAW);
        return;
    }

    std::string confirmed;
    std::string request_id;
    for (const auto & fv: kfvFieldsValues(event))
    {
        if (fvField(fv) == LACP_FIELD_COLLECTING_CONFIRMED)  confirmed = fvValue(fv);
        else if (fvField(fv) == LACP_FIELD_COLLECTING_REQUEST_ID) request_id = fvValue(fv);
    }

    std::string value;
    if (confirmed == LACP_VALUE_TRUE)
    {
        if (request_id.empty())
        {
            SWSS_LOG_WARN("Confirm for '%s' missing request id. Skipping", key.c_str());
            return;
        }
        // Echo the confirmed epoch; teamd latches only if it matches the
        // current request id, otherwise the write is rejected (stale).
        value = request_id;
    }
    else if (confirmed == LACP_VALUE_FALSE)
    {
        value = TEAMD_COLLECTING_CONFIRM_WITHDRAW;
    }
    else // no confirmed value
    {
        SWSS_LOG_WARN("Confirm SET for '%s' has unrecognized confirmed='%s'. Skipping",  
                      key.c_str(), confirmed.c_str()); 
        return;
    }

    m_teamdctl_mgr.set_member_state(lag, member, TEAMD_ITEM_COLLECTING_CONFIRM, value);
}
