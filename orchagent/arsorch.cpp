#include <assert.h>
#include <inttypes.h>
#include "arsorch.h"
#include "routeorch.h"
#include "portsorch.h"
#include "logger.h"
#include "swssnet.h"
#include <array>
#include <algorithm>
#include "sai_serialize.h"
#include "flow_counter_handler.h"
#include "flex_counter/flex_counter_manager.h"

extern sai_object_id_t gVirtualRouterId;
extern sai_object_id_t gSwitchId;

extern sai_ars_profile_api_t*       sai_ars_profile_api;
extern sai_ars_api_t*               sai_ars_api;
extern sai_port_api_t*              sai_port_api;
extern sai_lag_api_t*               sai_lag_api;
extern sai_next_hop_group_api_t*    sai_next_hop_group_api;
extern sai_route_api_t*             sai_route_api;
extern sai_switch_api_t*            sai_switch_api;
extern MacAddress                   gMacAddress;

extern RouteOrch *gRouteOrch;
extern PortsOrch *gPortsOrch;

// Utility function to convert a set of NextHopGroupKey to formatted strings with SAI IDs
// Returns pair of (nexthops_string, sai_ids_string) in matching order
static std::pair<std::string, std::string> nexthopGroupKeySetToString(
    const std::set<NextHopGroupKey>& nhg_set)
{
    std::string nexthops_str;
    std::string sai_ids_str;

    for (const auto& nhg : nhg_set)
    {
        if (!nexthops_str.empty())
        {
            nexthops_str += ",";
            sai_ids_str += ",";
        }

        nexthops_str += "{" + nhg.to_string() + "}";

        auto nhg_sai_id = gRouteOrch->getNextHopGroupId(nhg);
        sai_ids_str += sai_serialize_object_id(nhg_sai_id);
    }

    return {nexthops_str, sai_ids_str};
}

// Utility function to convert ArsAlgorithm enum to string
static std::string arsAlgorithmToString(ArsAlgorithm algo)
{
    switch (algo)
    {
        case ARS_ALGORITHM_EWMA:
            return "ewma";
        default:
            return "unknown";
    }
}

// Utility function to convert ArsNhgSelector enum to string
static std::string arsNhgSelectorToString(ArsNhgSelector selector)
{
    switch (selector)
    {
        case ARS_NHG_SELECTOR_GLOBAL:
            return "global";
        case ARS_NHG_SELECTOR_INTERFACE:
            return "interface";
        case ARS_NHG_SELECTOR_NEXTHOP:
            return "nexthop";
        default:
            return "unknown";
    }
}

// Derive a per-switch unique ARS random seed from the switch base MAC.
// FNV-1a hash of the 6 MAC bytes folded down to 16 bits via XOR. The SAI
// attribute is sai_uint32_t, but the BRCM SDK on XGS rejects seeds whose
// per-word value exceeds 0xFFFF with BCM_E_PARAM (the historical SDK default
// 0xd1be is also 16-bit). Folding via XOR preserves entropy from all 6 MAC
// bytes while fitting the SDK constraint.
//
// gMacAddress is populated in main() before any orch is constructed, so no
// SAI round-trip is needed. If the global MAC is somehow empty (init-order
// bug) we LOG_ERROR and return 1. The choice of 1 over 0 here is purely so
// the BCM register ends up at a value that's distinguishable from
// "orchagent didn't program the seed" — BRCM XGS SDK treats a 0 seed as
// "use default" and substitutes the historical 0xd1be. It does NOT prevent
// polarization: two boxes that both hit this fallback will collide on seed
// 1 just as surely as they would on 0xd1be. The whole point of this
// function is the MAC-derive path below; the fallback exists only so the
// orchagent can come up at all in a degenerate scenario, and the ERROR
// log is the loud signal that something earlier is wrong.
static uint32_t computeArsRandomSeedFromMac()
{
    SWSS_LOG_ENTER();

    if (!gMacAddress)
    {
        SWSS_LOG_ERROR("gMacAddress is empty when deriving ARS random seed — "
                       "init order bug; ARS will be polarized across any switches "
                       "that hit this fallback. Returning seed=1 only so the BCM "
                       "register is distinguishable from the historical default 0xd1be.");
        return 1;
    }

    const uint8_t *mac = gMacAddress.getMac();
    uint32_t hash = 2166136261u; // FNV-1a 32-bit offset basis
    for (int i = 0; i < 6; i++)
    {
        hash ^= mac[i];
        hash *= 16777619u;       // FNV-1a 32-bit prime
    }
    // Fold to 16 bits to satisfy BRCM XGS SDK range constraint.
    uint32_t seed = ((hash >> 16) ^ (hash & 0xFFFFu)) & 0xFFFFu;
    if (seed == 0)
    {
        seed = 1;
    }

    SWSS_LOG_NOTICE("Derived ARS random seed 0x%04x from switch MAC %s",
                    seed, gMacAddress.to_string().c_str());
    return seed;
}

static uint32_t getDefaultArsRandomSeed()
{
    static const uint32_t cached = computeArsRandomSeedFromMac();
    return cached;
}

// Utility function to get the default scaling factor based on port speed
static uint32_t getDefaultScalingFactor(const Port &port)
{
    const uint32_t gbps = 1000;
    switch (port.m_speed)
    {
        case 10 * gbps:
            return 1;
        case 25 * gbps:
            return 2;
        case 40 * gbps:
            return 4;
        case 50 * gbps:
            return 5;
        case 100 * gbps:
            return 10;
        case 200 * gbps:
            return 20;
        case 400 * gbps:
        case 800 * gbps:
            return 40;
        default:
            return 0;
    }
}

static ArsProfileEntry getDefaultArsProfile()
{
    SWSS_LOG_ENTER();

    ArsProfileEntry profile;
    sai_status_t status;
    sai_attribute_t attr;

    profile.algorithm = ARS_ALGORITHM_EWMA;
    profile.nhg_selector = ARS_NHG_SELECTOR_INTERFACE;
    profile.sampling_interval = 16;
    profile.max_flows = 0;
    profile.ipv4_enabled = false;
    profile.ipv6_enabled = false;
    profile.random_seed = getDefaultArsRandomSeed();

    profile.path_metrics.past_load.min_value = 1000;
    profile.path_metrics.past_load.max_value = 9000;
    profile.path_metrics.past_load.weight = 70;

    // Get total buffer size from switch
    attr.id = SAI_SWITCH_ATTR_TOTAL_BUFFER_SIZE;
    status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);

    uint64_t buffer_per_port = 0;

    if (status == SAI_STATUS_SUCCESS)
    {
        uint64_t total_buffer = attr.value.u32 * 1024;
        uint32_t num_ports = static_cast<uint32_t>(gPortsOrch->getAllPorts().size());

        if (num_ports > 0)
        {
            buffer_per_port = total_buffer / num_ports;
        }

        SWSS_LOG_NOTICE("Total buffer size: %" PRIu64 " bytes, num_ports: %u, buffer_per_port: %" PRIu64 " bytes",
                        total_buffer, num_ports, buffer_per_port);
    }
    else
    {
        SWSS_LOG_WARN("Failed to get total buffer size from switch, rv:%d. ", status);
    }

    // Calculate future_load and current_load values
    // min_value = 10% of buffer_per_port
    // max_value = 90% of buffer_per_port
    uint64_t min_val = (buffer_per_port * 10) / 100;
    uint64_t max_val = (buffer_per_port * 90) / 100;

    // Ensure values fit in uint32_t
    profile.path_metrics.future_load.min_value = (min_val > UINT32_MAX) ? UINT32_MAX : (uint32_t)min_val;
    profile.path_metrics.future_load.max_value = (max_val > UINT32_MAX) ? UINT32_MAX : (uint32_t)max_val;
    profile.path_metrics.future_load.weight = 30;

    // Use same values for current_load
    profile.path_metrics.current_load.min_value = profile.path_metrics.future_load.min_value;
    profile.path_metrics.current_load.max_value = profile.path_metrics.future_load.max_value;

    SWSS_LOG_NOTICE("Default ARS profile: sampling_interval=%u, past_load=[%u,%u, %u], "
                    "future_load=[%u,%u, %u], current_load=[%u,%u]",
                    profile.sampling_interval,
                    profile.path_metrics.past_load.min_value,
                    profile.path_metrics.past_load.max_value,
                    profile.path_metrics.past_load.weight,
                    profile.path_metrics.future_load.min_value,
                    profile.path_metrics.future_load.max_value,
                    profile.path_metrics.future_load.weight,
                    profile.path_metrics.current_load.min_value,
                    profile.path_metrics.current_load.max_value);

    return profile;
}


static ars_sai_attr_lookup_t ars_profile_attrs = {
    {SAI_ARS_PROFILE_ATTR_ALGO, {"SAI_ARS_PROFILE_ATTR_ALGO"}},
    {SAI_ARS_PROFILE_ATTR_SAMPLING_INTERVAL, {"SAI_ARS_PROFILE_ATTR_SAMPLING_INTERVAL"}},
    {SAI_ARS_PROFILE_ATTR_ARS_RANDOM_SEED, {"SAI_ARS_PROFILE_ATTR_ARS_RANDOM_SEED"}},
    {SAI_ARS_PROFILE_ATTR_ECMP_ARS_MAX_GROUPS, {"SAI_ARS_PROFILE_ATTR_ECMP_ARS_MAX_GROUPS"}},
    {SAI_ARS_PROFILE_ATTR_ECMP_ARS_MAX_MEMBERS_PER_GROUP, {"SAI_ARS_PROFILE_ATTR_ECMP_ARS_MAX_MEMBERS_PER_GROUP"}},
    {SAI_ARS_PROFILE_ATTR_LAG_ARS_MAX_GROUPS, {"SAI_ARS_PROFILE_ATTR_LAG_ARS_MAX_GROUPS"}},
    {SAI_ARS_PROFILE_ATTR_LAG_ARS_MAX_MEMBERS_PER_GROUP, {"SAI_ARS_PROFILE_ATTR_LAG_ARS_MAX_MEMBERS_PER_GROUP"}},
    {SAI_ARS_PROFILE_ATTR_PORT_LOAD_PAST, {"SAI_ARS_PROFILE_ATTR_PORT_LOAD_PAST"}},
    {SAI_ARS_PROFILE_ATTR_PORT_LOAD_PAST_WEIGHT, {"SAI_ARS_PROFILE_ATTR_PORT_LOAD_PAST_WEIGHT"}},
    {SAI_ARS_PROFILE_ATTR_PORT_LOAD_FUTURE, {"SAI_ARS_PROFILE_ATTR_PORT_LOAD_FUTURE"}},
    {SAI_ARS_PROFILE_ATTR_PORT_LOAD_FUTURE_WEIGHT, {"SAI_ARS_PROFILE_ATTR_PORT_LOAD_FUTURE_WEIGHT"}},
    {SAI_ARS_PROFILE_ATTR_PORT_LOAD_CURRENT, {"SAI_ARS_PROFILE_ATTR_PORT_LOAD_CURRENT"}},
    {SAI_ARS_PROFILE_ATTR_PORT_LOAD_EXPONENT, {"SAI_ARS_PROFILE_ATTR_PORT_LOAD_EXPONENT"}},
    {SAI_ARS_PROFILE_ATTR_QUANT_BANDS, {"SAI_ARS_PROFILE_ATTR_QUANT_BANDS"}},
    {SAI_ARS_PROFILE_ATTR_QUANT_BAND_0_MIN_THRESHOLD, {"SAI_ARS_PROFILE_ATTR_QUANT_BAND_0_MIN_THRESHOLD"}},
    {SAI_ARS_PROFILE_ATTR_QUANT_BAND_0_MAX_THRESHOLD, {"SAI_ARS_PROFILE_ATTR_QUANT_BAND_0_MAX_THRESHOLD"}},
    {SAI_ARS_PROFILE_ATTR_QUANT_BAND_1_MIN_THRESHOLD, {"SAI_ARS_PROFILE_ATTR_QUANT_BAND_1_MIN_THRESHOLD"}},
    {SAI_ARS_PROFILE_ATTR_QUANT_BAND_1_MAX_THRESHOLD, {"SAI_ARS_PROFILE_ATTR_QUANT_BAND_1_MAX_THRESHOLD"}},
    {SAI_ARS_PROFILE_ATTR_QUANT_BAND_2_MIN_THRESHOLD, {"SAI_ARS_PROFILE_ATTR_QUANT_BAND_2_MIN_THRESHOLD"}},
    {SAI_ARS_PROFILE_ATTR_QUANT_BAND_2_MAX_THRESHOLD, {"SAI_ARS_PROFILE_ATTR_QUANT_BAND_2_MAX_THRESHOLD"}},
    {SAI_ARS_PROFILE_ATTR_QUANT_BAND_3_MIN_THRESHOLD, {"SAI_ARS_PROFILE_ATTR_QUANT_BAND_3_MIN_THRESHOLD"}},
    {SAI_ARS_PROFILE_ATTR_QUANT_BAND_3_MAX_THRESHOLD, {"SAI_ARS_PROFILE_ATTR_QUANT_BAND_3_MAX_THRESHOLD"}},
    {SAI_ARS_PROFILE_ATTR_QUANT_BAND_4_MIN_THRESHOLD, {"SAI_ARS_PROFILE_ATTR_QUANT_BAND_4_MIN_THRESHOLD"}},
    {SAI_ARS_PROFILE_ATTR_QUANT_BAND_4_MAX_THRESHOLD, {"SAI_ARS_PROFILE_ATTR_QUANT_BAND_4_MAX_THRESHOLD"}},
    {SAI_ARS_PROFILE_ATTR_QUANT_BAND_5_MIN_THRESHOLD, {"SAI_ARS_PROFILE_ATTR_QUANT_BAND_5_MIN_THRESHOLD"}},
    {SAI_ARS_PROFILE_ATTR_QUANT_BAND_5_MAX_THRESHOLD, {"SAI_ARS_PROFILE_ATTR_QUANT_BAND_5_MAX_THRESHOLD"}},
    {SAI_ARS_PROFILE_ATTR_QUANT_BAND_6_MIN_THRESHOLD, {"SAI_ARS_PROFILE_ATTR_QUANT_BAND_6_MIN_THRESHOLD"}},
    {SAI_ARS_PROFILE_ATTR_QUANT_BAND_6_MAX_THRESHOLD, {"SAI_ARS_PROFILE_ATTR_QUANT_BAND_6_MAX_THRESHOLD"}},
    {SAI_ARS_PROFILE_ATTR_QUANT_BAND_7_MIN_THRESHOLD, {"SAI_ARS_PROFILE_ATTR_QUANT_BAND_7_MIN_THRESHOLD"}},
    {SAI_ARS_PROFILE_ATTR_QUANT_BAND_7_MAX_THRESHOLD, {"SAI_ARS_PROFILE_ATTR_QUANT_BAND_7_MAX_THRESHOLD"}},
    {SAI_ARS_PROFILE_ATTR_ENABLE_IPV4, {"SAI_ARS_PROFILE_ATTR_ENABLE_IPV4"}},
    {SAI_ARS_PROFILE_ATTR_ENABLE_IPV6, {"SAI_ARS_PROFILE_ATTR_ENABLE_IPV6"}},
    {SAI_ARS_PROFILE_ATTR_LOAD_PAST_MIN_VAL, {"SAI_ARS_PROFILE_ATTR_LOAD_PAST_MIN_VAL"}},
    {SAI_ARS_PROFILE_ATTR_LOAD_PAST_MAX_VAL, {"SAI_ARS_PROFILE_ATTR_LOAD_PAST_MAX_VAL"}},
    {SAI_ARS_PROFILE_ATTR_QUANT_BAND_MIN_THRESHOLD_LIST_LOAD_PAST, {"SAI_ARS_PROFILE_ATTR_QUANT_BAND_MIN_THRESHOLD_LIST_LOAD_PAST"}},
    {SAI_ARS_PROFILE_ATTR_QUANT_BAND_MAX_THRESHOLD_LIST_LOAD_PAST, {"SAI_ARS_PROFILE_ATTR_QUANT_BAND_MAX_THRESHOLD_LIST_LOAD_PAST"}},
    {SAI_ARS_PROFILE_ATTR_LOAD_FUTURE_MIN_VAL, {"SAI_ARS_PROFILE_ATTR_LOAD_FUTURE_MIN_VAL"}},
    {SAI_ARS_PROFILE_ATTR_LOAD_FUTURE_MAX_VAL, {"SAI_ARS_PROFILE_ATTR_LOAD_FUTURE_MAX_VAL"}},
    {SAI_ARS_PROFILE_ATTR_QUANT_BAND_MIN_THRESHOLD_LIST_LOAD_FUTURE, {"SAI_ARS_PROFILE_ATTR_QUANT_BAND_MIN_THRESHOLD_LIST_LOAD_FUTURE"}},
    {SAI_ARS_PROFILE_ATTR_QUANT_BAND_MAX_THRESHOLD_LIST_LOAD_FUTURE, {"SAI_ARS_PROFILE_ATTR_QUANT_BAND_MAX_THRESHOLD_LIST_LOAD_FUTURE"}},
    {SAI_ARS_PROFILE_ATTR_LOAD_CURRENT_MIN_VAL, {"SAI_ARS_PROFILE_ATTR_LOAD_CURRENT_MIN_VAL"}},
    {SAI_ARS_PROFILE_ATTR_LOAD_CURRENT_MAX_VAL, {"SAI_ARS_PROFILE_ATTR_LOAD_CURRENT_MAX_VAL"}},
    {SAI_ARS_PROFILE_ATTR_QUANT_BAND_MIN_THRESHOLD_LIST_LOAD_CURRENT, {"SAI_ARS_PROFILE_ATTR_QUANT_BAND_MIN_THRESHOLD_LIST_LOAD_CURRENT"}},
    {SAI_ARS_PROFILE_ATTR_QUANT_BAND_MAX_THRESHOLD_LIST_LOAD_CURRENT, {"SAI_ARS_PROFILE_ATTR_QUANT_BAND_MAX_THRESHOLD_LIST_LOAD_CURRENT"}},
    {SAI_ARS_PROFILE_ATTR_MAX_FLOWS, {"SAI_ARS_PROFILE_ATTR_MAX_FLOWS"}}
};
static ars_sai_attr_lookup_t ars_sai_attrs = {
    {SAI_ARS_ATTR_MODE, {"SAI_ARS_ATTR_MODE"}},
    {SAI_ARS_ATTR_IDLE_TIME, {"SAI_ARS_ATTR_IDLE_TIME"}},
    {SAI_ARS_ATTR_MAX_FLOWS, {"SAI_ARS_ATTR_MAX_FLOWS"}},
    {SAI_ARS_ATTR_MON_ENABLE, {"SAI_ARS_ATTR_MON_ENABLE"}},
    {SAI_ARS_ATTR_SAMPLEPACKET_ENABLE, {"SAI_ARS_ATTR_SAMPLEPACKET_ENABLE"}},
    {SAI_ARS_ATTR_MAX_ALT_MEMEBERS_PER_GROUP, {"SAI_ARS_ATTR_MAX_ALT_MEMEBERS_PER_GROUP"}},
    {SAI_ARS_ATTR_MAX_PRIMARY_MEMEBERS_PER_GROUP, {"SAI_ARS_ATTR_MAX_PRIMARY_MEMEBERS_PER_GROUP"}},
    {SAI_ARS_ATTR_PRIMARY_PATH_QUALITY_THRESHOLD, {"SAI_ARS_ATTR_PRIMARY_PATH_QUALITY_THRESHOLD"}},
    {SAI_ARS_ATTR_ALTERNATE_PATH_COST, {"SAI_ARS_ATTR_ALTERNATE_PATH_COST"}},
    {SAI_ARS_ATTR_ALTERNATE_PATH_BIAS, {"SAI_ARS_ATTR_ALTERNATE_PATH_BIAS"}}
};
static ars_sai_attr_lookup_t ars_port_attrs = {
    {SAI_PORT_ATTR_ARS_ENABLE, {"SAI_PORT_ATTR_ARS_ENABLE"}},
    {SAI_PORT_ATTR_ARS_PORT_LOAD_SCALING_FACTOR, {"SAI_PORT_ATTR_ARS_PORT_LOAD_SCALING_FACTOR"}},
    {SAI_PORT_ATTR_ARS_PORT_LOAD_PAST_WEIGHT, {"SAI_PORT_ATTR_ARS_PORT_LOAD_PAST_WEIGHT"}},
    {SAI_PORT_ATTR_ARS_PORT_LOAD_FUTURE_WEIGHT, {"SAI_PORT_ATTR_ARS_PORT_LOAD_FUTURE_WEIGHT"}},
    {SAI_PORT_ATTR_ARS_ALTERNATE_PATH, {"SAI_PORT_ATTR_ARS_ALTERNATE_PATH"}}
};

static ars_sai_attr_lookup_t ars_nhg_attrs = {
    {SAI_NEXT_HOP_GROUP_ATTR_ARS_OBJECT_ID, {"SAI_NEXT_HOP_GROUP_ATTR_ARS_OBJECT_ID"}},
    {SAI_NEXT_HOP_GROUP_ATTR_ARS_PACKET_DROPS, {"SAI_NEXT_HOP_GROUP_ATTR_ARS_PACKET_DROPS"}},
    {SAI_NEXT_HOP_GROUP_ATTR_ARS_NEXT_HOP_REASSIGNMENTS, {"SAI_NEXT_HOP_GROUP_ATTR_ARS_NEXT_HOP_REASSIGNMENTS"}},
    {SAI_NEXT_HOP_GROUP_ATTR_ARS_PORT_REASSIGNMENTS, {"SAI_NEXT_HOP_GROUP_ATTR_ARS_PORT_REASSIGNMENTS"}}
};

static ars_sai_attr_lookup_t ars_switch_attrs = {
    {SAI_SWITCH_ATTR_ARS_PROFILE, {"SAI_SWITCH_ATTR_ARS_PROFILE"}}
};

static ars_sai_attr_lookup_t ars_lag_attrs = {
    {SAI_LAG_ATTR_ARS_OBJECT_ID, {"SAI_LAG_ATTR_ARS_OBJECT_ID"}},
    {SAI_LAG_ATTR_ARS_PACKET_DROPS, {"SAI_LAG_ATTR_ARS_PACKET_DROPS"}},
    {SAI_LAG_ATTR_ARS_PORT_REASSIGNMENTS, {"SAI_LAG_ATTR_ARS_PORT_REASSIGNMENTS"}}
};

static ars_sai_feature_data_t ars_feature_switch_data =
    {"SAI_OBJECT_TYPE_SWITCH",ars_switch_attrs};

static ars_sai_feature_data_t ars_feature_profile_data =
    {"SAI_OBJECT_TYPE_ARS_PROFILE",ars_profile_attrs};

static ars_sai_feature_data_t ars_feature_ars_data =
    {"SAI_OBJECT_TYPE_ARS",ars_sai_attrs};

static ars_sai_feature_data_t ars_feature_port_data =
    {"SAI_OBJECT_TYPE_PORT",ars_port_attrs};

static ars_sai_feature_data_t ars_feature_nhg_data =
    {"SAI_OBJECT_TYPE_NEXT_HOP_GROUP",ars_nhg_attrs};

static ars_sai_feature_data_t ars_feature_lag_data =
    {"SAI_OBJECT_TYPE_LAG",ars_lag_attrs};

static ars_sai_feature_lookup_t ars_features =
{
    {SAI_OBJECT_TYPE_SWITCH, ars_feature_switch_data},
    {SAI_OBJECT_TYPE_ARS_PROFILE, ars_feature_profile_data},
    {SAI_OBJECT_TYPE_ARS, ars_feature_ars_data},
    {SAI_OBJECT_TYPE_PORT, ars_feature_port_data},
    {SAI_OBJECT_TYPE_NEXT_HOP_GROUP, ars_feature_nhg_data},
    {SAI_OBJECT_TYPE_LAG, ars_feature_lag_data}
};

#define ARS_FIELD_NAME_MAX_FLOWS              "max_flows"
#define ARS_FIELD_NAME_ALGORITHM              "algorithm"
#define ARS_FIELD_NAME_NHG_SELECTOR           "ars_nhg_path_selector_mode"
#define ARS_FIELD_NAME_DEFAULT_ARS_OBJECT     "default_ars_object"
#define ARS_FIELD_NAME_SAMPLING_INTERVAL      "sampling_interval"
#define ARS_FIELD_NAME_PAST_LOAD_MIN_VALUE    "past_load_min_value"
#define ARS_FIELD_NAME_PAST_LOAD_MAX_VALUE    "past_load_max_value"
#define ARS_FIELD_NAME_PAST_LOAD_WEIGHT       "past_load_weight"
#define ARS_FIELD_NAME_FUTURE_LOAD_MIN_VALUE  "future_load_min_value"
#define ARS_FIELD_NAME_FUTURE_LOAD_MAX_VALUE  "future_load_max_value"
#define ARS_FIELD_NAME_FUTURE_LOAD_WEIGHT     "future_load_weight"
#define ARS_FIELD_NAME_CURRENT_LOAD_MIN_VALUE "current_load_min_value"
#define ARS_FIELD_NAME_CURRENT_LOAD_MAX_VALUE "current_load_max_value"
#define ARS_FIELD_NAME_MIN_VALUE              "min_value"
#define ARS_FIELD_NAME_MAX_VALUE              "max_value"
#define ARS_FIELD_NAME_WEIGHT                 "weight"
#define ARS_FIELD_NAME_FUTURE_LOAD            "future_load"
#define ARS_FIELD_NAME_CURRENT_LOAD           "current_load"
#define ARS_FIELD_NAME_INDEX                  "index"
#define ARS_FIELD_NAME_IPV4_ENABLE            "ipv4_enable"
#define ARS_FIELD_NAME_IPV6_ENABLE            "ipv6_enable"
#define ARS_FIELD_NAME_RANDOM_SEED            "random_seed"

#define ARS_FIELD_NAME_PROFILE_NAME "profile_name"
#define ARS_FIELD_NAME_ARS_NAME "ars_name"
#define ARS_FIELD_NAME_ASSIGN_MODE "assign_mode"
#define ARS_FIELD_NAME_PER_FLOWLET "per_flowlet_quality"
#define ARS_FIELD_NAME_PER_PACKET "per_packet_quality"
#define ARS_FIELD_NAME_IDLE_TIME "flowlet_idle_time"
#define ARS_FIELD_NAME_QUALITY_THRESHOLD "quality_threshold"
#define ARS_FIELD_NAME_SCALING_FACTOR "scaling_factor"
#define ARS_FIELD_NAME_ARS_OBJECT_NAME "ars_obj_name"

ArsOrch::ArsOrch(DBConnector *config_db, DBConnector *appDb, DBConnector *stateDb, vector<string> &tableNames, VRFOrch *vrfOrch) :
        Orch(config_db, tableNames),
        m_vrfOrch(vrfOrch),
        m_arsProfileStateTable(std::unique_ptr<Table>(new Table(stateDb, STATE_ARS_PROFILE_TABLE_NAME))),
        m_arsIfStateTable(std::unique_ptr<Table>(new Table(stateDb, STATE_ARS_INTERFACE_TABLE_NAME))),
        m_arsNhgStateTable(std::unique_ptr<Table>(new Table(stateDb, STATE_ARS_NEXTHOP_GROUP_TABLE_NAME))),
        m_arsCapabilityStateTable(std::unique_ptr<Table>(new Table(stateDb, STATE_ARS_CAPABILITY_TABLE_NAME))),
        m_arsObjectStateTable(std::unique_ptr<Table>(new Table(stateDb, STATE_ARS_OBJECT_TABLE_NAME))),
        ars_nhg_stat_manager(ARS_NHG_STAT_COUNTER_FLEX_COUNTER_GROUP, StatsMode::READ, 5000, true)
{
    SWSS_LOG_ENTER();

    initCapabilities();

    if (m_isArsSupported)
    {
        gPortsOrch->attach(this);
    }
}


bool ArsOrch::isSetImplemented(sai_object_type_t object_type, sai_attr_id_t attr_id)
{
    auto feature = ars_features.find((uint32_t)object_type);
    if (feature == ars_features.end())
    {
        return false;
    }

    if (attr_id == SAI_ARS_PROFILE_ATTR_ENABLE_IPV4 || attr_id == SAI_ARS_PROFILE_ATTR_ENABLE_IPV6)
    {
        //SAI does not support set for these attributes, but HW does.
        return true;
    }

    auto attr = feature->second.attrs.find(attr_id);
    if (attr == feature->second.attrs.end())
    {
        return false;
    }
    return attr->second.set_implemented;
}

bool ArsOrch::isCreateImplemented(sai_object_type_t object_type, sai_attr_id_t attr_id)
{
    auto feature = ars_features.find((uint32_t)object_type);
    if (feature == ars_features.end())
    {
        return false;
    }

    if (attr_id == SAI_ARS_PROFILE_ATTR_ENABLE_IPV4 || attr_id == SAI_ARS_PROFILE_ATTR_ENABLE_IPV6)
    {
        //SAI does not support create for these attributes, but HW does.
        return true;
    }

    auto attr = feature->second.attrs.find(attr_id);
    if (attr == feature->second.attrs.end())
    {
        return false;
    }
    return attr->second.create_implemented;
}

bool ArsOrch::isGetImplemented(sai_object_type_t object_type, sai_attr_id_t attr_id)
{
    auto feature = ars_features.find((uint32_t)object_type);
    if (feature == ars_features.end())
    {
        return false;
    }

    if (attr_id == SAI_ARS_PROFILE_ATTR_ENABLE_IPV4 || attr_id == SAI_ARS_PROFILE_ATTR_ENABLE_IPV6)
    {
        //SAI does not support get for these attributes, but HW does.
        return true;
    }

    auto attr = feature->second.attrs.find(attr_id);
    if (attr == feature->second.attrs.end())
    {
        return false;
    }
    return attr->second.get_implemented;
}

void ArsOrch::initCapabilities()
{
    SWSS_LOG_ENTER();

    sai_attr_capability_t capability;
    string platform = getenv("platform") ? getenv("platform") : "";

    auto status = sai_query_attribute_capability(gSwitchId, SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_ARS_PROFILE, &capability);
    if (status == SAI_STATUS_SUCCESS && !capability.set_implemented)
    {
        SWSS_LOG_NOTICE("ARS is not supported on this platform");
        return;
    }
    for (auto it = ars_features.begin(); it != ars_features.end(); it++)
    {
        for (auto it2 = it->second.attrs.begin(); it2 != it->second.attrs.end(); it2++)
        {
            if (sai_query_attribute_capability(gSwitchId, (sai_object_type_t)it->first,
                                                    (sai_attr_id_t)it2->first,
                                                    &capability) == SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_NOTICE("Feature %s Attr %s is supported. Create %s Set %s Get %s", it->second.name.c_str(), it2->second.attr_name.c_str(), capability.create_implemented ? "Y" : "N", capability.set_implemented ? "Y" : "N", capability.get_implemented ? "Y" : "N");
            }
            else
            {
                SWSS_LOG_NOTICE("Feature %s Attr %s is NOT supported", it->second.name.c_str(), it2->second.attr_name.c_str());
            }

            it2->second.create_implemented = capability.create_implemented;
            it2->second.set_implemented = capability.set_implemented;
            it2->second.get_implemented = capability.get_implemented;

            vector<FieldValueTuple> fieldValues;
            fieldValues.emplace_back("create", capability.create_implemented ? "true" : "false");
            fieldValues.emplace_back("set", capability.set_implemented ? "true" : "false");
            fieldValues.emplace_back("get", capability.get_implemented ? "true" : "false");
            m_arsCapabilityStateTable->set(it->second.name + "|" + it2->second.attr_name, fieldValues);
        }
    }

    m_isArsSupported = true;
}

void ArsOrch::update(SubjectType type, void *cntx)
{
    SWSS_LOG_ENTER();
    assert(cntx);

    if (m_arsProfiles.empty())
    {
        SWSS_LOG_INFO("ARS not enabled - no action on interface or nexthop state change");
        return;
    }

    switch(type) {
        case SUBJECT_TYPE_PORT_OPER_STATE_CHANGE:
        {
            /* configure port scaling factor when port speed becomes available */
            PortOperStateUpdate *update = reinterpret_cast<PortOperStateUpdate *>(cntx);
            SWSS_LOG_NOTICE("ARS port notification - port %s state %s", update->port.m_alias.c_str(), update->operStatus == SAI_PORT_OPER_STATUS_UP ? "enable" : "disable");
            auto ars_if = m_arsEnabledInterfaces.find(update->port.m_alias);
            bool is_found = (ars_if != m_arsEnabledInterfaces.end());
            if (is_found)
            {
                SWSS_LOG_INFO("Interface %s %senabled for ARS - %s ARS",
                        update->port.m_alias.c_str(),
                        is_found ? "" : "not ",
                        update->operStatus == SAI_PORT_OPER_STATUS_UP ? "enable" : "disable");
                if (update->operStatus == SAI_PORT_OPER_STATUS_UP)
                {
                    updateArsEnabledInterface(update->port, ars_if->second.scaling_factor, true);
                }
            }
            break;
        }

        case SUBJECT_TYPE_NEXTHOP_GROUP_CHANGE:
        {
            NextHopGroupUpdate *update = reinterpret_cast<NextHopGroupUpdate *>(cntx);
            SWSS_LOG_NOTICE("ARS nexthop group notification - vrf 0x%" PRIx64 " prefix %s nexthops %s update %s",
                update->vrf_id,
                update->prefix.to_string().c_str(),
                update->nexthopGroup.to_string().c_str(),
                update->update ? "true" : "false");

            // Pick ars_nhg_selector_mode from the first ars_profile
            auto ars_profile = m_arsProfiles.begin();
            if (ars_profile == m_arsProfiles.end())
            {
                break;
            }
            auto ars_object = m_arsObjects.end();
            auto ars_nhg_selector_mode = ars_profile->second.nhg_selector;
            switch (ars_nhg_selector_mode)
            {
                case ARS_NHG_SELECTOR_GLOBAL:
                    // Get ars_object from default_ars_object
                    ars_object = m_arsObjects.find(ars_profile->second.default_ars_object_name);
                    if (ars_object == m_arsObjects.end())
                    {
                        SWSS_LOG_WARN("Cound not find default ars object %s to attach in global mode",
                            ars_profile->second.default_ars_object_name.c_str());
                    }
                    break;
                case ARS_NHG_SELECTOR_INTERFACE:
                    // Go through all the interfaces in this NHG.
                    // All interfaces must be ARS-enabled and use the same ars_object
                    for (auto &nh : update->nexthopGroup.getNextHops())
                    {
                        auto ars_if = m_arsEnabledInterfaces.find(nh.alias);
                        if (ars_if == m_arsEnabledInterfaces.end())
                        {
                            SWSS_LOG_WARN("Interface %s is not ARS-enabled", nh.alias.c_str());
                            ars_object = m_arsObjects.end();
                            break;
                        }
                        if (ars_object == m_arsObjects.end())
                        {
                            ars_object = m_arsObjects.find(ars_if->second.ars_object_name);
                        }
                        else if (ars_object->second.name != ars_if->second.ars_object_name)
                        {
                            SWSS_LOG_WARN("Interface %s uses different ars_object %s than previous interface",
                                nh.alias.c_str(), ars_if->second.ars_object_name.c_str());
                                ars_object = m_arsObjects.end();
                            break;
                        }
                    }
                    break;
                case ARS_NHG_SELECTOR_NEXTHOP:
                    // Not supported yet
                    SWSS_LOG_WARN("NHG selector mode %d not supported yet", ars_nhg_selector_mode);
                    break;
                default:
                    SWSS_LOG_WARN("Unknown NHG selector mode %d", ars_nhg_selector_mode);
                    break;
            }

            if (ars_object == m_arsObjects.end())
            {
                break;
            }

            /* if NHG is not yet configured */
            vector<FieldValueTuple> fvVector;
            auto nhg_sai_id = gRouteOrch->getNextHopGroupId(update->nexthopGroup);
            if (nhg_sai_id == SAI_NULL_OBJECT_ID)
            {
                break;
            }
            auto arsId = update->update ? ars_object->second.ars_object_id : SAI_NULL_OBJECT_ID;
            if (!isSetImplemented(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, SAI_NEXT_HOP_GROUP_ATTR_ARS_OBJECT_ID))
            {
                SWSS_LOG_NOTICE("Remove existing NHG and create ARS-enabled");
                if (!gRouteOrch->reconfigureNexthopGroupWithArsState(update->nexthopGroup, arsId))
                {
                    SWSS_LOG_ERROR("Failed to reconfigure nexthop group %s with ARS state", update->nexthopGroup.to_string().c_str());
                    break;
                }
            }
            /* just update the NHG attr */
            else
            {
                if (!gRouteOrch->updateNexthopGroupArsState(nhg_sai_id, arsId))
                {
                    SWSS_LOG_ERROR("Failed to update nexthop group ARS state for group ID 0x%" PRIx64, nhg_sai_id);
                    break;
                }
            }
            if (update->update)
            {
                // Insert the nhg to the ars_object, if not already there
                if (ars_object->second.nexthops.find(update->nexthopGroup) == ars_object->second.nexthops.end())
                {
                    ars_object->second.nexthops.insert(update->nexthopGroup);
                }
            }
            else
            {
                /* Disable ARS over NHG */
                ars_object->second.nexthops.erase(update->nexthopGroup);
            }
            // Update the nhg set in the state DB
            auto nhg_strings = nexthopGroupKeySetToString(ars_object->second.nexthops);
            fvVector.emplace_back("nexthops", nhg_strings.first);
            fvVector.emplace_back("nhg_ids", nhg_strings.second);
            m_arsNhgStateTable->set(ars_object->second.name, fvVector);
            break;
        }
        default:
            break;
    }
}

bool ArsOrch::bake()
{
    SWSS_LOG_ENTER();

    if (!m_isArsSupported)
    {
        SWSS_LOG_NOTICE("ARS not supported - no action");
        return true;
    }

    SWSS_LOG_NOTICE("Warm reboot: placeholder");

    return Orch::bake();
}

bool ArsOrch::createArsProfile(ArsProfileEntry &profile, vector<sai_attribute_t> &ars_attrs)
{
    SWSS_LOG_ENTER();

    sai_status_t    status = SAI_STATUS_NOT_SUPPORTED;
    vector<sai_attribute_t> supported_ars_attrs;
    sai_attribute_t attr;

    /* go over set of attr and set only supported attributes  */
    for (auto a : ars_attrs)
    {
        if (isCreateImplemented(SAI_OBJECT_TYPE_ARS_PROFILE, a.id))
        {
            supported_ars_attrs.push_back(a);
            SWSS_LOG_NOTICE("ARS profile %s. Setting Attr %s value %u",
                profile.profile_name.c_str(), ars_profile_attrs.find(a.id)->second.attr_name.c_str(), a.value.u32);
        }
        else
        {
            if (a.id == SAI_ARS_PROFILE_ATTR_ENABLE_IPV4 || a.id == SAI_ARS_PROFILE_ATTR_ENABLE_IPV6)
            {
                SWSS_LOG_WARN("Setting Attr %s is not supported. Failed to set ARS profile %s value %s",
                               ars_profile_attrs.find(a.id)->second.attr_name.c_str(), profile.profile_name.c_str(), a.value.booldata ? "true" : "false");
            }
            else
            {
                SWSS_LOG_WARN("Setting Attr %s is not supported. Failed to set ARS profile %s value %u",
                               ars_profile_attrs.find(a.id)->second.attr_name.c_str(), profile.profile_name.c_str(), a.value.u32);
            }
            continue;
        }
    }

    if (supported_ars_attrs.empty())
    {
        SWSS_LOG_WARN("No supported attributes found for ARS profile %s", profile.profile_name.c_str());
        return false;
    }

    status = sai_ars_profile_api->create_ars_profile(&profile.m_sai_ars_id,
                                                     gSwitchId,
                                                     (uint32_t)supported_ars_attrs.size(),
                                                     supported_ars_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create ars profile %s: %d", profile.profile_name.c_str(), status);
        task_process_status handle_status = handleSaiSetStatus(SAI_API_ARS_PROFILE, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    // update read-only attributes
    sai_attribute_t a;
    profile.max_ecmp_groups = 0; // Initialize with default value
    if (isGetImplemented(SAI_OBJECT_TYPE_ARS_PROFILE, SAI_ARS_PROFILE_ATTR_ECMP_ARS_MAX_GROUPS))
    {
        a.id = SAI_ARS_PROFILE_ATTR_ECMP_ARS_MAX_GROUPS;
        status = sai_ars_profile_api->get_ars_profile_attribute(profile.m_sai_ars_id, 1, &a);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to get ars profile %s (oid 0x%" PRIx64 ") attr SAI_ARS_PROFILE_ATTR_ECMP_ARS_MAX_GROUPS: %d",
                profile.profile_name.c_str(), profile.m_sai_ars_id, status);
            a.value.u32 = 0;
        }
        profile.max_ecmp_groups = a.value.u32;
    }

    profile.max_ecmp_members_per_group = 0; // Initialize with default value
    if (isGetImplemented(SAI_OBJECT_TYPE_ARS_PROFILE, SAI_ARS_PROFILE_ATTR_ECMP_ARS_MAX_MEMBERS_PER_GROUP))
    {
        a.id = SAI_ARS_PROFILE_ATTR_ECMP_ARS_MAX_MEMBERS_PER_GROUP;
        status = sai_ars_profile_api->get_ars_profile_attribute(profile.m_sai_ars_id, 1, &a);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to get ars profile %s (oid 0x%" PRIx64 ") attr SAI_ARS_PROFILE_ATTR_ECMP_ARS_MAX_MEMBERS_PER_GROUP: %d",
                profile.profile_name.c_str(), profile.m_sai_ars_id, status);
            a.value.u32 = 0;
        }
        profile.max_ecmp_members_per_group = a.value.u32;
    }

    // get switch attribute SAI_SWITCH_ATTR_ARS_PROFILE with profile id
    attr.id = SAI_SWITCH_ATTR_ARS_PROFILE;
    attr.value.oid = profile.m_sai_ars_id;
    status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set switch attribute SAI_SWITCH_ATTR_ARS_PROFILE to 0x%" PRIx64 ": %d",
            profile.m_sai_ars_id, status);
        task_process_status handle_status = handleSaiSetStatus(SAI_API_SWITCH, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }
    SWSS_LOG_NOTICE("Created ARS profile %s (oid 0x%" PRIx64 ")", profile.profile_name.c_str(), profile.m_sai_ars_id);
    return true;
}

bool ArsOrch::removeArsProfile(ArsProfileEntry &profile)
{
    SWSS_LOG_ENTER();

    sai_status_t    status = SAI_STATUS_NOT_SUPPORTED;
    // get switch attribute SAI_SWITCH_ATTR_ARS_PROFILE with NULL
    sai_attribute_t attr;
    attr.id = SAI_SWITCH_ATTR_ARS_PROFILE;
    attr.value.oid = SAI_NULL_OBJECT_ID;
    status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set switch attribute SAI_SWITCH_ATTR_ARS_PROFILE to NULL: %d", status);
        task_process_status handle_status = handleSaiRemoveStatus(SAI_API_SWITCH, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }
    status = sai_ars_profile_api->remove_ars_profile(profile.m_sai_ars_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove ars profile %s (oid 0x%" PRIx64 "): %d",
            profile.profile_name.c_str(), profile.m_sai_ars_id, status);
        task_process_status handle_status = handleSaiRemoveStatus(SAI_API_ARS_PROFILE, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }
    SWSS_LOG_NOTICE("Removed ARS profile %s (oid 0x%" PRIx64 ")", profile.profile_name.c_str(), profile.m_sai_ars_id);
    return true;
}

bool ArsOrch::setArsProfile(ArsProfileEntry &profile, vector<sai_attribute_t> &ars_attrs)
{
    SWSS_LOG_ENTER();

    sai_status_t    status = SAI_STATUS_NOT_SUPPORTED;

    /* go over set of attr and set only supported attributes  */
    for (auto a : ars_attrs)
    {
        if (!isSetImplemented(SAI_OBJECT_TYPE_ARS_PROFILE, a.id))
        {
            if (a.id == SAI_ARS_PROFILE_ATTR_ENABLE_IPV4 || a.id == SAI_ARS_PROFILE_ATTR_ENABLE_IPV6)
            {
                SWSS_LOG_WARN("Setting Attr %s is not supported. Failed to set ARS profile %s (oid 0x%" PRIx64 ") value %s",
                               ars_profile_attrs.find(a.id)->second.attr_name.c_str(), profile.profile_name.c_str(),
                               profile.m_sai_ars_id, a.value.booldata ? "true" : "false");
            }
            else
            {
                SWSS_LOG_WARN("Setting Attr %s is not supported. Failed to set ARS profile %s (oid 0x%" PRIx64 ") value %u",
                               ars_profile_attrs.find(a.id)->second.attr_name.c_str(), profile.profile_name.c_str(),
                               profile.m_sai_ars_id, a.value.u32);
            }
            continue;
        }

        status = sai_ars_profile_api->set_ars_profile_attribute(profile.m_sai_ars_id, &a);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to set ars profile %s (oid 0x%" PRIx64 ") attr %d: %d",
                profile.profile_name.c_str(), profile.m_sai_ars_id, a.id, status);
            task_process_status handle_status = handleSaiSetStatus(SAI_API_ARS, status);
            if (handle_status != task_success)
            {
                return parseHandleSaiStatusFailure(handle_status);
            }
        }
    }

    return true;
}

bool ArsOrch::createArsObject(ArsObjectEntry *object, vector<sai_attribute_t> &ars_attrs)
{
    SWSS_LOG_ENTER();

    sai_status_t    status = SAI_STATUS_NOT_SUPPORTED;
    vector<sai_attribute_t> supported_ars_attrs;

    /* go over set of attr and set only supported attributes  */
    for (auto a : ars_attrs)
    {
        if (isCreateImplemented(SAI_OBJECT_TYPE_ARS, a.id))
        {
            supported_ars_attrs.push_back(a);
            SWSS_LOG_NOTICE("ARS %s. Setting Attr %d value %u",
                             object->profile_name.c_str(), a.id, a.value.u32);
        }
        else
        {
            SWSS_LOG_WARN("Setting Attr %d is not supported. Failed to set ARS %s value %u",
                           a.id, object->profile_name.c_str(), a.value.u32);
            continue;
        }
    }

    if (supported_ars_attrs.empty())
    {
        SWSS_LOG_WARN("No supported attributes found for ARS %s", object->profile_name.c_str());
        return false;
    }

    status = sai_ars_api->create_ars(&object->ars_object_id,
                                     gSwitchId,
                                     (uint32_t)supported_ars_attrs.size(),
                                     supported_ars_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create ars %s: %d", object->profile_name.c_str(), status);
        task_process_status handle_status = handleSaiSetStatus(SAI_API_ARS, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    return true;
}

bool ArsOrch::setArsObject(ArsObjectEntry *object, vector<sai_attribute_t> &ars_attrs)
{
    SWSS_LOG_ENTER();

    sai_status_t    status = SAI_STATUS_NOT_SUPPORTED;

    /* go over set of attr and set only supported attributes  */
    for (auto a : ars_attrs)
    {
        if (!isSetImplemented(SAI_OBJECT_TYPE_ARS, a.id))
        {
            SWSS_LOG_WARN("Setting Attr %d is not supported. Failed to set ARS %s (oid 0x%" PRIx64 ") value %u",
                        a.id, object->profile_name.c_str(), object->ars_object_id, a.value.u32);
            continue;
        }

        status = sai_ars_api->set_ars_attribute(object->ars_object_id, &a);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to set ars %s (oid 0x%" PRIx64 ") attr %d: %d",
                            object->profile_name.c_str(), object->ars_object_id, a.id, status);
            task_process_status handle_status = handleSaiSetStatus(SAI_API_ARS, status);
            if (handle_status != task_success)
            {
                return parseHandleSaiStatusFailure(handle_status);
            }
        }
    }

    return true;
}

bool ArsOrch::delArsObject(ArsObjectEntry *object)
{
    SWSS_LOG_ENTER();

    sai_status_t    status = SAI_STATUS_NOT_SUPPORTED;

    status = sai_ars_api->remove_ars(object->ars_object_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove ars %s (oid 0x%" PRIx64 ": %d)",
                        object->profile_name.c_str(), object->ars_object_id, status);
        task_process_status handle_status = handleSaiSetStatus(SAI_API_ARS, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    return true;
}

bool ArsOrch::updateArsEnabledInterface(const Port &port, uint32_t scaling_factor, const bool is_enable)
{
    SWSS_LOG_ENTER();

    uint32_t port_load_past_weight = 70;
    uint32_t port_load_future_weight = 30;

    if (scaling_factor == 0)
    {
        scaling_factor = getDefaultScalingFactor(port);
    }

    if (isSetImplemented(SAI_OBJECT_TYPE_PORT, SAI_PORT_ATTR_ARS_PORT_LOAD_SCALING_FACTOR))
    {
        if (!gPortsOrch->setPortArsLoadScaling(port,is_enable ? scaling_factor : 1))
        {
            SWSS_LOG_ERROR("Failed to set ars load scaling factor for port %s", port.m_alias.c_str());
            return false;
        }
    }

    if (isSetImplemented(SAI_OBJECT_TYPE_PORT, SAI_PORT_ATTR_ARS_PORT_LOAD_PAST_WEIGHT) &&
        isSetImplemented(SAI_OBJECT_TYPE_PORT, SAI_PORT_ATTR_ARS_PORT_LOAD_FUTURE_WEIGHT))
    {
        // Get the weights from ars_profile, if defined
        auto ars_profile = m_arsProfiles.begin();
        if (ars_profile != m_arsProfiles.end())
        {
            port_load_past_weight = ars_profile->second.path_metrics.past_load.weight;
            port_load_future_weight = ars_profile->second.path_metrics.future_load.weight;
        }

        if (!gPortsOrch->setPortArsLoadWeight(port, port_load_past_weight, port_load_future_weight))
        {
            SWSS_LOG_ERROR("Failed to set ars load weight for port %s", port.m_alias.c_str());
            return false;
        }
    }

    if (isSetImplemented(SAI_OBJECT_TYPE_PORT, SAI_PORT_ATTR_ARS_ENABLE))
    {
        if (!gPortsOrch->setPortArsEnable(port, is_enable))
        {
            SWSS_LOG_ERROR("Failed to set ars enable for port %s", port.m_alias.c_str());
            return false;
        }
    }

    SWSS_LOG_NOTICE("Interface %s - %sable ARS on interface",
                    port.m_alias.c_str(),
                    is_enable ? "en" : "dis");

    return true;
}

bool ArsOrch::reattachArsObjectToNhgs()
{
    SWSS_LOG_ENTER();

    // Clear nexthops in all ARS objects since we are rebuilding from scratch
    for (auto& ars_obj : m_arsObjects)
    {
        ars_obj.second.nexthops.clear();

        vector<FieldValueTuple> fvVector;
        auto nhg_strings = nexthopGroupKeySetToString(ars_obj.second.nexthops);
        fvVector.emplace_back("nexthops", nhg_strings.first);
        fvVector.emplace_back("nhg_ids", nhg_strings.second);
        m_arsNhgStateTable->set(ars_obj.second.name, fvVector);
    }

    // Get the ARS profile to determine the selector mode
    auto ars_profile = m_arsProfiles.begin();
    ArsNhgSelector ars_nhg_selector_mode = ARS_NHG_SELECTOR_GLOBAL;
    if (ars_profile != m_arsProfiles.end())
    {
        ars_nhg_selector_mode = ars_profile->second.nhg_selector;
    }

    // We need to find the nhgs that are no longer attached to any ARS object
    // Lets start with the current list of nhgs with counters
    std::set<NextHopGroupKey> nhgs_without_counters = m_nhgsWithCounters;
    const NextHopGroupTable& syncdNhgs = gRouteOrch->getSyncdNextHopGroups();
    for (const auto& nhg_entry : syncdNhgs)
    {
        const NextHopGroupKey& nhg = nhg_entry.first;
        auto nhg_sai_id = gRouteOrch->getNextHopGroupId(nhg);
        if (nhg_sai_id == SAI_NULL_OBJECT_ID)
        {
            SWSS_LOG_WARN("Failed to get SAI ID for NHG %s", nhg.to_string().c_str());
            continue;
        }

        ArsObjects::iterator ars_object_entry = m_arsObjects.end();
        sai_object_id_t ars_object_id = SAI_NULL_OBJECT_ID;

        // If ars_profile is not present, set ars_object_id to NULL
        if (ars_profile == m_arsProfiles.end())
        {
            SWSS_LOG_DEBUG("No ARS profile present, detaching ARS object from NHG %s", nhg.to_string().c_str());
            ars_object_id = SAI_NULL_OBJECT_ID;
        }
        else if (ars_nhg_selector_mode == ARS_NHG_SELECTOR_GLOBAL)
        {
            // If no default_ars_object, set ars_object_id to NULL
            if (ars_profile->second.default_ars_object_name.empty())
            {
                SWSS_LOG_DEBUG("Global mode: no default ARS object configured, detaching from NHG %s", nhg.to_string().c_str());
                ars_object_id = SAI_NULL_OBJECT_ID;
            }
            else
            {
                ars_object_entry = m_arsObjects.find(ars_profile->second.default_ars_object_name);
                if (ars_object_entry != m_arsObjects.end())
                {
                    ars_object_id = ars_object_entry->second.ars_object_id;
                    SWSS_LOG_DEBUG("Global mode: attaching default ARS object %s to NHG %s",
                                 ars_object_entry->second.name.c_str(), nhg.to_string().c_str());
                }
                else
                {
                    SWSS_LOG_DEBUG("Global mode: default ARS object %s not found, detaching from NHG %s",
                                ars_profile->second.default_ars_object_name.c_str(), nhg.to_string().c_str());
                    ars_object_id = SAI_NULL_OBJECT_ID;
                }
            }
        }
        else if (ars_nhg_selector_mode == ARS_NHG_SELECTOR_INTERFACE)
        {
            // Get ars_object and ars_object_id only if all interfaces have same ars_object
            bool all_interfaces_valid = true;
            string common_ars_object_name = "";

            for (auto& nh : nhg.getNextHops())
            {
                auto ars_if = m_arsEnabledInterfaces.find(nh.alias);

                // Check if interface is ARS-enabled
                if (ars_if == m_arsEnabledInterfaces.end() || ars_if->second.ars_object_name.empty())
                {
                    SWSS_LOG_DEBUG("Interface mode: interface %s is not ARS-enabled", nh.alias.c_str());
                    all_interfaces_valid = false;
                    break;
                }

                // Check if all interfaces use the same ARS object
                if (common_ars_object_name.empty())
                {
                    common_ars_object_name = ars_if->second.ars_object_name;
                }
                else if (common_ars_object_name != ars_if->second.ars_object_name)
                {
                    SWSS_LOG_DEBUG("Interface mode: interface %s uses different ARS object %s (expected %s)",
                                nh.alias.c_str(), ars_if->second.ars_object_name.c_str(), common_ars_object_name.c_str());
                    all_interfaces_valid = false;
                    break;
                }
            }

            // If all interfaces are valid and use the same ARS object, attach it
            if (all_interfaces_valid && !common_ars_object_name.empty())
            {
                ars_object_entry = m_arsObjects.find(common_ars_object_name);
                if (ars_object_entry != m_arsObjects.end())
                {
                    ars_object_id = ars_object_entry->second.ars_object_id;
                    SWSS_LOG_DEBUG("Interface mode: attaching ARS object %s to NHG %s",
                                 ars_object_entry->second.name.c_str(), nhg.to_string().c_str());
                }
                else
                {
                    SWSS_LOG_WARN("Interface mode: ARS object %s not found, detaching from NHG %s",
                                common_ars_object_name.c_str(), nhg.to_string().c_str());
                    ars_object_id = SAI_NULL_OBJECT_ID;
                }
            }
            else
            {
                SWSS_LOG_DEBUG("Interface mode: not all interfaces are valid or have same ARS object, detaching from NHG %s", nhg.to_string().c_str());
                ars_object_id = SAI_NULL_OBJECT_ID;
            }
        }
        else if (ars_nhg_selector_mode == ARS_NHG_SELECTOR_NEXTHOP)
        {
            // For now, we set it to NULL as nexthop mode requires explicit configuration
            SWSS_LOG_DEBUG("Nexthop mode: NHG %s not supported yet", nhg.to_string().c_str());
            ars_object_id = SAI_NULL_OBJECT_ID;
        }

        // Update the NHG with the determined ARS object
        if (!gRouteOrch->updateNexthopGroupArsState(nhg_sai_id, ars_object_id))
        {
            if (ars_object_id == SAI_NULL_OBJECT_ID)
            {
                SWSS_LOG_ERROR("Failed to detach ARS object from NHG %s", nhg.to_string().c_str());
            }
            else
            {
                SWSS_LOG_ERROR("Failed to attach ARS object to NHG %s", nhg.to_string().c_str());
            }
        }
        else
        {
            // Update the ARS object's nexthops set
            if (ars_object_id != SAI_NULL_OBJECT_ID && ars_object_entry != m_arsObjects.end())
            {
                ars_object_entry->second.nexthops.insert(nhg);
                SWSS_LOG_NOTICE("Attached ARS object %s to NHG %s",
                                ars_object_entry->second.name.c_str(), nhg.to_string().c_str());

                vector<FieldValueTuple> fvVector;
                auto nhg_strings = nexthopGroupKeySetToString(ars_object_entry->second.nexthops);
                fvVector.emplace_back("nexthops", nhg_strings.first);
                fvVector.emplace_back("nhg_ids", nhg_strings.second);
                m_arsNhgStateTable->set(ars_object_entry->second.name, fvVector);

                // If ARS NHG counters are enabled and this NHG didn't have counters before,
                // enable counters for it now
                if (m_arsNhgCountersEnabled)
                {
                    if (m_nhgsWithCounters.find(nhg) == m_nhgsWithCounters.end())
                    {
                        // This is the first time this NHG is getting attached to an ARS object, enable counters
                        enableCountersForNhg(nhg);
                    }
                    // Remove from the "without counters" set
                    nhgs_without_counters.erase(nhg);
                }
            }
        }
    }

    // Handle cleanup of counters for NHGs that no longer have ARS objects attached
    if (m_arsNhgCountersEnabled)
    {
        for (const auto& nhg_key : nhgs_without_counters)
        {
            disableCountersForNhg(nhg_key);
        }
    }

    return true;
}

bool ArsOrch::doTaskArsProfile(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        string key = kfvKey(t);
        size_t found = key.find(consumer.getConsumerTable()->getTableNameSeparator().c_str());
        string table_id = key.substr(0, found);
        string ars_profile_name = key.substr(found + 1);
        string op = kfvOp(t);

        uint32_t max_flows = 0, sampling_interval = 0, past_load_min_val = 0, past_load_max_val = 0, past_load_weight = 0;
        uint32_t future_load_min_val = 0, future_load_max_val = 0, future_load_weight = 0, current_load_min_val = 0, current_load_max_val = 0;
        uint32_t random_seed = 0;
        char seed_hex[16];
        bool ipv6_enable = false, ipv4_enable = false;
        ArsAlgorithm algo = ARS_ALGORITHM_EWMA;
        ArsNhgSelector nhg_selector = ARS_NHG_SELECTOR_INTERFACE;
        string default_ars_object_name = "";

        sai_attribute_t         ars_attr;
        vector<sai_attribute_t> ars_attrs;

        SWSS_LOG_NOTICE("OP: %s, Profile: %s", op.c_str(), ars_profile_name.c_str());

        auto arsProfile_entry = m_arsProfiles.find(ars_profile_name);
        bool is_new_entry = (arsProfile_entry == m_arsProfiles.end());

        if (op == SET_COMMAND)
        {
            vector<FieldValueTuple> fvVector;
            FieldValueTuple name("profile_name", ars_profile_name);
            fvVector.push_back(name);

            // set default values for ars parameters
            auto default_ars_profile = getDefaultArsProfile();
            max_flows = default_ars_profile.max_flows;
            algo = default_ars_profile.algorithm;
            nhg_selector = default_ars_profile.nhg_selector;
            default_ars_object_name = default_ars_profile.default_ars_object_name;
            sampling_interval = default_ars_profile.sampling_interval;
            ipv4_enable = default_ars_profile.ipv4_enabled;
            ipv6_enable = default_ars_profile.ipv6_enabled;
            past_load_min_val = default_ars_profile.path_metrics.past_load.min_value;
            past_load_max_val = default_ars_profile.path_metrics.past_load.max_value;
            past_load_weight = default_ars_profile.path_metrics.past_load.weight;
            future_load_min_val = default_ars_profile.path_metrics.future_load.min_value;
            future_load_max_val = default_ars_profile.path_metrics.future_load.max_value;
            future_load_weight = default_ars_profile.path_metrics.future_load.weight;
            current_load_min_val = default_ars_profile.path_metrics.future_load.min_value;
            current_load_max_val = default_ars_profile.path_metrics.future_load.max_value;

            // For an existing profile, start from the seed already programmed into HW
            // (preserves an operator-pinned value). For a new profile, fall back to the
            // MAC-derived default. The CONFIG_DB-supplied value (parsed below) overrides
            // this, but we only push the attr to SAI on create — see note below.
            random_seed = is_new_entry ? default_ars_profile.random_seed
                                       : arsProfile_entry->second.random_seed;
            bool operator_provided_seed = false;

            // Track old values for change detection
            string old_default_ars_object_name = "";
            ArsNhgSelector old_nhg_selector = ARS_NHG_SELECTOR_INTERFACE;

            if (!is_new_entry)
            {
                old_nhg_selector = nhg_selector;
                old_default_ars_object_name = default_ars_object_name;
            }

            for (auto i : kfvFieldsValues(t))
            {
                if (fvField(i) == ARS_FIELD_NAME_MAX_FLOWS)
                {
                    max_flows = stoi(fvValue(i));
                }
                else if (fvField(i) == ARS_FIELD_NAME_ALGORITHM)
                {
                    if (fvValue(i) == "ewma")
                    {
                        algo = ARS_ALGORITHM_EWMA;
                    }
                    else
                    {
                        SWSS_LOG_WARN("Received unsupported algorithm %s", fvValue(i).c_str());
                        continue;
                    }
                }
                else if (fvField(i) == ARS_FIELD_NAME_NHG_SELECTOR)
                {
                    if (fvValue(i) == "global")
                    {
                        nhg_selector = ARS_NHG_SELECTOR_GLOBAL;
                    }
                    else if (fvValue(i) == "interface")
                    {
                        nhg_selector = ARS_NHG_SELECTOR_INTERFACE;
                    }
                    else if (fvValue(i) == "nexthop")
                    {
                        SWSS_LOG_WARN("Nexthop mode is not supported yet");
                        continue;
                    }
                    else
                    {
                        SWSS_LOG_WARN("Received unsupported NHG selector %s", fvValue(i).c_str());
                        continue;
                    }
                }
                else if (fvField(i) == ARS_FIELD_NAME_DEFAULT_ARS_OBJECT)
                {
                    default_ars_object_name = fvValue(i);
                }
                else if (fvField(i) == ARS_FIELD_NAME_SAMPLING_INTERVAL)
                {
                    sampling_interval = stoi(fvValue(i));
                }
                else if (fvField(i) == ARS_FIELD_NAME_IPV4_ENABLE)
                {
                    ipv4_enable = (fvValue(i) == "true");
                }
                else if (fvField(i) == ARS_FIELD_NAME_IPV6_ENABLE)
                {
                    ipv6_enable = (fvValue(i) == "true");
                }
                else if (fvField(i) == ARS_FIELD_NAME_PAST_LOAD_MIN_VALUE)
                {
                    past_load_min_val = stoi(fvValue(i));
                }
                else if (fvField(i) == ARS_FIELD_NAME_PAST_LOAD_MAX_VALUE)
                {
                    past_load_max_val = stoi(fvValue(i));
                }
                else if (fvField(i) == ARS_FIELD_NAME_PAST_LOAD_WEIGHT)
                {
                    past_load_weight = stoi(fvValue(i));
                }
                else if (fvField(i) == ARS_FIELD_NAME_FUTURE_LOAD_MIN_VALUE)
                {
                    future_load_min_val = stoi(fvValue(i));
                }
                else if (fvField(i) == ARS_FIELD_NAME_FUTURE_LOAD_MAX_VALUE)
                {
                    future_load_max_val = stoi(fvValue(i));
                }
                else if (fvField(i) == ARS_FIELD_NAME_FUTURE_LOAD_WEIGHT)
                {
                    future_load_weight = stoi(fvValue(i));
                }
                else if (fvField(i) == ARS_FIELD_NAME_CURRENT_LOAD_MIN_VALUE)
                {
                    current_load_min_val = stoi(fvValue(i));
                }
                else if (fvField(i) == ARS_FIELD_NAME_CURRENT_LOAD_MAX_VALUE)
                {
                    current_load_max_val = stoi(fvValue(i));
                }
                else if (fvField(i) == ARS_FIELD_NAME_RANDOM_SEED)
                {
                    try
                    {
                        // Accept decimal or 0x-prefixed hex; range and non-zero
                        // are enforced by the sonic-ars YANG model.
                        random_seed = static_cast<uint32_t>(std::stoul(fvValue(i), nullptr, 0));
                        operator_provided_seed = true;
                    }
                    catch (const std::exception &e)
                    {
                        SWSS_LOG_WARN("ARS profile %s: failed to parse random_seed='%s' (%s); keeping current 0x%08x",
                                      ars_profile_name.c_str(), fvValue(i).c_str(), e.what(), random_seed);
                    }
                }
                else
                {
                    SWSS_LOG_WARN("Received unsupported field %s", fvField(i).c_str());
                    break;
                }
            }

            ars_attr.id = SAI_ARS_PROFILE_ATTR_MAX_FLOWS;
            ars_attr.value.u32 = max_flows;
            ars_attrs.push_back(ars_attr);

            ars_attr.id = SAI_ARS_PROFILE_ATTR_ALGO;
            ars_attr.value.u32 = algo;
            ars_attrs.push_back(ars_attr);

            ars_attr.id = SAI_ARS_PROFILE_ATTR_SAMPLING_INTERVAL;
            ars_attr.value.u32 = sampling_interval;
            ars_attrs.push_back(ars_attr);

            ars_attr.id = SAI_ARS_PROFILE_ATTR_ENABLE_IPV4;
            ars_attr.value.booldata = ipv4_enable;
            ars_attrs.push_back(ars_attr);

            ars_attr.id = SAI_ARS_PROFILE_ATTR_ENABLE_IPV6;
            ars_attr.value.booldata = ipv6_enable;
            ars_attrs.push_back(ars_attr);

            ars_attr.id = SAI_ARS_PROFILE_ATTR_LOAD_PAST_MIN_VAL;
            ars_attr.value.u32 = past_load_min_val;
            ars_attrs.push_back(ars_attr);

            ars_attr.id = SAI_ARS_PROFILE_ATTR_LOAD_PAST_MAX_VAL;
            ars_attr.value.u32 = past_load_max_val;
            ars_attrs.push_back(ars_attr);

            // Lets use weights for port weights, and profile weights as exponents in EWMA
            ars_attr.id = SAI_ARS_PROFILE_ATTR_PORT_LOAD_PAST_WEIGHT;
            ars_attr.value.u8 = 3;
            ars_attrs.push_back(ars_attr);

            ars_attr.id = SAI_ARS_PROFILE_ATTR_LOAD_FUTURE_MIN_VAL;
            ars_attr.value.u32 = future_load_min_val;
            ars_attrs.push_back(ars_attr);

            ars_attr.id = SAI_ARS_PROFILE_ATTR_LOAD_FUTURE_MAX_VAL;
            ars_attr.value.u32 = future_load_max_val;
            ars_attrs.push_back(ars_attr);

            ars_attr.id = SAI_ARS_PROFILE_ATTR_PORT_LOAD_FUTURE_WEIGHT;
            ars_attr.value.u8 = 3;
            ars_attrs.push_back(ars_attr);

            ars_attr.id = SAI_ARS_PROFILE_ATTR_LOAD_CURRENT_MIN_VAL;
            ars_attr.value.u32 = current_load_min_val;
            ars_attrs.push_back(ars_attr);

            ars_attr.id = SAI_ARS_PROFILE_ATTR_LOAD_CURRENT_MAX_VAL;
            ars_attr.value.u32 = current_load_max_val;
            ars_attrs.push_back(ars_attr);

            // RANDOM_SEED is push-on-create only: BRCM SDK seen to reject re-setting
            // it while the profile is bound to the switch (SAI_SWITCH_ATTR_ARS_PROFILE).
            // For an existing profile we keep the seed already programmed into HW; if the
            // operator changes it in CONFIG_DB, log a warning that recreation is required.
            if (is_new_entry)
            {
                if (operator_provided_seed)
                {
                    SWSS_LOG_NOTICE("ARS profile %s: using operator-configured random_seed 0x%08x (overriding MAC-derived default)",
                                    ars_profile_name.c_str(), random_seed);
                }
                ars_attr.id = SAI_ARS_PROFILE_ATTR_ARS_RANDOM_SEED;
                ars_attr.value.u32 = random_seed;
                ars_attrs.push_back(ars_attr);
            }
            else if (random_seed != arsProfile_entry->second.random_seed)
            {
                SWSS_LOG_WARN("ARS profile %s: random_seed change from 0x%08x to 0x%08x will not take effect until profile is removed and recreated",
                              ars_profile_name.c_str(),
                              arsProfile_entry->second.random_seed,
                              random_seed);
                // Keep in-memory state aligned with HW so STATE_DB reflects reality
                random_seed = arsProfile_entry->second.random_seed;
            }

            if (is_new_entry)
            {
                ArsProfileEntry arsProfileEntry;
                arsProfileEntry.profile_name = ars_profile_name;
                m_arsProfiles[ars_profile_name] = arsProfileEntry;
                arsProfile_entry = m_arsProfiles.find(ars_profile_name);
                arsProfile_entry->second.ref_count = 0;
                SWSS_LOG_NOTICE("Added new ARS profile %s", ars_profile_name.c_str());
            }

            arsProfile_entry->second.max_flows = max_flows;
            arsProfile_entry->second.algorithm = algo;
            arsProfile_entry->second.nhg_selector = nhg_selector;
            arsProfile_entry->second.default_ars_object_name = default_ars_object_name;
            arsProfile_entry->second.sampling_interval = sampling_interval;
            arsProfile_entry->second.ipv4_enabled = ipv4_enable;
            arsProfile_entry->second.ipv6_enabled = ipv6_enable;
            arsProfile_entry->second.path_metrics.past_load.min_value = past_load_min_val;
            arsProfile_entry->second.path_metrics.past_load.max_value = past_load_max_val;
            arsProfile_entry->second.path_metrics.past_load.weight = past_load_weight;
            arsProfile_entry->second.path_metrics.future_load.min_value = future_load_min_val;
            arsProfile_entry->second.path_metrics.future_load.max_value = future_load_max_val;
            arsProfile_entry->second.path_metrics.future_load.weight = future_load_weight;
            arsProfile_entry->second.path_metrics.current_load.min_value = current_load_min_val;
            arsProfile_entry->second.path_metrics.current_load.max_value = current_load_max_val;
            arsProfile_entry->second.random_seed = random_seed;

            bool res;
            if (is_new_entry)
            {
                res = createArsProfile(arsProfile_entry->second, ars_attrs);
            }
            else
            {
                res = setArsProfile(arsProfile_entry->second, ars_attrs);
            }

            if (!res)
            {
                SWSS_LOG_ERROR("Failed to create/set ARS profile %s", ars_profile_name.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            if (is_new_entry)
            {
                gRouteOrch->attach(this);
            }
            else if ((old_nhg_selector != nhg_selector) || (old_default_ars_object_name != default_ars_object_name))
            {
                // re-evaluate and attach ARS objects to NHGs
                reattachArsObjectToNhgs();
            }

            // Lets fill the state table
            FieldValueTuple max_flows(ARS_FIELD_NAME_MAX_FLOWS,
                std::to_string(arsProfile_entry->second.max_flows));
            fvVector.push_back(max_flows);
            FieldValueTuple algo(ARS_FIELD_NAME_ALGORITHM, arsAlgorithmToString(arsProfile_entry->second.algorithm));
            fvVector.push_back(algo);
            FieldValueTuple nhg_selector(ARS_FIELD_NAME_NHG_SELECTOR, arsNhgSelectorToString(arsProfile_entry->second.nhg_selector));
            fvVector.push_back(nhg_selector);
            FieldValueTuple default_ars_object(ARS_FIELD_NAME_DEFAULT_ARS_OBJECT,
                arsProfile_entry->second.default_ars_object_name);
            fvVector.push_back(default_ars_object);
            FieldValueTuple sampling_interval(ARS_FIELD_NAME_SAMPLING_INTERVAL,
                std::to_string(arsProfile_entry->second.sampling_interval));
            fvVector.push_back(sampling_interval);
            FieldValueTuple past_load_min(ARS_FIELD_NAME_PAST_LOAD_MIN_VALUE,
                std::to_string(arsProfile_entry->second.path_metrics.past_load.min_value));
            fvVector.push_back(past_load_min);
            FieldValueTuple past_load_max(ARS_FIELD_NAME_PAST_LOAD_MAX_VALUE,
                std::to_string(arsProfile_entry->second.path_metrics.past_load.max_value));
            fvVector.push_back(past_load_max);
            FieldValueTuple past_load_weight(ARS_FIELD_NAME_PAST_LOAD_WEIGHT,
                std::to_string(arsProfile_entry->second.path_metrics.past_load.weight));
            fvVector.push_back(past_load_weight);
            FieldValueTuple future_load_min(ARS_FIELD_NAME_FUTURE_LOAD_MIN_VALUE,
                std::to_string(arsProfile_entry->second.path_metrics.future_load.min_value));
            fvVector.push_back(future_load_min);
            FieldValueTuple future_load_max(ARS_FIELD_NAME_FUTURE_LOAD_MAX_VALUE,
                std::to_string(arsProfile_entry->second.path_metrics.future_load.max_value));
            fvVector.push_back(future_load_max);
            FieldValueTuple future_load_weight(ARS_FIELD_NAME_FUTURE_LOAD_WEIGHT,
                std::to_string(arsProfile_entry->second.path_metrics.future_load.weight));
            fvVector.push_back(future_load_weight);
            FieldValueTuple current_load_min(ARS_FIELD_NAME_CURRENT_LOAD_MIN_VALUE,
                std::to_string(arsProfile_entry->second.path_metrics.current_load.min_value));
            fvVector.push_back(current_load_min);
            FieldValueTuple current_load_max(ARS_FIELD_NAME_CURRENT_LOAD_MAX_VALUE,
                std::to_string(arsProfile_entry->second.path_metrics.current_load.max_value));
            fvVector.push_back(current_load_max);
            FieldValueTuple ipv4_enable(ARS_FIELD_NAME_IPV4_ENABLE,
                std::to_string(arsProfile_entry->second.ipv4_enabled));
            fvVector.push_back(ipv4_enable);
            FieldValueTuple ipv6_enable(ARS_FIELD_NAME_IPV6_ENABLE,
                std::to_string(arsProfile_entry->second.ipv6_enabled));
            fvVector.push_back(ipv6_enable);
            snprintf(seed_hex, sizeof(seed_hex), "0x%08x", arsProfile_entry->second.random_seed);
            FieldValueTuple random_seed_fv(ARS_FIELD_NAME_RANDOM_SEED, seed_hex);
            fvVector.push_back(random_seed_fv);
            FieldValueTuple groups("max_ecmp_groups", std::to_string(arsProfile_entry->second.max_ecmp_groups));
            fvVector.push_back(groups);
            FieldValueTuple members("max_ecmp_members_per_group", std::to_string(arsProfile_entry->second.max_ecmp_members_per_group));
            fvVector.push_back(members);
            FieldValueTuple ars_id("m_sai_ars_id", sai_serialize_object_id(arsProfile_entry->second.m_sai_ars_id));
            fvVector.push_back(ars_id);
            m_arsProfileStateTable->set(ars_profile_name, fvVector);
        }
        else if (op == DEL_COMMAND)
        {
            if (arsProfile_entry == m_arsProfiles.end())
            {
                SWSS_LOG_NOTICE("Received delete call for non-existent entry %s", ars_profile_name.c_str());
            }
            else
            {
                /* Check if there are no child objects associated prior to deleting */
                if (arsProfile_entry->second.ref_count == 0 && m_arsEnabledInterfaces.empty())
                {
                    SWSS_LOG_INFO("Received delete call for valid entry with no further dependencies, deleting %s",
                        ars_profile_name.c_str());
                }
                else
                {
                    SWSS_LOG_NOTICE("Child Prefix/Member entries are still associated with this ARS profile %s",
                            ars_profile_name.c_str());
                    it = consumer.m_toSync.erase(it);
                    continue;
                }
                // Detach default ars object from all NHGs
                auto ars_object_entry = m_arsObjects.find(arsProfile_entry->second.default_ars_object_name);
                if (ars_object_entry != m_arsObjects.end())
                {
                    for (auto& nhg : ars_object_entry->second.nexthops)
                    {
                        sai_object_id_t nhg_sai_id = gRouteOrch->getNextHopGroupId(nhg);
                        if (nhg_sai_id != SAI_NULL_OBJECT_ID)
                        {
                            gRouteOrch->updateNexthopGroupArsState(nhg_sai_id, SAI_NULL_OBJECT_ID);
                        }
                    }
                    m_arsNhgStateTable->del(ars_object_entry->second.name);
                }

                bool res;
                res = removeArsProfile(arsProfile_entry->second);
                if (!res)
                {
                    SWSS_LOG_ERROR("Failed to remove ARS profile %s", ars_profile_name.c_str());
                    it = consumer.m_toSync.erase(it);
                    continue;
                }
                gRouteOrch->detach(this);
                m_arsProfiles.erase(arsProfile_entry);
                m_arsProfileStateTable->del(ars_profile_name);
            }
        }

        it = consumer.m_toSync.erase(it);
    }
    return true;
}

bool ArsOrch::doTaskArsInterfaces(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    if (m_arsProfiles.empty())
    {
        SWSS_LOG_WARN("No ARS profiles exist");
        return false;
    }

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        string key = kfvKey(t);
        size_t found = key.find(consumer.getConsumerTable()->getTableNameSeparator().c_str());
        string table_id = key.substr(0, found);
        string if_name = key.substr(found + 1);
        string op = kfvOp(t);
        vector<sai_attribute_t> ars_attrs;
        Port p;
        std::uint32_t scaling_factor = 0;

        SWSS_LOG_NOTICE("ARS Path Op %s Interface %s", op.c_str(), if_name.c_str());

        if (op == SET_COMMAND)
        {
            std::string ars_object_name = "";
            std::string old_ars_object_name = "";
            vector<FieldValueTuple> fvVector;

            // Track old ARS object name if interface already exists
            auto existing_if = m_arsEnabledInterfaces.find(if_name);
            if (existing_if != m_arsEnabledInterfaces.end())
            {
                old_ars_object_name = existing_if->second.ars_object_name;
            }

            for (auto i : kfvFieldsValues(t))
            {
                if (fvField(i) == ARS_FIELD_NAME_SCALING_FACTOR)
                {
                    scaling_factor = (uint32_t)stoi(fvValue(i));

                    if (scaling_factor == 0)
                    {
                        SWSS_LOG_WARN("Received invalid scaling factor %s", fvValue(i).c_str());
                        continue;
                    }
                }
                else if (fvField(i) == ARS_FIELD_NAME_ARS_OBJECT_NAME)
                {
                    ars_object_name = fvValue(i);
                }
                else
                {
                    SWSS_LOG_WARN("Received unsupported field %s", fvField(i).c_str());
                    continue;
                }

                FieldValueTuple value(fvField(i), fvValue(i));
                fvVector.push_back(value);
            }
            m_arsEnabledInterfaces[if_name] = ArsInterfaceEntry { ars_object_name, scaling_factor };
            SWSS_LOG_NOTICE("Added new ARS-enabled interface %s scaling factor %d, ars_object_name %s",
                if_name.c_str(), scaling_factor, ars_object_name.c_str());

            if (old_ars_object_name != ars_object_name)
            {
                // Re-evaluate and attach ARS objects to NHGs as ars_object name change might
                // affect all NHGs with this interface
                reattachArsObjectToNhgs();
            }

            m_arsIfStateTable->set(if_name, fvVector);
        }
        else if (op == DEL_COMMAND)
        {
            auto existing_if = m_arsEnabledInterfaces.find(if_name);
            if (existing_if == m_arsEnabledInterfaces.end())
            {
                SWSS_LOG_INFO("Received delete call for non-existent interface %s", if_name.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }
            else
            {
                m_arsEnabledInterfaces.erase(if_name);
                SWSS_LOG_INFO("Removed interface %s", if_name.c_str());

                // Need to reevaluate and attach ARS objects to NHGs as interface removal might
                // affect all NHGs with this interface
                reattachArsObjectToNhgs();
            }
            m_arsIfStateTable->del(if_name);
        }

        if (!gPortsOrch->getPort(if_name, p) || p.m_port_id == SAI_NULL_OBJECT_ID)
        {
            SWSS_LOG_WARN("Tried to %s non-existent/down interface %s - skipped", op.c_str(), if_name.c_str());
        }
        else
        {
            updateArsEnabledInterface(p, scaling_factor, (op == SET_COMMAND));
        }

        it = consumer.m_toSync.erase(it);
    }

    return true;
}

bool ArsOrch::doTaskArsObject(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        string key = kfvKey(t);
        size_t found = key.find(consumer.getConsumerTable()->getTableNameSeparator().c_str());
        string table_id = key.substr(0, found);
        string ars_object_name = key.substr(found + 1);
        string op = kfvOp(t);
        bool is_new_entry = false;
        sai_attribute_t         ars_attr;
        vector<sai_attribute_t> ars_attrs;

        SWSS_LOG_NOTICE("ARS Object Op %s, Object Name %s", op.c_str(), ars_object_name.c_str());

        auto ars_object_entry = m_arsObjects.find(ars_object_name);

        if (op == SET_COMMAND)
        {
            vector<FieldValueTuple> fvVector;

            if (ars_object_entry == m_arsObjects.end())
            {
                is_new_entry = true;
                ArsObjectEntry new_entry;
                new_entry.name = ars_object_name;
                m_arsObjects[ars_object_name] = new_entry;
                ars_object_entry = m_arsObjects.find(ars_object_name);
            }

            for (auto i : kfvFieldsValues(t))
            {
                if (fvField(i) == ARS_FIELD_NAME_MAX_FLOWS)
                {
                    ars_object_entry->second.max_flows = stoi(fvValue(i));
                    ars_attr.id = SAI_ARS_ATTR_MAX_FLOWS;
                    ars_attr.value.u32 = ars_object_entry->second.max_flows;
                    ars_attrs.push_back(ars_attr);
                }
                else if (fvField(i) == ARS_FIELD_NAME_IDLE_TIME)
                {
                    ars_object_entry->second.flowlet_idle_time = stoi(fvValue(i));
                    ars_attr.id = SAI_ARS_ATTR_IDLE_TIME;
                    ars_attr.value.u32 = ars_object_entry->second.flowlet_idle_time;
                    ars_attrs.push_back(ars_attr);
                }
                else if (fvField(i) == ARS_FIELD_NAME_ASSIGN_MODE)
                {
                    ars_object_entry->second.assign_mode = PER_FLOWLET_QUALITY;
                    ars_attr.id = SAI_ARS_ATTR_MODE;
                    ars_attr.value.u32 = SAI_ARS_MODE_FLOWLET_QUALITY;
                    if (fvValue(i) == ARS_FIELD_NAME_PER_PACKET)
                    {
                        ars_object_entry->second.assign_mode = PER_PACKET;
                        ars_attr.value.u32 = SAI_ARS_MODE_PER_PACKET_QUALITY;
                    }
                    else if (fvValue(i) != ARS_FIELD_NAME_PER_FLOWLET)
                    {
                        SWSS_LOG_WARN("Received unsupported assign_mode %s, defaulted to per_flowlet_quality",
                                        fvValue(i).c_str());
                        break;
                    }
                    ars_attrs.push_back(ars_attr);
                }
                else
                {
                    SWSS_LOG_WARN("Received unsupported field %s", fvField(i).c_str());
                    continue;
                }

                FieldValueTuple value(fvField(i), fvValue(i));
                fvVector.push_back(value);
            }

            if (is_new_entry)
            {
                createArsObject(&ars_object_entry->second, ars_attrs);

                // If this ars object is used by ars_profile or ars_interfaces,
                // re-attach it to relevant NHGs
                bool reattach = false;
                for (auto& ars_profile : m_arsProfiles)
                {
                    if (ars_profile.second.nhg_selector == ARS_NHG_SELECTOR_GLOBAL &&
                        ars_profile.second.default_ars_object_name == ars_object_name)
                    {
                        reattach = true;
                        break;
                    }
                }
                for (auto& ars_interface : m_arsEnabledInterfaces)
                {
                    auto ars_profile = m_arsProfiles.begin();
                    if (ars_profile != m_arsProfiles.end() &&
                        ars_profile->second.nhg_selector == ARS_NHG_SELECTOR_INTERFACE &&
                        ars_interface.second.ars_object_name == ars_object_name)
                    {
                        reattach = true;
                        break;
                    }
                }
                if (reattach)
                {
                    reattachArsObjectToNhgs();
                }
            }
            else
            {
                setArsObject(&ars_object_entry->second, ars_attrs);
            }

            SWSS_LOG_NOTICE("Ars entry %s added", ars_object_name.c_str());

            FieldValueTuple ars_id("ars_object_id", sai_serialize_object_id(ars_object_entry->second.ars_object_id));
            fvVector.push_back(ars_id);
            m_arsObjectStateTable->set(ars_object_name, fvVector);
        }
        else if (op == DEL_COMMAND)
        {
            auto ars_object_entry = m_arsObjects.find(ars_object_name);
            if (ars_object_entry == m_arsObjects.end())
            {
                SWSS_LOG_NOTICE("ARS_OBJECT %s doesn't exists, ignore", ars_object_name.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }
            else
            {
                // Go through the nexthops and remove the ars object from the NHG
                for (auto& nhg : ars_object_entry->second.nexthops)
                {
                    sai_object_id_t nhg_sai_id = gRouteOrch->getNextHopGroupId(nhg);
                    if (nhg_sai_id != SAI_NULL_OBJECT_ID)
                    {
                        gRouteOrch->updateNexthopGroupArsState(nhg_sai_id, SAI_NULL_OBJECT_ID);
                    }
                }
                m_arsNhgStateTable->del(ars_object_name);
                // Unregister ARS NHG flex counters before removing the object,
                if (m_arsNhgCountersEnabled)
                {
                    disableCountersForNhgs(ars_object_entry->second);
                }

                if (ars_object_entry->second.ars_object_id)
                {
                    /* remove ars sai object */
                    delArsObject(&ars_object_entry->second);
                }
                m_arsObjects.erase(ars_object_entry);

                SWSS_LOG_NOTICE("Ars entry %s removed", ars_object_name.c_str());

                m_arsObjectStateTable->del(ars_object_name);
            }
        }

        it = consumer.m_toSync.erase(it);
    }

    return true;
}
void ArsOrch::doTask(Consumer& consumer)
{
    SWSS_LOG_ENTER();
    const string & table_name = consumer.getTableName();

    if (!m_isArsSupported)
    {
        SWSS_LOG_WARN("ARS is not supported");
        return;
    }

	if (table_name == CFG_ARS_PROFILE_TABLE_NAME || table_name == APP_ARS_PROFILE_TABLE_NAME)
    {
        doTaskArsProfile(consumer);
    }
    else if (table_name == CFG_ARS_INTERFACE_TABLE_NAME || table_name == APP_ARS_INTERFACE_TABLE_NAME)
    {
        doTaskArsInterfaces(consumer);
    }
    else if (table_name == CFG_ARS_OBJECT_TABLE_NAME || table_name == APP_ARS_OBJECT_TABLE_NAME)
    {
        doTaskArsObject(consumer);
    }
    else
    {
        SWSS_LOG_ERROR("Unknown table : %s", table_name.c_str());
    }
}

void ArsOrch::generateArsNhgCounterMap()
{
    SWSS_LOG_ENTER();

    m_arsNhgCountersEnabled = true;

    // Iterate through all ARS objects and their associated NHGs
    for (const auto& ars_obj : m_arsObjects)
    {
        const ArsObjectEntry& ars_entry = ars_obj.second;
        enableCountersForNhgs(ars_entry);
    }

    SWSS_LOG_NOTICE("Generated ARS NHG counter map for %zu ARS objects", m_arsObjects.size());
}

void ArsOrch::clearArsNhgCounterMap()
{
    SWSS_LOG_ENTER();

    m_arsNhgCountersEnabled = false;

    // Iterate through all ARS objects and their associated NHGs
    for (const auto& ars_obj : m_arsObjects)
    {
        const ArsObjectEntry& ars_entry = ars_obj.second;
        disableCountersForNhgs(ars_entry);
    }
}

void ArsOrch::disableCountersForNhg(const NextHopGroupKey &nhg)
{
    SWSS_LOG_ENTER();

    sai_object_id_t nhg_sai_id = gRouteOrch->getNextHopGroupId(nhg);
    if (nhg_sai_id == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_WARN("Failed to get SAI ID for ARS NHG %s", nhg.to_string().c_str());
        return;
    }

    // Remove flex counter entry for this NHG
    ars_nhg_stat_manager.clearCounterIdList(nhg_sai_id);

    // Stop tracking that this NHG has counters
    m_nhgsWithCounters.erase(nhg);

    SWSS_LOG_NOTICE("Removed ARS NHG flex counter for NHG %s (SAI ID: %" PRIu64 ")",
                   nhg.to_string().c_str(), nhg_sai_id);
}

void ArsOrch::disableCountersForNhgs(const ArsObjectEntry &ars_entry)
{
    SWSS_LOG_ENTER();

    // For each NHG associated with this ARS object
    for (const auto& nhg_key : ars_entry.nexthops)
    {
        disableCountersForNhg(nhg_key);
    }
}

void ArsOrch::enableCountersForNhg(const NextHopGroupKey &nhg)
{
    SWSS_LOG_ENTER();

    sai_object_id_t nhg_sai_id = gRouteOrch->getNextHopGroupId(nhg);
    if (nhg_sai_id == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_WARN("Failed to get SAI ID for ARS-enabled NHG %s", nhg.to_string().c_str());
        return;
    }

    // Create flex counter entry for this NHG with ARS-specific attributes
    std::unordered_set<std::string> counter_stats;
    counter_stats.insert("SAI_NEXT_HOP_GROUP_ATTR_ARS_PACKET_DROPS");
    counter_stats.insert("SAI_NEXT_HOP_GROUP_ATTR_ARS_NEXT_HOP_REASSIGNMENTS");
    counter_stats.insert("SAI_NEXT_HOP_GROUP_ATTR_ARS_PORT_REASSIGNMENTS");

    ars_nhg_stat_manager.setCounterIdList(nhg_sai_id, CounterType::ARS_NEXTHOP_GROUP, counter_stats);

    // Track that this NHG now has counters
    m_nhgsWithCounters.insert(nhg);

    SWSS_LOG_NOTICE("Added ARS NHG flex counter for NHG %s (SAI ID: %" PRIu64 ")",
                   nhg.to_string().c_str(), nhg_sai_id);
}

void ArsOrch::enableCountersForNhgs(const ArsObjectEntry &ars_entry)
{
    SWSS_LOG_ENTER();

    // For each NHG associated with this ARS object
    for (const auto& nhg_key : ars_entry.nexthops)
    {
        enableCountersForNhg(nhg_key);
    }
}
