#pragma once

#include <string>
#include <vector>

#include <jansson.h>

#include <dbconnector.h>

using StringPair = std::pair<std::string, std::string>;
using Records = std::unordered_map<std::string, std::string>;
using HashOfRecords = std::unordered_map<std::string, Records>;

class ValuesStore
{
public:
    ValuesStore(const swss::DBConnector * db) : m_db(db) {};
    void update(const std::vector<StringPair> & dumps);

private:
    enum class json_type
    {
        string,
        boolean,
        integer,
    };

    json_t * load_json(const std::string & data);
    std::vector<std::string> get_ports(json_t * root);
    std::pair<std::vector<std::string>, std::string> convert_path(const std::string & path);
    json_t * traverse(json_t * root, const std::vector<std::string> & path_array, const std::string & path);
    std::string unpack_string(json_t * root, const std::string & key, const std::string & path);
    std::string unpack_boolean(json_t * root, const std::string & key, const std::string & path);
    std::string unpack_integer(json_t * root, const std::string & key, const std::string & path);
    std::string get_value(json_t * root, const std::string & path, ValuesStore::json_type type);
<<<<<<< HEAD
    HashOfRecords from_json(const std::vector<StringPair> & dumps);
=======
    // Like get_value, but returns false instead of throwing when the path is absent.
    bool try_get_value(json_t * root, const std::string & path, ValuesStore::json_type type, std::string & value);
>>>>>>> cf9d2c9b (NOS-11514: tlm_teamd: relay independent-mode LACP collecting handshake (#786))
    std::vector<std::string> get_old_keys(const HashOfRecords & storage);
    void remove_keys_storage(const std::vector<std::string> & keys);
    void remove_keys_db(const std::vector<std::string> & keys);
    StringPair split_key(const std::string & key);
    std::vector<std::string> update_storage(const HashOfRecords & storage);
    void update_db(const HashOfRecords & storage, const std::vector<std::string> & keys_to_refresh);
    void extract_values(const std::string & lag_name, json_t * root, HashOfRecords & storage);

    HashOfRecords m_storage;  // our main storage
    const swss::DBConnector * m_db;

    const std::vector<std::pair<std::string, ValuesStore::json_type>> m_lag_paths = {
        { "setup.kernel_team_mode_name", ValuesStore::json_type::string  },
        { "setup.pid",                   ValuesStore::json_type::integer },
        { "runner.active",               ValuesStore::json_type::boolean },
        { "runner.fallback",             ValuesStore::json_type::boolean },
        { "runner.fast_rate",            ValuesStore::json_type::boolean },
        { "team_device.ifinfo.dev_addr", ValuesStore::json_type::string  },
        { "team_device.ifinfo.ifindex",  ValuesStore::json_type::integer },
    };
    const std::vector<std::pair<std::string, ValuesStore::json_type>> m_member_paths = {
        { "ifinfo.dev_addr",                   ValuesStore::json_type::string  },
        { "ifinfo.ifindex",                    ValuesStore::json_type::integer },
        { "link.up",                           ValuesStore::json_type::boolean },
        { "link_watches.list.link_watch_0.up", ValuesStore::json_type::boolean },
        { "runner.actor_lacpdu_info.port",     ValuesStore::json_type::integer },
        { "runner.actor_lacpdu_info.state",    ValuesStore::json_type::integer },
        { "runner.actor_lacpdu_info.system",   ValuesStore::json_type::string  },
        { "runner.partner_lacpdu_info.port",   ValuesStore::json_type::integer },
        { "runner.partner_lacpdu_info.state",  ValuesStore::json_type::integer },
        { "runner.partner_lacpdu_info.system", ValuesStore::json_type::string  },
        { "runner.aggregator.id",              ValuesStore::json_type::integer },
        { "runner.aggregator.selected",        ValuesStore::json_type::boolean },
        { "runner.selected",                   ValuesStore::json_type::boolean },
        { "runner.state",                      ValuesStore::json_type::string  },
    };
<<<<<<< HEAD
=======
    // Per-member request state present only when the LAG runs in independent
    // mode; read only when runner.independent_mode is reported.
    const std::vector<std::pair<std::string, ValuesStore::json_type>> m_member_independent_paths = {
        { "runner.collecting_requested",  ValuesStore::json_type::boolean },
        { "runner.collecting_request_id", ValuesStore::json_type::integer },
    };
    // Per-member LACP counters, exposed by teamd under ports.<member>.runner.stats.*
    // (libteam patch 0020). Published to COUNTERS_DB LACP_COUNTERS:<lag>:<member>
    // (COUNTERS_DB's key separator is ':'); the leaf name is used verbatim as the
    // COUNTERS_DB field. Absent on teamd that predates the patch -> extraction is
    // skipped, not fatal.
    const std::vector<std::pair<std::string, ValuesStore::json_type>> m_member_counter_paths = {
        { "runner.stats.lacp_tx",                ValuesStore::json_type::integer },
        { "runner.stats.lacp_rx",                ValuesStore::json_type::integer },
        { "runner.stats.lacp_malformed",         ValuesStore::json_type::integer },
        { "runner.stats.lacp_unknown_version",   ValuesStore::json_type::integer },
        { "runner.stats.lacp_key_mismatch",      ValuesStore::json_type::integer },
        { "runner.stats.lacp_system_id_mismatch",ValuesStore::json_type::integer },
        { "runner.stats.state_current",          ValuesStore::json_type::integer },
        { "runner.stats.state_expired",          ValuesStore::json_type::integer },
        { "runner.stats.state_defaulted",        ValuesStore::json_type::integer },
        { "runner.stats.timeouts",               ValuesStore::json_type::integer },
        { "runner.stats.retries",                ValuesStore::json_type::integer },
        { "runner.stats.state_age",              ValuesStore::json_type::integer },
    };
>>>>>>> cf9d2c9b (NOS-11514: tlm_teamd: relay independent-mode LACP collecting handshake (#786))
};
