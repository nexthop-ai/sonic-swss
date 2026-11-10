#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_orch_test.h"
#include "mock_table.h"

#define private public
#include "pfcwdhworch.h"
#undef private

#include <gtest/gtest.h>

namespace pfcwdhworch_test
{
    using namespace std;
    using namespace mock_orch_test;

	    class PfcWdHwCountersTest : public MockOrchTest
	    {
	    protected:
	        shared_ptr<swss::DBConnector> m_counters_db;
	        shared_ptr<swss::Table> m_counters_table;
	        PfcWdHwOrch *m_pfcwd_hw_orch;
	        
	        void SetUp() override
	        {
	            MockOrchTest::SetUp();

	            // Bring up ports so gPortsOrch can resolve Ethernet0 and its queues/PGs.
	            bringUpPorts();
	            
	            // Initialize COUNTERS_DB and table
	            m_counters_db = make_shared<swss::DBConnector>("COUNTERS_DB", 0);
	            m_counters_table = make_shared<swss::Table>(m_counters_db.get(), COUNTERS_TABLE);
	            
	            vector<string> pfc_wd_tables = { CFG_PFC_WD_TABLE_NAME };
	            
	            static const vector<sai_port_stat_t> portStatIds = {};
	            
	            static const vector<sai_queue_stat_t> queueStatIds = {
	                SAI_QUEUE_STAT_PACKETS,
	                SAI_QUEUE_STAT_DROPPED_PACKETS
	            };
	            
	            static const vector<sai_queue_attr_t> queueAttrIds = {};
	            
	            m_pfcwd_hw_orch = new PfcWdHwOrch(
	                m_config_db.get(), 
	                pfc_wd_tables, 
	                portStatIds, 
	                queueStatIds, 
	                queueAttrIds);
	        }
	        
	        void TearDown() override
	        {
	            delete m_pfcwd_hw_orch;
	            MockOrchTest::TearDown();
	        }
	        
	        // Populate APP_PORT_TABLE and run PortsOrch so ports are ready.
	        void bringUpPorts()
	        {
	            swss::Table portTable(m_app_db.get(), APP_PORT_TABLE_NAME);
	            auto ports = ut_helper::getInitialSaiPorts();

	            for (const auto &it : ports)
	            {
	                portTable.set(it.first, it.second);
	            }

	            portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });
	            portTable.set("PortInitDone",   { { "lanes", "0" } });

	            gPortsOrch->addExistingData(&portTable);
	            static_cast<Orch *>(gPortsOrch)->doTask();
	            static_cast<Orch *>(gPortsOrch)->doTask();
	            ASSERT_TRUE(gPortsOrch->allPortsReady());
	        }
	        
	        // Helper: Set hardware counter values in COUNTERS_DB for a queue
        void setQueueHwCounters(sai_object_id_t queueId, uint64_t txPkt, uint64_t txDropPkt)
        {
            string queueIdStr = sai_serialize_object_id(queueId);
            vector<FieldValueTuple> fvs = {
                {"SAI_QUEUE_STAT_PACKETS", to_string(txPkt)},
                {"SAI_QUEUE_STAT_DROPPED_PACKETS", to_string(txDropPkt)}
            };
            m_counters_table->set(queueIdStr, fvs);
        }
        
        // Helper: Set hardware counter values in COUNTERS_DB for a priority group
        void setPgHwCounters(sai_object_id_t pgId, uint64_t rxPkt, uint64_t rxDropPkt)
        {
            string pgIdStr = sai_serialize_object_id(pgId);
            vector<FieldValueTuple> fvs = {
                {"SAI_INGRESS_PRIORITY_GROUP_STAT_PACKETS", to_string(rxPkt)},
                {"SAI_INGRESS_PRIORITY_GROUP_STAT_DROPPED_PACKETS", to_string(rxDropPkt)}
            };
            m_counters_table->set(pgIdStr, fvs);
        }
        
        // Helper: Get queue statistics from COUNTERS_DB via the orch method
        PfcWdHwOrch::PfcWdQueueStats getQueueStats(sai_object_id_t queueId)
        {
            string queueIdStr = sai_serialize_object_id(queueId);
            return m_pfcwd_hw_orch->getQueueStats(queueIdStr);
        }
        
        // Helper: Verify counter values match expectations
        void verifyQueueStats(sai_object_id_t queueId,
                             uint64_t expectedDetect, 
                             uint64_t expectedRestore,
                             uint64_t expectedTxPkt,
                             uint64_t expectedTxDropPkt,
                             uint64_t expectedRxPkt,
                             uint64_t expectedRxDropPkt,
                             bool expectedOperational)
        {
            auto stats = getQueueStats(queueId);
            EXPECT_EQ(stats.detectCount, expectedDetect) << "detectCount mismatch";
            EXPECT_EQ(stats.restoreCount, expectedRestore) << "restoreCount mismatch";
            EXPECT_EQ(stats.txPkt, expectedTxPkt) << "txPkt mismatch";
            EXPECT_EQ(stats.txDropPkt, expectedTxDropPkt) << "txDropPkt mismatch";
            EXPECT_EQ(stats.rxPkt, expectedRxPkt) << "rxPkt mismatch";
            EXPECT_EQ(stats.rxDropPkt, expectedRxDropPkt) << "rxDropPkt mismatch";
            EXPECT_EQ(stats.operational, expectedOperational) << "operational status mismatch";
        }
        
        void resetQueueStats(sai_object_id_t queueId)
        {
            string queueIdStr = sai_serialize_object_id(queueId);

            // Delete the stats entry from COUNTERS_DB
            m_counters_table->del(queueIdStr);

            // Clear baseline so next initQueueCounters starts fresh
            m_pfcwd_hw_orch->m_queueBaselineStats.erase(queueId);
        }

        void setupQueuePortMapping(sai_object_id_t queueId, const Port& port, uint8_t queueIndex)
        {
            PfcWdHwOrch::PortQueueInfo info;
            info.port_id = port.m_port_id;
            info.port_alias = port.m_alias;
            info.queue_index = queueIndex;
            m_pfcwd_hw_orch->m_queueToPortMap[queueId] = info;
        }
        
        // Helper: Verify baseline counters are stored correctly
        void verifyBaseline(sai_object_id_t queueId, 
                           uint64_t expectedTxPkt,
                           uint64_t expectedTxDropPkt,
                           uint64_t expectedRxPkt,
                           uint64_t expectedRxDropPkt)
        {
            ASSERT_NE(m_pfcwd_hw_orch->m_queueBaselineStats.find(queueId),
                     m_pfcwd_hw_orch->m_queueBaselineStats.end()) 
                << "Baseline not found for queue";
            
            auto& baseline = m_pfcwd_hw_orch->m_queueBaselineStats[queueId];
            EXPECT_EQ(baseline.txPkt, expectedTxPkt) << "Baseline txPkt mismatch";
            EXPECT_EQ(baseline.txDropPkt, expectedTxDropPkt) << "Baseline txDropPkt mismatch";
            EXPECT_EQ(baseline.rxPkt, expectedRxPkt) << "Baseline rxPkt mismatch";
            EXPECT_EQ(baseline.rxDropPkt, expectedRxDropPkt) << "Baseline rxDropPkt mismatch";
        }
    };

    // ========================================================================
    // Test Cases: deferral of a port with no lossless TC
    // ========================================================================

    // Reaches the protected pending set the tests inspect
    struct BaseAccess : PfcWdBaseOrch
    {
        static bool pending(PfcWdBaseOrch &orch, const string &alias)
        {
            return (orch.*(&BaseAccess::m_pfcwdPendingPorts)).count(alias) != 0;
        }
    };

    sai_port_api_t deferral_sai_port_api;
    sai_port_api_t *deferral_old_sai_port_api;
    sai_queue_api_t deferral_sai_queue_api;
    sai_queue_api_t *deferral_old_sai_queue_api;
    sai_switch_api_t deferral_sai_switch_api;
    sai_switch_api_t *deferral_old_sai_switch_api;

    // Watchdog SAI calls made by orchagent, including teardown writes of empty lists
    uint32_t watchdog_sai_sets;

    sai_status_t _deferral_set_port_attribute(sai_object_id_t port_id, const sai_attribute_t *attr)
    {
        if (attr->id == SAI_PORT_ATTR_PFC_TC_DLD_INTERVAL || attr->id == SAI_PORT_ATTR_PFC_TC_DLR_INTERVAL)
        {
            watchdog_sai_sets++;
            return SAI_STATUS_SUCCESS;
        }
        return deferral_old_sai_port_api->set_port_attribute(port_id, attr);
    }

    sai_status_t _deferral_get_port_attribute(sai_object_id_t port_id, uint32_t attr_count, sai_attribute_t *attr_list)
    {
        if (attr_count == 1 &&
            (attr_list[0].id == SAI_PORT_ATTR_PFC_TC_DLD_INTERVAL || attr_list[0].id == SAI_PORT_ATTR_PFC_TC_DLR_INTERVAL))
        {
            for (uint32_t i = 0; i < attr_list[0].value.maplist.count; i++)
            {
                attr_list[0].value.maplist.list[i].value = 400;
            }
            return SAI_STATUS_SUCCESS;
        }
        return deferral_old_sai_port_api->get_port_attribute(port_id, attr_count, attr_list);
    }

    sai_status_t _deferral_set_queue_attribute(sai_object_id_t queue_id, const sai_attribute_t *attr)
    {
        if (attr->id == SAI_QUEUE_ATTR_ENABLE_PFC_DLDR)
        {
            watchdog_sai_sets++;
            return SAI_STATUS_SUCCESS;
        }
        return deferral_old_sai_queue_api->set_queue_attribute(queue_id, attr);
    }

    sai_status_t _deferral_set_switch_attribute(sai_object_id_t switch_id, const sai_attribute_t *attr)
    {
        if (attr->id == SAI_SWITCH_ATTR_PFC_DLR_PACKET_ACTION)
        {
            return SAI_STATUS_SUCCESS;
        }
        return deferral_old_sai_switch_api->set_switch_attribute(switch_id, attr);
    }

    class PfcWdHwDeferralTest : public PfcWdHwCountersTest
    {
    protected:
        Port m_port;
        shared_ptr<swss::DBConnector> m_state_db;

        const vector<FieldValueTuple> m_config = {
            { PFC_WD_ACTION, "drop" },
            { PFC_WD_DETECTION_TIME, "400" },
            { PFC_WD_RESTORATION_TIME, "400" },
        };

        void SetUp() override
        {
            PfcWdHwCountersTest::SetUp();

            deferral_sai_port_api = *sai_port_api;
            deferral_old_sai_port_api = sai_port_api;
            deferral_sai_port_api.set_port_attribute = _deferral_set_port_attribute;
            deferral_sai_port_api.get_port_attribute = _deferral_get_port_attribute;
            sai_port_api = &deferral_sai_port_api;

            deferral_sai_queue_api = *sai_queue_api;
            deferral_old_sai_queue_api = sai_queue_api;
            deferral_sai_queue_api.set_queue_attribute = _deferral_set_queue_attribute;
            sai_queue_api = &deferral_sai_queue_api;

            deferral_sai_switch_api = *sai_switch_api;
            deferral_old_sai_switch_api = sai_switch_api;
            deferral_sai_switch_api.set_switch_attribute = _deferral_set_switch_attribute;
            sai_switch_api = &deferral_sai_switch_api;

            m_state_db = make_shared<swss::DBConnector>("STATE_DB", 0);
            ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", m_port));
            setLosslessMask(0);
            watchdog_sai_sets = 0;
        }

        void TearDown() override
        {
            sai_port_api = deferral_old_sai_port_api;
            sai_queue_api = deferral_old_sai_queue_api;
            sai_switch_api = deferral_old_sai_switch_api;
            PfcWdHwCountersTest::TearDown();
        }

        void setLosslessMask(uint8_t mask)
        {
            // Only a queue with PFC enabled is watched, so set both masks
            ASSERT_TRUE(gPortsOrch->setPortPfc(m_port.m_port_id, mask));
            ASSERT_TRUE(gPortsOrch->setPortPfcWatchdogStatus(m_port.m_port_id, mask));
            ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", m_port));
        }

        bool hwStatus(string *value)
        {
            swss::Table hwState(m_state_db.get(), STATE_PFC_WD_HW_STATE_TABLE_NAME);
            return hwState.hget("Ethernet0", "status", *value);
        }
    };

    TEST_F(PfcWdHwDeferralTest, PortWithoutLosslessTcIsReportedOnceThenWaitsQuietly)
    {
        ASSERT_EQ(m_pfcwd_hw_orch->createEntry("Ethernet0", m_config), task_process_status::task_need_retry);

        string status;
        ASSERT_TRUE(hwStatus(&status));
        EXPECT_EQ(status, "failed");
        EXPECT_TRUE(BaseAccess::pending(*m_pfcwd_hw_orch, "Ethernet0"));

        // Retries while the port is still not PFC-ready touch neither the ASIC nor STATE_DB
        swss::Table hwState(m_state_db.get(), STATE_PFC_WD_HW_STATE_TABLE_NAME);
        hwState.del("Ethernet0");
        watchdog_sai_sets = 0;
        for (int i = 0; i < 5; i++)
        {
            ASSERT_EQ(m_pfcwd_hw_orch->createEntry("Ethernet0", m_config), task_process_status::task_need_retry);
        }
        EXPECT_EQ(watchdog_sai_sets, 0u);
        EXPECT_FALSE(hwStatus(&status));
    }

    TEST_F(PfcWdHwDeferralTest, PendingPortStartsOnceLosslessTcsAppear)
    {
        ASSERT_EQ(m_pfcwd_hw_orch->createEntry("Ethernet0", m_config), task_process_status::task_need_retry);

        setLosslessMask((1 << 3) | (1 << 4));
        ASSERT_EQ(m_pfcwd_hw_orch->createEntry("Ethernet0", m_config), task_process_status::task_success);

        string status;
        ASSERT_TRUE(hwStatus(&status));
        EXPECT_EQ(status, "configured");
        EXPECT_TRUE(m_pfcwd_hw_orch->m_hwWdPorts.count("Ethernet0"));
        EXPECT_FALSE(BaseAccess::pending(*m_pfcwd_hw_orch, "Ethernet0"));
        EXPECT_GT(watchdog_sai_sets, 0u);
    }

    // ========================================================================
    // Test Cases: initQueueCounters()
    // ========================================================================

    TEST_F(PfcWdHwCountersTest, InitQueueCounters_NewStorm)
    {
        // Get port from existing PortsOrch setup
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));

        uint8_t queueIndex = 3;
        sai_object_id_t queueId = port.m_queue_ids[queueIndex];
        string queueIdStr = sai_serialize_object_id(queueId);

        // Setup: Populate HW counters in COUNTERS_DB
        setQueueHwCounters(queueId, 1000, 50);

        // Setup: Add queue to port mapping
        setupQueuePortMapping(queueId, port, queueIndex);

        // Test: Call initQueueCounters (simulates storm detection)
        m_pfcwd_hw_orch->initQueueCounters(queueIdStr, queueId);

        // Verify: detectCount = 1, restoreCount = 0
        // Verify: operational = false (storm state)
        // Verify: *_LAST counters = 0 (reset on detection)
        // Verify: cumulative counters = 0 (new storm)
        verifyQueueStats(queueId,
                        1,      // detectCount
                        0,      // restoreCount
                        0,      // txPkt
                        0,      // txDropPkt
                        0,      // rxPkt
                        0,      // rxDropPkt
                        false); // operational

        // Verify: Baseline stored with current HW values
        verifyBaseline(queueId, 1000, 50, 0, 0);
    }

    TEST_F(PfcWdHwCountersTest, InitQueueCounters_SubsequentStorm)
    {
        // Setup: Simulate previous storm that was restored
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));

        uint8_t queueIndex = 3;
        sai_object_id_t queueId = port.m_queue_ids[queueIndex];
        string queueIdStr = sai_serialize_object_id(queueId);

        setupQueuePortMapping(queueId, port, queueIndex);

        // Pre-populate COUNTERS_DB with previous storm history
        PfcWdHwOrch::PfcWdQueueStats prevStats;
        memset(&prevStats, 0, sizeof(prevStats));
        prevStats.detectCount = 1;
        prevStats.restoreCount = 1;
        prevStats.txPkt = 500;
        prevStats.txDropPkt = 25;
        prevStats.rxPkt = 1000;
        prevStats.rxDropPkt = 50;
        prevStats.operational = true;
        m_pfcwd_hw_orch->updateQueueStats(queueIdStr, prevStats);

        // Setup: New HW counter values for second storm
        setQueueHwCounters(queueId, 5000, 200);

        // Test: Call initQueueCounters for second storm
        m_pfcwd_hw_orch->initQueueCounters(queueIdStr, queueId);

        auto stats = getQueueStats(queueId);
        EXPECT_EQ(stats.detectCount, 2);
        EXPECT_EQ(stats.restoreCount, 1);
        EXPECT_EQ(stats.operational, false);

        // Verify: *_LAST counters reset to 0 for new storm
        EXPECT_EQ(stats.txPktLast, 0);
        EXPECT_EQ(stats.txDropPktLast, 0);
        EXPECT_EQ(stats.rxPktLast, 0);
        EXPECT_EQ(stats.rxDropPktLast, 0);

        // Verify: Cumulative counters from previous storm preserved
        EXPECT_EQ(stats.txPkt, 500);
        EXPECT_EQ(stats.txDropPkt, 25);

        // Verify: New baseline set
        verifyBaseline(queueId, 5000, 200, 0, 0);
    }

    TEST_F(PfcWdHwCountersTest, InitQueueCounters_WarmRebootOngoingStorm)
    {
        // Test scenario: Storm was ongoing during warm reboot
        // detectCount > restoreCount, so we don't increment detectCount again
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));

        uint8_t queueIndex = 3;
        sai_object_id_t queueId = port.m_queue_ids[queueIndex];
        string queueIdStr = sai_serialize_object_id(queueId);

        setupQueuePortMapping(queueId, port, queueIndex);

        // Pre-populate: Storm detected before warm reboot
        PfcWdHwOrch::PfcWdQueueStats prevStats;
        memset(&prevStats, 0, sizeof(prevStats));
        prevStats.detectCount = 1;
        prevStats.restoreCount = 0;  // Storm still ongoing
        prevStats.operational = false;
        m_pfcwd_hw_orch->updateQueueStats(queueIdStr, prevStats);

        // Setup: HW counters
        setQueueHwCounters(queueId, 3000, 100);

        // Test: Call initQueueCounters (warm reboot recovery)
        m_pfcwd_hw_orch->initQueueCounters(queueIdStr, queueId);

        // Verify: detectCount NOT incremented (still 1)
        auto stats = getQueueStats(queueId);
        EXPECT_EQ(stats.detectCount, 1) << "detectCount should not increment on warm reboot recovery";
        EXPECT_EQ(stats.restoreCount, 0);
        EXPECT_EQ(stats.operational, false);

        // Verify: Baseline re-established for continued monitoring
        verifyBaseline(queueId, 3000, 100, 0, 0);
    }

    // ========================================================================
    // Test Cases: updateQueueCounters() - Periodic Updates
    // ========================================================================

    TEST_F(PfcWdHwCountersTest, UpdateQueueCounters_PeriodicFirstUpdate)
    {
        // Test first periodic update during an ongoing storm
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));

        uint8_t queueIndex = 3;
        sai_object_id_t queueId = port.m_queue_ids[queueIndex];
        string queueIdStr = sai_serialize_object_id(queueId);

        resetQueueStats(queueId);
        setupQueuePortMapping(queueId, port, queueIndex);

        // Setup: Initialize counters (storm detected)
        setQueueHwCounters(queueId, 1000, 50);
        m_pfcwd_hw_orch->initQueueCounters(queueIdStr, queueId);

        // Setup: Update HW counters (traffic during storm)
        setQueueHwCounters(queueId, 1500, 75);

        // Test: Call updateQueueCounters with periodic=true
        m_pfcwd_hw_orch->updateQueueCounters(queueIdStr, queueId, true);

        // Verify: Deltas accumulated
        auto stats = getQueueStats(queueId);
        EXPECT_EQ(stats.txPkt, 500) << "txPkt should be delta: 1500 - 1000";
        EXPECT_EQ(stats.txDropPkt, 25) << "txDropPkt should be delta: 75 - 50";
        EXPECT_EQ(stats.rxPkt, 0);
        EXPECT_EQ(stats.rxDropPkt, 0);

        // Verify: *_LAST counters also updated
        EXPECT_EQ(stats.txPktLast, 500);
        EXPECT_EQ(stats.txDropPktLast, 25);
        EXPECT_EQ(stats.rxPktLast, 0);
        EXPECT_EQ(stats.rxDropPktLast, 0);

        // Verify: Still in storm state
        EXPECT_EQ(stats.operational, false);
        EXPECT_EQ(stats.detectCount, 1);
        EXPECT_EQ(stats.restoreCount, 0) << "restoreCount should not change on periodic update";

        // Verify: Baseline updated to current HW values
        verifyBaseline(queueId, 1500, 75, 0, 0);
    }

    TEST_F(PfcWdHwCountersTest, UpdateQueueCounters_PeriodicMultipleUpdates)
    {
        // Test multiple periodic updates accumulate correctly
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));

        uint8_t queueIndex = 3;
        sai_object_id_t queueId = port.m_queue_ids[queueIndex];
        string queueIdStr = sai_serialize_object_id(queueId);

        resetQueueStats(queueId);
        setupQueuePortMapping(queueId, port, queueIndex);

        // Setup: Initialize counters
        setQueueHwCounters(queueId, 1000, 50);
        m_pfcwd_hw_orch->initQueueCounters(queueIdStr, queueId);

        // First periodic update
        setQueueHwCounters(queueId, 1500, 75);
        m_pfcwd_hw_orch->updateQueueCounters(queueIdStr, queueId, true);

        // Second periodic update
        setQueueHwCounters(queueId, 2200, 110);
        m_pfcwd_hw_orch->updateQueueCounters(queueIdStr, queueId, true);

        // Third periodic update
        setQueueHwCounters(queueId, 3000, 150);
        m_pfcwd_hw_orch->updateQueueCounters(queueIdStr, queueId, true);

        // Verify: Cumulative counters = sum of all deltas
        auto stats = getQueueStats(queueId);
        EXPECT_EQ(stats.txPkt, 2000) << "Should be total: (1500-1000) + (2200-1500) + (3000-2200)";
        EXPECT_EQ(stats.txDropPkt, 100) << "Should be total: (75-50) + (110-75) + (150-110)";
        EXPECT_EQ(stats.rxPkt, 0);
        EXPECT_EQ(stats.rxDropPkt, 0);

        // Verify: *_LAST counters also accumulate
        EXPECT_EQ(stats.txPktLast, 2000);
        EXPECT_EQ(stats.txDropPktLast, 100);

        // Verify: Final baseline is latest HW value
        verifyBaseline(queueId, 3000, 150, 0, 0);
    }

    TEST_F(PfcWdHwCountersTest, UpdateQueueCounters_PeriodicNoTraffic)
    {
        // Test periodic update when no new traffic (HW counters unchanged)
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));

        uint8_t queueIndex = 3;
        sai_object_id_t queueId = port.m_queue_ids[queueIndex];
        string queueIdStr = sai_serialize_object_id(queueId);

        resetQueueStats(queueId);
        setupQueuePortMapping(queueId, port, queueIndex);

        // Setup: Initialize counters
        setQueueHwCounters(queueId, 1000, 50);
        m_pfcwd_hw_orch->initQueueCounters(queueIdStr, queueId);
        m_pfcwd_hw_orch->updateQueueCounters(queueIdStr, queueId, true);

        // Verify: All delta counters remain 0
        auto stats = getQueueStats(queueId);
        EXPECT_EQ(stats.txPkt, 0);
        EXPECT_EQ(stats.txDropPkt, 0);
        EXPECT_EQ(stats.rxPkt, 0);
        EXPECT_EQ(stats.rxDropPkt, 0);
        EXPECT_EQ(stats.operational, false);
    }

    // ========================================================================
    // Test Cases: updateQueueCounters() - Restoration
    // ========================================================================

    TEST_F(PfcWdHwCountersTest, UpdateQueueCounters_Restoration)
    {
        // Test storm restoration (periodic=false)
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));

        uint8_t queueIndex = 3;
        sai_object_id_t queueId = port.m_queue_ids[queueIndex];
        string queueIdStr = sai_serialize_object_id(queueId);

        resetQueueStats(queueId);
        setupQueuePortMapping(queueId, port, queueIndex);

        // Setup: Storm detected and some traffic during storm
        setQueueHwCounters(queueId, 1000, 50);
        m_pfcwd_hw_orch->initQueueCounters(queueIdStr, queueId);

        // Periodic update during storm
        setQueueHwCounters(queueId, 2000, 100);
        m_pfcwd_hw_orch->updateQueueCounters(queueIdStr, queueId, true);

        // Test: Restoration (periodic=false)
        setQueueHwCounters(queueId, 3000, 150);
        m_pfcwd_hw_orch->updateQueueCounters(queueIdStr, queueId, false);

        // Verify: restoreCount incremented
        auto stats = getQueueStats(queueId);
        EXPECT_EQ(stats.restoreCount, 1);
        EXPECT_EQ(stats.detectCount, 1);

        // Verify: operational = true (restored)
        EXPECT_EQ(stats.operational, true);

        // Verify: Deltas still calculated and accumulated
        EXPECT_EQ(stats.txPkt, 2000) << "Total: (2000-1000) + (3000-2000)";
        EXPECT_EQ(stats.txDropPkt, 100) << "Total: (100-50) + (150-100)";
        EXPECT_EQ(stats.rxPkt, 0);
        EXPECT_EQ(stats.rxDropPkt, 0);
    }

    TEST_F(PfcWdHwCountersTest, UpdateQueueCounters_RestorationAfterMultiplePeriodic)
    {
        // Test restoration after multiple periodic updates
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));

        uint8_t queueIndex = 3;
        sai_object_id_t queueId = port.m_queue_ids[queueIndex];
        string queueIdStr = sai_serialize_object_id(queueId);

        resetQueueStats(queueId);
        setupQueuePortMapping(queueId, port, queueIndex);

        // Init
        setQueueHwCounters(queueId, 1000, 50);
        m_pfcwd_hw_orch->initQueueCounters(queueIdStr, queueId);

        // Multiple periodic updates
        for (int i = 1; i <= 5; i++)
        {
            setQueueHwCounters(queueId, 1000 + (i * 500), 50 + (i * 25));
            m_pfcwd_hw_orch->updateQueueCounters(queueIdStr, queueId, true);
        }

        // Final restoration
        setQueueHwCounters(queueId, 5000, 250);
        m_pfcwd_hw_orch->updateQueueCounters(queueIdStr, queueId, false);

        // Verify restoration
        auto stats = getQueueStats(queueId);
        EXPECT_EQ(stats.restoreCount, 1);
        EXPECT_EQ(stats.operational, true);
        EXPECT_EQ(stats.txPkt, 4000) << "Total accumulated: 5000 - 1000";
        EXPECT_EQ(stats.txDropPkt, 200) << "Total accumulated: 250 - 50";
    }

    // ========================================================================
    // Test Cases: updateQueueCounters() - No Baseline (Pre-Storm Poll)
    // ========================================================================

    TEST_F(PfcWdHwCountersTest, UpdateQueueCounters_NoBaselineSeedsWithoutDeltas)
    {
        // Test a queue polled before its first storm: no baseline exists yet.
        // updateQueueCounters must still apply restoreCount/operational and seed
        // the baseline, but must not apply any deltas.
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));

        uint8_t queueIndex = 3;
        sai_object_id_t queueId = port.m_queue_ids[queueIndex];
        string queueIdStr = sai_serialize_object_id(queueId);

        resetQueueStats(queueId);
        setupQueuePortMapping(queueId, port, queueIndex);

        // Deliberately do NOT call initQueueCounters(): no baseline must exist.
        // resetQueueStats() already erased any leftover baseline, but make the
        // precondition explicit since it is the whole point of this test.
        m_pfcwd_hw_orch->m_queueBaselineStats.erase(queueId);

        // Setup: Non-zero HW counters
        setQueueHwCounters(queueId, 1000, 50);

        // Test: Call updateQueueCounters with no baseline present (periodic=false)
        m_pfcwd_hw_orch->updateQueueCounters(queueIdStr, queueId, false);

        // Verify: restoreCount incremented and operational restored, but no
        // deltas applied since there was no baseline to diff against.
        auto stats = getQueueStats(queueId);
        EXPECT_EQ(stats.restoreCount, 1);
        EXPECT_EQ(stats.detectCount, 0);
        EXPECT_EQ(stats.operational, true);
        EXPECT_EQ(stats.txPkt, 0);
        EXPECT_EQ(stats.txDropPkt, 0);
        EXPECT_EQ(stats.rxPkt, 0);
        EXPECT_EQ(stats.rxDropPkt, 0);
        EXPECT_EQ(stats.txPktLast, 0);
        EXPECT_EQ(stats.txDropPktLast, 0);
        EXPECT_EQ(stats.rxPktLast, 0);
        EXPECT_EQ(stats.rxDropPktLast, 0);

        // Verify: Baseline now seeded with the current HW sample
        verifyBaseline(queueId, 1000, 50, 0, 0);
    }

    // ========================================================================
    // Test Cases: Counter Underflow Protection
    // ========================================================================

    TEST_F(PfcWdHwCountersTest, UpdateQueueCounters_CounterResetTxPkt)
    {
        // Test underflow protection when txPkt counter resets/wraps
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));

        uint8_t queueIndex = 3;
        sai_object_id_t queueId = port.m_queue_ids[queueIndex];
        string queueIdStr = sai_serialize_object_id(queueId);

        resetQueueStats(queueId);
        setupQueuePortMapping(queueId, port, queueIndex);

        // Setup: Initialize with high counter values
        setQueueHwCounters(queueId, 5000, 250);
        m_pfcwd_hw_orch->initQueueCounters(queueIdStr, queueId);

        // Test: HW counter reset/wrapped (txPkt < baseline)
        setQueueHwCounters(queueId, 100, 260);
        m_pfcwd_hw_orch->updateQueueCounters(queueIdStr, queueId, true);

        // Verify: txPkt NOT updated (underflow protection)
        auto stats = getQueueStats(queueId);
        EXPECT_EQ(stats.txPkt, 0) << "txPkt should not update when HW < baseline";

        // Verify: Other counters that didn't wrap still update
        EXPECT_EQ(stats.txDropPkt, 10) << "txDropPkt should still update: 260 - 250";
    }

    TEST_F(PfcWdHwCountersTest, UpdateQueueCounters_AllCountersReset)
    {
        // Test when all HW counters reset (complete reset scenario)
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));

        uint8_t queueIndex = 3;
        sai_object_id_t queueId = port.m_queue_ids[queueIndex];
        string queueIdStr = sai_serialize_object_id(queueId);

        resetQueueStats(queueId);
        setupQueuePortMapping(queueId, port, queueIndex);

        // Setup: Initialize with high values
        setQueueHwCounters(queueId, 5000, 250);
        m_pfcwd_hw_orch->initQueueCounters(queueIdStr, queueId);

        // Test: All counters reset to 0
        setQueueHwCounters(queueId, 0, 0);
        m_pfcwd_hw_orch->updateQueueCounters(queueIdStr, queueId, true);

        // Verify: No counters updated (all skipped due to underflow)
        auto stats = getQueueStats(queueId);
        EXPECT_EQ(stats.txPkt, 0);
        EXPECT_EQ(stats.txDropPkt, 0);
        EXPECT_EQ(stats.rxPkt, 0);
        EXPECT_EQ(stats.rxDropPkt, 0);
    }

    // ========================================================================
    // Test Cases: readHwCounters()
    // ========================================================================

    TEST_F(PfcWdHwCountersTest, ReadHwCounters_Success)
    {
        // Test successful read of the queue's HW counters
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));

        uint8_t queueIndex = 3;
        sai_object_id_t queueId = port.m_queue_ids[queueIndex];

        setupQueuePortMapping(queueId, port, queueIndex);

        // Setup: Populate HW counters in COUNTERS_DB
        setQueueHwCounters(queueId, 1000, 50);

        // Test: Read HW counters
        PfcWdHwOrch::PfcWdHwStats hwStats;
        std::vector<swss::FieldValueTuple> queueFvs;
        bool result = m_pfcwd_hw_orch->readHwCounters(queueId, hwStats, queueFvs);

        // Verify: Success
        EXPECT_TRUE(result);
        // The queue entry is handed back to the caller, not just consumed here.
        EXPECT_FALSE(queueFvs.empty());
        EXPECT_EQ(hwStats.txPkt, 1000);
        EXPECT_EQ(hwStats.txDropPkt, 50);
        EXPECT_EQ(hwStats.rxPkt, 0);
        EXPECT_EQ(hwStats.rxDropPkt, 0);
    }

    TEST_F(PfcWdHwCountersTest, ReadHwCounters_MissingQueueCounters)
    {
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));

        uint8_t queueIndex = 3;
        sai_object_id_t queueId = port.m_queue_ids[queueIndex];
        string queueIdStr = sai_serialize_object_id(queueId);

        setupQueuePortMapping(queueId, port, queueIndex);

        // Ensure queue entry is deleted from COUNTERS_DB
        m_counters_table->del(queueIdStr);

        // Test: Read HW counters
        PfcWdHwOrch::PfcWdHwStats hwStats;
        std::vector<swss::FieldValueTuple> queueFvs;
        bool result = m_pfcwd_hw_orch->readHwCounters(queueId, hwStats, queueFvs);

        // Verify: a missing queue entry is unreadable, not an all-zero sample, so
        // no baseline is taken from it (see InitQueueCounters_NoCounterRowSeedsLaterBaseline)
        EXPECT_FALSE(result);
    }

    // The hardware watchdog acts only on the egress queue, so RX is never read,
    // even when the PG_DROP group has published the priority group's counters.
    TEST_F(PfcWdHwCountersTest, ReadHwCounters_IgnoresPgCounters)
    {
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));

        uint8_t queueIndex = 3;
        sai_object_id_t queueId = port.m_queue_ids[queueIndex];

        setupQueuePortMapping(queueId, port, queueIndex);
        setQueueHwCounters(queueId, 1000, 50);
        setPgHwCounters(port.m_priority_group_ids[queueIndex], 2000, 100);

        PfcWdHwOrch::PfcWdHwStats hwStats;
        std::vector<swss::FieldValueTuple> queueFvs;
        EXPECT_TRUE(m_pfcwd_hw_orch->readHwCounters(queueId, hwStats, queueFvs));
        EXPECT_EQ(hwStats.txPkt, 1000);
        EXPECT_EQ(hwStats.txDropPkt, 50);
        EXPECT_EQ(hwStats.rxPkt, 0);
        EXPECT_EQ(hwStats.rxDropPkt, 0);
    }

    // ========================================================================
    // Test Cases: getQueueStats() and updateQueueStats()
    // ========================================================================

    TEST_F(PfcWdHwCountersTest, GetQueueStats_NonExistent)
    {
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));

        sai_object_id_t queueId = port.m_queue_ids[7];

        resetQueueStats(queueId);
        auto stats = getQueueStats(queueId);

        // Verify: All fields are zero, but operational defaults to true (implementation behavior)
        EXPECT_EQ(stats.detectCount, 0);
        EXPECT_EQ(stats.restoreCount, 0);
        EXPECT_EQ(stats.txPkt, 0);
        EXPECT_EQ(stats.txDropPkt, 0);
        EXPECT_EQ(stats.rxPkt, 0);
        EXPECT_EQ(stats.rxDropPkt, 0);
        EXPECT_EQ(stats.txPktLast, 0);
        EXPECT_EQ(stats.txDropPktLast, 0);
        EXPECT_EQ(stats.rxPktLast, 0);
        EXPECT_EQ(stats.rxDropPktLast, 0);
        EXPECT_EQ(stats.operational, true);  // getQueueStats() defaults to true when entry missing
    }

    TEST_F(PfcWdHwCountersTest, UpdateAndGetQueueStats)
    {
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));

        sai_object_id_t queueId = port.m_queue_ids[3];
        string queueIdStr = sai_serialize_object_id(queueId);

        // Create and populate stats
        PfcWdHwOrch::PfcWdQueueStats expectedStats;
        memset(&expectedStats, 0, sizeof(expectedStats));
        expectedStats.detectCount = 5;
        expectedStats.restoreCount = 3;
        expectedStats.txPkt = 12345;
        expectedStats.txDropPkt = 678;
        expectedStats.rxPkt = 98765;
        expectedStats.rxDropPkt = 432;
        expectedStats.txPktLast = 1000;
        expectedStats.txDropPktLast = 50;
        expectedStats.rxPktLast = 2000;
        expectedStats.rxDropPktLast = 100;
        expectedStats.operational = true;

        // Test: Update stats
        m_pfcwd_hw_orch->updateQueueStats(queueIdStr, expectedStats);

        // Test: Read stats back
        auto actualStats = getQueueStats(queueId);

        // Verify: All fields match
        EXPECT_EQ(actualStats.detectCount, expectedStats.detectCount);
        EXPECT_EQ(actualStats.restoreCount, expectedStats.restoreCount);
        EXPECT_EQ(actualStats.txPkt, expectedStats.txPkt);
        EXPECT_EQ(actualStats.txDropPkt, expectedStats.txDropPkt);
        EXPECT_EQ(actualStats.rxPkt, expectedStats.rxPkt);
        EXPECT_EQ(actualStats.rxDropPkt, expectedStats.rxDropPkt);
        EXPECT_EQ(actualStats.txPktLast, expectedStats.txPktLast);
        EXPECT_EQ(actualStats.txDropPktLast, expectedStats.txDropPktLast);
        EXPECT_EQ(actualStats.rxPktLast, expectedStats.rxPktLast);
        EXPECT_EQ(actualStats.rxDropPktLast, expectedStats.rxDropPktLast);
        EXPECT_EQ(actualStats.operational, expectedStats.operational);
    }

    // Reconfiguring the watchdog must not erase accumulated packet counters.
    // The hardware watchdog implements a reconfiguration as stop-then-start, so
    // zeroing here made a detection-time change look like a counter wrap to any
    // delta-based collector. sonic-clear pfcwd counters remains the way to clear.
    TEST_F(PfcWdHwCountersTest, InitializeQueueStats_PreservesPacketCounters)
    {
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));
        const uint8_t tc = 3;
        sai_object_id_t queueId = port.m_queue_ids[tc];
        string queueIdStr = sai_serialize_object_id(queueId);

        resetQueueStats(queueId);

        PfcWdQueueStats seeded;
        memset(&seeded, 0, sizeof(seeded));
        seeded.detectCount = 2;
        seeded.restoreCount = 2;
        seeded.txPkt = 1234;
        seeded.txDropPkt = 111;
        seeded.rxPkt = 5678;
        seeded.rxDropPkt = 222;
        seeded.txPktLast = 333;
        seeded.txDropPktLast = 444;
        seeded.rxPktLast = 555;
        seeded.rxDropPktLast = 666;
        seeded.operational = false;
        m_pfcwd_hw_orch->updateQueueStats(queueIdStr, seeded);

        set<uint8_t> losslessTc = { tc };
        m_pfcwd_hw_orch->initializeQueueStats(port, losslessTc);

        auto stats = m_pfcwd_hw_orch->getQueueStats(queueIdStr);
        EXPECT_EQ(stats.detectCount, 2u);
        EXPECT_EQ(stats.restoreCount, 2u);
        EXPECT_EQ(stats.txPkt, 1234u);         // preserved
        EXPECT_EQ(stats.txDropPkt, 111u);      // preserved
        EXPECT_EQ(stats.rxPkt, 5678u);         // preserved
        EXPECT_EQ(stats.rxDropPkt, 222u);      // preserved
        EXPECT_EQ(stats.txPktLast, 333u);      // preserved
        EXPECT_EQ(stats.txDropPktLast, 444u);  // preserved
        EXPECT_EQ(stats.rxPktLast, 555u);      // preserved
        EXPECT_EQ(stats.rxDropPktLast, 666u);  // preserved
        EXPECT_TRUE(stats.operational);        // reset to operational
    }

    TEST_F(PfcWdHwCountersTest, DisableHwWatchdog_RemovesQueueStatusKeepsStats)
    {
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));
        ASSERT_TRUE(gPortsOrch->setPortPfcWatchdogStatus(port.m_port_id, 1 << 3));
        sai_object_id_t queueId = port.m_queue_ids[3];
        string queueIdStr = sai_serialize_object_id(queueId);

        resetQueueStats(queueId);
        PfcWdQueueStats seeded;
        memset(&seeded, 0, sizeof(seeded));
        seeded.detectCount = 1;
        seeded.restoreCount = 1;
        m_pfcwd_hw_orch->updateQueueStats(queueIdStr, seeded);
        m_pfcwd_hw_orch->initializeQueueStats(port, { 3 });

        string value;
        ASSERT_TRUE(m_counters_table->hget(queueIdStr, PFC_WD_QUEUE_STATUS, value));
        EXPECT_EQ(value, PFC_WD_QUEUE_STATUS_OPERATIONAL);

        m_pfcwd_hw_orch->disableHwWatchdog(port);

        EXPECT_FALSE(m_counters_table->hget(queueIdStr, PFC_WD_QUEUE_STATUS, value));
        ASSERT_TRUE(m_counters_table->hget(queueIdStr, PFC_WD_QUEUE_STATS_DEADLOCK_DETECTED, value));
        EXPECT_EQ(value, "1");
    }

    TEST_F(PfcWdHwCountersTest, DisableHwWatchdog_RemovesStatusOfQueueNoLongerLossless)
    {
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));
        ASSERT_TRUE(gPortsOrch->setPortPfcWatchdogStatus(port.m_port_id, 1 << 3));
        string queueIdStr = sai_serialize_object_id(port.m_queue_ids[3]);

        resetQueueStats(port.m_queue_ids[3]);
        m_pfcwd_hw_orch->initializeQueueStats(port, { 3 });
        ASSERT_TRUE(gPortsOrch->setPortPfcWatchdogStatus(port.m_port_id, 0));

        m_pfcwd_hw_orch->disableHwWatchdog(port);

        string value;
        EXPECT_FALSE(m_counters_table->hget(queueIdStr, PFC_WD_QUEUE_STATUS, value));
    }

    // pfcwd_sw_enable can name a queue that pfc_enable no longer covers.
    // Such a queue cannot be paused, so it is not watched.
    TEST_F(PfcWdHwCountersTest, GetLosslessTcs_SkipsQueuesWithPfcDisabled)
    {
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));
        ASSERT_TRUE(gPortsOrch->setPortPfcWatchdogStatus(port.m_port_id, (1 << 3) | (1 << 4)));

        set<uint8_t> losslessTc;
        ASSERT_TRUE(gPortsOrch->setPortPfc(port.m_port_id, (1 << 3) | (1 << 4)));
        ASSERT_TRUE(m_pfcwd_hw_orch->getLosslessTcsForPort(port, losslessTc));
        EXPECT_EQ(losslessTc, (set<uint8_t>{3, 4}));

        ASSERT_TRUE(gPortsOrch->setPortPfc(port.m_port_id, 1 << 3));
        ASSERT_TRUE(m_pfcwd_hw_orch->getLosslessTcsForPort(port, losslessTc));
        EXPECT_EQ(losslessTc, (set<uint8_t>{3}));

        ASSERT_TRUE(gPortsOrch->setPortPfc(port.m_port_id, 0));
        EXPECT_FALSE(m_pfcwd_hw_orch->getLosslessTcsForPort(port, losslessTc));
        EXPECT_TRUE(losslessTc.empty());
    }

    // A port with PFC off on every watched queue gets no hardware watchdog, so it
    // is not reported as configured.
    TEST_F(PfcWdHwCountersTest, ConfigureHwWatchdog_RefusesPortWithPfcDisabled)
    {
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));
        ASSERT_TRUE(gPortsOrch->setPortPfcWatchdogStatus(port.m_port_id, (1 << 3) | (1 << 4)));
        ASSERT_TRUE(gPortsOrch->setPortPfc(port.m_port_id, 0));
        // COUNTERS_DB is shared across the binary; start from queues with no status
        for (auto tc : {3, 4})
        {
            resetQueueStats(port.m_queue_ids[tc]);
        }

        EXPECT_FALSE(m_pfcwd_hw_orch->configureHwWatchdog(port, 200, 400, PfcWdAction::PFC_WD_ACTION_DROP));
        EXPECT_FALSE(m_pfcwd_hw_orch->isPortInStormedState(port));
        for (auto tc : {3, 4})
        {
            string value;
            EXPECT_FALSE(m_counters_table->hget(sai_serialize_object_id(port.m_queue_ids[tc]),
                                                PFC_WD_QUEUE_STATUS, value));
        }
    }

    // ========================================================================
    // Test Cases: storms that begin before the counters are readable
    // ========================================================================

    // After a fast reboot the queue's row is missing until the flex counters
    // publish. The storm is recorded without a baseline, and the first readable
    // sample seeds one instead of charging the counters' absolute values.
    TEST_F(PfcWdHwCountersTest, InitQueueCounters_NoCounterRowSeedsLaterBaseline)
    {
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));
        uint8_t queueIndex = 3;
        sai_object_id_t queueId = port.m_queue_ids[queueIndex];
        string queueIdStr = sai_serialize_object_id(queueId);

        resetQueueStats(queueId);
        setupQueuePortMapping(queueId, port, queueIndex);

        m_pfcwd_hw_orch->initQueueCounters(queueIdStr, queueId);
        verifyQueueStats(queueId, 1, 0, 0, 0, 0, 0, false);
        EXPECT_EQ(m_pfcwd_hw_orch->m_queueBaselineStats.count(queueId), 0u);
        EXPECT_EQ(m_pfcwd_hw_orch->m_queuesWithoutBaseline.count(queueId), 1u);

        setQueueHwCounters(queueId, 1000, 50);
        m_pfcwd_hw_orch->updateQueueCounters(queueIdStr, queueId, true);
        verifyQueueStats(queueId, 1, 0, 0, 0, 0, 0, false);
        verifyBaseline(queueId, 1000, 50, 0, 0);
        EXPECT_EQ(m_pfcwd_hw_orch->m_queuesWithoutBaseline.count(queueId), 0u);

        setQueueHwCounters(queueId, 1010, 52);
        m_pfcwd_hw_orch->updateQueueCounters(queueIdStr, queueId, true);
        verifyQueueStats(queueId, 1, 0, 10, 2, 0, 0, false);
    }

    // The storm writes the watchdog's own fields into the queue's hash, so a row
    // can exist before the flex counters have published any counter into it.
    TEST_F(PfcWdHwCountersTest, InitQueueCounters_RowWithoutPacketCounterIsUnreadable)
    {
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));
        uint8_t queueIndex = 3;
        sai_object_id_t queueId = port.m_queue_ids[queueIndex];
        string queueIdStr = sai_serialize_object_id(queueId);

        resetQueueStats(queueId);
        setupQueuePortMapping(queueId, port, queueIndex);
        m_counters_table->set(queueIdStr, {
            { PFC_WD_QUEUE_STATUS, PFC_WD_QUEUE_STATUS_OPERATIONAL },
            { PFC_WD_QUEUE_STATS_DEADLOCK_DETECTED, "2" },
            { PFC_WD_QUEUE_STATS_DEADLOCK_RESTORED, "2" } });

        PfcWdHwStats counters = {};
        vector<FieldValueTuple> queueFvs;
        EXPECT_FALSE(m_pfcwd_hw_orch->readHwCounters(queueId, counters, queueFvs));

        // The storm counts on from the watchdog's fields already in the row.
        m_pfcwd_hw_orch->initQueueCounters(queueIdStr, queueId);
        EXPECT_EQ(m_pfcwd_hw_orch->m_queueBaselineStats.count(queueId), 0u);
        EXPECT_EQ(m_pfcwd_hw_orch->m_queuesWithoutBaseline.count(queueId), 1u);
        verifyQueueStats(queueId, 3, 2, 0, 0, 0, 0, false);
    }

    // A malformed value fails the sample instead of throwing out of the timer.
    TEST_F(PfcWdHwCountersTest, ReadHwCounters_MalformedValueIsUnreadable)
    {
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));
        uint8_t queueIndex = 3;
        sai_object_id_t queueId = port.m_queue_ids[queueIndex];
        string queueIdStr = sai_serialize_object_id(queueId);

        resetQueueStats(queueId);
        setupQueuePortMapping(queueId, port, queueIndex);

        PfcWdHwStats counters = {};
        vector<FieldValueTuple> queueFvs;
        m_counters_table->set(queueIdStr, { { "SAI_QUEUE_STAT_PACKETS", "n/a" } });
        EXPECT_FALSE(m_pfcwd_hw_orch->readHwCounters(queueId, counters, queueFvs));

        m_counters_table->set(queueIdStr, {
            { "SAI_QUEUE_STAT_PACKETS", "1000" },
            { "SAI_QUEUE_STAT_DROPPED_PACKETS", "" } });
        EXPECT_FALSE(m_pfcwd_hw_orch->readHwCounters(queueId, counters, queueFvs));
    }

    // A restore closes the storm even when nothing is readable, with no deltas.
    TEST_F(PfcWdHwCountersTest, UpdateQueueCounters_RestoreWithoutReadableRowClosesStorm)
    {
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));
        uint8_t queueIndex = 3;
        sai_object_id_t queueId = port.m_queue_ids[queueIndex];
        string queueIdStr = sai_serialize_object_id(queueId);

        resetQueueStats(queueId);
        setupQueuePortMapping(queueId, port, queueIndex);

        m_pfcwd_hw_orch->initQueueCounters(queueIdStr, queueId);
        m_pfcwd_hw_orch->updateQueueCounters(queueIdStr, queueId, false);

        verifyQueueStats(queueId, 1, 1, 0, 0, 0, 0, true);
        EXPECT_EQ(m_pfcwd_hw_orch->m_queuesWithoutBaseline.count(queueId), 0u);
    }

    // The periodic update also polls storms still waiting for a baseline.
    TEST_F(PfcWdHwCountersTest, PeriodicTimer_SeedsBaselineOfStormWithoutOne)
    {
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));
        uint8_t queueIndex = 3;
        sai_object_id_t queueId = port.m_queue_ids[queueIndex];
        string queueIdStr = sai_serialize_object_id(queueId);

        resetQueueStats(queueId);
        setupQueuePortMapping(queueId, port, queueIndex);
        m_pfcwd_hw_orch->initQueueCounters(queueIdStr, queueId);

        setQueueHwCounters(queueId, 1000, 50);
        SelectableTimer timer(timespec({ .tv_sec = 1, .tv_nsec = 0 }), 0);
        m_pfcwd_hw_orch->doTask(timer);

        verifyBaseline(queueId, 1000, 50, 0, 0);
        EXPECT_EQ(m_pfcwd_hw_orch->m_queuesWithoutBaseline.count(queueId), 0u);
        verifyQueueStats(queueId, 1, 0, 0, 0, 0, 0, false);
    }

    // A row that vanishes mid-storm (counters cleared, flex counters unregistered)
    // skips the tick and keeps the baseline, instead of reading as a counter reset.
    TEST_F(PfcWdHwCountersTest, UpdateQueueCounters_RowVanishingAtTickKeepsBaseline)
    {
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));
        uint8_t queueIndex = 3;
        sai_object_id_t queueId = port.m_queue_ids[queueIndex];
        string queueIdStr = sai_serialize_object_id(queueId);

        resetQueueStats(queueId);
        setupQueuePortMapping(queueId, port, queueIndex);
        setQueueHwCounters(queueId, 1000, 50);
        m_pfcwd_hw_orch->initQueueCounters(queueIdStr, queueId);

        auto watchdogFields = getQueueStats(queueId);
        m_counters_table->del(queueIdStr);
        m_pfcwd_hw_orch->updateQueueCounters(queueIdStr, queueId, true);
        verifyBaseline(queueId, 1000, 50, 0, 0);

        m_pfcwd_hw_orch->updateQueueStats(queueIdStr, watchdogFields);
        setQueueHwCounters(queueId, 1010, 52);
        m_pfcwd_hw_orch->updateQueueCounters(queueIdStr, queueId, true);
        verifyQueueStats(queueId, 1, 0, 10, 2, 0, 0, false);
    }

    // ========================================================================
    // Test Cases: action change while a stop is still being processed
    // ========================================================================

    class PfcWdHwActionTest : public PfcWdHwCountersTest
    {
    protected:
        Consumer *m_consumer;

        void SetUp() override
        {
            PfcWdHwCountersTest::SetUp();
            m_consumer = dynamic_cast<Consumer *>(m_pfcwd_hw_orch->getExecutor(CFG_PFC_WD_TABLE_NAME));
            ASSERT_NE(m_consumer, nullptr);
        }

        // Ports already running the hardware watchdog with a shared action
        void seedRunning(const set<string> &ports, PfcWdAction action)
        {
            m_pfcwd_hw_orch->m_hwWdPorts = ports;
            m_pfcwd_hw_orch->setPfcDlrPacketAction(action);
        }

        static KeyOpFieldsValuesTuple startAlert(const string &port)
        {
            return { port, SET_COMMAND, {
                { PFC_WD_ACTION, "alert" },
                { PFC_WD_DETECTION_TIME, "400" },
                { PFC_WD_RESTORATION_TIME, "400" },
            } };
        }

        static KeyOpFieldsValuesTuple stop(const string &port)
        {
            return { port, DEL_COMMAND, {} };
        }

        bool setQueued(const string &port)
        {
            auto range = m_consumer->m_toSync.equal_range(port);
            for (auto it = range.first; it != range.second; ++it)
            {
                if (kfvOp(it->second) == SET_COMMAND)
                {
                    return true;
                }
            }
            return false;
        }
    };

    // The stop's DEL sorts after the start's SET, so the SET is processed while
    // the old action is still held; it must be kept for the next pass.
    TEST_F(PfcWdHwActionTest, StartWithNewActionWaitsForStop)
    {
        seedRunning({ "Ethernet4" }, PfcWdAction::PFC_WD_ACTION_DROP);
        m_consumer->addToSync(deque<KeyOpFieldsValuesTuple>{ stop("Ethernet4"), startAlert("Ethernet0") });

        static_cast<PfcWdBaseOrch *>(m_pfcwd_hw_orch)->doTask(*m_consumer);

        EXPECT_TRUE(m_pfcwd_hw_orch->m_hwWdPorts.empty());
        EXPECT_EQ(m_pfcwd_hw_orch->getPfcDlrPacketAction(), PfcWdAction::PFC_WD_ACTION_UNKNOWN);
        EXPECT_TRUE(setQueued("Ethernet0"));
        EXPECT_EQ(m_consumer->m_toSync.size(), 1u);
    }

    TEST_F(PfcWdHwActionTest, StartWithNewActionRejectedWithoutStop)
    {
        seedRunning({ "Ethernet4" }, PfcWdAction::PFC_WD_ACTION_DROP);
        m_consumer->addToSync(deque<KeyOpFieldsValuesTuple>{ startAlert("Ethernet0") });

        static_cast<PfcWdBaseOrch *>(m_pfcwd_hw_orch)->doTask(*m_consumer);

        EXPECT_TRUE(m_consumer->m_toSync.empty());
        EXPECT_EQ(m_pfcwd_hw_orch->m_hwWdPorts, set<string>({ "Ethernet4" }));
        EXPECT_EQ(m_pfcwd_hw_orch->getPfcDlrPacketAction(), PfcWdAction::PFC_WD_ACTION_DROP);
    }

    // A port that keeps the old action makes the mismatch permanent, so the
    // entry must not be retried forever.
    TEST_F(PfcWdHwActionTest, StartWithNewActionRejectedWhenAPortKeepsOldAction)
    {
        seedRunning({ "Ethernet4", "Ethernet8" }, PfcWdAction::PFC_WD_ACTION_DROP);
        m_consumer->addToSync(deque<KeyOpFieldsValuesTuple>{ stop("Ethernet4"), startAlert("Ethernet0") });

        static_cast<PfcWdBaseOrch *>(m_pfcwd_hw_orch)->doTask(*m_consumer);

        EXPECT_TRUE(m_consumer->m_toSync.empty());
        EXPECT_EQ(m_pfcwd_hw_orch->m_hwWdPorts, set<string>({ "Ethernet8" }));
        EXPECT_EQ(m_pfcwd_hw_orch->getPfcDlrPacketAction(), PfcWdAction::PFC_WD_ACTION_DROP);
    }
}
