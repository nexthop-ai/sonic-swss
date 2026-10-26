#include "pfcwdhworch.h"
#include "schema.h"
#include "switchorch.h"
#include "portsorch.h"
#include "notifications.h"
#include "notifier.h"
#include "saiextensions.h"
#include "sai_serialize.h"
#include "redisapi.h"
#include <algorithm>
#include <set>

#define PFC_WD_COUNTER_POLL_TIMEOUT_SEC  1
#define PFC_HW_WD_POLL_MSECS 50

extern sai_object_id_t gSwitchId;
extern sai_switch_api_t* sai_switch_api;
extern sai_port_api_t* sai_port_api;
extern sai_queue_api_t* sai_queue_api;
extern sai_buffer_api_t* sai_buffer_api;
extern SwitchOrch *gSwitchOrch;
extern PortsOrch *gPortsOrch;

PfcWdHwOrch::PfcWdHwOrch(DBConnector *db, vector<string> &tableNames,
                         const vector<sai_port_stat_t> &portStatIds,
                         const vector<sai_queue_stat_t> &queueStatIds,
                         const vector<sai_queue_attr_t> &queueAttrIds):
    PfcWdBaseOrch(db, tableNames),
    c_portStatIds(portStatIds),
    c_queueStatIds(queueStatIds),
    c_queueAttrIds(queueAttrIds),
    m_detectionTimeMin(0),
    m_detectionTimeMax(0),
    m_restorationTimeMin(0),
    m_restorationTimeMax(0),
    m_stateDb(make_shared<DBConnector>("STATE_DB", 0)),
    m_pfcWdHwStateTable(make_shared<Table>(m_stateDb.get(), STATE_PFC_WD_HW_STATE_TABLE_NAME)),
    m_statHistoryTable(make_shared<Table>(getCountersDb().get(), PFC_WD_STAT_HISTORY_TABLE))
{
    SWSS_LOG_ENTER();

    // Mark hardware watchdog recovery in STATE_DB
    this->updateStateTable(PFC_WD_RECOVERY_MECHANISM, PFC_WD_RECOVERY_HARDWARE);

    SWSS_LOG_NOTICE("Initializing hardware-based PFC watchdog");

    // COUNTERS_DB survives a warm restart, and a port whose watchdog config went
    // away meanwhile is never stopped; configured ports are re-listed on start.
    m_statHistoryTable->del(PFC_WD_STAT_HISTORY_PORTS);

    // STATE_DB outlives orchagent too, so every row here describes the previous
    // run's programming. Nothing is programmed yet; a row this run never rewrites
    // would go on reporting a watchdog that no longer exists.
    vector<string> stalePorts;
    m_pfcWdHwStateTable->getKeys(stalePorts);
    for (const auto &portAlias : stalePorts)
    {
        m_pfcWdHwStateTable->del(portAlias);
    }

    initializeTimerRanges();
    registerCallbacks();
    recoverWarmReboot(db);

    SWSS_LOG_NOTICE("Hardware-based PFC watchdog initialization complete");
}

PfcWdHwOrch::~PfcWdHwOrch(void)
{
    SWSS_LOG_ENTER();
}

void PfcWdHwOrch::initializeTimerRanges()
{
    SWSS_LOG_ENTER();

    // Query hardware timer range capabilities
    sai_attribute_t attr_dld, attr_dlr;
    attr_dld.id = SAI_SWITCH_ATTR_PFC_TC_DLD_INTERVAL_RANGE;
    attr_dlr.id = SAI_SWITCH_ATTR_PFC_TC_DLR_INTERVAL_RANGE;

    sai_status_t status_dld = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr_dld);
    sai_status_t status_dlr = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr_dlr);

    if (status_dld == SAI_STATUS_SUCCESS && status_dlr == SAI_STATUS_SUCCESS)
    {
        m_detectionTimeMin = attr_dld.value.u32range.min;
        m_detectionTimeMax = attr_dld.value.u32range.max;
        m_restorationTimeMin = attr_dlr.value.u32range.min;
        m_restorationTimeMax = attr_dlr.value.u32range.max;

        SWSS_LOG_NOTICE("Hardware timer ranges - Detection: %u-%u ms, Restoration: %u-%u ms",
                       m_detectionTimeMin, m_detectionTimeMax,
                       m_restorationTimeMin, m_restorationTimeMax);

        // Store ranges in STATE_DB
        this->updateStateTable(PFC_WD_HW_DETECTION_TIME_MIN, to_string(m_detectionTimeMin));
        this->updateStateTable(PFC_WD_HW_DETECTION_TIME_MAX, to_string(m_detectionTimeMax));
        this->updateStateTable(PFC_WD_HW_RESTORATION_TIME_MIN, to_string(m_restorationTimeMin));
        this->updateStateTable(PFC_WD_HW_RESTORATION_TIME_MAX, to_string(m_restorationTimeMax));
    }
    else
    {
        SWSS_LOG_WARN("Failed to query PFC watchdog hardware timer ranges (detection: %d, restoration: %d)",
                     status_dld, status_dlr);
    }
}

void PfcWdHwOrch::registerCallbacks()
{
    SWSS_LOG_ENTER();

    // The hardware detects storms, so the only plugin is the PFC stat history
    string historySha;
    try
    {
        string historyLuaScript = swss::loadLuaScript(PFC_STAT_HISTORY_PLUGIN);
        historySha = swss::loadRedisScript(getCountersDb().get(), historyLuaScript);
    }
    catch (...)
    {
        SWSS_LOG_WARN("Failed to load %s, PFC stat history will not be recorded",
                      PFC_STAT_HISTORY_PLUGIN);
    }

    // FlexCounter manager for PFC_WD queue statistics
    m_pfcwdFlexCounterManager = make_shared<FlexCounterTaggedCachedManager<sai_object_type_t>>(
        PFC_WD_FLEX_COUNTER_GROUP,
        StatsMode::READ,
        PFC_HW_WD_POLL_MSECS,
        true,
        historySha.empty() ? make_pair(string(), string()) : make_pair(string(QUEUE_PLUGIN_FIELD), historySha));

    SWSS_LOG_NOTICE("Initialized FlexCounter for hardware PFC watchdog with %d ms poll interval",
                   PFC_HW_WD_POLL_MSECS);

    // Create timer for periodic counter updates
    auto interv = timespec { .tv_sec = PFC_WD_COUNTER_POLL_TIMEOUT_SEC, .tv_nsec = 0 };
    auto timer = new SelectableTimer(interv);
    auto executor = new ExecutableTimer(timer, this, "PFC_WD_HW_COUNTERS_POLL");
    Orch::addExecutor(executor);
    timer->start();
    SWSS_LOG_NOTICE("Started periodic counter update timer with %d second interval",
                   PFC_WD_COUNTER_POLL_TIMEOUT_SEC);

    // Register PFC deadlock notification callback
    bool supported = gSwitchOrch->querySwitchCapability(SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_QUEUE_PFC_DEADLOCK_NOTIFY);
    if (supported)
    {
        /* The SAI notification runs on a libsairedis thread, so it is forwarded to
         * the ASIC_DB NOTIFICATIONS channel and consumed here on the orchagent
         * thread instead, keeping the watchdog state single-threaded. */
        m_notificationsDb = make_shared<DBConnector>("ASIC_DB", 0);
        m_deadlockNotificationConsumer = new swss::NotificationConsumer(
            m_notificationsDb.get(), "NOTIFICATIONS");
        m_deadlockNotificationConsumer->setOpAllowList({SAI_SWITCH_NOTIFICATION_NAME_QUEUE_PFC_DEADLOCK});
        m_deadlockNotificationConsumer->setStatsLabel("PfcWdHwOrch:" SAI_SWITCH_NOTIFICATION_NAME_QUEUE_PFC_DEADLOCK);
        auto deadlockNotifier = new Notifier(m_deadlockNotificationConsumer, this,
                                             "PFC_WD_HW_DEADLOCK_NOTIFICATIONS");
        Orch::addExecutor(deadlockNotifier);

        sai_attribute_t attr;
        attr.id = SAI_SWITCH_ATTR_QUEUE_PFC_DEADLOCK_NOTIFY;
        attr.value.ptr = (void *)on_queue_pfc_deadlock;   // forwards to ASIC_DB NOTIFICATIONS

        sai_status_t status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to register PFC deadlock notification callback (status: %d)", status);
        }
        else
        {
            SWSS_LOG_NOTICE("Registered PFC deadlock notification callback");
        }
    }
    else
    {
        SWSS_LOG_NOTICE("PFC deadlock notification not supported by hardware");
    }
}

void PfcWdHwOrch::recoverWarmReboot(DBConnector *db)
{
    SWSS_LOG_ENTER();

    // Re-apply existing CONFIG_DB entries after warm reboot
    Table cfgPfcWdTable(db, CFG_PFC_WD_TABLE_NAME);
    vector<string> keys;
    cfgPfcWdTable.getKeys(keys);

    if (!keys.empty())
    {
        SWSS_LOG_NOTICE("Found %zu existing PFC watchdog configuration(s), will re-apply after ports are ready",
                       keys.size());

        for (const auto &key : keys)
        {
            vector<FieldValueTuple> data;
            if (key != PFC_WD_GLOBAL && cfgPfcWdTable.get(key, data))
            {
                recordSavedTimers(key, data);
            }
        }

        addExistingData(&cfgPfcWdTable);
    }
    else
    {
        SWSS_LOG_INFO("No existing PFC watchdog configuration found");
    }
}

void PfcWdHwOrch::doTask(NotificationConsumer &consumer)
{
    SWSS_LOG_ENTER();

    if (&consumer != m_deadlockNotificationConsumer)
    {
        return;
    }

    std::deque<KeyOpFieldsValuesTuple> entries;
    consumer.pops(entries);

    for (auto &entry : entries)
    {
        string op   = kfvOp(entry);
        string data = kfvKey(entry);

        if (op != SAI_SWITCH_NOTIFICATION_NAME_QUEUE_PFC_DEADLOCK)
        {
            continue;
        }

        uint32_t count = 0;
        sai_queue_deadlock_notification_data_t *deadlockData = nullptr;

        sai_deserialize_queue_deadlock_ntf(data, count, &deadlockData);

        onQueuePfcDeadlock(count, deadlockData);

        sai_deserialize_free_queue_deadlock_ntf(count, deadlockData);
    }
}

void PfcWdHwOrch::onQueuePfcDeadlock(uint32_t count, sai_queue_deadlock_notification_data_t *data)
{
    SWSS_LOG_ENTER();

    for (uint32_t i = 0; i < count; i++)
    {
        auto& notification = data[i];

        // Look up port information for this queue
        sai_object_id_t port_id = 0;
        string port_alias;
        uint8_t queue_index = 0;
        bool port_found = false;

        auto it = m_queueToPortMap.find(notification.queue_id);
        if (it != m_queueToPortMap.end())
        {
            port_id = it->second.port_id;
            port_alias = it->second.port_alias;
            queue_index = it->second.queue_index;
            port_found = true;
        }

        if (notification.event == SAI_QUEUE_PFC_DEADLOCK_EVENT_TYPE_DETECTED)
        {
            // Initialize counters on storm detection
            if (port_found)
            {
                // Report the storm only on the transition into it. The hardware
                // repeats DETECTED while the queue stays deadlocked, and each
                // report publishes a pfc-storm event, which would otherwise
                // inflate anything counting storms downstream.
                if (m_stormedQueues.insert(notification.queue_id).second)
                {
                    string queueIdStr = sai_serialize_object_id(notification.queue_id);
                    initQueueCounters(queueIdStr, notification.queue_id, queue_index);
                    SWSS_LOG_DEBUG("PfcWdHwOrch: Initialized counters for queue 0x%" PRIx64 " on storm detection",
                                  notification.queue_id);

                    this->report_pfc_storm(notification.queue_id, port_id,
                                          queue_index, port_alias, "");
                }
            }
            else
            {
                SWSS_LOG_WARN("PFC deadlock DETECTED on queue 0x%" PRIx64 " (no port info)", notification.queue_id);
            }

            // Recovery mode is not selectable from here. This notification reached us over
            // the ASIC_DB NOTIFICATIONS channel, so `notification` is a local copy that
            // sai_deserialize_free_queue_deadlock_ntf releases once this handler returns;
            // writing app_managed_recovery would set a field nothing reads and could not
            // travel back to SAI.
        }
        else if (notification.event == SAI_QUEUE_PFC_DEADLOCK_EVENT_TYPE_RECOVERED)
        {
            // Update counters on recovery
            if (port_found)
            {
                // Same as the detection side: report only the transition, so a
                // repeated RECOVERED does not publish a restore for a queue that
                // is not in storm.
                if (m_stormedQueues.erase(notification.queue_id) != 0)
                {
                    string queueIdStr = sai_serialize_object_id(notification.queue_id);
                    updateQueueCounters(queueIdStr, notification.queue_id, queue_index, false);  // false = not periodic (recovery)
                    SWSS_LOG_DEBUG("PfcWdHwOrch: Updated counters for queue 0x%" PRIx64 " on storm restoration",
                                  notification.queue_id);

                    // Remove baseline stats
                    m_queueBaselineStats.erase(notification.queue_id);

                    this->report_pfc_restored(notification.queue_id, port_id,
                                             queue_index, port_alias);
                }
            }
            else
            {
                SWSS_LOG_NOTICE("PFC deadlock RECOVERED on queue 0x%" PRIx64 " (no port info)", notification.queue_id);
            }
        }
        else
        {
            SWSS_LOG_ERROR("Unknown PFC deadlock event type %d on queue 0x%" PRIx64,
                          notification.event, notification.queue_id);
        }
    }
}

task_process_status PfcWdHwOrch::createEntry(const string& key, const vector<FieldValueTuple>& data)
{
    SWSS_LOG_ENTER();

    // GLOBAL configuration not supported for hardware watchdog
    if (key == PFC_WD_GLOBAL)
    {
        SWSS_LOG_WARN("GLOBAL configuration is not supported for hardware-based PFC watchdog");

        for (const auto& field : data)
        {
            SWSS_LOG_WARN("Field '%s' with value '%s' is not supported for hardware-based PFC watchdog",
                         fvField(field).c_str(), fvValue(field).c_str());
        }

        return task_process_status::task_invalid_entry;
    }

    uint32_t detectionTime = 0;
    uint32_t restorationTime = 0;
    // Default action is drop
    PfcWdAction action = PfcWdAction::PFC_WD_ACTION_DROP;
    string pfcStatHistory = "disable";
    Port port;

    if (!gPortsOrch->getPort(key, port))
    {
        SWSS_LOG_ERROR("Invalid port interface %s", key.c_str());
        return task_process_status::task_invalid_entry;
    }

    if (port.m_type != Port::PHY)
    {
        SWSS_LOG_ERROR("Interface %s is not physical port", key.c_str());
        return task_process_status::task_invalid_entry;
    }

    // Parse configuration fields
    for (auto i : data)
    {
        const auto &field = fvField(i);
        const auto &value = fvValue(i);

        try
        {
            if (field == PFC_WD_DETECTION_TIME)
            {
                detectionTime = migrateSavedTimer(key, field, static_cast<uint32_t>(stoul(value)),
                                                  m_detectionTimeMin, m_detectionTimeMax);

                // Check detection time is within supported range
                if (m_detectionTimeMin > 0 && m_detectionTimeMax > 0)
                {
                    if (detectionTime < m_detectionTimeMin)
                    {
                        SWSS_LOG_ERROR("Detection time %u ms is below minimum supported value %u ms on port %s",
                                      detectionTime, m_detectionTimeMin, key.c_str());
                        return task_process_status::task_invalid_entry;
                    }

                    if (detectionTime > m_detectionTimeMax)
                    {
                        SWSS_LOG_ERROR("Detection time %u ms exceeds maximum supported value %u ms on port %s",
                                      detectionTime, m_detectionTimeMax, key.c_str());
                        return task_process_status::task_invalid_entry;
                    }
                }
            }
            else if (field == PFC_WD_RESTORATION_TIME)
            {
                restorationTime = migrateSavedTimer(key, field, static_cast<uint32_t>(stoul(value)),
                                                    m_restorationTimeMin, m_restorationTimeMax);

                // Check restoration time is within supported range
                if (m_restorationTimeMin > 0 && m_restorationTimeMax > 0)
                {
                    if (restorationTime < m_restorationTimeMin)
                    {
                        SWSS_LOG_ERROR("Restoration time %u ms is below minimum supported value %u ms on port %s",
                                      restorationTime, m_restorationTimeMin, key.c_str());
                        return task_process_status::task_invalid_entry;
                    }

                    if (restorationTime > m_restorationTimeMax)
                    {
                        SWSS_LOG_ERROR("Restoration time %u ms exceeds maximum supported value %u ms on port %s",
                                      restorationTime, m_restorationTimeMax, key.c_str());
                        return task_process_status::task_invalid_entry;
                    }
                }
            }
            else if (field == PFC_WD_ACTION)
            {
                action = deserializeAction(value);
                if (action == PfcWdAction::PFC_WD_ACTION_UNKNOWN)
                {
                    SWSS_LOG_ERROR("Invalid PFC Watchdog action %s", value.c_str());
                    return task_process_status::task_invalid_entry;
                }
            }
            else if (field == PFC_STAT_HISTORY)
            {
                pfcStatHistory = value;
            }
            else
            {
                SWSS_LOG_ERROR("Unknown PFC Watchdog configuration field %s", field.c_str());
                return task_process_status::task_invalid_entry;
            }
        }
        catch (const invalid_argument& e)
        {
            SWSS_LOG_ERROR("Failed to parse PFC Watchdog %s attribute %s invalid argument error",
                          key.c_str(), field.c_str());
            return task_process_status::task_invalid_entry;
        }
        catch (const out_of_range& e)
        {
            SWSS_LOG_ERROR("Failed to parse PFC Watchdog %s attribute %s out of range error",
                          key.c_str(), field.c_str());
            return task_process_status::task_invalid_entry;
        }
        catch (...)
        {
            SWSS_LOG_ERROR("Failed to parse PFC Watchdog %s attribute %s. Unknown error has been occurred",
                          key.c_str(), field.c_str());
            return task_process_status::task_invalid_entry;
        }
    }

    // Validation
    if (detectionTime == 0)
    {
        SWSS_LOG_ERROR("%s missing", PFC_WD_DETECTION_TIME);
        return task_process_status::task_invalid_entry;
    }
    if (pfcStatHistory != "enable" && pfcStatHistory != "disable")
    {
        SWSS_LOG_ERROR("%s is invalid value for %s", pfcStatHistory.c_str(), PFC_STAT_HISTORY);
        return task_process_status::task_invalid_entry;
    }

    // All ports must use the same switch-level PFC DLR packet action.
    // Action can only change when no ports are configured, or when
    // reconfiguring the single existing port.
    PfcWdAction currentAction = this->getPfcDlrPacketAction();

    bool isSinglePortReconfiguration = (m_hwWdPorts.size() == 1 &&
                                        m_hwWdPorts.find(port.m_alias) != m_hwWdPorts.end());

    if (currentAction != PfcWdAction::PFC_WD_ACTION_UNKNOWN &&
        currentAction != action &&
        !isSinglePortReconfiguration)
    {
        SWSS_LOG_ERROR("PFC DLR packet action mismatch on port %s: current=%s, requested=%s. "
                      "All ports must use the same action. "
                      "Action can only be changed when no ports are configured or when reconfiguring the only configured port.",
                      port.m_alias.c_str(),
                      serializeAction(currentAction).c_str(),
                      serializeAction(action).c_str());
        return task_process_status::task_invalid_entry;
    }

    // Check if port is already configured and has any queue in stormed state
    if (m_hwWdPorts.find(port.m_alias) != m_hwWdPorts.end())
    {
        if (isPortInStormedState(port))
        {
            SWSS_LOG_ERROR("Cannot modify PFC watchdog configuration on port %s: port is in stormed state. "
                          "Wait for storm to pass before making changes.",
                          port.m_alias.c_str());
            return task_process_status::task_invalid_entry;
        }
    }

    // Hardware watchdog doesn't support in-place updates, so stop
    // existing configuration before applying new one
    SWSS_LOG_INFO("Attempting to disable any existing hardware PFC watchdog on port %s before applying new configuration",
                  port.m_alias.c_str());
    stopWdOnPort(port);

    if (!startWdOnPort(port, detectionTime, restorationTime, action, pfcStatHistory))
    {
        return handleStartWdOnPortFailure(port);
    }

    clearPfcWdPending(port);
    SWSS_LOG_NOTICE("Started PFC Watchdog on port %s", port.m_alias.c_str());
    // Port is tracked in m_hwWdPorts by configureHwWatchdog
    return task_process_status::task_success;
}

task_process_status PfcWdHwOrch::deleteEntry(const string& key)
{
	SWSS_LOG_ENTER();

	m_savedOutOfRangeTimers.erase(key);

	Port port;
	if (!gPortsOrch->getPort(key, port))
	{
		SWSS_LOG_ERROR("Invalid port interface %s", key.c_str());
		return task_process_status::task_invalid_entry;
	}

	// If hardware watchdog is configured on this port, disallow deletion
	// while any lossless queue is still in stormed state.
	if (m_hwWdPorts.find(port.m_alias) != m_hwWdPorts.end())
	{
		if (isPortInStormedState(port))
		{
			SWSS_LOG_ERROR(
				"Cannot delete PFC watchdog configuration on port %s: port is in stormed state. "
				"Wait for storm to pass before making changes.",
				port.m_alias.c_str());
			return task_process_status::task_invalid_entry;
		}
	}

	// Delegate to base implementation to stop watchdog on the port and
	// update common bookkeeping.
	return PfcWdBaseOrch::deleteEntry(key);
}

bool PfcWdHwOrch::startWdOnPort(const Port& port,
	    uint32_t detectionTime, uint32_t restorationTime, PfcWdAction action, string pfcStatHistory)
{
	SWSS_LOG_ENTER();

	// For hardware-based watchdog, all hardware programming and flex counter
	// registration are handled in configureHwWatchdog()/initializeQueueStats().
	// Any existing configuration is cleaned up via stopWdOnPort() before this
	// function is invoked from createEntry().
	if (!configureHwWatchdog(port, detectionTime, restorationTime, action))
	{
		return false;
	}

	if (pfcStatHistory == "enable")
	{
		enableStatHistory(port);
	}

	return true;
}

void PfcWdHwOrch::enableStatHistory(const Port& port)
{
    SWSS_LOG_ENTER();

    set<uint8_t> losslessTc;
    if (!getLosslessTcsForPort(port, losslessTc))
    {
        return;
    }

    // The plugin reads PFC RX every poll, so it is polled at the watchdog's
    // interval; ON2OFF is the CLI's transition count, which no other group polls.
    if (!c_portStatIds.empty())
    {
        auto portStatIdSet = filterPfcCounters(
            counterIdsToStr(c_portStatIds, &sai_serialize_port_stat), losslessTc);
        m_pfcwdFlexCounterManager->setCounterIdList(port.m_port_id, CounterType::PORT,
                                                    portStatIdSet, SAI_OBJECT_TYPE_PORT);
    }

    for (auto tc : losslessTc)
    {
        if (tc >= port.m_queue_ids.size())
        {
            continue;
        }

        string queueIdStr = sai_serialize_object_id(port.m_queue_ids[tc]);
        getCountersTable()->hset(queueIdStr, "PFC_STAT_HISTORY", "enable");
    }

    m_statHistoryTable->hset(PFC_WD_STAT_HISTORY_PORTS, sai_serialize_object_id(port.m_port_id), port.m_alias);

    SWSS_LOG_NOTICE("Enabled PFC stat history on port %s", port.m_alias.c_str());
}

void PfcWdHwOrch::clearStatHistoryState(sai_object_id_t portId, sai_object_id_t queueId, uint8_t tc)
{
    SWSS_LOG_ENTER();

    // The history itself (EST_* fields) is kept; only the enable flag and the
    // previous poll's samples go, so a new registration starts from a baseline.
    // The plugin's poll timestamp is global and survives a stop, so it is these
    // deletions that keep a stopped period from being charged on the next poll.
    string queueIdStr = sai_serialize_object_id(queueId);
    getCountersTable()->hdel(queueIdStr, "PFC_STAT_HISTORY");
    getCountersTable()->hdel(queueIdStr, "SAI_QUEUE_ATTR_PAUSE_STATUS_last");
    getCountersTable()->hdel(sai_serialize_object_id(portId),
                             SAI_PORT_STAT_PFC_PREFIX + to_string(tc) + "_RX_PKTS_last");
}

bool PfcWdHwOrch::stopWdOnPort(const Port& port)
{
    SWSS_LOG_ENTER();

    return disableHwWatchdog(port);
}

void PfcWdHwOrch::doTask(SelectableTimer &timer)
{
    SWSS_LOG_ENTER();

    // Update counters for queues in storm
    for (auto& entry : m_queueBaselineStats)
    {
        sai_object_id_t queueId = entry.first;
        auto it = m_queueToPortMap.find(queueId);
        if (it != m_queueToPortMap.end())
        {
            string queueIdStr = sai_serialize_object_id(queueId);
            updateQueueCounters(queueIdStr, queueId, it->second.queue_index, true);  // true = periodic
            SWSS_LOG_DEBUG("Periodic counter update for queue 0x%" PRIx64, queueId);
        }
        else
        {
            SWSS_LOG_WARN("Queue 0x%" PRIx64 " has baseline stats but no port mapping", queueId);
        }
    }
}

bool PfcWdHwOrch::startWdActionOnQueue(const string &event, sai_object_id_t queueId, const string &info)
{
    SWSS_LOG_ENTER();

    // Not used - hardware watchdog reports storms via SAI notifications

    SWSS_LOG_ERROR("startWdActionOnQueue is not supported for hardware-based PFC watchdog. "
                  "Queue 0x%" PRIx64 ", event: %s. Hardware handles actions automatically.",
                  queueId, event.c_str());

    return false;
}



bool PfcWdHwOrch::readBackTimerValue(const Port& port, sai_port_attr_t attrId,
                                     const set<uint8_t>& losslessTc, uint32_t expected,
                                     uint32_t& actual, const string& timerName)
{
    vector<sai_map_t> readBack(PFC_WD_TC_MAX);
    for (uint32_t i = 0; i < PFC_WD_TC_MAX; i++)
    {
        readBack[i].key = i;
        readBack[i].value = 0;
    }

    sai_attribute_t attr;
    attr.id = attrId;
    attr.value.maplist.count = PFC_WD_TC_MAX;
    attr.value.maplist.list = readBack.data();

    sai_status_t status = sai_port_api->get_port_attribute(port.m_port_id, 1, &attr);
    if (status == SAI_STATUS_SUCCESS && attr.value.maplist.count > 0)
    {
        for (uint32_t i = 0; i < attr.value.maplist.count; i++)
        {
            uint8_t tcKey = static_cast<uint8_t>(attr.value.maplist.list[i].key);
            if (losslessTc.find(tcKey) != losslessTc.end())
            {
                actual = attr.value.maplist.list[i].value;
                break;
            }
        }

        if (actual != expected)
        {
            SWSS_LOG_WARN("%s time mismatch on port %s: sent %u, hardware has %u",
                          timerName.c_str(), port.m_alias.c_str(), expected, actual);
        }
        return true;
    }
    else
    {
        SWSS_LOG_WARN("Failed to read back %s time on port %s (status: %d)",
                      timerName.c_str(), port.m_alias.c_str(), status);
        return false;
    }
}

bool PfcWdHwOrch::configureHwWatchdog(const Port& port, uint32_t detectionTime,
                                      uint32_t restorationTime, PfcWdAction action)
{
    SWSS_LOG_ENTER();

    // Validate port has lossless TCs before configuring hardware
    set<uint8_t> losslessTc;
    if (!getLosslessTcsForPort(port, losslessTc))
    {
        writeFailureStatus(port);
        return false;
    }

    SWSS_LOG_NOTICE("Configuring hardware watchdog on port %s: detection=%ums, restoration=%ums, action=%s",
                    port.m_alias.c_str(), detectionTime, restorationTime, serializeAction(action).c_str());

    // Cleanup handler called on configuration failure
    auto handleFailure = [this, &port](const string& errorMsg) -> bool {
        SWSS_LOG_ERROR("%s", errorMsg.c_str());
        disableHwWatchdog(port);
        writeFailureStatus(port);
        return false;
    };

    // Configure switch-level action if needed
    if (!configureSwitchAction(port, action, handleFailure))
    {
        return false;
    }

    // Configure detection and restoration intervals
    if (!configureTimerIntervals(port, losslessTc, detectionTime, restorationTime, handleFailure))
    {
        return false;
    }

    // Enable deadlock detection/recovery on queues
    if (!enableDldrOnLosslessQueues(port, losslessTc, detectionTime, restorationTime, handleFailure))
    {
        return false;
    }

    // Initialize queue statistics in COUNTERS_DB
    initializeQueueStats(port, losslessTc);

    // Track this port
    m_hwWdPorts.insert(port.m_alias);

    SWSS_LOG_NOTICE("Successfully configured hardware PFC watchdog on port %s with %zu lossless TCs",
                   port.m_alias.c_str(), losslessTc.size());

    // Read back and verify timer values from hardware
    uint32_t actualHwDetectionTime = detectionTime;
    uint32_t actualHwRestorationTime = restorationTime;

    readBackTimerValue(port, SAI_PORT_ATTR_PFC_TC_DLD_INTERVAL, losslessTc,
                      detectionTime, actualHwDetectionTime, "Detection");
    readBackTimerValue(port, SAI_PORT_ATTR_PFC_TC_DLR_INTERVAL, losslessTc,
                      restorationTime, actualHwRestorationTime, "Restoration");

    // Write success to STATE_DB
    vector<FieldValueTuple> fvs;
    fvs.emplace_back("recovery_type", "hardware");
    fvs.emplace_back("status", "configured");
    fvs.emplace_back("hw_detection_time", to_string(actualHwDetectionTime));
    fvs.emplace_back("hw_restoration_time", to_string(actualHwRestorationTime));
    fvs.emplace_back("configured_detection_time", to_string(detectionTime));
    fvs.emplace_back("configured_restoration_time", to_string(restorationTime));

    fvs.emplace_back("action", serializeAction(action));
    m_pfcWdHwStateTable->set(port.m_alias, fvs);

    return true;
}

bool PfcWdHwOrch::configureSwitchAction(const Port& port, PfcWdAction action,
                                        const function<bool(const string&)>& handleFailure)
{
    SWSS_LOG_ENTER();

    // Only set action if not already configured
    if (this->getPfcDlrPacketAction() != PfcWdAction::PFC_WD_ACTION_UNKNOWN)
    {
        return true;
    }

    sai_packet_action_t sai_action;
    if (action == PfcWdAction::PFC_WD_ACTION_DROP)
    {
        sai_action = SAI_PACKET_ACTION_DROP;
    }
    else if (action == PfcWdAction::PFC_WD_ACTION_FORWARD || action == PfcWdAction::PFC_WD_ACTION_ALERT)
    {
        sai_action = SAI_PACKET_ACTION_FORWARD;
    }
    else
    {
        return handleFailure("Unsupported PFC DLR packet action: " + serializeAction(action));
    }

    sai_attribute_t attr;
    attr.id = SAI_SWITCH_ATTR_PFC_DLR_PACKET_ACTION;
    attr.value.u32 = sai_action;

    sai_status_t status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        return handleFailure("Failed to set switch level PFC DLR packet action to " +
                           serializeAction(action) + " on port " + port.m_alias +
                           ": " + to_string(status));
    }

    SWSS_LOG_NOTICE("Set PFC DLR packet action to %s at switch level (SAI action: %d)",
                   serializeAction(action).c_str(), sai_action);

    this->setPfcDlrPacketAction(action);
    this->updateDlrPacketActionInStateTable();

    return true;
}

bool PfcWdHwOrch::configureTimerIntervals(const Port& port, const set<uint8_t>& losslessTc,
                                          uint32_t detectionTime, uint32_t restorationTime,
                                          const function<bool(const string&)>& handleFailure)
{
    SWSS_LOG_ENTER();

    // Build map lists for detection and restoration intervals
    std::vector<sai_map_t> dld_map_list;
    std::vector<sai_map_t> dlr_map_list;

    for (auto tc : losslessTc)
    {
        sai_map_t dld_map;
        dld_map.key = tc;
        dld_map.value = detectionTime;
        dld_map_list.push_back(dld_map);

        sai_map_t dlr_map;
        dlr_map.key = tc;
        dlr_map.value = restorationTime;
        dlr_map_list.push_back(dlr_map);
    }

    // Set detection interval on port
    sai_attribute_t attr_dld;
    attr_dld.id = SAI_PORT_ATTR_PFC_TC_DLD_INTERVAL;
    attr_dld.value.maplist.count = static_cast<uint32_t>(dld_map_list.size());
    attr_dld.value.maplist.list = dld_map_list.data();

    sai_status_t status = sai_port_api->set_port_attribute(port.m_port_id, &attr_dld);
    if (status != SAI_STATUS_SUCCESS)
    {
        return handleFailure("Failed to set PFC DLD interval on port " + port.m_alias +
                           ": " + to_string(status));
    }

    SWSS_LOG_NOTICE("Set PFC DLD (detection) interval on port %s to %u ms for %zu TCs",
                   port.m_alias.c_str(), detectionTime, losslessTc.size());

    // Set restoration interval on port
    sai_attribute_t attr_dlr;
    attr_dlr.id = SAI_PORT_ATTR_PFC_TC_DLR_INTERVAL;
    attr_dlr.value.maplist.count = static_cast<uint32_t>(dlr_map_list.size());
    attr_dlr.value.maplist.list = dlr_map_list.data();

    status = sai_port_api->set_port_attribute(port.m_port_id, &attr_dlr);
    if (status != SAI_STATUS_SUCCESS)
    {
        return handleFailure("Failed to set PFC DLR interval on port " + port.m_alias +
                           ": " + to_string(status));
    }

    SWSS_LOG_NOTICE("Set PFC DLR (restoration) interval on port %s to %u ms for %zu TCs",
                   port.m_alias.c_str(), restorationTime, losslessTc.size());

    return true;
}

bool PfcWdHwOrch::enableDldrOnLosslessQueues(const Port& port, const set<uint8_t>& losslessTc,
                                             uint32_t detectionTime, uint32_t restorationTime,
                                             const function<bool(const string&)>& handleFailure)
{
    SWSS_LOG_ENTER();

    // Enable PFC DLDR on each lossless queue
    for (auto tc : losslessTc)
    {
        if (tc >= port.m_queue_ids.size())
        {
            SWSS_LOG_ERROR("TC %d exceeds queue count %zu on port %s",
                          tc, port.m_queue_ids.size(), port.m_alias.c_str());
            continue;
        }

        sai_object_id_t queueId = port.m_queue_ids[tc];

        sai_attribute_t attr_enable;
        attr_enable.id = SAI_QUEUE_ATTR_ENABLE_PFC_DLDR;
        attr_enable.value.booldata = true;

        sai_status_t status = sai_queue_api->set_queue_attribute(queueId, &attr_enable);
        if (status != SAI_STATUS_SUCCESS)
        {
            return handleFailure("Failed to enable PFC DLDR on port " + port.m_alias +
                               " queue " + to_string(tc) + " (0x" +
                               sai_serialize_object_id(queueId) + "): " + to_string(status));
        }

        SWSS_LOG_NOTICE("Enabled PFC DLDR on port %s TC %d queue 0x%" PRIx64 " (detection: %u ms, restoration: %u ms)",
                       port.m_alias.c_str(), tc, queueId, detectionTime, restorationTime);
    }

    return true;
}

void PfcWdHwOrch::initializeQueueStats(const Port& port, const set<uint8_t>& losslessTc)
{
    SWSS_LOG_ENTER();

    // For each lossless queue, register PFC_WD FlexCounters, initialize
    // watchdog counters, and populate the queue→port mapping.
    for (auto tc : losslessTc)
    {
        if (tc >= port.m_queue_ids.size())
        {
            continue;
        }

        sai_object_id_t queueId = port.m_queue_ids[tc];
        string queueIdStr = sai_serialize_object_id(queueId);

        // Register queue stats/attributes in the dedicated PFC_WD FlexCounter group.
        if (m_pfcwdFlexCounterManager)
        {
            if (!c_queueStatIds.empty())
            {
                auto queueStatIdSet = PfcWdBaseOrch::counterIdsToStr(c_queueStatIds, sai_serialize_queue_stat);
                if (queueStatIdSet.empty())
                {
                    SWSS_LOG_WARN("Failed to convert queue stat IDs for queue 0x%" PRIx64 " on port %s",
                                  queueId, port.m_alias.c_str());
                }
                else
                {
                    m_pfcwdFlexCounterManager->setCounterIdList(queueId,
                                                                 CounterType::QUEUE,
                                                                 queueStatIdSet,
                                                                 SAI_OBJECT_TYPE_QUEUE);
                    SWSS_LOG_DEBUG("Registered %zu queue stats for queue 0x%" PRIx64 " on port %s",
                                   queueStatIdSet.size(), queueId, port.m_alias.c_str());
                }
            }

            if (!c_queueAttrIds.empty())
            {
                auto queueAttrIdSet = PfcWdBaseOrch::counterIdsToStr(c_queueAttrIds, sai_serialize_queue_attr);
                if (queueAttrIdSet.empty())
                {
                    SWSS_LOG_WARN("Failed to convert queue attr IDs for queue 0x%" PRIx64 " on port %s",
                                  queueId, port.m_alias.c_str());
                }
                else
                {
                    auto *fcMgr = dynamic_cast<FlexCounterManager*>(m_pfcwdFlexCounterManager.get());
                    if (fcMgr)
                    {
                        fcMgr->setCounterIdList(queueId,
                                                CounterType::QUEUE_ATTR,
                                                queueAttrIdSet);
                        SWSS_LOG_DEBUG("Registered %zu queue attrs for queue 0x%" PRIx64 " on port %s",
                                       queueAttrIdSet.size(), queueId, port.m_alias.c_str());
                    }
                    else
                    {
                        SWSS_LOG_WARN("PFC WD FlexCounter manager is not FlexCounterManager for queue 0x%" PRIx64 " on port %s",
                                      queueId, port.m_alias.c_str());
                    }
                }
            }
        }

        // Add to queue→port mapping for fast lookup in the notification callback.
        PortQueueInfo info;
        info.port_id = port.m_port_id;
        info.port_alias = port.m_alias;
        info.queue_index = tc;
        m_queueToPortMap[queueId] = info;

        // Mark the queue operational without disturbing its accumulated
        // counters, matching the software watchdog
        // (PfcWdActionHandler::initWdCounters). A reconfiguration is a
        // stop-then-start here, so zeroing would make a detection-time change
        // read as a counter reset to a delta-based collector. Clearing the
        // counters is left to the explicit clear path rather than done as a
        // side effect of configuration.
        auto stats = getQueueStats(queueIdStr);
        stats.operational = true;
        updateQueueStats(queueIdStr, stats);

        // Baseline stats will be created on-demand when storm is detected.

        SWSS_LOG_NOTICE("Initialized PFC watchdog for queue 0x%" PRIx64 " on port %s TC %d",
                       queueId, port.m_alias.c_str(), tc);
    }
}

void PfcWdHwOrch::disableQueueDldr(const Port& port, uint8_t tc, sai_object_id_t queueId)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr_enable;
    attr_enable.id = SAI_QUEUE_ATTR_ENABLE_PFC_DLDR;
    attr_enable.value.booldata = false;

    sai_status_t status = sai_queue_api->set_queue_attribute(queueId, &attr_enable);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to disable PFC DLDR on port %s queue %d (0x%" PRIx64 "): %d",
                      port.m_alias.c_str(), tc, queueId, status);
    }
    else
    {
        SWSS_LOG_INFO("Disabled PFC DLDR on port %s TC %d queue 0x%" PRIx64,
                     port.m_alias.c_str(), tc, queueId);
    }
}

bool PfcWdHwOrch::disableHwWatchdog(const Port& port)
{
    SWSS_LOG_ENTER();

    // A PORT_QOS_MAP update can clear the PFC mask while the watchdog is running,
    // so an empty lossless TC set only means there is no queue left to reprogram.
    // The port level teardown and the bookkeeping below still have to run, or the
    // tracked action stays set with no port configured and rejects later changes.
    std::set<uint8_t> losslessTc;
    if (!getLosslessTcsForPort(port, losslessTc))
    {
        SWSS_LOG_INFO("No lossless TC on port %s, clearing the port level watchdog state only",
                      port.m_alias.c_str());
    }

    SWSS_LOG_NOTICE("Disabling hardware watchdog on port %s", port.m_alias.c_str());

    // Disable PFC DLDR on each lossless queue and remove from queue→port mapping
    for (auto tc : losslessTc)
    {
        if (tc >= port.m_queue_ids.size())
        {
            SWSS_LOG_ERROR("TC %d exceeds queue count %zu on port %s",
                          tc, port.m_queue_ids.size(), port.m_alias.c_str());
            continue;
        }

        sai_object_id_t queueId = port.m_queue_ids[tc];

        disableQueueDldr(port, tc, queueId);

        // Clear this queue's registration in the dedicated PFC_WD FlexCounter group.
        if (m_pfcwdFlexCounterManager)
        {
            m_pfcwdFlexCounterManager->clearCounterIdList(queueId, SAI_OBJECT_TYPE_QUEUE);
            SWSS_LOG_DEBUG("Cleared FlexCounter registration for queue 0x%" PRIx64 " on port %s",
                           queueId, port.m_alias.c_str());
        }

        clearStatHistoryState(port.m_port_id, queueId, tc);

        // Remove from queue→port mapping
        m_queueToPortMap.erase(queueId);

        // A queue torn down mid-storm must be reported again if it storms after
        // the watchdog is re-enabled.
        m_stormedQueues.erase(queueId);

        // Remove baseline stats
        m_queueBaselineStats.erase(queueId);
        SWSS_LOG_DEBUG("Removed baseline stats for queue 0x%" PRIx64, queueId);
    }

    // The lossless TC set may have shrunk since the port was configured, so drop
    // anything still mapped to this port instead of only the TCs seen above.
    for (auto it = m_queueToPortMap.begin(); it != m_queueToPortMap.end(); )
    {
        if (it->second.port_alias != port.m_alias)
        {
            ++it;
            continue;
        }

        disableQueueDldr(port, it->second.queue_index, it->first);

        if (m_pfcwdFlexCounterManager)
        {
            m_pfcwdFlexCounterManager->clearCounterIdList(it->first, SAI_OBJECT_TYPE_QUEUE);
        }
        clearStatHistoryState(it->second.port_id, it->first, it->second.queue_index);
        m_stormedQueues.erase(it->first);
        m_queueBaselineStats.erase(it->first);
        it = m_queueToPortMap.erase(it);
    }

    // PFC port counters are registered only while stat history is enabled
    if (m_pfcwdFlexCounterManager)
    {
        m_pfcwdFlexCounterManager->clearCounterIdList(port.m_port_id, SAI_OBJECT_TYPE_PORT);
    }
    m_statHistoryTable->hdel(PFC_WD_STAT_HISTORY_PORTS, sai_serialize_object_id(port.m_port_id));

    // Clear detection and restoration intervals on port level
    std::vector<sai_map_t> empty_map_list;

    // Clear detection interval
    sai_attribute_t attr_dld;
    attr_dld.id = SAI_PORT_ATTR_PFC_TC_DLD_INTERVAL;
    attr_dld.value.maplist.count = 0;
    attr_dld.value.maplist.list = nullptr;

    sai_status_t status = sai_port_api->set_port_attribute(port.m_port_id, &attr_dld);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("Failed to clear PFC DLD interval on port %s: %d", port.m_alias.c_str(), status);
    }

    // Clear restoration interval
    sai_attribute_t attr_dlr;
    attr_dlr.id = SAI_PORT_ATTR_PFC_TC_DLR_INTERVAL;
    attr_dlr.value.maplist.count = 0;
    attr_dlr.value.maplist.list = nullptr;

    status = sai_port_api->set_port_attribute(port.m_port_id, &attr_dlr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("Failed to clear PFC DLR interval on port %s: %d", port.m_alias.c_str(), status);
    }

    // Remove port from tracking set
    m_hwWdPorts.erase(port.m_alias);

    // Remove entry from STATE_DB
    m_pfcWdHwStateTable->del(port.m_alias);

    // If no ports have hardware watchdog configured, reset action to unknown
    if (m_hwWdPorts.empty())
    {
        this->setPfcDlrPacketAction(PfcWdAction::PFC_WD_ACTION_UNKNOWN);
        this->updateDlrPacketActionInStateTable();
        SWSS_LOG_NOTICE("All hardware PFC watchdog ports disabled, reset action to UNKNOWN");
    }

    SWSS_LOG_NOTICE("Successfully disabled hardware PFC watchdog on port %s",
                   port.m_alias.c_str());

    return true;
}

// Timers saved by an image whose watchdog ran in software are not held to this
// hardware's range: the software defaults scale with the port count, so a 256
// port box saves 1600 ms into a 1-1500 ms detection range. Refusing them would
// leave every port without a watchdog after the upgrade, so a value that has
// stayed in CONFIG_DB since orchagent started is brought into range instead.
// Anything written while orchagent runs is still refused by createEntry().
void PfcWdHwOrch::recordSavedTimers(const string& key, const vector<FieldValueTuple>& data)
{
    SWSS_LOG_ENTER();

    for (const auto &fv : data)
    {
        uint32_t minTime = 0;
        uint32_t maxTime = 0;

        if (fvField(fv) == PFC_WD_DETECTION_TIME)
        {
            minTime = m_detectionTimeMin;
            maxTime = m_detectionTimeMax;
        }
        else if (fvField(fv) == PFC_WD_RESTORATION_TIME)
        {
            minTime = m_restorationTimeMin;
            maxTime = m_restorationTimeMax;
        }
        else
        {
            continue;
        }

        uint32_t savedTime = 0;
        try
        {
            savedTime = static_cast<uint32_t>(stoul(fvValue(fv)));
        }
        catch (...)
        {
            // createEntry() reports what it cannot parse
            continue;
        }

        // No software-mode default is 0, so createEntry() keeps refusing it
        if (savedTime == 0)
        {
            continue;
        }

        if (minTime > 0 && maxTime > 0 && (savedTime < minTime || savedTime > maxTime))
        {
            m_savedOutOfRangeTimers[key][fvField(fv)] = savedTime;
        }
    }
}

uint32_t PfcWdHwOrch::migrateSavedTimer(const string& key, const string& field, uint32_t value,
                                        uint32_t minTime, uint32_t maxTime)
{
    SWSS_LOG_ENTER();

    auto port = m_savedOutOfRangeTimers.find(key);
    if (port == m_savedOutOfRangeTimers.end())
    {
        return value;
    }

    auto saved = port->second.find(field);
    if (saved == port->second.end())
    {
        return value;
    }

    // The row has been rewritten since startup, so this value is the operator's
    if (saved->second != value)
    {
        port->second.erase(saved);
        return value;
    }

    uint32_t migrated = value < minTime ? minTime : maxTime;

    SWSS_LOG_WARN("Saved %s %u ms on port %s is outside the supported range %u-%u ms, programming %u ms",
                  field.c_str(), value, key.c_str(), minTime, maxTime, migrated);

    return migrated;
}

void PfcWdHwOrch::writeFailureStatus(const Port& port)
{
    vector<FieldValueTuple> fvs;
    fvs.emplace_back("recovery_type", "hardware");
    fvs.emplace_back("status", "failed");
    m_pfcWdHwStateTable->set(port.m_alias, fvs);
}

bool PfcWdHwOrch::isPortInStormedState(const Port& port)
{
    SWSS_LOG_ENTER();

    // Get PFC mask to identify lossless queues
    uint8_t pfcMask = 0;
    if (!gPortsOrch->getPortPfcWatchdogStatus(port.m_port_id, &pfcMask))
    {
        SWSS_LOG_WARN("Failed to get PFC mask on port %s", port.m_alias.c_str());
        return false;
    }

    // Check each lossless queue to see if any is in stormed state
    for (uint8_t i = 0; i < PFC_WD_TC_MAX; i++)
    {
        if ((pfcMask & (1 << i)) == 0)
        {
            continue;  // Skip non-lossless queues
        }

        if (i >= port.m_queue_ids.size())
        {
            continue;
        }

        sai_object_id_t queueId = port.m_queue_ids[i];
        string queueIdStr = sai_serialize_object_id(queueId);

        // Get queue statistics
        auto stats = getQueueStats(queueIdStr);

        // If operational is false, the queue is in stormed state
        if (!stats.operational)
        {
            SWSS_LOG_WARN("Port %s has queue %d (0x%" PRIx64 ") in stormed state",
                         port.m_alias.c_str(), i, queueId);
            return true;
        }
    }

    return false;
}

bool PfcWdHwOrch::readHwCounters(sai_object_id_t queueId, uint8_t queueIndex, PfcWdHwStats& counters)
{
    SWSS_LOG_ENTER();

    // For HW PFC watchdog, read queue/PG counters from COUNTERS_DB (via FlexCounter)
    // instead of querying SAI directly.

    string queueIdStr = sai_serialize_object_id(queueId);
    vector<FieldValueTuple> fieldValues;

    auto countersTable = getCountersTable();
    if (!countersTable || !countersTable->get(queueIdStr, fieldValues))
    {
        SWSS_LOG_DEBUG("No counter entry found for queue 0x%" PRIx64, queueId);
        memset(&counters, 0, sizeof(PfcWdHwStats));
        return true;
    }

    // Initialize to zero
    counters.txPkt = 0;
    counters.txDropPkt = 0;
    counters.rxPkt = 0;
    counters.rxDropPkt = 0;

    // Read TX counters (queue stats)
    for (const auto& fv : fieldValues)
    {
        const auto field = fvField(fv);
        const auto value = fvValue(fv);

        if (field == "SAI_QUEUE_STAT_PACKETS")
        {
            counters.txPkt = stoull(value);
        }
        else if (field == "SAI_QUEUE_STAT_DROPPED_PACKETS")
        {
            counters.txDropPkt = stoull(value);
        }
    }

    // Read RX counters from the priority group mapped to this queue.
    Port portInstance;
    auto it = m_queueToPortMap.find(queueId);
    if (it == m_queueToPortMap.end())
    {
        SWSS_LOG_ERROR("Queue 0x%" PRIx64 " not found in queue-to-port map", queueId);
        return false;
    }

    if (!gPortsOrch->getPort(it->second.port_id, portInstance))
    {
        SWSS_LOG_ERROR("Cannot get port by ID 0x%" PRIx64, it->second.port_id);
        return false;
    }

    if (queueIndex >= portInstance.m_priority_group_ids.size())
    {
        SWSS_LOG_ERROR("Invalid queue index %u for port 0x%" PRIx64, queueIndex, it->second.port_id);
        return false;
    }

    sai_object_id_t pg = portInstance.m_priority_group_ids[static_cast<size_t>(queueIndex)];
    string pgIdStr = sai_serialize_object_id(pg);

    fieldValues.clear();
    if (countersTable->get(pgIdStr, fieldValues))
    {
        for (const auto& fv : fieldValues)
        {
            const auto field = fvField(fv);
            const auto value = fvValue(fv);

            if (field == "SAI_INGRESS_PRIORITY_GROUP_STAT_PACKETS")
            {
                counters.rxPkt = stoull(value);
            }
            else if (field == "SAI_INGRESS_PRIORITY_GROUP_STAT_DROPPED_PACKETS")
            {
                counters.rxDropPkt = stoull(value);
            }
        }
    }

    SWSS_LOG_DEBUG("Read HW counters for queue 0x%" PRIx64 ": txPkt=%" PRIu64 ", txDropPkt=%" PRIu64 ", rxPkt=%" PRIu64 ", rxDropPkt=%" PRIu64,
                   queueId, counters.txPkt, counters.txDropPkt, counters.rxPkt, counters.rxDropPkt);

    return true;
}

void PfcWdHwOrch::initQueueCounters(const string& queueIdStr, sai_object_id_t queueId, uint8_t queueIndex)
{
    SWSS_LOG_ENTER();

    PfcWdHwStats hwStats;
    if (!readHwCounters(queueId, queueIndex, hwStats))
    {
        return;
    }

    PfcWdBaseOrch::beginStormCounters(getCountersTable(), queueIdStr, hwStats,
                                      m_queueBaselineStats[queueId]);
}

void PfcWdHwOrch::updateQueueCounters(const string& queueIdStr, sai_object_id_t queueId,
                                      uint8_t queueIndex, bool periodic)
{
    SWSS_LOG_ENTER();

    PfcWdHwStats hwStats;
    if (!readHwCounters(queueId, queueIndex, hwStats))
    {
        return;
    }

    // A queue polled before its first storm has no baseline yet. Seed one and
    // record the storm state, but do not count the whole counter as a delta.
    auto it = m_queueBaselineStats.find(queueId);
    const bool baselineValid = (it != m_queueBaselineStats.end());
    if (!baselineValid)
    {
        SWSS_LOG_WARN("No baseline stats found for queue 0x%" PRIx64 ", initializing", queueId);
    }

    PfcWdBaseOrch::accumulateStormCounters(getCountersTable(), queueIdStr, hwStats,
                                           m_queueBaselineStats[queueId], periodic,
                                           baselineValid);
}
