#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

#include <teamdctl.h>

using TeamdCtlDump = std::pair<bool, std::string>;
using TeamdCtlDumpsEntry = std::pair<std::string, std::string>;
using TeamdCtlDumps = std::vector<TeamdCtlDumpsEntry>;

class TeamdCtlMgr
{
public:
    TeamdCtlMgr() = default;
    ~TeamdCtlMgr();
    bool add_lag(const std::string & lag_name);
    bool remove_lag(const std::string & lag_name);
    void process_add_queue();
    // Retry logic added to prevent incorrect error reporting in dump API's
    TeamdCtlDump get_dump(const std::string & lag_name, bool to_retry);
    TeamdCtlDumps get_dumps(bool to_retry);

    // Write a per-member runner state item back to teamd over teamdctl
    void set_member_state(const std::string & lag_name, const std::string & member,
                          const std::string & item, const std::string & value);
    std::unordered_set<std::string> get_lags() const;

private:
    bool has_key(const std::string & lag_name) const;
    bool try_add_lag(const std::string & lag_name);

    std::unordered_map<std::string, struct teamdctl*> m_handlers;
    std::unordered_map<std::string, int> m_lags_to_add;
    std::unordered_map<std::string, int> m_lags_err_retry;

    const int max_attempts_to_add = 10;
};
