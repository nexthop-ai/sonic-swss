#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_orch_test.h"
#include "mock_table.h"

#define private public
#include "pfcwdhworch.h"
#undef private

#include <gtest/gtest.h>

namespace pfcwdhw_timer_range_test
{
    using namespace std;
    using namespace mock_orch_test;

    // The ranges a TH6 reports, and the timers 'pfcwd start_default' saves in
    // software mode on a 256 port box.
    static const uint32_t DETECTION_MIN = 1;
    static const uint32_t DETECTION_MAX = 1500;
    static const uint32_t RESTORATION_MIN = 100;
    static const uint32_t RESTORATION_MAX = 1000;
    static const char *SOFTWARE_MODE_TIME = "1600";

    class PfcWdHwTimerRangeTest : public MockOrchTest
    {
    protected:
        shared_ptr<swss::DBConnector> m_counters_db;
        shared_ptr<swss::DBConnector> m_state_db;
        shared_ptr<swss::Table> m_hw_state_table;
        shared_ptr<swss::Table> m_cfg_pfc_wd_table;
        PfcWdHwOrch *m_pfcwd_hw_orch;
        Port m_port;

        void SetUp() override
        {
            MockOrchTest::SetUp();
            bringUpPorts();

            m_counters_db = make_shared<swss::DBConnector>("COUNTERS_DB", 0);
            m_state_db = make_shared<swss::DBConnector>("STATE_DB", 0);
            m_hw_state_table = make_shared<swss::Table>(m_state_db.get(),
                                                        STATE_PFC_WD_HW_STATE_TABLE_NAME);
            m_cfg_pfc_wd_table = make_shared<swss::Table>(m_config_db.get(),
                                                          CFG_PFC_WD_TABLE_NAME);

            m_pfcwd_hw_orch = newOrch();

            ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", m_port));
            ASSERT_TRUE(gPortsOrch->setPortPfcWatchdogStatus(m_port.m_port_id, (1 << 3) | (1 << 4)));
            ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", m_port));
        }

        void TearDown() override
        {
            delete m_pfcwd_hw_orch;
            MockOrchTest::TearDown();
        }

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

        PfcWdHwOrch *newOrch()
        {
            static vector<string> pfc_wd_tables = { CFG_PFC_WD_TABLE_NAME };
            static const vector<sai_port_stat_t> portStatIds = {};
            static const vector<sai_queue_stat_t> queueStatIds = { SAI_QUEUE_STAT_PACKETS };
            static const vector<sai_queue_attr_t> queueAttrIds = {};

            auto *orch = new PfcWdHwOrch(m_config_db.get(), pfc_wd_tables,
                                         portStatIds, queueStatIds, queueAttrIds);

            // The VS switch does not report the PFC deadlock interval ranges, so
            // stand in for what initializeTimerRanges() reads on a TH6.
            orch->m_detectionTimeMin = DETECTION_MIN;
            orch->m_detectionTimeMax = DETECTION_MAX;
            orch->m_restorationTimeMin = RESTORATION_MIN;
            orch->m_restorationTimeMax = RESTORATION_MAX;

            return orch;
        }

        // Stand in for the PFC_WD table an upgrade carries over
        void saveConfig(const string &detectionTime, const string &restorationTime)
        {
            m_cfg_pfc_wd_table->set("Ethernet0", {
                { PFC_WD_DETECTION_TIME, detectionTime },
                { PFC_WD_RESTORATION_TIME, restorationTime },
                { PFC_WD_ACTION, "drop" },
            });
        }

        // The ranges are only known after initializeTimerRanges(), which the VS
        // switch cannot answer, so the startup re-apply is driven here instead
        // of through the constructor. On a switch the CONFIG_DB subscriber then
        // delivers the same saved rows again from its initial snapshot.
        void startupReapply()
        {
            m_pfcwd_hw_orch->recoverWarmReboot(m_config_db.get());
            m_pfcwd_hw_orch->addExistingData(m_cfg_pfc_wd_table.get());
            static_cast<Orch *>(m_pfcwd_hw_orch)->doTask();
        }

        task_process_status configure(const string &detectionTime, const string &restorationTime)
        {
            return m_pfcwd_hw_orch->createEntry("Ethernet0", {
                { PFC_WD_DETECTION_TIME, detectionTime },
                { PFC_WD_RESTORATION_TIME, restorationTime },
                { PFC_WD_ACTION, "drop" },
            });
        }

        string hwStateField(const string &field)
        {
            string value;
            if (!m_hw_state_table->hget("Ethernet0", field, value))
            {
                return "";
            }
            return value;
        }

        bool portConfigured()
        {
            return m_pfcwd_hw_orch->m_hwWdPorts.count("Ethernet0") != 0;
        }
    };

    // An upgrade from a software watchdog image carries 1600/1600 into a
    // 1-1500 / 100-1000 ms hardware range. The port has to end up protected.
    TEST_F(PfcWdHwTimerRangeTest, SavedOutOfRangeTimersAreMigrated)
    {
        saveConfig(SOFTWARE_MODE_TIME, SOFTWARE_MODE_TIME);

        startupReapply();

        EXPECT_TRUE(portConfigured());
        EXPECT_EQ(hwStateField("status"), "configured");
        EXPECT_EQ(hwStateField("hw_detection_time"), to_string(DETECTION_MAX));
        EXPECT_EQ(hwStateField("hw_restoration_time"), to_string(RESTORATION_MAX));
    }

    TEST_F(PfcWdHwTimerRangeTest, SavedBelowRangeTimerIsRaisedToTheMinimum)
    {
        saveConfig("200", "10");

        startupReapply();

        EXPECT_TRUE(portConfigured());
        EXPECT_EQ(hwStateField("hw_detection_time"), "200");
        EXPECT_EQ(hwStateField("hw_restoration_time"), to_string(RESTORATION_MIN));
    }

    // 0 is not a timer any image saves by default, so it is still refused
    TEST_F(PfcWdHwTimerRangeTest, SavedZeroTimerIsStillRefused)
    {
        saveConfig("400", "0");

        startupReapply();

        EXPECT_FALSE(portConfigured());
    }

    TEST_F(PfcWdHwTimerRangeTest, SavedInRangeTimersAreLeftAlone)
    {
        saveConfig("400", "400");

        startupReapply();

        EXPECT_TRUE(portConfigured());
        EXPECT_EQ(hwStateField("hw_detection_time"), "400");
        EXPECT_EQ(hwStateField("hw_restoration_time"), "400");
    }

    // The migration covers config that was already on disk. A timer written
    // while orchagent runs is still held to the hardware range, so the CLI and
    // GCU stay the place an out-of-range value is reported to the operator.
    TEST_F(PfcWdHwTimerRangeTest, RuntimeOutOfRangeTimerIsStillRejected)
    {
        EXPECT_EQ(configure(SOFTWARE_MODE_TIME, SOFTWARE_MODE_TIME),
                  task_process_status::task_invalid_entry);
        EXPECT_FALSE(portConfigured());
    }

    // Once the saved row is rewritten, its timers are the operator's and are
    // held to the range again, even if they return to the saved value.
    TEST_F(PfcWdHwTimerRangeTest, RewrittenRowIsHeldToTheRange)
    {
        saveConfig(SOFTWARE_MODE_TIME, SOFTWARE_MODE_TIME);
        startupReapply();
        ASSERT_EQ(hwStateField("hw_detection_time"), to_string(DETECTION_MAX));

        EXPECT_EQ(configure("1700", "400"), task_process_status::task_invalid_entry);

        ASSERT_EQ(configure("400", "400"), task_process_status::task_success);
        EXPECT_EQ(configure(SOFTWARE_MODE_TIME, SOFTWARE_MODE_TIME),
                  task_process_status::task_invalid_entry);
        EXPECT_EQ(hwStateField("hw_detection_time"), "400");
    }

    // STATE_DB survives an orchagent restart, so a row from the previous run
    // would keep reporting a watchdog that this run has not programmed.
    TEST_F(PfcWdHwTimerRangeTest, ConstructionDropsStaleHwState)
    {
        m_hw_state_table->set("Ethernet0", {
            { "recovery_type", "hardware" },
            { "status", "configured" },
            { "hw_detection_time", "400" },
            { "hw_restoration_time", "400" },
        });
        ASSERT_EQ(hwStateField("status"), "configured");

        delete m_pfcwd_hw_orch;
        m_pfcwd_hw_orch = newOrch();

        EXPECT_EQ(hwStateField("status"), "");
    }
}
