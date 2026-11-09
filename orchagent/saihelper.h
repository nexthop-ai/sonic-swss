#pragma once

#include "gearboxutils.h"

#include <chrono>
#include <iosfwd>
#include <string>

extern "C" {
#include "sairedis.h"
}

#include "orch.h"
#include "producertable.h"
#include "events.h"

#define IS_ATTR_ID_IN_RANGE(attrId, objectType, attrPrefix) \
    ((attrId) >= SAI_ ## objectType ## _ATTR_ ## attrPrefix ## _START && (attrId) <= SAI_ ## objectType ## _ATTR_ ## attrPrefix ## _END)

void initFlexCounterTables();
void initSaiApi();
void initSaiRedis();
sai_status_t initSaiPhyApi(swss::gearbox_phy_t *phy);

sai_redis_communication_mode_t resolveCommunicationModeFromContextConfig(
        std::istream& jsonStream,
        sai_redis_communication_mode_t currentMode);

/* Handling SAI status*/
task_process_status handleSaiCreateStatus(sai_api_t api, sai_status_t status, void *context = nullptr);
task_process_status handleSaiSetStatus(sai_api_t api, sai_status_t status, void *context = nullptr);
task_process_status handleSaiRemoveStatus(sai_api_t api, sai_status_t status, void *context = nullptr);
task_process_status handleSaiGetStatus(sai_api_t api, sai_status_t status, void *context = nullptr);
bool parseHandleSaiStatusFailure(task_process_status status);
bool isSaiStatusResourceFull(sai_status_t status);
void handleSaiFailure(sai_api_t api, std::string oper, sai_status_t status, bool abort_on_failure);

void initSaiFailureTable();
void setSaiFailureStatus(bool unhealthy, const std::string& error = "");
bool getSaiFailureStatus(std::string& error);

<<<<<<< HEAD
=======
/* How often a latched SAI failure is restated while it stays set. */
constexpr std::chrono::milliseconds SAI_FAILURE_REMINDER_INTERVAL = std::chrono::minutes(10);

/* Restate a latched SAI failure at NOTICE, at most once per
 * SAI_FAILURE_REMINDER_INTERVAL, so the standing condition stays visible in
 * syslog without turning one failure into an unbounded ERROR stream. The
 * failure itself is logged at ERROR where it happens, and STATE_DB
 * PROCESS_HEALTH|orchagent carries the authoritative status. Returns true
 * when a line was logged. The clock is injectable so tests can drive the
 * cadence without sleeping. */
bool logSaiFailureReminder(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

/* Fixed bounded retry around create_switch so a transient "syncd not ready yet"
 * failure during a ZTP config reload does not immediately abort orchagent. The
 * defaults (36 attempts, 5s apart = 180s) are sized to cover a single
 * uninterrupted syncd SAI init. The parameters are defaulted so production
 * always uses the fixed policy while tests can inject a smaller budget. */
constexpr uint32_t SWITCH_CREATE_MAX_ATTEMPTS = 36;
constexpr uint32_t SWITCH_CREATE_RETRY_INTERVAL_SEC = 5;

sai_status_t createSwitchWithRetry(sai_object_id_t *switch_id,
                                   uint32_t attr_count,
                                   const sai_attribute_t *attr_list,
                                   uint32_t max_attempts = SWITCH_CREATE_MAX_ATTEMPTS,
                                   uint32_t retry_interval_sec = SWITCH_CREATE_RETRY_INTERVAL_SEC);

>>>>>>> f10e234d (NOS-20131: [orchagent] Restate a latched SAI failure at NOTICE once per 10 min, not at ERROR every second (#1822))
void setFlexCounterGroupParameter(const std::string &group,
                                  const std::string &poll_interval,
                                  const std::string &stats_mode,
                                  const std::string &plugin_name="",
                                  const std::string &plugins="",
                                  const std::string &operation="",
                                  bool is_gearbox=false);
void setFlexCounterGroupPollInterval(const std::string &group,
                                     const std::string &poll_interval,
                                     bool is_gearbox=false);
void setFlexCounterGroupOperation(const std::string &group,
                                  const std::string &operation,
                                  bool is_gearbox=false);
void setFlexCounterGroupStatsMode(const std::string &group,
                                  const std::string &stats_mode,
                                  bool is_gearbox=false);

void setFlexCounterGroupBulkChunkSize(const std::string &group,
                                      const std::string &bulk_size,
                                      const std::string &bulk_chunk_size_per_prefix,
                                      bool is_gearbox=false);

void delFlexCounterGroup(const std::string &group,
                         bool is_gearbox=false);

void startFlexCounterPolling(sai_object_id_t switch_oid,
                             const std::string &key,
                             const std::string &counter_ids,
                             const std::string &counter_field_name,
                             const std::string &stats_mode="");
void stopFlexCounterPolling(sai_object_id_t switch_oid,
                            const std::string &key);

std::vector<sai_stat_id_t> queryAvailableCounterStats(const sai_object_type_t);

/*
 * Query the ACL table group lookup type (PARALLEL/SEQUENTIAL) supported by the
 * platform for SAI_ACL_TABLE_GROUP_ATTR_TYPE. Returns the historical PARALLEL
 * default on every non-definitive result so existing platforms are never
 * regressed; only a *successful* query that reports PARALLEL unsupported (and
 * SEQUENTIAL supported) yields SEQUENTIAL. See saihelper.cpp for details.
 */
sai_acl_table_group_type_t querySupportedAclTableGroupType(sai_object_id_t switch_id);
