#include <unordered_map>
#include "pfcactionhandler.h"
#include "logger.h"
#include "sai_serialize.h"
#include "portsorch.h"
#include <vector>
#include <algorithm>
#include <inttypes.h>

#define PFC_WD_QUEUE_STATUS             "PFC_WD_STATUS"
#define PFC_WD_QUEUE_STATUS_OPERATIONAL "operational"
#define PFC_WD_QUEUE_STATUS_STORMED     "stormed"

#define PFC_WD_QUEUE_STATS_DEADLOCK_DETECTED "PFC_WD_QUEUE_STATS_DEADLOCK_DETECTED"
#define PFC_WD_QUEUE_STATS_DEADLOCK_RESTORED "PFC_WD_QUEUE_STATS_DEADLOCK_RESTORED"

#define PFC_WD_QUEUE_STATS_TX_PACKETS "PFC_WD_QUEUE_STATS_TX_PACKETS"
#define PFC_WD_QUEUE_STATS_TX_DROPPED_PACKETS "PFC_WD_QUEUE_STATS_TX_DROPPED_PACKETS"
#define PFC_WD_QUEUE_STATS_RX_PACKETS "PFC_WD_QUEUE_STATS_RX_PACKETS"
#define PFC_WD_QUEUE_STATS_RX_DROPPED_PACKETS "PFC_WD_QUEUE_STATS_RX_DROPPED_PACKETS"

#define PFC_WD_QUEUE_STATS_TX_PACKETS_LAST "PFC_WD_QUEUE_STATS_TX_PACKETS_LAST"
#define PFC_WD_QUEUE_STATS_TX_DROPPED_PACKETS_LAST "PFC_WD_QUEUE_STATS_TX_DROPPED_PACKETS_LAST"
#define PFC_WD_QUEUE_STATS_RX_PACKETS_LAST "PFC_WD_QUEUE_STATS_RX_PACKETS_LAST"
#define PFC_WD_QUEUE_STATS_RX_DROPPED_PACKETS_LAST "PFC_WD_QUEUE_STATS_RX_DROPPED_PACKETS_LAST"

extern sai_object_id_t gSwitchId;
extern PortsOrch *gPortsOrch;
extern SwitchOrch *gSwitchOrch;
extern AclOrch * gAclOrch;
extern sai_port_api_t *sai_port_api;
extern sai_queue_api_t *sai_queue_api;
extern sai_buffer_api_t *sai_buffer_api;
extern sai_acl_api_t *sai_acl_api;

PfcWdActionHandler::PfcWdActionHandler(sai_object_id_t port, sai_object_id_t queue,
        uint8_t queueId, shared_ptr<Table> countersTable):
    m_port(port),
    m_queue(queue),
    m_queueId(queueId),
    m_countersTable(countersTable)
{
    SWSS_LOG_ENTER();
}

PfcWdActionHandler::~PfcWdActionHandler(void)
{
    SWSS_LOG_ENTER();

}

void PfcWdActionHandler::initCounters(void)
{
    SWSS_LOG_ENTER();

    if (!getHwCounters(m_hwStats))
    {
        return;
    }

    auto wdQueueStats = getQueueStats(m_countersTable, sai_serialize_object_id(m_queue));
    // initCounters() is called when the event channel receives
    // a storm signal. This can happen when there is a true new storm or
    // when there is an existing storm ongoing before warm-reboot. In the latter case,
    // we treat the storm as an old storm. In particular,
    // we do not increment the detectCount so as to clamp the
    // gap between detectCount and restoreCount by 1 at maximum
    if (!(wdQueueStats.detectCount > wdQueueStats.restoreCount))
    {
        wdQueueStats.detectCount++;

        wdQueueStats.txPktLast = 0;
        wdQueueStats.txDropPktLast = 0;
        wdQueueStats.rxPktLast = 0;
        wdQueueStats.rxDropPktLast = 0;
    }
    wdQueueStats.operational = false;

    updateWdCounters(sai_serialize_object_id(m_queue), wdQueueStats);
}

void PfcWdActionHandler::commitCounters(bool periodic /* = false */)
{
    SWSS_LOG_ENTER();

    PfcWdHwStats hwStats;

<<<<<<< HEAD
    if (!getHwCounters(hwStats))
=======
    // See initCounters(): one read of the queue entry serves both consumers.
    // This path runs once per second for every queue in storm, so the saved
    // read scales with the number of simultaneous storms.
    const string queueIdStr = sai_serialize_object_id(m_queue);
    vector<FieldValueTuple> queueFvs;
    m_countersTable->get(queueIdStr, queueFvs);

    PfcWdHwStats hwStats = {};
    const bool readable = getHwCounters(hwStats, queueFvs);

    if (readable && m_baselineValid)
    {
        // A restore closes the storm with RX and TX drops read from the SAI, as
        // initCounters() opens it. On failure each stays the COUNTERS_DB value
        // already read.
        bool rxRead = false;
        bool txDropRead = false;
        if (!periodic)
        {
            rxRead = readRxFromSai(hwStats);
            txDropRead = readTxDropFromSai(hwStats);
        }

        seedUnknownBaselines(hwStats, rxRead || m_sampleHasRx, txDropRead || m_sampleHasTxDrop);
        holdAtBaseline(hwStats);

        // Only a storm with a baseline is charged here; otherwise nothing is
        // added, and a handler that defers its final count credits it all later.
        if (!periodic)
        {
            addFinalCounters(hwStats);
        }
    }
    else if (readable)
    {
        // The first readable sample of a storm that began without one becomes
        // the baseline, so open it from the SAI as initCounters() would have.
        readBaselineFromSai(hwStats);
    }

    if (!readable && periodic)
>>>>>>> cb949ebc (NOS-20722: [pfcwd] count egress ACL drops in the Broadcom software watchdog's TX drop stats (#1627))
    {
        return;
    }

    auto finalStats = getQueueStats(m_countersTable, sai_serialize_object_id(m_queue));

    if (!periodic)
    {
        finalStats.restoreCount++;
    }
    finalStats.operational = !periodic;

    finalStats.txPktLast += hwStats.txPkt - m_hwStats.txPkt;
    finalStats.txDropPktLast += hwStats.txDropPkt - m_hwStats.txDropPkt;
    finalStats.rxPktLast += hwStats.rxPkt - m_hwStats.rxPkt;
    finalStats.rxDropPktLast += hwStats.rxDropPkt - m_hwStats.rxDropPkt;

    finalStats.txPkt += hwStats.txPkt - m_hwStats.txPkt;
    finalStats.txDropPkt += hwStats.txDropPkt - m_hwStats.txDropPkt;
    finalStats.rxPkt += hwStats.rxPkt - m_hwStats.rxPkt;
    finalStats.rxDropPkt += hwStats.rxDropPkt - m_hwStats.rxDropPkt;

    m_hwStats = hwStats;

    updateWdCounters(sai_serialize_object_id(m_queue), finalStats);
}

PfcWdActionHandler::PfcWdQueueStats PfcWdActionHandler::getQueueStats(shared_ptr<Table> countersTable, const string &queueIdStr)
{
    SWSS_LOG_ENTER();

    PfcWdQueueStats stats;
    memset(&stats, 0, sizeof(PfcWdQueueStats));
    stats.operational = true;
    vector<FieldValueTuple> fieldValues;

    if (!countersTable->get(queueIdStr, fieldValues))
    {
        return stats;
    }

    for (const auto& fv : fieldValues)
    {
        const auto field = fvField(fv);
        const auto value = fvValue(fv);

        if (field == PFC_WD_QUEUE_STATS_DEADLOCK_DETECTED)
        {
            stats.detectCount = stoul(value);
        }
        else if (field == PFC_WD_QUEUE_STATS_DEADLOCK_RESTORED)
        {
            stats.restoreCount = stoul(value);
        }
        else if (field == PFC_WD_QUEUE_STATUS)
        {
            stats.operational = value == PFC_WD_QUEUE_STATUS_OPERATIONAL ? true : false;
        }
        else if (field == PFC_WD_QUEUE_STATS_TX_PACKETS)
        {
            stats.txPkt = stoul(value);
        }
        else if (field == PFC_WD_QUEUE_STATS_TX_DROPPED_PACKETS)
        {
            stats.txDropPkt = stoul(value);
        }
        else if (field == PFC_WD_QUEUE_STATS_RX_PACKETS)
        {
            stats.rxPkt = stoul(value);
        }
        else if (field == PFC_WD_QUEUE_STATS_RX_DROPPED_PACKETS)
        {
            stats.rxDropPkt = stoul(value);
        }
        else if (field == PFC_WD_QUEUE_STATS_TX_PACKETS_LAST)
        {
            stats.txPktLast = stoul(value);
        }
        else if (field == PFC_WD_QUEUE_STATS_TX_DROPPED_PACKETS_LAST)
        {
            stats.txDropPktLast = stoul(value);
        }
        else if (field == PFC_WD_QUEUE_STATS_RX_PACKETS_LAST)
        {
            stats.rxPktLast = stoul(value);
        }
        else if (field == PFC_WD_QUEUE_STATS_RX_DROPPED_PACKETS_LAST)
        {
            stats.rxDropPktLast = stoul(value);
        }
    }

    return stats;
}

void PfcWdActionHandler::initWdCounters(shared_ptr<Table> countersTable, const string &queueIdStr)
{
    SWSS_LOG_ENTER();

    vector<FieldValueTuple> resultFvValues;

    auto stats = getQueueStats(countersTable, queueIdStr);

    resultFvValues.emplace_back(PFC_WD_QUEUE_STATS_DEADLOCK_DETECTED, to_string(stats.detectCount));
    resultFvValues.emplace_back(PFC_WD_QUEUE_STATS_DEADLOCK_RESTORED, to_string(stats.restoreCount));
    resultFvValues.emplace_back(PFC_WD_QUEUE_STATUS, PFC_WD_QUEUE_STATUS_OPERATIONAL);

    countersTable->set(queueIdStr, resultFvValues);
}

void PfcWdActionHandler::updateWdCounters(const string& queueIdStr, const PfcWdQueueStats& stats)
{
    SWSS_LOG_ENTER();

    vector<FieldValueTuple> resultFvValues;

    resultFvValues.emplace_back(PFC_WD_QUEUE_STATS_DEADLOCK_DETECTED, to_string(stats.detectCount));
    resultFvValues.emplace_back(PFC_WD_QUEUE_STATS_DEADLOCK_RESTORED, to_string(stats.restoreCount));

    resultFvValues.emplace_back(PFC_WD_QUEUE_STATS_TX_PACKETS, to_string(stats.txPkt));
    resultFvValues.emplace_back(PFC_WD_QUEUE_STATS_TX_DROPPED_PACKETS, to_string(stats.txDropPkt));
    resultFvValues.emplace_back(PFC_WD_QUEUE_STATS_RX_PACKETS, to_string(stats.rxPkt));
    resultFvValues.emplace_back(PFC_WD_QUEUE_STATS_RX_DROPPED_PACKETS, to_string(stats.rxDropPkt));

    resultFvValues.emplace_back(PFC_WD_QUEUE_STATS_TX_PACKETS_LAST, to_string(stats.txPktLast));
    resultFvValues.emplace_back(PFC_WD_QUEUE_STATS_TX_DROPPED_PACKETS_LAST, to_string(stats.txDropPktLast));
    resultFvValues.emplace_back(PFC_WD_QUEUE_STATS_RX_PACKETS_LAST, to_string(stats.rxPktLast));
    resultFvValues.emplace_back(PFC_WD_QUEUE_STATS_RX_DROPPED_PACKETS_LAST, to_string(stats.rxDropPktLast));

    resultFvValues.emplace_back(PFC_WD_QUEUE_STATUS, stats.operational ?
                                                     PFC_WD_QUEUE_STATUS_OPERATIONAL :
                                                     PFC_WD_QUEUE_STATUS_STORMED);

    m_countersTable->set(queueIdStr, resultFvValues);
}

PfcWdDlrHandler::PfcWdDlrHandler(sai_object_id_t port, sai_object_id_t queue,
                                               uint8_t queueId, shared_ptr<Table> countersTable):
    PfcWdActionHandler(port, queue, queueId, countersTable)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = SAI_QUEUE_ATTR_PFC_DLR_INIT;
    attr.value.booldata = true;

    // Set DLR init to true to start PFC deadlock recovery
    sai_status_t status = sai_queue_api->set_queue_attribute(queue, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set PFC DLR INIT on port 0x%" PRIx64 " queue 0x%" PRIx64
                       " queueId %d : %d",
                       port, queue, queueId, status);
        return;
    }
}

PfcWdDlrHandler::~PfcWdDlrHandler(void)
{
    SWSS_LOG_ENTER();

    sai_object_id_t port = getPort();
    sai_object_id_t queue = getQueue();
    uint8_t queueId = getQueueId();

    sai_attribute_t attr;
    attr.id = SAI_QUEUE_ATTR_PFC_DLR_INIT;
    attr.value.booldata = false;

    // Set DLR init to false to stop PFC deadlock recovery
    sai_status_t status = sai_queue_api->set_queue_attribute(getQueue(), &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to clear PFC DLR INIT on port 0x%" PRIx64 " queue 0x%" PRIx64
                       " queueId %d : %d", port, queue, queueId, status);
        return;
    }
}

<<<<<<< HEAD
=======
// PfcWdAclHandler mitigates a storm with two ACL constructs, which select the
// mitigated ports differently:
//
//   ingress  One shared DROP table, one rule per queue index matching
//            TC == <index> AND IN_PORTS IN {storming ports}. The rule names its
//            own ports, so the storm path only edits the IN_PORTS list.
//
//   egress   One table per queue index, whose rule matches TC == <index> only.
//            The mitigated ports are the ports bound to the table, so binding
//            stays on the storm path. On Broadcom a single table is bound at
//            switch level instead, with one rule per (port, queue) matching
//            TC and OUT_PORT; that rule's counter adds to the queue's TX drops.

bool PfcWdAclHandler::useSharedEgressAclTable(void)
{
    // Read the environment each time; the DVS tests set it per test case.
    string platform = getenv("platform") ? getenv("platform") : "";

    return platform == BRCM_PLATFORM_SUBSTRING;
}

string PfcWdAclHandler::ingressRuleName(uint8_t queueId)
{
    return "Rule_PfcWdAclHandler_" + to_string(queueId);
}

string PfcWdAclHandler::egressTableName(uint8_t queueId)
{
    return useSharedEgressAclTable()
        ? string("EgressTable_PfcWdAclHandler")
        : "EgressTable_PfcWdAclHandler_" + to_string(queueId);
}

bool PfcWdAclHandler::ensureAclTable(const string &strTable, bool ingress)
{
    if (m_aclTables.find(strTable) != m_aclTables.end())
    {
        return true;
    }

    return createPfcAclTable(strTable, ingress);
}

bool PfcWdAclHandler::flushQueueId(uint8_t queueId, std::set<sai_object_id_t> &failedQueues)
{
    SWSS_LOG_ENTER();

    const string strRule = ingressRuleName(queueId);
    auto &desired = m_desiredInPorts[queueId];

    // A port that is gone or not physical fails only its own queue.
    for (auto it = desired.begin(); it != desired.end(); )
    {
        Port p;
        if (!gPortsOrch->getPort(it->first, p))
        {
            // Deleted while stormed; its handler's teardown follows.
            SWSS_LOG_NOTICE("Dropping port oid:0x%" PRIx64 " from ingress PFCWD drop rule %s: "
                            "port no longer exists", it->first, strRule.c_str());
            failedQueues.insert(it->second);
            it = desired.erase(it);
            continue;
        }
        if (p.m_type != Port::PHY)
        {
            SWSS_LOG_ERROR("Dropping port oid:0x%" PRIx64 " from ingress PFCWD drop rule %s: "
                           "not a physical port", it->first, strRule.c_str());
            failedQueues.insert(it->second);
            it = desired.erase(it);
            continue;
        }
        ++it;
    }

    AclRule *rule = gAclOrch->getAclRule(INGRESS_TABLE_DROP, strRule);
    std::set<sai_object_id_t> current;
    if (rule != nullptr)
    {
        for (auto p : rule->getInPorts())
        {
            current.insert(p);
        }
    }

    vector<sai_object_id_t> toAdd, toRemove;
    for (const auto &kv : desired)
    {
        if (current.find(kv.first) == current.end())
        {
            toAdd.push_back(kv.first);
        }
    }
    for (auto p : current)
    {
        if (desired.find(p) == desired.end())
        {
            toRemove.push_back(p);
        }
    }

    if (toAdd.empty() && toRemove.empty())
    {
        m_failedFlushes.erase(queueId);
        return true;
    }

    bool ok = false;
    try
    {
        if (desired.empty())
        {
            // A rule with an empty IN_PORTS list would match every port.
            ok = gAclOrch->removeAclRule(INGRESS_TABLE_DROP, strRule);
        }
        else if (rule == nullptr)
        {
            ok = ensureAclTable(INGRESS_TABLE_DROP, true);
            if (ok)
            {
                auto newRule = make_shared<AclRulePacket>(gAclOrch, strRule, INGRESS_TABLE_DROP);
                ok = createPfcAclRule(newRule, queueId, INGRESS_TABLE_DROP, toAdd);
            }
        }
        else
        {
            ok = gAclOrch->updateAclRuleInPorts(INGRESS_TABLE_DROP, strRule, toAdd, toRemove);
        }
    }
    catch (const std::exception &e)
    {
        SWSS_LOG_ERROR("Exception while programming ingress PFCWD drop rule %s: %s",
                       strRule.c_str(), e.what());
        ok = false;
    }

    if (ok)
    {
        m_failedFlushes.erase(queueId);
        return true;
    }

    uint32_t failures = ++m_failedFlushes[queueId];
    if (failures == 1 || failures % 60 == 0)
    {
        SWSS_LOG_ERROR("Failed to program ingress PFCWD drop rule %s (+%zu/-%zu ports, attempt %u)",
                       strRule.c_str(), toAdd.size(), toRemove.size(), failures);
    }

    // The rule is unchanged. The ports it should have gained are not mitigated,
    // so their queues are reported and no longer asked for; removals stay
    // pending and are retried by the next flush.
    for (auto p : toAdd)
    {
        failedQueues.insert(desired[p]);
        desired.erase(p);
    }

    return toRemove.empty();
}

std::set<sai_object_id_t> PfcWdAclHandler::flush(void)
{
    SWSS_LOG_ENTER();

    std::set<sai_object_id_t> failedQueues;

    for (auto it = m_dirtyQueueIds.begin(); it != m_dirtyQueueIds.end(); )
    {
        if (flushQueueId(*it, failedQueues))
        {
            it = m_dirtyQueueIds.erase(it);
        }
        else
        {
            ++it;
        }
    }

    return failedQueues;
}

void PfcWdAclHandler::markInvalid(void)
{
    SWSS_LOG_ENTER();

    m_rolledBack = true;

    auto &desired = m_desiredInPorts[getQueueId()];
    auto it = desired.find(getPort());
    if (it != desired.end() && it->second == getQueue())
    {
        desired.erase(it);
        m_dirtyQueueIds.insert(getQueueId());
    }
}

>>>>>>> cb949ebc (NOS-20722: [pfcwd] count egress ACL drops in the Broadcom software watchdog's TX drop stats (#1627))
PfcWdAclHandler::PfcWdAclHandler(sai_object_id_t port, sai_object_id_t queue,
        uint8_t queueId, shared_ptr<Table> countersTable):
    PfcWdLossyHandler(port, queue, queueId, countersTable)
{
    SWSS_LOG_ENTER();

    string table_type;

    string queuestr = to_string(queueId);
    m_strRule = "Rule_PfcWdAclHandler_" + queuestr;

    // Ingress table/rule creation
    table_type = TABLE_TYPE_DROP;
    m_strIngressTable = INGRESS_TABLE_DROP;
    auto found = m_aclTables.find(m_strIngressTable);
    if (found == m_aclTables.end())
    {
        // First time of handling PFC for this queue, create ACL table, and bind
        if (createPfcAclTable(port, m_strIngressTable, true))
        {
            shared_ptr<AclRulePacket> newRule = make_shared<AclRulePacket>(gAclOrch, m_strRule, m_strIngressTable);
            if (!createPfcAclRule(newRule, queueId, m_strIngressTable, port))
            {
                SWSS_LOG_ERROR("Failed to create ingress PFCWD drop rule %s, last SAI status %s",
                               m_strRule.c_str(), sai_serialize_status(newRule->getLastSaiStatus()).c_str());
                // The ingress DROP table is shared and persistent; intentionally keep
                // the just-created empty table so the next queue can reuse it.
                m_rolledBack = true;
                return;
            }
        }
        else
        {
            SWSS_LOG_ERROR("Failed to create ingress PFCWD ACL table %s; PFCWD drop action is not installed",
                           m_strIngressTable.c_str());
            m_rolledBack = true;
            return;
        }
    }
    else
    {
        AclRule* rule = gAclOrch->getAclRule(m_strIngressTable, m_strRule);
        if (rule == nullptr)
        {
            shared_ptr<AclRulePacket> newRule = make_shared<AclRulePacket>(gAclOrch, m_strRule, m_strIngressTable);
            if (!createPfcAclRule(newRule, queueId, m_strIngressTable, port))
            {
                SWSS_LOG_ERROR("Failed to create ingress PFCWD drop rule %s, last SAI status %s",
                               m_strRule.c_str(), sai_serialize_status(newRule->getLastSaiStatus()).c_str());
                m_rolledBack = true;
                return;
            }
        }
        else
        {
            gAclOrch->updateAclRule(m_strIngressTable, m_strRule, MATCH_IN_PORTS, &port, RULE_OPER_ADD);
        }
    }

    // Egress table/rule creation
    table_type = TABLE_TYPE_PFCWD;

    // Use shared egress acl table for BRCM DNX platform.
    string platform = getenv("platform") ? getenv("platform") : "";
    string sub_platform = getenv("sub_platform") ? getenv("sub_platform") : "";
    shared_egress_acl_table = (platform == BRCM_PLATFORM_SUBSTRING &&
                               sub_platform == BRCM_DNX_PLATFORM_SUBSTRING);

    if (shared_egress_acl_table)
    {
        Port p;
        if (!gPortsOrch->getPort(port, p))
        {
            SWSS_LOG_ERROR("Failed to get port structure from port oid 0x%" PRIx64, port);
            return;
        }
        m_strEgressRule = "Egress_Rule_PfcWdAclHandler_" + p.m_alias + "_" + queuestr;
<<<<<<< HEAD
        m_strEgressTable = "EgressTable_PfcWdAclHandler";
        found = m_aclTables.find(m_strEgressTable);
        if (found == m_aclTables.end())
        {
            // First time of handling PFC, create ACL table and also ACL rule.
            if (createPfcAclTable(port, m_strEgressTable, false))
            {
                shared_ptr<AclRulePacket> newRule = make_shared<AclRulePacket>(gAclOrch, m_strEgressRule, m_strEgressTable);
                if (!createPfcAclRule(newRule, queueId, m_strEgressTable, port))
                {
                    SWSS_LOG_ERROR("Failed to create egress PFCWD drop rule %s, last SAI status %s",
                                   m_strEgressRule.c_str(), sai_serialize_status(newRule->getLastSaiStatus()).c_str());
                    // Roll back the egress table created just above (no rule could be
                    // installed in it) along with the ingress drop rule.
                    gAclOrch->removeAclTable(m_strEgressTable);
                    m_aclTables.erase(m_strEgressTable);
                    removeIngressBinding(port);
                    m_rolledBack = true;
                }
            }
            else
            {
                SWSS_LOG_ERROR("Failed to create egress PFCWD ACL table %s; rolling back ingress PFCWD drop rule %s",
                               m_strEgressTable.c_str(), m_strRule.c_str());
                removeIngressBinding(port);
                m_rolledBack = true;
            }
        }
        else
        {
            // ACL table already exists. Add ACL rule if needed.
            AclRule* rule = gAclOrch->getAclRule(m_strEgressTable, m_strEgressRule);
            if (rule == nullptr)
            {
                shared_ptr<AclRulePacket> newRule = make_shared<AclRulePacket>(gAclOrch, m_strEgressRule, m_strEgressTable);
                if (!createPfcAclRule(newRule, queueId, m_strEgressTable, port))
                {
                    // The egress table is shared with other queues/ports - leave it in place.
                    SWSS_LOG_ERROR("Failed to create egress PFCWD drop rule %s, last SAI status %s",
                                   m_strEgressRule.c_str(), sai_serialize_status(newRule->getLastSaiStatus()).c_str());
                    removeIngressBinding(port);
                    m_rolledBack = true;
                }
            }
=======

        // A queue that storms again before its previous storm's final count
        // was taken: take it now, so the new storm starts with a fresh rule.
        finishDeferredEgressRule(m_strEgressRule);

        // A rule still here is one whose removal failed. Its counter holds an
        // earlier storm's drops, so it is never reused.
        if (gAclOrch->getAclRule(m_strEgressTable, m_strEgressRule) != nullptr &&
            !gAclOrch->removeAclRule(m_strEgressTable, m_strEgressRule))
        {
            SWSS_LOG_ERROR("Failed to remove stale egress PFCWD drop rule %s", m_strEgressRule.c_str());
            m_strEgressRule.clear();
            m_rolledBack = true;
            return;
        }

        // The table is bound at switch level on this platform, so the rule
        // itself selects the port with OUT_PORT.
        auto newRule = make_shared<AclRulePacket>(gAclOrch, m_strEgressRule, m_strEgressTable);
        if (!createPfcAclRule(newRule, queueId, m_strEgressTable, vector<sai_object_id_t>{ port }))
        {
            SWSS_LOG_ERROR("Failed to create egress PFCWD drop rule %s", m_strEgressRule.c_str());
            m_rolledBack = true;
            return;
>>>>>>> cb949ebc (NOS-20722: [pfcwd] count egress ACL drops in the Broadcom software watchdog's TX drop stats (#1627))
        }
    }
    else
    {
        m_strEgressTable = "EgressTable_PfcWdAclHandler_" + queuestr;
        found = m_aclTables.find(m_strEgressTable);
        if (found == m_aclTables.end())
        {
            // First time of handling PFC for this queue, create ACL table, and bind
            if (createPfcAclTable(port, m_strEgressTable, false))
            {
                shared_ptr<AclRulePacket> newRule = make_shared<AclRulePacket>(gAclOrch, m_strRule, m_strEgressTable);
                if (!createPfcAclRule(newRule, queueId, m_strEgressTable, port))
                {
                    SWSS_LOG_ERROR("Failed to create egress PFCWD drop rule %s, last SAI status %s",
                                   m_strRule.c_str(), sai_serialize_status(newRule->getLastSaiStatus()).c_str());
                    // Roll back the egress table created just above (no rule could be
                    // installed in it) along with the ingress drop rule.
                    gAclOrch->removeAclTable(m_strEgressTable);
                    m_aclTables.erase(m_strEgressTable);
                    removeIngressBinding(port);
                    m_rolledBack = true;
                }
            }
            else
            {
                SWSS_LOG_ERROR("Failed to create egress PFCWD ACL table %s; rolling back ingress PFCWD drop rule %s",
                               m_strEgressTable.c_str(), m_strRule.c_str());
                removeIngressBinding(port);
                m_rolledBack = true;
            }
        }
        else
        {
            // Otherwise just bind ACL table with the port
            found->second.bind(port);
        }
    }
}

void PfcWdAclHandler::removeIngressBinding(sai_object_id_t port)
{
    SWSS_LOG_ENTER();

    // The rule may be absent if creation was skipped (egress PMF full) - clean up
    // gracefully; this must not throw when called from the destructor.
    AclRule* rule = gAclOrch->getAclRule(m_strIngressTable, m_strRule);
    if (rule == nullptr)
    {
        SWSS_LOG_NOTICE("ACL Rule %s does not exist for table %s", m_strRule.c_str(), m_strIngressTable.c_str());
        return;
    }

    vector<sai_object_id_t> port_set = rule->getInPorts();
    if ((port_set.size() == 1) && (port_set[0] == port))
    {
        gAclOrch->removeAclRule(m_strIngressTable, m_strRule);
    }
    else
    {
        gAclOrch->updateAclRule(m_strIngressTable, m_strRule, MATCH_IN_PORTS, &port, RULE_OPER_DELETE);
    }
}

PfcWdAclHandler::~PfcWdAclHandler(void)
{
    SWSS_LOG_ENTER();

    sai_object_id_t port = getPort();

    if (!m_rolledBack)
    {
        removeIngressBinding(port);
    }

    if (shared_egress_acl_table && !m_strEgressRule.empty())
    {
        AclRule* rule = gAclOrch->getAclRule(m_strEgressTable, m_strEgressRule);
        if (rule == nullptr)
        {
            SWSS_LOG_NOTICE("Egress ACL Rule %s does not exist for table %s", m_strEgressRule.c_str(), m_strEgressTable.c_str());
        }
        else if (!deferEgressRuleRemoval(gAclOrch->getAclRule(m_strEgressTable, m_strEgressRule)))
        {
            gAclOrch->removeAclRule(m_strEgressTable, m_strEgressRule);
        }
    }
    else
    {
        auto found = m_aclTables.find(m_strEgressTable);
        if (found != m_aclTables.end())
        {
            found->second.unbind(port);
        }
    }
}

void PfcWdAclHandler::addFinalCounters(PfcWdHwStats& counters)
{
    SWSS_LOG_ENTER();

    if (!shared_egress_acl_table)
    {
        return;
    }

    // An egress ACL drop never reaches the queue's drop counter, so add the
    // drops counted by this handler's own egress rule. If the read fails, the
    // deferred final count credits them.
    AclRule *rule = gAclOrch->getAclRule(m_strEgressTable, m_strEgressRule);
    uint64_t drops = 0;
    if (rule == nullptr || !rule->hasCounter() || !readRuleDrops(rule, drops))
    {
        return;
    }

    counters.txDropPkt += drops;
    m_egressDropsCounted = drops;
}

bool PfcWdAclHandler::readRuleDrops(AclRule *rule, uint64_t &drops)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = SAI_ACL_COUNTER_ATTR_PACKETS;

    sai_status_t status = sai_acl_api->get_acl_counter_attribute(rule->getCounterOid(), 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to fetch ACL rule %s counter: %d", rule->getId().c_str(), status);
        return false;
    }

    drops = attr.value.u64;

    return true;
}

bool PfcWdAclHandler::deferEgressRuleRemoval(AclRule *rule)
{
    SWSS_LOG_ENTER();

    // The SDK's counter cache can lag the hardware by several seconds while the
    // port receives PFC, so the count read at restore may miss the storm's last
    // drops. Stop the rule dropping now and take its final count once the cache
    // has caught up.
    static const auto readDelay = std::chrono::seconds(5);

    if (!rule->hasCounter())
    {
        return false;
    }

    sai_attribute_t attr;
    attr.id = SAI_ACL_ENTRY_ATTR_ADMIN_STATE;
    attr.value.booldata = false;

    sai_status_t status = sai_acl_api->set_acl_entry_attribute(rule->getOid(), &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to disable egress ACL rule %s: %d", m_strEgressRule.c_str(), status);
        return false;
    }

    m_deferredEgressRules[m_strEgressRule] = { m_strEgressTable, sai_serialize_object_id(getQueue()),
                                               getCountersTable(), m_egressDropsCounted,
                                               std::chrono::steady_clock::now() + readDelay };

    return true;
}

void PfcWdAclHandler::finishDeferredEgressRule(const string &ruleName, bool retry /* = false */)
{
    SWSS_LOG_ENTER();

    static const uint32_t maxAttempts = 3;
    static const auto retryDelay = std::chrono::seconds(1);

    auto it = m_deferredEgressRules.find(ruleName);
    if (it == m_deferredEgressRules.end())
    {
        return;
    }

    auto &deferred = it->second;
    AclRule *rule = gAclOrch->getAclRule(deferred.table, ruleName);
    if (rule == nullptr)
    {
        m_deferredEgressRules.erase(it);
        return;
    }

    uint64_t drops = 0;
    bool read = readRuleDrops(rule, drops);
    if (read && drops > deferred.counted)
    {
        // Credit the drops the restore-time read missed to the storm just restored.
        auto stats = PfcWdBaseOrch::getQueueStats(deferred.countersTable, deferred.queueIdStr);
        stats.txDropPkt += drops - deferred.counted;
        stats.txDropPktLast += drops - deferred.counted;
        PfcWdBaseOrch::updateQueueStats(deferred.countersTable, deferred.queueIdStr, stats);
        deferred.counted = drops;
    }

    // A failed read or removal is tried again on a later tick, a few times.
    // Drops credited above are not credited again.
    bool removed = read && gAclOrch->removeAclRule(deferred.table, ruleName);
    if (!removed && retry && ++deferred.attempts < maxAttempts)
    {
        deferred.due = std::chrono::steady_clock::now() + retryDelay;
        return;
    }

    if (!read)
    {
        removed = gAclOrch->removeAclRule(deferred.table, ruleName);
    }
    if (!removed)
    {
        SWSS_LOG_ERROR("Failed to remove egress PFCWD drop rule %s", ruleName.c_str());
    }

    m_deferredEgressRules.erase(it);
}

void PfcWdAclHandler::finishDeferredForQueue(sai_object_id_t queue)
{
    SWSS_LOG_ENTER();

    const string queueIdStr = sai_serialize_object_id(queue);
    for (auto it = m_deferredEgressRules.begin(); it != m_deferredEgressRules.end(); )
    {
        auto cur = it++;
        if (cur->second.queueIdStr == queueIdStr)
        {
            const string ruleName = cur->first;
            finishDeferredEgressRule(ruleName);
        }
    }
}

void PfcWdAclHandler::finishDeferred(void)
{
    SWSS_LOG_ENTER();

    // The time to remove an egress rule grows with how full the table is, so
    // bound the time spent per timer tick rather than the number of rules.
    // Oldest first, at least one per call.
    static const auto timeBudget = std::chrono::milliseconds(200);
    static const size_t maxPerCall = 128;

    const auto start = std::chrono::steady_clock::now();
    vector<pair<std::chrono::steady_clock::time_point, string>> due;

    for (const auto &it : m_deferredEgressRules)
    {
        if (it.second.due <= start)
        {
            due.emplace_back(it.second.due, it.first);
        }
    }

    sort(due.begin(), due.end());

    size_t finished = 0;
    for (const auto &it : due)
    {
        if (finished == maxPerCall ||
            (finished > 0 && std::chrono::steady_clock::now() - start >= timeBudget))
        {
            break;
        }
        finishDeferredEgressRule(it.second, true);
        finished++;
    }
}

void PfcWdAclHandler::clear()
{
    SWSS_LOG_ENTER();

<<<<<<< HEAD
    for (auto& tablepair: m_aclTables)
=======
    // Their tables are going away, so take the final counts now.
    while (!m_deferredEgressRules.empty())
    {
        finishDeferredEgressRule(m_deferredEgressRules.begin()->first);
    }

    // The tables are going away, so any pending IN_PORTS change is moot.
    m_desiredInPorts.clear();
    m_dirtyQueueIds.clear();
    m_failedFlushes.clear();

    for (auto it = m_aclTables.begin(); it != m_aclTables.end(); )
>>>>>>> cb949ebc (NOS-20722: [pfcwd] count egress ACL drops in the Broadcom software watchdog's TX drop stats (#1627))
    {
        auto& table = tablepair.second;
        gAclOrch->removeAclTable(table.getId());
    }
}

bool PfcWdAclHandler::createPfcAclTable(sai_object_id_t port, string strTable, bool ingress)
{
    SWSS_LOG_ENTER();

    auto inserted = m_aclTables.emplace(piecewise_construct,
        std::forward_as_tuple(strTable),
        std::forward_as_tuple(gAclOrch, strTable));

    assert(inserted.second);

    AclTable& aclTable = inserted.first->second;

    sai_object_id_t table_oid = gAclOrch->getTableById(strTable);
    if (ingress && table_oid != SAI_NULL_OBJECT_ID)
    {
        // DROP ACL table is already created
        SWSS_LOG_NOTICE("ACL table %s exists, reuse the same", strTable.c_str());
        aclTable = *(gAclOrch->getTableByOid(table_oid));
        return true;
    }

    // Link port only for ingress ACL table or unshared egress ACL table.
    if (ingress || !shared_egress_acl_table)
    {
        aclTable.link(port);
    }

    if (ingress) 
    {
        auto dropType = gAclOrch->getAclTableType(TABLE_TYPE_DROP);
        assert(dropType);
        aclTable.validateAddType(*dropType);
        aclTable.stage = ACL_STAGE_INGRESS;
    } 
    else 
    {
        auto pfcwdType = gAclOrch->getAclTableType(TABLE_TYPE_PFCWD);
        assert(pfcwdType);
        aclTable.validateAddType(*pfcwdType);
        aclTable.stage = ACL_STAGE_EGRESS;
    }

    if (!gAclOrch->addAclTable(aclTable))
    {
        // ACL table create failed (e.g. egress PMF full). Drop the half-built
        // entry so the caller skips rule creation instead of throwing later.
        SWSS_LOG_ERROR("Failed to create PFCWD ACL table %s", strTable.c_str());
        m_aclTables.erase(strTable);
        return false;
    }

    return true;
}

bool PfcWdAclHandler::createPfcAclRule(shared_ptr<AclRulePacket> rule, uint8_t queueId, string strTable, sai_object_id_t portOid)
{
    SWSS_LOG_ENTER();

    string attr_name, attr_value;

    attr_name = RULE_PRIORITY;
    attr_value = "999";
    rule->validateAddPriority(attr_name, attr_value);

    attr_name = MATCH_TC;
    attr_value = to_string(queueId);
    rule->validateAddMatch(attr_name, attr_value);

    // Add MATCH_IN_PORTS as match criteria for ingress table and MATCH_OUT_PORT as match creiteria for shared egress table.
    if (strTable == INGRESS_TABLE_DROP || shared_egress_acl_table)
    {
        Port p;
        if (strTable == INGRESS_TABLE_DROP) 
        {
            attr_name = MATCH_IN_PORTS;
        }
        else if (shared_egress_acl_table) {
            attr_name = MATCH_OUT_PORT;
        }
    
        if (!gPortsOrch->getPort(portOid, p))
        {
            SWSS_LOG_ERROR("Failed to get port structure from port oid 0x%" PRIx64, portOid);
            return false;
        }

        attr_value = p.m_alias;
        rule->validateAddMatch(attr_name, attr_value);
    }

    attr_name = ACTION_PACKET_ACTION;
    attr_value = PACKET_ACTION_DROP;
    rule->validateAddAction(attr_name, attr_value);

    return gAclOrch->addAclRule(rule, strTable);
}

std::map<std::string, AclTable> PfcWdAclHandler::m_aclTables;
<<<<<<< HEAD
=======
std::set<std::string> PfcWdAclHandler::m_ownedAclTables;
std::map<uint8_t, std::map<sai_object_id_t, sai_object_id_t>> PfcWdAclHandler::m_desiredInPorts;
std::set<uint8_t> PfcWdAclHandler::m_dirtyQueueIds;
std::map<uint8_t, uint32_t> PfcWdAclHandler::m_failedFlushes;
std::map<std::string, PfcWdAclHandler::DeferredEgressRule> PfcWdAclHandler::m_deferredEgressRules;
>>>>>>> cb949ebc (NOS-20722: [pfcwd] count egress ACL drops in the Broadcom software watchdog's TX drop stats (#1627))

PfcWdLossyHandler::PfcWdLossyHandler(sai_object_id_t port, sai_object_id_t queue,
        uint8_t queueId, shared_ptr<Table> countersTable):
    PfcWdActionHandler(port, queue, queueId, countersTable)
{
    SWSS_LOG_ENTER();

    uint8_t pfcMask = 0;

    if (!gPortsOrch->getPortPfc(port, &pfcMask))
    {
        SWSS_LOG_ERROR("Failed to get PFC mask on port 0x%" PRIx64, port);
    }

    pfcMask = static_cast<uint8_t>(pfcMask & ~(1 << queueId));

    if (!gPortsOrch->setPortPfc(port, pfcMask))
    {
        SWSS_LOG_ERROR("Failed to set PFC mask on port 0x%" PRIx64, port);
    }
}

PfcWdLossyHandler::~PfcWdLossyHandler(void)
{
    SWSS_LOG_ENTER();

    uint8_t pfcMask = 0;

    if (!gPortsOrch->getPortPfc(getPort(), &pfcMask))
    {
        SWSS_LOG_ERROR("Failed to get PFC mask on port 0x%" PRIx64, getPort());
    }

    pfcMask = static_cast<uint8_t>(pfcMask | (1 << getQueueId()));

    if (!gPortsOrch->setPortPfc(getPort(), pfcMask))
    {
        SWSS_LOG_ERROR("Failed to set PFC mask on port 0x%" PRIx64, getPort());
    }
}

bool PfcWdActionHandler::getHwCounters(PfcWdHwStats& counters)
{
    SWSS_LOG_ENTER();

    static const vector<sai_stat_id_t> queueStatIds =
    {
        SAI_QUEUE_STAT_PACKETS,
        SAI_QUEUE_STAT_DROPPED_PACKETS,
    };

    static const vector<sai_stat_id_t> pgStatIds =
    {
        SAI_INGRESS_PRIORITY_GROUP_STAT_PACKETS,
        SAI_INGRESS_PRIORITY_GROUP_STAT_DROPPED_PACKETS,
    };

    vector<uint64_t> queueStats;
    queueStats.resize(queueStatIds.size());

    sai_status_t status = sai_queue_api->get_queue_stats(
            getQueue(),
            static_cast<uint32_t>(queueStatIds.size()),
            queueStatIds.data(),
            queueStats.data());

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to fetch queue 0x%" PRIx64 " stats: %d", getQueue(), status);
        return false;
    }

    // PG counters not yet supported in Mellanox platform
    Port portInstance;
    if (!gPortsOrch->getPort(getPort(), portInstance))
    {
        SWSS_LOG_ERROR("Cannot get port by ID 0x%" PRIx64, getPort());
        return false;
    }

    sai_object_id_t pg = portInstance.m_priority_group_ids[static_cast <size_t> (getQueueId())];
    vector<uint64_t> pgStats;
    pgStats.resize(pgStatIds.size());

    status = sai_buffer_api->get_ingress_priority_group_stats(
            pg,
            static_cast<uint32_t>(pgStatIds.size()),
            pgStatIds.data(),
            pgStats.data());

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to fetch pg 0x%" PRIx64 " stats: %d", pg, status);
        return false;
    }

    counters.txPkt = queueStats[0];
    counters.txDropPkt = queueStats[1];
    counters.rxPkt = pgStats[0];
    counters.rxDropPkt = pgStats[1];

    return true;
}

PfcWdZeroBufferHandler::PfcWdZeroBufferHandler(sai_object_id_t port,
        sai_object_id_t queue, uint8_t queueId, shared_ptr<Table> countersTable):
    PfcWdLossyHandler(port, queue, queueId, countersTable)
{
    SWSS_LOG_ENTER();

    Port portInstance;
    if (!gPortsOrch->getPort(port, portInstance))
    {
        SWSS_LOG_ERROR("Cannot get port by ID 0x%" PRIx64, port);
        return;
    }

    setQueueLockFlag(portInstance, true);

    sai_attribute_t attr;
    attr.id = SAI_QUEUE_ATTR_BUFFER_PROFILE_ID;

    // Get queue's buffer profile ID
    sai_status_t status = sai_queue_api->get_queue_attribute(queue, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get buffer profile ID on queue 0x%" PRIx64 ": %d", queue, status);
        return;
    }

    sai_object_id_t oldQueueProfileId = attr.value.oid;

    attr.id = SAI_QUEUE_ATTR_BUFFER_PROFILE_ID;
    attr.value.oid = ZeroBufferProfile::getZeroBufferProfile();

    // Set our zero buffer profile
    status = sai_queue_api->set_queue_attribute(queue, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set buffer profile ID on queue 0x%" PRIx64 ": %d", queue, status);
        return;
    }

    // Save original buffer profile
    m_originalQueueBufferProfile = oldQueueProfileId;
}

PfcWdZeroBufferHandler::~PfcWdZeroBufferHandler(void)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = SAI_QUEUE_ATTR_BUFFER_PROFILE_ID;
    attr.value.oid = m_originalQueueBufferProfile;

    // Set our zero buffer profile on a queue
    sai_status_t status = sai_queue_api->set_queue_attribute(getQueue(), &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set buffer profile ID on queue 0x%" PRIx64 ": %d", getQueue(), status);
        return;
    }

    Port portInstance;
    if (!gPortsOrch->getPort(getPort(), portInstance))
    {
        SWSS_LOG_ERROR("Cannot get port by ID 0x%" PRIx64, getPort());
        return;
    }

    setQueueLockFlag(portInstance, false);
}

void PfcWdZeroBufferHandler::setQueueLockFlag(Port& port, bool isLocked) const
{
    // set lock bits on queue
    for (size_t i = 0; i < port.m_queue_ids.size(); ++i)
    {
        if (port.m_queue_ids[i] == getQueue())
        {
            port.m_queue_lock[i] = isLocked;
        }
    }
    gPortsOrch->setPort(port.m_alias, port);
}

PfcWdZeroBufferHandler::ZeroBufferProfile::ZeroBufferProfile(void)
{
    SWSS_LOG_ENTER();
}

PfcWdZeroBufferHandler::ZeroBufferProfile::~ZeroBufferProfile(void)
{
    SWSS_LOG_ENTER();

    // Destroy egress profiles and pools
    destroyZeroBufferProfile();
}

PfcWdZeroBufferHandler::ZeroBufferProfile &PfcWdZeroBufferHandler::ZeroBufferProfile::getInstance(void)
{
    SWSS_LOG_ENTER();

    static ZeroBufferProfile instance;

    return instance;
}

sai_object_id_t PfcWdZeroBufferHandler::ZeroBufferProfile::getZeroBufferProfile()
{
    SWSS_LOG_ENTER();

    if (getInstance().getProfile() == SAI_NULL_OBJECT_ID)
    {
        getInstance().createZeroBufferProfile();
    }

    return getInstance().getProfile();
}

void PfcWdZeroBufferHandler::ZeroBufferProfile::createZeroBufferProfile()
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    vector<sai_attribute_t> attribs;
    sai_status_t status;

    // Create zero pool
    attr.id = SAI_BUFFER_POOL_ATTR_SIZE;
    attr.value.u64 = 0;
    attribs.push_back(attr);

    attr.id = SAI_BUFFER_POOL_ATTR_TYPE;
    attr.value.u32 = SAI_BUFFER_POOL_TYPE_EGRESS;
    attribs.push_back(attr);

    attr.id = SAI_BUFFER_POOL_ATTR_THRESHOLD_MODE;
    attr.value.u32 = SAI_BUFFER_POOL_THRESHOLD_MODE_DYNAMIC;
    attribs.push_back(attr);

    status = sai_buffer_api->create_buffer_pool(
        &getPool(),
        gSwitchId,
        static_cast<uint32_t>(attribs.size()),
        attribs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create dynamic zero buffer pool for PFC WD: %d", status);
        return;
    }

    // Create zero profile
    attribs.clear();

    attr.id = SAI_BUFFER_PROFILE_ATTR_POOL_ID;
    attr.value.oid = getPool();
    attribs.push_back(attr);

    attr.id = SAI_BUFFER_PROFILE_ATTR_THRESHOLD_MODE;
    attr.value.u32 = SAI_BUFFER_PROFILE_THRESHOLD_MODE_DYNAMIC;
    attribs.push_back(attr);

    attr.id = SAI_BUFFER_PROFILE_ATTR_BUFFER_SIZE;
    attr.value.u64 = 0;
    attribs.push_back(attr);

    attr.id = SAI_BUFFER_PROFILE_ATTR_SHARED_DYNAMIC_TH;
    attr.value.s8 = -8;
    attribs.push_back(attr);

    status = sai_buffer_api->create_buffer_profile(
            &getProfile(),
            gSwitchId,
            static_cast<uint32_t>(attribs.size()),
            attribs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create dynamic zero buffer profile for PFC WD: %d", status);
        return;
    }
}

void PfcWdZeroBufferHandler::ZeroBufferProfile::destroyZeroBufferProfile()
{
    SWSS_LOG_ENTER();

    sai_status_t status = sai_buffer_api->remove_buffer_profile(getProfile());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove static zero buffer profile for PFC WD: %d", status);
        return;
    }

    status = sai_buffer_api->remove_buffer_pool(getPool());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove static zero buffer pool for PFC WD: %d", status);
    }
}
