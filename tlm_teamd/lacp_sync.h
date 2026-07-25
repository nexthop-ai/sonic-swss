#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>

#include <dbconnector.h>
#include <producerstatetable.h>

#include "teamdctl_mgr.h"

// Bridges the independent-mode LACP handshake: publishes teamd's per-member
// collecting requests to APP_DB, and relays orchagent's STATE_DB confirms back
// to teamd's collecting_confirm.
class LacpSync
{
public:
    LacpSync(swss::DBConnector * appl_db, TeamdCtlMgr & teamdctl_mgr);
    void publish_all_members_collecting_requests_db(const TeamdCtlDumps & dumps);
    void process_member_collecting_confirmation(const swss::KeyOpFieldsValuesTuple & event);

private:
    void publish_member_db(const std::string & app_key, bool requested, const std::string & request_id);
    void prune_member_db(const std::unordered_set<std::string> & present_lags,
               const std::unordered_set<std::string> & seen_keys);

    swss::ProducerStateTable m_app_table;
    TeamdCtlMgr & m_teamdctl_mgr;
    // Local cache to store latest {requested, request_id} published per "<lag>:<member>" key
    std::unordered_map<std::string, std::pair<std::string, std::string>> m_published;
    // Members logged as missing their request fields, so the notice is emitted
    // once per member (mid-transition is expected) rather than every poll.
    std::unordered_set<std::string> m_incomplete_logged;
};
