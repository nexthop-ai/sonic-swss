#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_orch_test.h"
#include "mock_table.h"

#define private public
#include "pfcwdhworch.h"
#undef private

#include <gtest/gtest.h>

namespace pfcwdhw_stat_history_test
{
    using namespace std;
    using namespace mock_orch_test;

    // Reaches the protected members the tests inspect
    struct BaseAccess : PfcWdBaseOrch
    {
        using PfcWdBaseOrch::filterPfcCounters;

        static FlexCounterTaggedCachedManager<sai_object_type_t> &manager(PfcWdBaseOrch &orch)
        {
            return *(orch.*(&BaseAccess::m_pfcwdFlexCounterManager));
        }
    };

    struct ManagerAccess : FlexCounterManager
    {
        static bool installed(FlexCounterManager &mgr, sai_object_id_t oid)
        {
            return (mgr.*(&ManagerAccess::installed_counters)).count(oid) != 0;
        }
    };

    class PfcWdHwStatHistoryTest : public MockOrchTest
    {
    protected:
        shared_ptr<swss::DBConnector> m_counters_db;
        shared_ptr<swss::Table> m_counters_table;
        PfcWdHwOrch *m_pfcwd_hw_orch;
        Port m_port;

        void SetUp() override
        {
            MockOrchTest::SetUp();
            bringUpPorts();

            m_counters_db = make_shared<swss::DBConnector>("COUNTERS_DB", 0);
            m_counters_table = make_shared<swss::Table>(m_counters_db.get(), COUNTERS_TABLE);

            m_pfcwd_hw_orch = newOrch();

            ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", m_port));
            // Only a queue with PFC enabled is watched, so set both masks
            ASSERT_TRUE(gPortsOrch->setPortPfc(m_port.m_port_id, (1 << 3) | (1 << 4)));
            ASSERT_TRUE(gPortsOrch->setPortPfcWatchdogStatus(m_port.m_port_id, (1 << 3) | (1 << 4)));
            ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", m_port));
        }

        void TearDown() override
        {
            delete m_pfcwd_hw_orch;
            MockOrchTest::TearDown();
        }

        PfcWdHwOrch *newOrch()
        {
            static vector<string> pfc_wd_tables = { CFG_PFC_WD_TABLE_NAME };
            static const vector<sai_port_stat_t> portStatIds = {
                SAI_PORT_STAT_PFC_3_RX_PKTS,
                SAI_PORT_STAT_PFC_4_RX_PKTS,
                SAI_PORT_STAT_PFC_5_RX_PKTS,
                SAI_PORT_STAT_PFC_3_ON2OFF_RX_PKTS,
                SAI_PORT_STAT_PFC_4_ON2OFF_RX_PKTS,
                SAI_PORT_STAT_PFC_5_ON2OFF_RX_PKTS,
            };
            static const vector<sai_queue_stat_t> queueStatIds = { SAI_QUEUE_STAT_PACKETS };
            static const vector<sai_queue_attr_t> queueAttrIds = { SAI_QUEUE_ATTR_PAUSE_STATUS };

            return new PfcWdHwOrch(m_config_db.get(), pfc_wd_tables,
                                   portStatIds, queueStatIds, queueAttrIds);
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

        string queueKey(uint8_t tc)
        {
            return sai_serialize_object_id(m_port.m_queue_ids[tc]);
        }

        string portKey()
        {
            return sai_serialize_object_id(m_port.m_port_id);
        }

        bool hasField(const string &key, const string &field, string *value = nullptr)
        {
            string v;
            bool found = m_counters_table->hget(key, field, v);
            if (found && value)
            {
                *value = v;
            }
            return found;
        }

        bool portListed()
        {
            swss::Table ports(m_counters_db.get(), PFC_WD_STAT_HISTORY_TABLE);
            string v;
            return ports.hget(PFC_WD_STAT_HISTORY_PORTS, portKey(), v);
        }

        bool portCountersPolled()
        {
            return ManagerAccess::installed(BaseAccess::manager(*m_pfcwd_hw_orch), m_port.m_port_id);
        }
    };

    TEST_F(PfcWdHwStatHistoryTest, FilterPfcCountersKeepsLosslessPriorities)
    {
        unordered_set<string> counters = {
            "SAI_PORT_STAT_PFC_3_RX_PKTS",
            "SAI_PORT_STAT_PFC_4_ON2OFF_RX_PKTS",
            "SAI_PORT_STAT_PFC_5_RX_PKTS",
            "SAI_PORT_STAT_IF_IN_OCTETS",
        };

        auto filtered = BaseAccess::filterPfcCounters(counters, { 3, 4 });

        EXPECT_EQ(filtered, (unordered_set<string>{
            "SAI_PORT_STAT_PFC_3_RX_PKTS",
            "SAI_PORT_STAT_PFC_4_ON2OFF_RX_PKTS",
            "SAI_PORT_STAT_IF_IN_OCTETS",
        }));
    }

    TEST_F(PfcWdHwStatHistoryTest, EnableFlagsLosslessQueuesAndPollsPortPfcCounters)
    {
        ASSERT_FALSE(portCountersPolled());
        ASSERT_FALSE(portListed());

        m_pfcwd_hw_orch->enableStatHistory(m_port);

        string value;
        ASSERT_TRUE(hasField(queueKey(3), "PFC_STAT_HISTORY", &value));
        EXPECT_EQ(value, "enable");
        ASSERT_TRUE(hasField(queueKey(4), "PFC_STAT_HISTORY", &value));
        EXPECT_EQ(value, "enable");
        EXPECT_FALSE(hasField(queueKey(0), "PFC_STAT_HISTORY"));
        EXPECT_FALSE(hasField(queueKey(5), "PFC_STAT_HISTORY"));

        EXPECT_TRUE(portCountersPolled());
        EXPECT_TRUE(portListed());
    }

    TEST_F(PfcWdHwStatHistoryTest, ClearStateKeepsRecordedHistory)
    {
        m_counters_table->set(queueKey(4), {
            { "PFC_STAT_HISTORY", "enable" },
            { "SAI_QUEUE_ATTR_PAUSE_STATUS", "true" },
            { "SAI_QUEUE_ATTR_PAUSE_STATUS_last", "true" },
        });
        m_counters_table->set(portKey(), {
            { "SAI_PORT_STAT_PFC_4_RX_PKTS", "100" },
            { "SAI_PORT_STAT_PFC_4_RX_PKTS_last", "90" },
            { "SAI_PORT_STAT_PFC_3_RX_PKTS_last", "7" },
            { "EST_PORT_STAT_PFC_4_RX_PAUSE_DURATION_US", "5000" },
            { "EST_PORT_STAT_PFC_4_RECENT_PAUSE_TIME_US", "3000" },
            { "EST_PORT_STAT_PFC_4_RECENT_PAUSE_TIMESTAMP", "1791967131719412" },
        });

        m_pfcwd_hw_orch->clearStatHistoryState(m_port.m_port_id, m_port.m_queue_ids[4], 4);

        EXPECT_FALSE(hasField(queueKey(4), "PFC_STAT_HISTORY"));
        EXPECT_FALSE(hasField(queueKey(4), "SAI_QUEUE_ATTR_PAUSE_STATUS_last"));
        EXPECT_TRUE(hasField(queueKey(4), "SAI_QUEUE_ATTR_PAUSE_STATUS"));

        EXPECT_FALSE(hasField(portKey(), "SAI_PORT_STAT_PFC_4_RX_PKTS_last"));
        EXPECT_TRUE(hasField(portKey(), "SAI_PORT_STAT_PFC_3_RX_PKTS_last"));
        EXPECT_TRUE(hasField(portKey(), "SAI_PORT_STAT_PFC_4_RX_PKTS"));
        EXPECT_TRUE(hasField(portKey(), "EST_PORT_STAT_PFC_4_RX_PAUSE_DURATION_US"));
        EXPECT_TRUE(hasField(portKey(), "EST_PORT_STAT_PFC_4_RECENT_PAUSE_TIME_US"));
        EXPECT_TRUE(hasField(portKey(), "EST_PORT_STAT_PFC_4_RECENT_PAUSE_TIMESTAMP"));
    }

    TEST_F(PfcWdHwStatHistoryTest, DisableStopsHistory)
    {
        m_pfcwd_hw_orch->enableStatHistory(m_port);
        m_counters_table->hset(queueKey(4), "SAI_QUEUE_ATTR_PAUSE_STATUS_last", "true");
        m_counters_table->hset(portKey(), "SAI_PORT_STAT_PFC_4_RX_PKTS_last", "90");
        m_counters_table->hset(portKey(), "EST_PORT_STAT_PFC_4_RECENT_PAUSE_TIME_US", "3000");
        ASSERT_TRUE(portCountersPolled());
        ASSERT_TRUE(portListed());

        m_pfcwd_hw_orch->disableHwWatchdog(m_port);

        EXPECT_FALSE(portCountersPolled());
        EXPECT_FALSE(portListed());
        EXPECT_FALSE(hasField(queueKey(3), "PFC_STAT_HISTORY"));
        EXPECT_FALSE(hasField(queueKey(4), "PFC_STAT_HISTORY"));
        EXPECT_FALSE(hasField(queueKey(4), "SAI_QUEUE_ATTR_PAUSE_STATUS_last"));
        EXPECT_FALSE(hasField(portKey(), "SAI_PORT_STAT_PFC_4_RX_PKTS_last"));
        EXPECT_TRUE(hasField(portKey(), "EST_PORT_STAT_PFC_4_RECENT_PAUSE_TIME_US"));
    }

    TEST_F(PfcWdHwStatHistoryTest, ConstructionDropsStaleListing)
    {
        // Left by a previous orchagent across a warm restart, for a port whose
        // watchdog config is gone
        swss::Table ports(m_counters_db.get(), PFC_WD_STAT_HISTORY_TABLE);
        ports.hset(PFC_WD_STAT_HISTORY_PORTS, portKey(), "Ethernet0");
        ASSERT_TRUE(portListed());

        delete m_pfcwd_hw_orch;
        m_pfcwd_hw_orch = newOrch();

        EXPECT_FALSE(portListed());
    }

    // Restoration time is optional: without one, orchagent keeps the recovery timer the hardware holds

    // Recovery timer the stubbed SAI reports when none was written (a fresh port's default)
    const uint32_t PLATFORM_DLR_MS = 100;

    sai_port_api_t restoration_sai_port_api;
    sai_port_api_t *restoration_old_sai_port_api;
    sai_queue_api_t restoration_sai_queue_api;
    sai_queue_api_t *restoration_old_sai_queue_api;
    sai_switch_api_t restoration_sai_switch_api;
    sai_switch_api_t *restoration_old_sai_switch_api;

    bool dlr_written;
    uint32_t programmed_dlr_ms;

    sai_status_t _restoration_set_port_attribute(sai_object_id_t port_id, const sai_attribute_t *attr)
    {
        if (attr->id == SAI_PORT_ATTR_PFC_TC_DLD_INTERVAL || attr->id == SAI_PORT_ATTR_PFC_TC_DLR_INTERVAL)
        {
            // Teardown clears the intervals with an empty list; only count programming
            if (attr->id == SAI_PORT_ATTR_PFC_TC_DLR_INTERVAL && attr->value.maplist.count > 0)
            {
                dlr_written = true;
                programmed_dlr_ms = attr->value.maplist.list[0].value;
            }
            return SAI_STATUS_SUCCESS;
        }
        return restoration_old_sai_port_api->set_port_attribute(port_id, attr);
    }

    sai_status_t _restoration_get_port_attribute(sai_object_id_t port_id, uint32_t attr_count, sai_attribute_t *attr_list)
    {
        if (attr_count == 1 &&
            (attr_list[0].id == SAI_PORT_ATTR_PFC_TC_DLD_INTERVAL || attr_list[0].id == SAI_PORT_ATTR_PFC_TC_DLR_INTERVAL))
        {
            uint32_t value = attr_list[0].id == SAI_PORT_ATTR_PFC_TC_DLR_INTERVAL ? programmed_dlr_ms : 400;
            for (uint32_t i = 0; i < attr_list[0].value.maplist.count; i++)
            {
                attr_list[0].value.maplist.list[i].value = value;
            }
            return SAI_STATUS_SUCCESS;
        }
        return restoration_old_sai_port_api->get_port_attribute(port_id, attr_count, attr_list);
    }

    sai_status_t _restoration_set_queue_attribute(sai_object_id_t queue_id, const sai_attribute_t *attr)
    {
        if (attr->id == SAI_QUEUE_ATTR_ENABLE_PFC_DLDR)
        {
            return SAI_STATUS_SUCCESS;
        }
        return restoration_old_sai_queue_api->set_queue_attribute(queue_id, attr);
    }

    sai_status_t _restoration_set_switch_attribute(sai_object_id_t switch_id, const sai_attribute_t *attr)
    {
        if (attr->id == SAI_SWITCH_ATTR_PFC_DLR_PACKET_ACTION)
        {
            return SAI_STATUS_SUCCESS;
        }
        return restoration_old_sai_switch_api->set_switch_attribute(switch_id, attr);
    }

    class PfcWdHwRestorationTest : public PfcWdHwStatHistoryTest
    {
    protected:
        shared_ptr<swss::DBConnector> m_state_db;

        void SetUp() override
        {
            PfcWdHwStatHistoryTest::SetUp();

            restoration_sai_port_api = *sai_port_api;
            restoration_old_sai_port_api = sai_port_api;
            restoration_sai_port_api.set_port_attribute = _restoration_set_port_attribute;
            restoration_sai_port_api.get_port_attribute = _restoration_get_port_attribute;
            sai_port_api = &restoration_sai_port_api;

            restoration_sai_queue_api = *sai_queue_api;
            restoration_old_sai_queue_api = sai_queue_api;
            restoration_sai_queue_api.set_queue_attribute = _restoration_set_queue_attribute;
            sai_queue_api = &restoration_sai_queue_api;

            restoration_sai_switch_api = *sai_switch_api;
            restoration_old_sai_switch_api = sai_switch_api;
            restoration_sai_switch_api.set_switch_attribute = _restoration_set_switch_attribute;
            sai_switch_api = &restoration_sai_switch_api;

            m_state_db = make_shared<swss::DBConnector>("STATE_DB", 0);
            dlr_written = false;
            programmed_dlr_ms = PLATFORM_DLR_MS;
        }

        void TearDown() override
        {
            sai_port_api = restoration_old_sai_port_api;
            sai_queue_api = restoration_old_sai_queue_api;
            sai_switch_api = restoration_old_sai_switch_api;
            PfcWdHwStatHistoryTest::TearDown();
        }

        bool hwState(const string &field, string *value)
        {
            swss::Table hwState(m_state_db.get(), STATE_PFC_WD_HW_STATE_TABLE_NAME);
            return hwState.hget("Ethernet0", field, *value);
        }
    };

    TEST_F(PfcWdHwRestorationTest, StartsWithoutRestorationTime)
    {
        auto status = m_pfcwd_hw_orch->createEntry("Ethernet0", {
            { PFC_WD_ACTION, "drop" },
            { PFC_WD_DETECTION_TIME, "400" },
        });

        ASSERT_EQ(status, task_process_status::task_success);
        EXPECT_FALSE(dlr_written);

        string value;
        ASSERT_TRUE(hwState("status", &value));
        EXPECT_EQ(value, "configured");
        ASSERT_TRUE(hwState("hw_restoration_time", &value));
        EXPECT_EQ(value, to_string(PLATFORM_DLR_MS));
        EXPECT_FALSE(hwState("configured_restoration_time", &value));
    }

    TEST_F(PfcWdHwRestorationTest, ProgramsConfiguredRestorationTime)
    {
        auto status = m_pfcwd_hw_orch->createEntry("Ethernet0", {
            { PFC_WD_ACTION, "drop" },
            { PFC_WD_DETECTION_TIME, "400" },
            { PFC_WD_RESTORATION_TIME, "600" },
        });

        ASSERT_EQ(status, task_process_status::task_success);
        EXPECT_TRUE(dlr_written);
        EXPECT_EQ(programmed_dlr_ms, 600u);

        string value;
        ASSERT_TRUE(hwState("configured_restoration_time", &value));
        EXPECT_EQ(value, "600");
        ASSERT_TRUE(hwState("hw_restoration_time", &value));
        EXPECT_EQ(value, "600");
    }
}
