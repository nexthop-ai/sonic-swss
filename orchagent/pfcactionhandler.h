#ifndef PFC_ACTION_HANDLER_H
#define PFC_ACTION_HANDLER_H

#include <vector>
#include <memory>
<<<<<<< HEAD
=======
#include <set>
#include <chrono>
#include <map>
>>>>>>> cb949ebc (NOS-20722: [pfcwd] count egress ACL drops in the Broadcom software watchdog's TX drop stats (#1627))
#include "aclorch.h"
#include "table.h"

extern "C" {
#include "sai.h"
}

using namespace std;
using namespace swss;

struct PfcWdHwStats
{
    uint64_t txPkt;
    uint64_t txDropPkt;
    uint64_t rxPkt;
    uint64_t rxDropPkt;
};

// PFC queue interface class
// It resembles RAII behavior - pause storm is mitigated (queue is locked) on creation,
// and is restored (queue released) on removal
class PfcWdActionHandler
{
    public:
        PfcWdActionHandler(sai_object_id_t port, sai_object_id_t queue,
                uint8_t queueId, shared_ptr<Table> countersTable);
        virtual ~PfcWdActionHandler(void);

        inline sai_object_id_t getPort(void) const
        {
            return m_port;
        }

        inline sai_object_id_t getQueue(void) const
        {
            return m_queue;
        }

        inline uint8_t getQueueId(void) const
        {
            return m_queueId;
        }

        virtual bool isValid(void) const { return true; }

<<<<<<< HEAD
        static void initWdCounters(shared_ptr<Table> countersTable, const string &queueIdStr);
        void initCounters(void);
        void commitCounters(bool periodic = false);
        bool getHwCounters(PfcWdHwStats& counters);
=======
        // Called by PfcWdSwOrch when a handler that deferred its programming
        // turns out to have failed once the batch was flushed. Only handlers
        // that defer work override this.
        virtual void markInvalid(void) { }

        // Called once after a whole batch of storm/restore events has been
        // handled, so handlers that defer switch programming can apply it.
        // Returns the OIDs of the queues whose programming failed. The default
        // is a no-op for handlers that program nothing shared.
        static std::set<sai_object_id_t> flush(void) { return std::set<sai_object_id_t>(); }

        // Overwrites the RX fields with a SAI read, for the storm-start and restore
        // samples: the PG entry in COUNTERS_DB is the PG_DROP group's copy, refreshed
        // every 10 s. False when nothing was read.
        virtual bool readRxFromSai(PfcWdHwStats&)
        {
            return false;
        }

        // Same for the TX drop field: no software watchdog polls queue drops in
        // the PFC_WD group, so the COUNTERS_DB copy is the QUEUE group's,
        // refreshed every 10 s, or absent when queue counters are not polled.
        virtual bool readTxDropFromSai(PfcWdHwStats&)
        {
            return false;
        }

        // Called from the watchdog's periodic timer, so handlers that left work
        // behind when they were destroyed can finish it. No-op by default.
        static void finishDeferred(void) { }

        // Called when the watchdog stops on a queue, so work left for it is
        // finished at once. No-op by default.
        static void finishDeferredForQueue(sai_object_id_t queue) { }

        static void initWdCounters(shared_ptr<Table> countersTable, const string &queueIdStr);
        void initCounters(void);
        void commitCounters(bool periodic = false);

        // queueFvs is this queue's COUNTERS_DB entry, already fetched by the
        // caller. It is passed in rather than read here so that one pass over
        // the counters reads the queue entry once instead of once per consumer.
        virtual bool getHwCounters(PfcWdHwStats& counters,
                                   const std::vector<swss::FieldValueTuple>& /* queueFvs */)
        {
            memset(&counters, 0, sizeof(PfcWdHwStats));

            return true;
        };

        // Adds counts read only when the storm's counters are committed for
        // the last time, not on the periodic tick. No-op by default.
        virtual void addFinalCounters(PfcWdHwStats& counters) { }

    protected:
        // Whether the last getHwCounters() sample carried the TX drop field and
        // both RX fields. Handlers that report a fixed sample leave them true.
        bool m_sampleHasTxDrop = true;
        bool m_sampleHasRx = true;
>>>>>>> cb949ebc (NOS-20722: [pfcwd] count egress ACL drops in the Broadcom software watchdog's TX drop stats (#1627))

    private:
        struct PfcWdQueueStats
        {
            uint64_t detectCount;
            uint64_t restoreCount;
            uint64_t txPkt;
            uint64_t txDropPkt;
            uint64_t rxPkt;
            uint64_t rxDropPkt;
            uint64_t txPktLast;
            uint64_t txDropPktLast;
            uint64_t rxPktLast;
            uint64_t rxDropPktLast;
            bool     operational;
        };

        static PfcWdQueueStats getQueueStats(shared_ptr<Table> countersTable, const string &queueIdStr);
        void updateWdCounters(const string& queueIdStr, const PfcWdQueueStats& stats);

        sai_object_id_t m_port = SAI_NULL_OBJECT_ID;
        sai_object_id_t m_queue = SAI_NULL_OBJECT_ID;
        uint8_t m_queueId = 0;
        string m_portAlias;
        shared_ptr<Table> m_countersTable = nullptr;
        PfcWdHwStats m_hwStats;
};

// Pfc queue that implements forward action by disabling PFC on queue
class PfcWdLossyHandler: public PfcWdActionHandler
{
    public:
        PfcWdLossyHandler(sai_object_id_t port, sai_object_id_t queue,
                uint8_t queueId, shared_ptr<Table> countersTable);
        virtual ~PfcWdLossyHandler(void);
};

class PfcWdAclHandler: public PfcWdLossyHandler
{
    public:
        PfcWdAclHandler(sai_object_id_t port, sai_object_id_t queue,
                uint8_t queueId, shared_ptr<Table> countersTable);
        virtual ~PfcWdAclHandler(void);

        // class shared cleanup
        static void clear();

        bool isValid(void) const override { return !m_rolledBack; }
<<<<<<< HEAD
=======
        void markInvalid(void) override;
        void addFinalCounters(PfcWdHwStats& counters) override;

        // Bring each changed queue index's ingress drop rule in line with the
        // ports its live handlers mitigate: one SAI call per queue index,
        // instead of one per (port, queue). A failed write leaves the rule as
        // it was. Only removals are retried by the next flush: a port that
        // could not be added is dropped from the desired ports and not tried
        // again, so its queue keeps only the egress drop until the storm ends,
        // as before batching. Returns the OIDs of the queues whose port could
        // not be added, so the caller can mark those handlers invalid.
        static std::set<sai_object_id_t> flush(void);

        // Takes the final drop count of restored queues' egress rules once it
        // is due, then removes the rules.
        static void finishDeferred(void);

        // Takes the final drop count of the queue's egress rule now and removes
        // the rule, so nothing writes to the queue's counters afterwards.
        static void finishDeferredForQueue(sai_object_id_t queue);

>>>>>>> cb949ebc (NOS-20722: [pfcwd] count egress ACL drops in the Broadcom software watchdog's TX drop stats (#1627))
    private:
        // A restored queue's egress rule, disabled but kept until the counter
        // cache has caught up with its last drops.
        struct DeferredEgressRule
        {
            string table;
            string queueIdStr;
            shared_ptr<Table> countersTable;
            uint64_t counted;
            std::chrono::steady_clock::time_point due;
            uint32_t attempts = 0;
        };

        // class shared dict: egress rule name -> rule waiting for its final count
        static std::map<std::string, DeferredEgressRule> m_deferredEgressRules;

        // class shared dict: ACL table name -> ACL table
        static std::map<std::string, AclTable> m_aclTables;

        bool shared_egress_acl_table = false;

        bool m_rolledBack = false;

        string m_strIngressTable;
        string m_strEgressTable;
        string m_strRule;
        string m_strEgressRule;
<<<<<<< HEAD
        bool createPfcAclTable(sai_object_id_t port, string strTable, bool ingress);
        bool createPfcAclRule(shared_ptr<AclRulePacket> rule, uint8_t queueId, string strTable, sai_object_id_t port);
        void updatePfcAclRule(shared_ptr<AclRule> rule, uint8_t queueId, string strTable, vector<sai_object_id_t> port);
        void removeIngressBinding(sai_object_id_t port);
=======

        // Egress rule drops already included in this storm's stats.
        uint64_t m_egressDropsCounted = 0;

        static bool flushQueueId(uint8_t queueId, std::set<sai_object_id_t> &failedQueues);
        static bool readRuleDrops(AclRule *rule, uint64_t &drops);
        bool deferEgressRuleRemoval(AclRule *rule);
        static void finishDeferredEgressRule(const string &ruleName, bool retry = false);
        static bool useSharedEgressAclTable(void);
        static string ingressRuleName(uint8_t queueId);
        static string egressTableName(uint8_t queueId);
        static bool ensureAclTable(const string &strTable, bool ingress);
        static bool createPfcAclTable(string strTable, bool ingress);
        static bool createPfcAclRule(shared_ptr<AclRulePacket> rule, uint8_t queueId,
                const string &strTable, const vector<sai_object_id_t> &ports);
>>>>>>> cb949ebc (NOS-20722: [pfcwd] count egress ACL drops in the Broadcom software watchdog's TX drop stats (#1627))
};

class PfcWdDlrHandler: public PfcWdActionHandler
{
    public:
        PfcWdDlrHandler(sai_object_id_t port, sai_object_id_t queue,
                uint8_t queueId, shared_ptr<Table> countersTable);
        virtual ~PfcWdDlrHandler(void);
};

// PFC queue that implements drop action by draining queue with buffer of zero size
class PfcWdZeroBufferHandler: public PfcWdLossyHandler
{
    public:
        PfcWdZeroBufferHandler(sai_object_id_t port, sai_object_id_t queue,
                uint8_t queueId, shared_ptr<Table> countersTable);
        virtual ~PfcWdZeroBufferHandler(void);

    private:
        /*
         * Sets lock bits on port's queue
         * to protect it from being changed by other Orch's
        */
        void setQueueLockFlag(Port& port, bool isLocked) const;

        // Singletone class for keeping shared data - zero buffer profiles
        class ZeroBufferProfile
        {
            public:
                ~ZeroBufferProfile(void);
                static sai_object_id_t getZeroBufferProfile();

            private:
                ZeroBufferProfile(void);
                static ZeroBufferProfile &getInstance(void);
                void createZeroBufferProfile();
                void destroyZeroBufferProfile();

                sai_object_id_t& getProfile()
                {
                    return m_zeroEgressBufferProfile;
                }

                sai_object_id_t& getPool()
                {
                    return m_zeroEgressBufferPool;
                }

                sai_object_id_t m_zeroEgressBufferPool = SAI_NULL_OBJECT_ID;
                sai_object_id_t m_zeroEgressBufferProfile = SAI_NULL_OBJECT_ID;
        };

        sai_object_id_t m_originalQueueBufferProfile = SAI_NULL_OBJECT_ID;
};

#endif
