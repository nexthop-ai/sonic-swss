#include <algorithm>
#include <string>
#include <vector>
#include <map>
#include <set>

#include "ut_helper.h"
#include "mock_orchagent_main.h"

using namespace swss;

namespace copporch_test
{
    sai_hostif_api_t ut_sai_hostif_api;
    sai_hostif_api_t *pold_sai_hostif_api;
    sai_hostif_trap_type_t rejected_trap_type;
    sai_object_id_t rejected_trap_oid;
    sai_object_id_t rejected_table_entry_oid;
    sai_object_id_t rejected_removal_oid;
    sai_hostif_trap_type_t rejected_table_trap_type;
    sai_status_t rejected_trap_status;
    std::set<sai_object_id_t> set_trap_oids;
    std::map<sai_object_id_t, sai_object_id_t> table_entry_traps;

    sai_status_t _ut_stub_sai_create_hostif_trap(
        _Out_ sai_object_id_t *hostif_trap_id,
        _In_ sai_object_id_t switch_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
    {
        for (uint32_t i = 0; i < attr_count; i++)
        {
            if (attr_list[i].id == SAI_HOSTIF_TRAP_ATTR_TRAP_TYPE &&
                attr_list[i].value.s32 == rejected_trap_type)
            {
                return rejected_trap_status;
            }
        }
        return pold_sai_hostif_api->create_hostif_trap(hostif_trap_id, switch_id, attr_count, attr_list);
    }

    sai_status_t _ut_stub_sai_set_hostif_trap_attribute(
        _In_ sai_object_id_t hostif_trap_id,
        _In_ const sai_attribute_t *attr)
    {
        if (hostif_trap_id == rejected_trap_oid)
        {
            return rejected_trap_status;
        }
        set_trap_oids.insert(hostif_trap_id);
        return pold_sai_hostif_api->set_hostif_trap_attribute(hostif_trap_id, attr);
    }

    sai_status_t _ut_stub_sai_remove_hostif_trap(
        _In_ sai_object_id_t hostif_trap_id)
    {
        // As in SAI, a trap can't be removed while a table entry still names it.
        for (const auto &entry : table_entry_traps)
        {
            if (entry.second == hostif_trap_id)
            {
                return SAI_STATUS_OBJECT_IN_USE;
            }
        }
        if (hostif_trap_id == rejected_trap_oid || hostif_trap_id == rejected_removal_oid)
        {
            return rejected_trap_status;
        }
        return pold_sai_hostif_api->remove_hostif_trap(hostif_trap_id);
    }

    // Fakes the entries, so a test's host interface can be a seeded OID with no SAI object behind it.
    sai_status_t _ut_stub_sai_create_hostif_table_entry(
        _Out_ sai_object_id_t *hostif_table_entry_id,
        _In_ sai_object_id_t switch_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
    {
        sai_object_id_t trap_oid = SAI_NULL_OBJECT_ID;
        for (uint32_t i = 0; i < attr_count; i++)
        {
            if (attr_list[i].id == SAI_HOSTIF_TABLE_ENTRY_ATTR_TRAP_ID)
            {
                trap_oid = attr_list[i].value.oid;
            }
        }
        if (trap_oid == rejected_trap_oid)
        {
            return rejected_trap_status;
        }
        if (rejected_table_trap_type != SAI_HOSTIF_TRAP_TYPE_END)
        {
            sai_attribute_t type_attr;
            type_attr.id = SAI_HOSTIF_TRAP_ATTR_TRAP_TYPE;
            if (pold_sai_hostif_api->get_hostif_trap_attribute(trap_oid, 1, &type_attr) == SAI_STATUS_SUCCESS &&
                type_attr.value.s32 == rejected_table_trap_type)
            {
                return rejected_trap_status;
            }
        }
        static sai_object_id_t next_entry_id = 0x1000;
        *hostif_table_entry_id = next_entry_id++;
        table_entry_traps[*hostif_table_entry_id] = trap_oid;
        return SAI_STATUS_SUCCESS;
    }

    sai_status_t _ut_stub_sai_remove_hostif_table_entry(
        _In_ sai_object_id_t hostif_table_entry_id)
    {
        if (hostif_table_entry_id == rejected_table_entry_oid)
        {
            return rejected_trap_status;
        }
        table_entry_traps.erase(hostif_table_entry_id);
        return SAI_STATUS_SUCCESS;
    }

    // Pass the trap's OID to also reject attribute sets on it, its removal, and table entries for it once it exists.
    // With no arguments, nothing is rejected.
    void _hook_sai_hostif_api(sai_hostif_trap_type_t trap_type = SAI_HOSTIF_TRAP_TYPE_END,
                              sai_status_t status = SAI_STATUS_FAILURE,
                              sai_object_id_t trap_oid = SAI_NULL_OBJECT_ID)
    {
        rejected_trap_type = trap_type;
        rejected_trap_oid = trap_oid;
        rejected_table_entry_oid = SAI_NULL_OBJECT_ID;
        rejected_removal_oid = SAI_NULL_OBJECT_ID;
        rejected_table_trap_type = SAI_HOSTIF_TRAP_TYPE_END;
        rejected_trap_status = status;
        set_trap_oids.clear();
        table_entry_traps.clear();
        ut_sai_hostif_api = *sai_hostif_api;
        pold_sai_hostif_api = sai_hostif_api;
        ut_sai_hostif_api.create_hostif_trap = _ut_stub_sai_create_hostif_trap;
        ut_sai_hostif_api.set_hostif_trap_attribute = _ut_stub_sai_set_hostif_trap_attribute;
        ut_sai_hostif_api.remove_hostif_trap = _ut_stub_sai_remove_hostif_trap;
        ut_sai_hostif_api.create_hostif_table_entry = _ut_stub_sai_create_hostif_table_entry;
        ut_sai_hostif_api.remove_hostif_table_entry = _ut_stub_sai_remove_hostif_table_entry;
        sai_hostif_api = &ut_sai_hostif_api;
    }

    void _unhook_sai_hostif_api()
    {
        sai_hostif_api = pold_sai_hostif_api;
    }

    sai_policer_api_t ut_sai_policer_api;
    sai_policer_api_t *pold_sai_policer_api;
    sai_attr_id_t rejected_policer_attr_id;
    sai_status_t rejected_policer_status;

    sai_status_t _ut_stub_sai_set_policer_attribute(
        _In_ sai_object_id_t policer_id,
        _In_ const sai_attribute_t *attr)
    {
        if (attr->id == rejected_policer_attr_id)
        {
            return rejected_policer_status;
        }
        return pold_sai_policer_api->set_policer_attribute(policer_id, attr);
    }

    void _hook_sai_policer_api(sai_attr_id_t attr_id, sai_status_t status)
    {
        rejected_policer_attr_id = attr_id;
        rejected_policer_status = status;
        ut_sai_policer_api = *sai_policer_api;
        pold_sai_policer_api = sai_policer_api;
        ut_sai_policer_api.set_policer_attribute = _ut_stub_sai_set_policer_attribute;
        sai_policer_api = &ut_sai_policer_api;
    }

    void _unhook_sai_policer_api()
    {
        sai_policer_api = pold_sai_policer_api;
    }

    class MockCoppOrch final
    {
    public:
        MockCoppOrch()
        {
            this->appDb = std::make_shared<DBConnector>("APPL_DB", 0);
            this->coppOrch = std::make_shared<CoppOrch>(this->appDb.get(), APP_COPP_TABLE_NAME);
        }
        ~MockCoppOrch() = default;

        void doCoppTableTask(const std::deque<KeyOpFieldsValuesTuple> &entries)
        {
            // ConsumerStateTable is used for APP DB
            auto consumer = std::unique_ptr<Consumer>(new Consumer(
                new ConsumerStateTable(this->appDb.get(), APP_COPP_TABLE_NAME, 1, 1),
                this->coppOrch.get(), APP_COPP_TABLE_NAME
            ));

            consumer->addToSync(entries);
            static_cast<Orch*>(this->coppOrch.get())->doTask(*consumer);
        }

        task_process_status doProcessCoppRule(const std::deque<KeyOpFieldsValuesTuple> &entries)
        {
            // ConsumerStateTable is used for APP DB
            auto consumer = std::unique_ptr<Consumer>(new Consumer(
                new ConsumerStateTable(this->appDb.get(), APP_COPP_TABLE_NAME, 1, 1),
                this->coppOrch.get(), APP_COPP_TABLE_NAME
            ));

            consumer->addToSync(entries);
            return Portal::CoppOrchInternal::processCoppRule(*coppOrch, *consumer);
        }

        CoppOrch& get()
        {
            return *coppOrch;
        }

    private:
        std::shared_ptr<CoppOrch> coppOrch;
        std::shared_ptr<DBConnector> appDb;
    };

    class CoppOrchTest : public ::testing::Test
    {
    public:
        CoppOrchTest()
        {
            this->initDb();
        }
        virtual ~CoppOrchTest() = default;

        void SetUp() override
        {
            this->initSaiApi();
            this->initSwitch();
            this->initOrch();
            this->initPorts();
        }

        void TearDown() override
        {
            this->deinitOrch();
            this->deinitSwitch();
            this->deinitSaiApi();
        }

    private:
        void initSaiApi()
        {
            std::map<std::string, std::string> profileMap = {
                { "SAI_VS_SWITCH_TYPE", "SAI_VS_SWITCH_TYPE_BCM56850" },
                { "KV_DEVICE_MAC_ADDRESS", "20:03:04:05:06:00"        }
            };
            auto status = ut_helper::initSaiApi(profileMap);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);
        }

        void deinitSaiApi()
        {
            auto status = ut_helper::uninitSaiApi();
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);
        }

        void initSwitch()
        {
            sai_status_t status;
            sai_attribute_t attr;

            // Create switch
            attr.id = SAI_SWITCH_ATTR_INIT_SWITCH;
            attr.value.booldata = true;

            status = sai_switch_api->create_switch(&gSwitchId, 1, &attr);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);

            // Get switch source MAC address
            attr.id = SAI_SWITCH_ATTR_SRC_MAC_ADDRESS;

            status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);

            gMacAddress = attr.value.mac;

            // Get switch default virtual router ID
            attr.id = SAI_SWITCH_ATTR_DEFAULT_VIRTUAL_ROUTER_ID;

            status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);

            gVirtualRouterId = attr.value.oid;
        }

        void deinitSwitch()
        {
            // Remove switch
            auto status = sai_switch_api->remove_switch(gSwitchId);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);

            gSwitchId = SAI_NULL_OBJECT_ID;
            gVirtualRouterId = SAI_NULL_OBJECT_ID;
        }

        void initOrch()
        {
            //
            // SwitchOrch
            //

            TableConnector switchCapTableStateDb(this->stateDb.get(), "SWITCH_CAPABILITY");
            TableConnector asicSensorsTableCfgDb(this->configDb.get(), CFG_ASIC_SENSORS_TABLE_NAME);
            TableConnector switchTableAppDb(this->appDb.get(), APP_SWITCH_TABLE_NAME);

            std::vector<TableConnector> switchTableList = {
                asicSensorsTableCfgDb,
                switchTableAppDb
            };

            gSwitchOrch = new SwitchOrch(this->appDb.get(), switchTableList, switchCapTableStateDb);
            gDirectory.set(gSwitchOrch);
            resourcesList.push_back(gSwitchOrch);

            //
            // PortsOrch
            //

            const int portsorchBasePri = 40;

            std::vector<table_name_with_pri_t> portTableList = {
                { APP_PORT_TABLE_NAME,        portsorchBasePri + 5 },
                { APP_VLAN_TABLE_NAME,        portsorchBasePri + 2 },
                { APP_VLAN_MEMBER_TABLE_NAME, portsorchBasePri     },
                { APP_LAG_TABLE_NAME,         portsorchBasePri + 4 },
                { APP_LAG_MEMBER_TABLE_NAME,  portsorchBasePri     }
            };

            gPortsOrch = new PortsOrch(this->appDb.get(), this->stateDb.get(), portTableList, this->chassisAppDb.get());
            gDirectory.set(gPortsOrch);
            resourcesList.push_back(gPortsOrch);

            //
            // QosOrch
            //

            std::vector<std::string> qosTableList = {
                CFG_TC_TO_QUEUE_MAP_TABLE_NAME,
                CFG_SCHEDULER_TABLE_NAME,
                CFG_DSCP_TO_TC_MAP_TABLE_NAME,
                CFG_MPLS_TC_TO_TC_MAP_TABLE_NAME,
                CFG_DOT1P_TO_TC_MAP_TABLE_NAME,
                CFG_QUEUE_TABLE_NAME,
                CFG_PORT_QOS_MAP_TABLE_NAME,
                CFG_WRED_PROFILE_TABLE_NAME,
                CFG_TC_TO_PRIORITY_GROUP_MAP_TABLE_NAME,
                CFG_PFC_PRIORITY_TO_PRIORITY_GROUP_MAP_TABLE_NAME,
                CFG_PFC_PRIORITY_TO_QUEUE_MAP_TABLE_NAME,
                CFG_DSCP_TO_FC_MAP_TABLE_NAME,
                CFG_EXP_TO_FC_MAP_TABLE_NAME
            };
            gQosOrch = new QosOrch(this->configDb.get(), qosTableList);
            gDirectory.set(gQosOrch);
            resourcesList.push_back(gQosOrch);

            //
            // BufferOrch
            //

            std::vector<std::string> bufferTableList = {
                APP_BUFFER_POOL_TABLE_NAME,
                APP_BUFFER_PROFILE_TABLE_NAME,
                APP_BUFFER_QUEUE_TABLE_NAME,
                APP_BUFFER_PG_TABLE_NAME,
                APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME,
                APP_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME
            };
            gBufferOrch = new BufferOrch(this->appDb.get(), this->configDb.get(), this->stateDb.get(), bufferTableList);
            gDirectory.set(gBufferOrch);
            resourcesList.push_back(gBufferOrch);

            //
            // PolicerOrch
            //

            vector<TableConnector> policer_tables = {
                TableConnector(this->configDb.get(), CFG_POLICER_TABLE_NAME),
                TableConnector(this->configDb.get(), CFG_PORT_STORM_CONTROL_TABLE_NAME)
            };
            auto policerOrch = new PolicerOrch(policer_tables, gPortsOrch);
            gDirectory.set(policerOrch);
            resourcesList.push_back(policerOrch);

            //
            // FlexCounterOrch
            //

            std::vector<std::string> flexCounterTableList = {
                CFG_FLEX_COUNTER_TABLE_NAME
            };

            auto flexCounterOrch = new FlexCounterOrch(this->configDb.get(), flexCounterTableList);
            gDirectory.set(flexCounterOrch);
            resourcesList.push_back(flexCounterOrch);
        }

        void deinitOrch()
        {
            std::reverse(this->resourcesList.begin(), this->resourcesList.end());
            for (auto &it : this->resourcesList)
            {
                delete it;
            }

            gSwitchOrch = nullptr;
            gPortsOrch = nullptr;
            gQosOrch = nullptr;
            gBufferOrch = nullptr;

            Portal::DirectoryInternal::clear(gDirectory);
            EXPECT_TRUE(Portal::DirectoryInternal::empty(gDirectory));
        }

        void initPorts()
        {
            auto portTable = Table(this->appDb.get(), APP_PORT_TABLE_NAME);

            // Get SAI default ports to populate DB
            auto ports = ut_helper::getInitialSaiPorts();

            // Populate port table with SAI ports
            for (const auto &cit : ports)
            {
                portTable.set(cit.first, cit.second);
            }

            // Set PortConfigDone
            portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });
            gPortsOrch->addExistingData(&portTable);
            static_cast<Orch*>(gPortsOrch)->doTask();

            // Set PortInitDone
            portTable.set("PortInitDone", { { "lanes", "0" } });
            gPortsOrch->addExistingData(&portTable);
            static_cast<Orch*>(gPortsOrch)->doTask();
        }

        void initDb()
        {
            this->appDb = std::make_shared<swss::DBConnector>("APPL_DB", 0);
            this->configDb = std::make_shared<swss::DBConnector>("CONFIG_DB", 0);
            this->stateDb = std::make_shared<swss::DBConnector>("STATE_DB", 0);
            this->chassisAppDb = std::make_shared<swss::DBConnector>("CHASSIS_APP_DB", 0);
        }

        std::shared_ptr<DBConnector> appDb;
        std::shared_ptr<DBConnector> configDb;
        std::shared_ptr<DBConnector> stateDb;
        std::shared_ptr<DBConnector> chassisAppDb;

        std::vector<Orch*> resourcesList;
    };

    TEST_F(CoppOrchTest, VerifySupportedTrapIds)
    {
        MockCoppOrch coppOrch;

        const auto &supportedTrapIds = Portal::CoppOrchInternal::getSupportedTrapIds(coppOrch.get());
        EXPECT_TRUE(supportedTrapIds.find(SAI_HOSTIF_TRAP_TYPE_IP2ME) != supportedTrapIds.end());
        EXPECT_TRUE(supportedTrapIds.find(SAI_HOSTIF_TRAP_TYPE_NEIGHBOR_MISS) != supportedTrapIds.end());
    }

    TEST_F(CoppOrchTest, TrapGroup_AddRemove)
    {
        const std::string trapGroupName = "queue4_group1";

        MockCoppOrch coppOrch;

        // Create CoPP Trap Group
        {
            auto tableKofvt = std::deque<KeyOpFieldsValuesTuple>(
                {
                    {
                        trapGroupName,
                        SET_COMMAND,
                        {
                            { copp_trap_action_field,   "trap" },
                            { copp_trap_priority_field, "4"    },
                            { copp_queue_field,         "4"    }
                        }
                    }
                }
            );
            coppOrch.doCoppTableTask(tableKofvt);

            const auto &trapGroupMap = coppOrch.get().getTrapGroupMap();
            const auto &cit = trapGroupMap.find(trapGroupName);
            EXPECT_TRUE(cit != trapGroupMap.end());
        }

        // Delete CoPP Trap Group
        {
            auto tableKofvt = std::deque<KeyOpFieldsValuesTuple>(
                { { trapGroupName, DEL_COMMAND, { } } }
            );
            coppOrch.doCoppTableTask(tableKofvt);

            const auto &trapGroupMap = coppOrch.get().getTrapGroupMap();
            const auto &cit = trapGroupMap.find(trapGroupName);
            EXPECT_TRUE(cit == trapGroupMap.end());
        }
    }

    TEST_F(CoppOrchTest, TrapGroupWithPolicer_AddUpdateRemove)
    {
        const std::string trapGroupName = "queue4_group2";

        MockCoppOrch coppOrch;

        // Create CoPP Trap Group
        {
            auto tableKofvt = std::deque<KeyOpFieldsValuesTuple>(
                {
                    {
                        trapGroupName,
                        SET_COMMAND,
                        {
                            { copp_trap_action_field,        "copy"    },
                            { copp_trap_priority_field,      "4"       },
                            { copp_queue_field,              "4"       },
                            { copp_policer_meter_type_field, "packets" },
                            { copp_policer_mode_field,       "sr_tcm"  },
                            { copp_policer_color_field,      "aware"   },
                            { copp_policer_cir_field,        "600"     },
                            { copp_policer_cbs_field,        "600"     },
                            { copp_policer_action_red_field, "drop"    }
                        }
                    }
                }
            );
            coppOrch.doCoppTableTask(tableKofvt);

            const auto &trapGroupMap = coppOrch.get().getTrapGroupMap();
            const auto &cit1 = trapGroupMap.find(trapGroupName);
            EXPECT_TRUE(cit1 != trapGroupMap.end());

            const auto &trapGroupPolicerMap = Portal::CoppOrchInternal::getTrapGroupPolicerMap(coppOrch.get());
            const auto &trapGroupOid = cit1->second;
            const auto &cit2 = trapGroupPolicerMap.find(trapGroupOid);
            EXPECT_TRUE(cit2 != trapGroupPolicerMap.end());
            EXPECT_TRUE(cit2->second.meter == SAI_METER_TYPE_PACKETS);
            EXPECT_TRUE(cit2->second.mode == SAI_POLICER_MODE_SR_TCM);

            /* Update the non create only attributes */
            auto tableKofvt2 = std::deque<KeyOpFieldsValuesTuple>(
                {
                    {
                        trapGroupName,
                        SET_COMMAND,
                        {
                            { copp_policer_cir_field,        "1000"    },
                            { copp_policer_cbs_field,        "1000"   },
                            { copp_policer_action_red_field, "drop"    }
                        }
                    }
                }
            );
            ASSERT_EQ(coppOrch.doProcessCoppRule(tableKofvt2), task_process_status::task_success);
        }


        // Delete CoPP Trap Group
        {
            auto tableKofvt = std::deque<KeyOpFieldsValuesTuple>(
                { { trapGroupName, DEL_COMMAND, { } } }
            );
            coppOrch.doCoppTableTask(tableKofvt);

            const auto &trapGroupMap = coppOrch.get().getTrapGroupMap();
            const auto &cit = trapGroupMap.find(trapGroupName);
            EXPECT_TRUE(cit == trapGroupMap.end());

            const auto &trapGroupPolicerMap = Portal::CoppOrchInternal::getTrapGroupPolicerMap(coppOrch.get());
            EXPECT_TRUE(trapGroupPolicerMap.empty());
        }
    }

    TEST_F(CoppOrchTest, TrapGroupWithPolicer_nothrowExec)
    {
        const std::string trapGroupName = "queue4_group2";

        MockCoppOrch coppOrch;

        {
            // Create CoPP Trap Group
            auto tableKofvt = std::deque<KeyOpFieldsValuesTuple>(
                {
                    {
                        trapGroupName,
                        SET_COMMAND,
                        {
                            { copp_trap_action_field,        "copy"    },
                            { copp_trap_priority_field,      "4"       },
                            { copp_queue_field,              "4"       },
                            { copp_policer_meter_type_field, "packets" },
                            { copp_policer_mode_field,       "sr_tcm"  },
                            { copp_policer_cir_field,        "600"     },
                            { copp_policer_cbs_field,        "600"     },
                            { copp_policer_action_red_field, "drop"    }
                        }
                    }
                }
            );
            coppOrch.doCoppTableTask(tableKofvt);

            // Update create-only Policer Attributes
            auto tableKofvt2 = std::deque<KeyOpFieldsValuesTuple>(
                {
                    {
                        trapGroupName,
                        SET_COMMAND,
                        {
                            { copp_policer_meter_type_field, "bytes"    },
                            { copp_policer_mode_field,       "tr_tcm"   },
                            { copp_policer_color_field,      "blind"    },
                        }
                    }
                }
            );
            EXPECT_NO_THROW(coppOrch.doProcessCoppRule(tableKofvt2));
        }
    }

    // cir is set before cbs and rejected, so a rejection that stops the loop leaves cbs at its old value.
    TEST_F(CoppOrchTest, TrapGroup_PolicerUpdateContinuesPastRejectedAttribute)
    {
        const std::string trapGroupName = "queue4_group2";

        MockCoppOrch coppOrch;

        auto addKofvt = std::deque<KeyOpFieldsValuesTuple>(
            {
                {
                    trapGroupName,
                    SET_COMMAND,
                    {
                        { copp_trap_action_field,        "copy"    },
                        { copp_trap_priority_field,      "4"       },
                        { copp_queue_field,              "4"       },
                        { copp_policer_meter_type_field, "packets" },
                        { copp_policer_mode_field,       "sr_tcm"  },
                        { copp_policer_cir_field,        "600"     },
                        { copp_policer_cbs_field,        "600"     },
                        { copp_policer_action_red_field, "drop"    }
                    }
                }
            }
        );
        ASSERT_EQ(coppOrch.doProcessCoppRule(addKofvt), task_process_status::task_success);

        const auto trapGroupMap = coppOrch.get().getTrapGroupMap();
        const auto cit = trapGroupMap.find(trapGroupName);
        ASSERT_TRUE(cit != trapGroupMap.end());
        const auto policerId = Portal::CoppOrchInternal::getTrapGroupPolicerMap(coppOrch.get())[cit->second].policer_id;

        auto updateKofvt = std::deque<KeyOpFieldsValuesTuple>(
            {
                {
                    trapGroupName,
                    SET_COMMAND,
                    {
                        { copp_policer_cir_field, "1000" },
                        { copp_policer_cbs_field, "1000" }
                    }
                }
            }
        );

        _hook_sai_policer_api(SAI_POLICER_ATTR_CIR, SAI_STATUS_NOT_SUPPORTED);
        auto taskStatus = coppOrch.doProcessCoppRule(updateKofvt);
        _unhook_sai_policer_api();

        EXPECT_EQ(taskStatus, task_process_status::task_success);

        sai_attribute_t attr;
        attr.id = SAI_POLICER_ATTR_CBS;
        ASSERT_EQ(sai_policer_api->get_policer_attribute(policerId, 1, &attr), SAI_STATUS_SUCCESS);
        EXPECT_EQ(attr.value.u64, 1000);
    }

    TEST_F(CoppOrchTest, Trap_AddRemove)
    {
        const std::string trapGroupName = "queue4_group1";
        const std::string trapNameList = "bgp,bgpv6,neighbor_miss";
        const std::set<sai_hostif_trap_type_t> trapIDSet = {
            SAI_HOSTIF_TRAP_TYPE_BGP,
            SAI_HOSTIF_TRAP_TYPE_BGPV6,
            SAI_HOSTIF_TRAP_TYPE_NEIGHBOR_MISS
        };

        MockCoppOrch coppOrch;

        // Create CoPP Trap
        {
            auto tableKofvt = std::deque<KeyOpFieldsValuesTuple>(
                {
                    {
                        trapGroupName,
                        SET_COMMAND,
                        {
                            { copp_trap_action_field,   "trap"       },
                            { copp_trap_priority_field, "4"          },
                            { copp_queue_field,         "4"          },
                            { copp_trap_id_list,        trapNameList }
                        }
                    }
                }
            );
            coppOrch.doCoppTableTask(tableKofvt);

            const auto &trapGroupMap = coppOrch.get().getTrapGroupMap();
            const auto &cit = trapGroupMap.find(trapGroupName);
            EXPECT_TRUE(cit != trapGroupMap.end());

            const auto &tgOid = cit->second;
            const auto &tidList = Portal::CoppOrchInternal::getTrapIdsFromTrapGroup(coppOrch.get(), tgOid);
            const auto &tidSet = std::set<sai_hostif_trap_type_t>(tidList.begin(), tidList.end());

            // Verify that bgp, bgpv6 and neighbor_miss are installed
            EXPECT_TRUE(trapIDSet == tidSet);
        }

        // Delete CoPP Trap
        {
            auto tableKofvt = std::deque<KeyOpFieldsValuesTuple>(
                { { trapGroupName, DEL_COMMAND, { } } }
            );
            coppOrch.doCoppTableTask(tableKofvt);

            const auto &trapGroupMap = coppOrch.get().getTrapGroupMap();
            const auto &cit1 = trapGroupMap.find(trapGroupName);
            EXPECT_TRUE(cit1 == trapGroupMap.end());

            const auto &trapGroupIdMap = Portal::CoppOrchInternal::getTrapGroupIdMap(coppOrch.get());
            const auto &cit2 = trapGroupIdMap.find(SAI_HOSTIF_TRAP_TYPE_TTL_ERROR);
            EXPECT_TRUE(cit2 != trapGroupIdMap.end());
            ASSERT_EQ(trapGroupIdMap.size(), 1);
        }
    }

    TEST_F(CoppOrchTest, TrapWithPolicer_AddRemove)
    {
        const std::string trapGroupName = "queue4_group2";
        const std::string trapNameList = "arp_req,arp_resp,neigh_discovery";
        const std::set<sai_hostif_trap_type_t> trapIDSet = {
            SAI_HOSTIF_TRAP_TYPE_ARP_REQUEST,
            SAI_HOSTIF_TRAP_TYPE_ARP_RESPONSE,
            SAI_HOSTIF_TRAP_TYPE_IPV6_NEIGHBOR_DISCOVERY
        };

        MockCoppOrch coppOrch;

        // Create CoPP Trap
        {
            auto tableKofvt = std::deque<KeyOpFieldsValuesTuple>(
                {
                    {
                        trapGroupName,
                        SET_COMMAND,
                        {
                            { copp_trap_action_field,        "copy"       },
                            { copp_trap_priority_field,      "4"          },
                            { copp_queue_field,              "4"          },
                            { copp_policer_meter_type_field, "packets"    },
                            { copp_policer_mode_field,       "sr_tcm"     },
                            { copp_policer_cir_field,        "600"        },
                            { copp_policer_cbs_field,        "600"        },
                            { copp_policer_action_red_field, "drop"       },
                            { copp_trap_id_list,             trapNameList }
                        }
                    }
                }
            );
            coppOrch.doCoppTableTask(tableKofvt);

            const auto &trapGroupMap = coppOrch.get().getTrapGroupMap();
            const auto &cit1 = trapGroupMap.find(trapGroupName);
            EXPECT_TRUE(cit1 != trapGroupMap.end());

            const auto &trapGroupPolicerMap = Portal::CoppOrchInternal::getTrapGroupPolicerMap(coppOrch.get());
            const auto &trapGroupOid = cit1->second;
            const auto &cit2 = trapGroupPolicerMap.find(trapGroupOid);
            EXPECT_TRUE(cit2 != trapGroupPolicerMap.end());

            const auto &tidList = Portal::CoppOrchInternal::getTrapIdsFromTrapGroup(coppOrch.get(), trapGroupOid);
            const auto &tidSet = std::set<sai_hostif_trap_type_t>(tidList.begin(), tidList.end());
            EXPECT_TRUE(trapIDSet == tidSet);
        }

        // Delete CoPP Trap
        {
            auto tableKofvt = std::deque<KeyOpFieldsValuesTuple>(
                { { trapGroupName, DEL_COMMAND, { } } }
            );
            coppOrch.doCoppTableTask(tableKofvt);

            const auto &trapGroupMap = coppOrch.get().getTrapGroupMap();
            const auto &cit1 = trapGroupMap.find(trapGroupName);
            EXPECT_TRUE(cit1 == trapGroupMap.end());

            const auto &trapGroupPolicerMap = Portal::CoppOrchInternal::getTrapGroupPolicerMap(coppOrch.get());
            EXPECT_TRUE(trapGroupPolicerMap.empty());

            const auto &trapGroupIdMap = Portal::CoppOrchInternal::getTrapGroupIdMap(coppOrch.get());
            const auto &cit2 = trapGroupIdMap.find(SAI_HOSTIF_TRAP_TYPE_TTL_ERROR);
            EXPECT_TRUE(cit2 != trapGroupIdMap.end());
            ASSERT_EQ(trapGroupIdMap.size(), 1);
        }
    }

    // bgp is listed first and rejected, so a rejection that stops the loop leaves nothing installed.
    void addTrapsWithFirstRejected(sai_status_t rejectStatus, task_process_status expectedTaskStatus)
    {
        const std::string trapGroupName = "queue4_group1";
        const std::string trapNameList = "bgp,bgpv6,neighbor_miss";
        const std::set<sai_hostif_trap_type_t> trapIDSet = {
            SAI_HOSTIF_TRAP_TYPE_BGPV6,
            SAI_HOSTIF_TRAP_TYPE_NEIGHBOR_MISS
        };

        MockCoppOrch coppOrch;

        auto tableKofvt = std::deque<KeyOpFieldsValuesTuple>(
            {
                {
                    trapGroupName,
                    SET_COMMAND,
                    {
                        { copp_trap_action_field,   "trap"       },
                        { copp_trap_priority_field, "4"          },
                        { copp_queue_field,         "4"          },
                        { copp_trap_id_list,        trapNameList }
                    }
                }
            }
        );

        _hook_sai_hostif_api(SAI_HOSTIF_TRAP_TYPE_BGP, rejectStatus);
        auto taskStatus = coppOrch.doProcessCoppRule(tableKofvt);
        _unhook_sai_hostif_api();

        EXPECT_EQ(taskStatus, expectedTaskStatus);

        const auto &trapGroupMap = coppOrch.get().getTrapGroupMap();
        const auto &cit = trapGroupMap.find(trapGroupName);
        ASSERT_TRUE(cit != trapGroupMap.end());

        const auto &tidList = Portal::CoppOrchInternal::getTrapIdsFromTrapGroup(coppOrch.get(), cit->second);
        const auto &tidSet = std::set<sai_hostif_trap_type_t>(tidList.begin(), tidList.end());
        EXPECT_TRUE(trapIDSet == tidSet);
    }

    TEST_F(CoppOrchTest, Trap_AddContinuesPastUnsupportedTrap)
    {
        addTrapsWithFirstRejected(SAI_STATUS_NOT_IMPLEMENTED, task_process_status::task_success);
    }

    TEST_F(CoppOrchTest, Trap_AddContinuesPastRetryableFailure)
    {
        addTrapsWithFirstRejected(SAI_STATUS_INSUFFICIENT_RESOURCES, task_process_status::task_failed);
    }

    void addGenetlinkTrapsWithFirstRejected(sai_status_t rejectStatus)
    {
        const std::string trapGroupName = "queue2_group1";
        const std::string trapNameList = "bgp,bgpv6,neighbor_miss";
        const std::set<sai_hostif_trap_type_t> trapIDSet = {
            SAI_HOSTIF_TRAP_TYPE_BGPV6,
            SAI_HOSTIF_TRAP_TYPE_NEIGHBOR_MISS
        };

        MockCoppOrch coppOrch;

        auto groupKofvt = std::deque<KeyOpFieldsValuesTuple>(
            {
                {
                    trapGroupName,
                    SET_COMMAND,
                    {
                        { copp_trap_action_field,   "trap" },
                        { copp_trap_priority_field, "1"    },
                        { copp_queue_field,         "2"    }
                    }
                }
            }
        );
        coppOrch.doProcessCoppRule(groupKofvt);

        const auto &trapGroupMap = coppOrch.get().getTrapGroupMap();
        const auto &cit = trapGroupMap.find(trapGroupName);
        ASSERT_TRUE(cit != trapGroupMap.end());
        Portal::CoppOrchInternal::seedTrapGroupHostIf(coppOrch.get(), cit->second, 0x2000);

        auto trapKofvt = std::deque<KeyOpFieldsValuesTuple>(
            {
                {
                    trapGroupName,
                    SET_COMMAND,
                    {
                        { copp_trap_action_field,   "trap"       },
                        { copp_trap_priority_field, "1"          },
                        { copp_queue_field,         "2"          },
                        { copp_trap_id_list,        trapNameList }
                    }
                }
            }
        );

        _hook_sai_hostif_api(SAI_HOSTIF_TRAP_TYPE_BGP, rejectStatus);
        coppOrch.doProcessCoppRule(trapKofvt);
        _unhook_sai_hostif_api();

        std::set<sai_hostif_trap_type_t> tableEntrySet;
        for (const auto &entry : Portal::CoppOrchInternal::getTrapIdHostIfTableMap(coppOrch.get()))
        {
            tableEntrySet.insert(entry.first);
        }
        EXPECT_TRUE(trapIDSet == tableEntrySet);

        const auto &trapIdMap = Portal::CoppOrchInternal::getTrapGroupIdMap(coppOrch.get());
        EXPECT_EQ(trapIdMap.count(SAI_HOSTIF_TRAP_TYPE_BGP), 0);
    }

    TEST_F(CoppOrchTest, Trap_GenetlinkTableSkipsUnsupportedTrap)
    {
        addGenetlinkTrapsWithFirstRejected(SAI_STATUS_NOT_IMPLEMENTED);
    }

    TEST_F(CoppOrchTest, Trap_GenetlinkTableCoversTrapsAfterRetryableFailure)
    {
        addGenetlinkTrapsWithFirstRejected(SAI_STATUS_INSUFFICIENT_RESOURCES);
    }

    // neighbor_miss sorts first among the group's traps, so a rejection that stops the loop updates no other trap.
    void updateTrapsWithFirstRejected(MockCoppOrch &coppOrch, sai_status_t rejectStatus,
                                      task_process_status expectedTaskStatus)
    {
        const std::string trapGroupName = "queue4_group1";

        auto addKofvt = std::deque<KeyOpFieldsValuesTuple>(
            {
                {
                    trapGroupName,
                    SET_COMMAND,
                    {
                        { copp_trap_action_field,   "trap"                    },
                        { copp_trap_priority_field, "4"                       },
                        { copp_queue_field,         "4"                       },
                        { copp_trap_id_list,        "bgp,bgpv6,neighbor_miss" }
                    }
                }
            }
        );
        ASSERT_EQ(coppOrch.doProcessCoppRule(addKofvt), task_process_status::task_success);

        const auto trapIdMap = Portal::CoppOrchInternal::getTrapGroupIdMap(coppOrch.get());

        auto updateKofvt = std::deque<KeyOpFieldsValuesTuple>(
            {
                {
                    trapGroupName,
                    SET_COMMAND,
                    {
                        { copp_trap_action_field,   "trap"                            },
                        { copp_trap_priority_field, "5"                               },
                        { copp_queue_field,         "4"                               },
                        { copp_trap_id_list,        "arp_req,bgp,bgpv6,neighbor_miss" }
                    }
                }
            }
        );

        _hook_sai_hostif_api(SAI_HOSTIF_TRAP_TYPE_NEIGHBOR_MISS, rejectStatus,
                             trapIdMap.at(SAI_HOSTIF_TRAP_TYPE_NEIGHBOR_MISS).trap_obj);
        auto taskStatus = coppOrch.doProcessCoppRule(updateKofvt);
        _unhook_sai_hostif_api();

        EXPECT_EQ(taskStatus, expectedTaskStatus);

        const std::set<sai_object_id_t> updatedTrapOids = {
            trapIdMap.at(SAI_HOSTIF_TRAP_TYPE_BGP).trap_obj,
            trapIdMap.at(SAI_HOSTIF_TRAP_TYPE_BGPV6).trap_obj
        };
        EXPECT_TRUE(updatedTrapOids == set_trap_oids);
    }

    TEST_F(CoppOrchTest, Trap_AttrUpdateContinuesPastUnsupportedTrap)
    {
        MockCoppOrch coppOrch;
        updateTrapsWithFirstRejected(coppOrch, SAI_STATUS_NOT_IMPLEMENTED, task_process_status::task_failed);

        // arp_req joins the group in the same SET, with the new priority.
        const auto trapIdMap = Portal::CoppOrchInternal::getTrapGroupIdMap(coppOrch.get());
        ASSERT_EQ(trapIdMap.count(SAI_HOSTIF_TRAP_TYPE_ARP_REQUEST), 1);

        sai_attribute_t attr;
        attr.id = SAI_HOSTIF_TRAP_ATTR_TRAP_PRIORITY;
        ASSERT_EQ(sai_hostif_api->get_hostif_trap_attribute(
                      trapIdMap.at(SAI_HOSTIF_TRAP_TYPE_ARP_REQUEST).trap_obj, 1, &attr),
                  SAI_STATUS_SUCCESS);
        EXPECT_EQ(attr.value.u32, 5);
    }

    TEST_F(CoppOrchTest, Trap_AttrUpdateContinuesPastRetryableFailure)
    {
        MockCoppOrch coppOrch;
        updateTrapsWithFirstRejected(coppOrch, SAI_STATUS_INSUFFICIENT_RESOURCES, task_process_status::task_need_retry);
    }

    const std::vector<sai_hostif_trap_type_t> genetlinkTrapIds = {
        SAI_HOSTIF_TRAP_TYPE_BGP,
        SAI_HOSTIF_TRAP_TYPE_BGPV6,
        SAI_HOSTIF_TRAP_TYPE_NEIGHBOR_MISS
    };

    // Installs the traps first and attaches the host interface after, so no table entries exist yet.
    void addGenetlinkGroupTraps(MockCoppOrch &coppOrch)
    {
        const std::string trapGroupName = "queue2_group1";

        auto kofvt = std::deque<KeyOpFieldsValuesTuple>(
            {
                {
                    trapGroupName,
                    SET_COMMAND,
                    {
                        { copp_trap_action_field,   "trap"                    },
                        { copp_trap_priority_field, "1"                       },
                        { copp_queue_field,         "2"                       },
                        { copp_trap_id_list,        "bgp,bgpv6,neighbor_miss" }
                    }
                }
            }
        );
        ASSERT_EQ(coppOrch.doProcessCoppRule(kofvt), task_process_status::task_success);

        const auto &trapGroupMap = coppOrch.get().getTrapGroupMap();
        const auto &cit = trapGroupMap.find(trapGroupName);
        ASSERT_TRUE(cit != trapGroupMap.end());
        Portal::CoppOrchInternal::seedTrapGroupHostIf(coppOrch.get(), cit->second, 0x2000);
    }

    std::set<sai_hostif_trap_type_t> getGenetlinkTableTrapIds(MockCoppOrch &coppOrch)
    {
        std::set<sai_hostif_trap_type_t> tableEntrySet;
        for (const auto &entry : Portal::CoppOrchInternal::getTrapIdHostIfTableMap(coppOrch.get()))
        {
            tableEntrySet.insert(entry.first);
        }
        return tableEntrySet;
    }

    // bgp is first in the list and rejected, so a rejection that stops the loop creates no entries.
    void createGenetlinkTableWithFirstRejected(sai_status_t rejectStatus, bool expectedResult)
    {
        MockCoppOrch coppOrch;
        addGenetlinkGroupTraps(coppOrch);

        const auto trapIdMap = Portal::CoppOrchInternal::getTrapGroupIdMap(coppOrch.get());
        ASSERT_EQ(trapIdMap.count(SAI_HOSTIF_TRAP_TYPE_BGP), 1);

        _hook_sai_hostif_api(SAI_HOSTIF_TRAP_TYPE_BGP, rejectStatus,
                             trapIdMap.at(SAI_HOSTIF_TRAP_TYPE_BGP).trap_obj);
        auto result = Portal::CoppOrchInternal::createGenetlinkHostIfTable(coppOrch.get(), genetlinkTrapIds);
        _unhook_sai_hostif_api();

        EXPECT_EQ(result, expectedResult);

        const std::set<sai_hostif_trap_type_t> trapIDSet = {
            SAI_HOSTIF_TRAP_TYPE_BGPV6,
            SAI_HOSTIF_TRAP_TYPE_NEIGHBOR_MISS
        };
        EXPECT_TRUE(trapIDSet == getGenetlinkTableTrapIds(coppOrch));
    }

    TEST_F(CoppOrchTest, Trap_GenetlinkTableCreateContinuesPastUnsupportedTrap)
    {
        createGenetlinkTableWithFirstRejected(SAI_STATUS_NOT_IMPLEMENTED, true);
    }

    TEST_F(CoppOrchTest, Trap_GenetlinkTableCreateContinuesPastRetryableFailure)
    {
        createGenetlinkTableWithFirstRejected(SAI_STATUS_INSUFFICIENT_RESOURCES, false);
    }

    // bgp's entry is first in the list and its removal rejected, so a rejection that stops the loop removes nothing.
    void removeGenetlinkTableWithFirstRejected(sai_status_t rejectStatus, bool expectedResult)
    {
        MockCoppOrch coppOrch;
        addGenetlinkGroupTraps(coppOrch);

        _hook_sai_hostif_api(SAI_HOSTIF_TRAP_TYPE_BGP, rejectStatus);
        auto created = Portal::CoppOrchInternal::createGenetlinkHostIfTable(coppOrch.get(), genetlinkTrapIds);
        auto tableMap = Portal::CoppOrchInternal::getTrapIdHostIfTableMap(coppOrch.get());
        rejected_table_entry_oid = tableMap[SAI_HOSTIF_TRAP_TYPE_BGP];
        auto result = Portal::CoppOrchInternal::removeGenetlinkHostIfTable(coppOrch.get(), genetlinkTrapIds);
        _unhook_sai_hostif_api();

        ASSERT_TRUE(created);
        EXPECT_EQ(result, expectedResult);

        const std::set<sai_hostif_trap_type_t> trapIDSet = {
            SAI_HOSTIF_TRAP_TYPE_BGP
        };
        EXPECT_TRUE(trapIDSet == getGenetlinkTableTrapIds(coppOrch));
    }

    TEST_F(CoppOrchTest, Trap_GenetlinkTableRemoveContinuesPastUnsupportedTrap)
    {
        removeGenetlinkTableWithFirstRejected(SAI_STATUS_NOT_IMPLEMENTED, true);
    }

    TEST_F(CoppOrchTest, Trap_GenetlinkTableRemoveContinuesPastRetryableFailure)
    {
        removeGenetlinkTableWithFirstRejected(SAI_STATUS_OBJECT_IN_USE, false);
    }

    std::set<sai_hostif_trap_type_t> getGroupTrapIds(MockCoppOrch &coppOrch, const std::string &trapGroupName)
    {
        const auto trapGroupMap = coppOrch.get().getTrapGroupMap();
        const auto cit = trapGroupMap.find(trapGroupName);
        if (cit == trapGroupMap.end())
        {
            return {};
        }
        const auto tidList = Portal::CoppOrchInternal::getTrapIdsFromTrapGroup(coppOrch.get(), cit->second);
        return std::set<sai_hostif_trap_type_t>(tidList.begin(), tidList.end());
    }

    // bgp is first in the list and its removal from the old group rejected, so a rejection that stops the loop moves nothing.
    TEST_F(CoppOrchTest, Trap_MoveContinuesPastRetryableFailure)
    {
        const std::string trapNameList = "bgp,bgpv6,neighbor_miss";

        MockCoppOrch coppOrch;

        auto oldKofvt = std::deque<KeyOpFieldsValuesTuple>(
            {
                {
                    "queue4_group1",
                    SET_COMMAND,
                    {
                        { copp_trap_action_field,   "trap"       },
                        { copp_trap_priority_field, "4"          },
                        { copp_queue_field,         "4"          },
                        { copp_trap_id_list,        trapNameList }
                    }
                }
            }
        );
        ASSERT_EQ(coppOrch.doProcessCoppRule(oldKofvt), task_process_status::task_success);

        const auto trapIdMap = Portal::CoppOrchInternal::getTrapGroupIdMap(coppOrch.get());
        ASSERT_EQ(trapIdMap.count(SAI_HOSTIF_TRAP_TYPE_BGP), 1);

        auto newKofvt = std::deque<KeyOpFieldsValuesTuple>(
            {
                {
                    "queue4_group2",
                    SET_COMMAND,
                    {
                        { copp_trap_action_field,   "trap"       },
                        { copp_trap_priority_field, "4"          },
                        { copp_queue_field,         "4"          },
                        { copp_trap_id_list,        trapNameList }
                    }
                }
            }
        );

        _hook_sai_hostif_api(SAI_HOSTIF_TRAP_TYPE_BGP, SAI_STATUS_OBJECT_IN_USE,
                             trapIdMap.at(SAI_HOSTIF_TRAP_TYPE_BGP).trap_obj);
        auto taskStatus = coppOrch.doProcessCoppRule(newKofvt);
        _unhook_sai_hostif_api();

        EXPECT_EQ(taskStatus, task_process_status::task_failed);

        const std::set<sai_hostif_trap_type_t> oldGroupTrapIDSet = {
            SAI_HOSTIF_TRAP_TYPE_BGP
        };
        const std::set<sai_hostif_trap_type_t> newGroupTrapIDSet = {
            SAI_HOSTIF_TRAP_TYPE_BGPV6,
            SAI_HOSTIF_TRAP_TYPE_NEIGHBOR_MISS
        };
        EXPECT_TRUE(oldGroupTrapIDSet == getGroupTrapIds(coppOrch, "queue4_group1"));
        EXPECT_TRUE(newGroupTrapIDSet == getGroupTrapIds(coppOrch, "queue4_group2"));
    }

    // bgp leaves its old group and is rejected in its new one.
    TEST_F(CoppOrchTest, Trap_MoveForgetsTrapItCouldNotCreate)
    {
        MockCoppOrch coppOrch;

        auto oldKofvt = std::deque<KeyOpFieldsValuesTuple>(
            {
                {
                    "queue4_group1",
                    SET_COMMAND,
                    {
                        { copp_trap_action_field,   "trap"      },
                        { copp_trap_priority_field, "4"         },
                        { copp_queue_field,         "4"         },
                        { copp_trap_id_list,        "bgp,bgpv6" }
                    }
                }
            }
        );
        ASSERT_EQ(coppOrch.doProcessCoppRule(oldKofvt), task_process_status::task_success);

        auto newKofvt = std::deque<KeyOpFieldsValuesTuple>(
            {
                {
                    "queue4_group2",
                    SET_COMMAND,
                    {
                        { copp_trap_action_field,   "trap" },
                        { copp_trap_priority_field, "4"    },
                        { copp_queue_field,         "4"    },
                        { copp_trap_id_list,        "bgp"  }
                    }
                }
            }
        );

        _hook_sai_hostif_api(SAI_HOSTIF_TRAP_TYPE_BGP, SAI_STATUS_NOT_IMPLEMENTED);
        auto taskStatus = coppOrch.doProcessCoppRule(newKofvt);
        _unhook_sai_hostif_api();

        EXPECT_EQ(taskStatus, task_process_status::task_success);

        const auto trapIdMap = Portal::CoppOrchInternal::getTrapGroupIdMap(coppOrch.get());
        EXPECT_EQ(trapIdMap.count(SAI_HOSTIF_TRAP_TYPE_BGP), 0);

        const std::set<sai_hostif_trap_type_t> oldGroupTrapIDSet = {
            SAI_HOSTIF_TRAP_TYPE_BGPV6
        };
        EXPECT_TRUE(oldGroupTrapIDSet == getGroupTrapIds(coppOrch, "queue4_group1"));
        EXPECT_TRUE(getGroupTrapIds(coppOrch, "queue4_group2").empty());
    }

    // neighbor_miss is the first trap removed and its removal rejected, so a rejection that stops the loop removes nothing.
    TEST_F(CoppOrchTest, Trap_RemoveContinuesPastRetryableFailure)
    {
        MockCoppOrch coppOrch;
        addGenetlinkGroupTraps(coppOrch);

        const auto trapIdMap = Portal::CoppOrchInternal::getTrapGroupIdMap(coppOrch.get());
        ASSERT_EQ(trapIdMap.count(SAI_HOSTIF_TRAP_TYPE_NEIGHBOR_MISS), 1);

        auto kofvt = std::deque<KeyOpFieldsValuesTuple>(
            {
                {
                    "queue2_group1",
                    SET_COMMAND,
                    {
                        { copp_trap_id_list, "arp_req" }
                    }
                }
            }
        );

        const auto neighborMissTrapOid = trapIdMap.at(SAI_HOSTIF_TRAP_TYPE_NEIGHBOR_MISS).trap_obj;

        // Only the removal is refused, so the table entry can be created for the trap again.
        _hook_sai_hostif_api(SAI_HOSTIF_TRAP_TYPE_NEIGHBOR_MISS, SAI_STATUS_OBJECT_IN_USE);
        auto created = Portal::CoppOrchInternal::createGenetlinkHostIfTable(coppOrch.get(), genetlinkTrapIds);
        rejected_removal_oid = neighborMissTrapOid;
        auto taskStatus = coppOrch.doProcessCoppRule(kofvt);
        _unhook_sai_hostif_api();

        ASSERT_TRUE(created);
        EXPECT_EQ(taskStatus, task_process_status::task_failed);

        // neighbor_miss stays installed and gets back the table entry that had to go before its removal was tried.
        const std::set<sai_hostif_trap_type_t> groupTrapIDSet = {
            SAI_HOSTIF_TRAP_TYPE_ARP_REQUEST,
            SAI_HOSTIF_TRAP_TYPE_NEIGHBOR_MISS
        };
        EXPECT_TRUE(groupTrapIDSet == getGroupTrapIds(coppOrch, "queue2_group1"));
        EXPECT_TRUE(groupTrapIDSet == getGenetlinkTableTrapIds(coppOrch));
        EXPECT_TRUE(std::any_of(table_entry_traps.begin(), table_entry_traps.end(),
                                [&](const std::pair<const sai_object_id_t, sai_object_id_t> &entry)
                                { return entry.second == neighborMissTrapOid; }));
    }

    // bgp rejects the new priority. The group's record still takes it, so a trap that joins later is created with it.
    TEST_F(CoppOrchTest, Trap_AttrUpdateRecordsValuePastRejectedTrap)
    {
        const std::string trapGroupName = "queue4_group1";

        MockCoppOrch coppOrch;

        auto addKofvt = std::deque<KeyOpFieldsValuesTuple>(
            {
                {
                    trapGroupName,
                    SET_COMMAND,
                    {
                        { copp_trap_action_field,   "trap"      },
                        { copp_trap_priority_field, "4"         },
                        { copp_queue_field,         "4"         },
                        { copp_trap_id_list,        "bgp,bgpv6" }
                    }
                }
            }
        );
        ASSERT_EQ(coppOrch.doProcessCoppRule(addKofvt), task_process_status::task_success);

        const auto trapIdMap = Portal::CoppOrchInternal::getTrapGroupIdMap(coppOrch.get());
        ASSERT_EQ(trapIdMap.count(SAI_HOSTIF_TRAP_TYPE_BGP), 1);
        const auto bgpTrapOid = trapIdMap.at(SAI_HOSTIF_TRAP_TYPE_BGP).trap_obj;
        const auto bgpv6TrapOid = trapIdMap.at(SAI_HOSTIF_TRAP_TYPE_BGPV6).trap_obj;

        auto trapPriority = [](sai_object_id_t trapOid)
        {
            sai_attribute_t attr;
            attr.id = SAI_HOSTIF_TRAP_ATTR_TRAP_PRIORITY;
            EXPECT_EQ(sai_hostif_api->get_hostif_trap_attribute(trapOid, 1, &attr), SAI_STATUS_SUCCESS);
            return attr.value.u32;
        };

        auto updateKofvt = std::deque<KeyOpFieldsValuesTuple>(
            {
                {
                    trapGroupName,
                    SET_COMMAND,
                    {
                        { copp_trap_action_field,   "trap"      },
                        { copp_trap_priority_field, "5"         },
                        { copp_queue_field,         "4"         },
                        { copp_trap_id_list,        "bgp,bgpv6" }
                    }
                }
            }
        );

        _hook_sai_hostif_api(SAI_HOSTIF_TRAP_TYPE_END, SAI_STATUS_NOT_IMPLEMENTED, bgpTrapOid);
        auto taskStatus = coppOrch.doProcessCoppRule(updateKofvt);
        _unhook_sai_hostif_api();

        EXPECT_EQ(taskStatus, task_process_status::task_failed);
        EXPECT_EQ(trapPriority(bgpTrapOid), 4);
        EXPECT_EQ(trapPriority(bgpv6TrapOid), 5);

        auto joinKofvt = std::deque<KeyOpFieldsValuesTuple>(
            {
                {
                    trapGroupName,
                    SET_COMMAND,
                    {
                        { copp_trap_id_list, "arp_req,bgp,bgpv6" }
                    }
                }
            }
        );
        ASSERT_EQ(coppOrch.doProcessCoppRule(joinKofvt), task_process_status::task_success);

        const auto trapIdMapAfter = Portal::CoppOrchInternal::getTrapGroupIdMap(coppOrch.get());
        ASSERT_EQ(trapIdMapAfter.count(SAI_HOSTIF_TRAP_TYPE_ARP_REQUEST), 1);
        EXPECT_EQ(trapPriority(trapIdMapAfter.at(SAI_HOSTIF_TRAP_TYPE_ARP_REQUEST).trap_obj), 5);

        // Writing the priority again sets it on every trap in the group, bgp included.
        updateKofvt = std::deque<KeyOpFieldsValuesTuple>(
            {
                {
                    trapGroupName,
                    SET_COMMAND,
                    {
                        { copp_trap_action_field,   "trap"              },
                        { copp_trap_priority_field, "5"                 },
                        { copp_queue_field,         "4"                 },
                        { copp_trap_id_list,        "arp_req,bgp,bgpv6" }
                    }
                }
            }
        );
        ASSERT_EQ(coppOrch.doProcessCoppRule(updateKofvt), task_process_status::task_success);
        EXPECT_EQ(trapPriority(bgpTrapOid), 5);
    }

    // bgp's table entry in its old group names its old trap, which the move removes.
    TEST_F(CoppOrchTest, Trap_MoveOutOfGenetlinkGroup)
    {
        MockCoppOrch coppOrch;
        addGenetlinkGroupTraps(coppOrch);

        auto kofvt = std::deque<KeyOpFieldsValuesTuple>(
            {
                {
                    "queue4_group2",
                    SET_COMMAND,
                    {
                        { copp_trap_action_field,   "trap" },
                        { copp_trap_priority_field, "4"    },
                        { copp_queue_field,         "4"    },
                        { copp_trap_id_list,        "bgp"  }
                    }
                }
            }
        );

        _hook_sai_hostif_api();
        auto created = Portal::CoppOrchInternal::createGenetlinkHostIfTable(coppOrch.get(), genetlinkTrapIds);
        auto taskStatus = coppOrch.doProcessCoppRule(kofvt);
        _unhook_sai_hostif_api();

        ASSERT_TRUE(created);
        EXPECT_EQ(taskStatus, task_process_status::task_success);

        const std::set<sai_hostif_trap_type_t> newGroupTrapIDSet = {
            SAI_HOSTIF_TRAP_TYPE_BGP
        };
        const std::set<sai_hostif_trap_type_t> oldGroupTrapIDSet = {
            SAI_HOSTIF_TRAP_TYPE_BGPV6,
            SAI_HOSTIF_TRAP_TYPE_NEIGHBOR_MISS
        };
        EXPECT_TRUE(newGroupTrapIDSet == getGroupTrapIds(coppOrch, "queue4_group2"));
        EXPECT_TRUE(oldGroupTrapIDSet == getGroupTrapIds(coppOrch, "queue2_group1"));
        EXPECT_TRUE(oldGroupTrapIDSet == getGenetlinkTableTrapIds(coppOrch));
    }

    // arp_req's create is rejected in the same SET that drops lacp.
    TEST_F(CoppOrchTest, Trap_RemoveContinuesPastFailedAdd)
    {
        const std::string trapGroupName = "queue4_group1";

        MockCoppOrch coppOrch;

        auto addKofvt = std::deque<KeyOpFieldsValuesTuple>(
            {
                {
                    trapGroupName,
                    SET_COMMAND,
                    {
                        { copp_trap_action_field,   "trap"           },
                        { copp_trap_priority_field, "4"              },
                        { copp_queue_field,         "4"              },
                        { copp_trap_id_list,        "bgp,bgpv6,lacp" }
                    }
                }
            }
        );
        ASSERT_EQ(coppOrch.doProcessCoppRule(addKofvt), task_process_status::task_success);

        auto changeKofvt = std::deque<KeyOpFieldsValuesTuple>(
            {
                {
                    trapGroupName,
                    SET_COMMAND,
                    {
                        { copp_trap_id_list, "arp_req,bgp,bgpv6" }
                    }
                }
            }
        );

        _hook_sai_hostif_api(SAI_HOSTIF_TRAP_TYPE_ARP_REQUEST, SAI_STATUS_INSUFFICIENT_RESOURCES);
        auto taskStatus = coppOrch.doProcessCoppRule(changeKofvt);
        _unhook_sai_hostif_api();

        EXPECT_EQ(taskStatus, task_process_status::task_failed);

        const std::set<sai_hostif_trap_type_t> trapIDSet = {
            SAI_HOSTIF_TRAP_TYPE_BGP,
            SAI_HOSTIF_TRAP_TYPE_BGPV6
        };
        EXPECT_TRUE(trapIDSet == getGroupTrapIds(coppOrch, trapGroupName));
    }

    // bgp's table entry is rejected as the group gets its host interface, in the same SET that adds neighbor_miss.
    TEST_F(CoppOrchTest, Trap_GenetlinkAttachContinuesPastFailedTableEntry)
    {
        const std::string trapGroupName = "queue2_group1";

        MockCoppOrch coppOrch;

        auto addKofvt = std::deque<KeyOpFieldsValuesTuple>(
            {
                {
                    trapGroupName,
                    SET_COMMAND,
                    {
                        { copp_trap_action_field,   "trap"      },
                        { copp_trap_priority_field, "1"         },
                        { copp_queue_field,         "2"         },
                        { copp_trap_id_list,        "bgp,bgpv6" }
                    }
                }
            }
        );
        ASSERT_EQ(coppOrch.doProcessCoppRule(addKofvt), task_process_status::task_success);

        const auto trapIdMap = Portal::CoppOrchInternal::getTrapGroupIdMap(coppOrch.get());
        ASSERT_EQ(trapIdMap.count(SAI_HOSTIF_TRAP_TYPE_BGP), 1);

        auto attachKofvt = std::deque<KeyOpFieldsValuesTuple>(
            {
                {
                    trapGroupName,
                    SET_COMMAND,
                    {
                        { copp_genetlink_name,       "psample"                 },
                        { copp_genetlink_mcgrp_name, "packets"                 },
                        { copp_trap_id_list,         "bgp,bgpv6,neighbor_miss" }
                    }
                }
            }
        );

        _hook_sai_hostif_api(SAI_HOSTIF_TRAP_TYPE_BGP, SAI_STATUS_INSUFFICIENT_RESOURCES,
                             trapIdMap.at(SAI_HOSTIF_TRAP_TYPE_BGP).trap_obj);
        auto taskStatus = coppOrch.doProcessCoppRule(attachKofvt);
        _unhook_sai_hostif_api();

        EXPECT_EQ(taskStatus, task_process_status::task_failed);

        const std::set<sai_hostif_trap_type_t> groupTrapIDSet = {
            SAI_HOSTIF_TRAP_TYPE_BGP,
            SAI_HOSTIF_TRAP_TYPE_BGPV6,
            SAI_HOSTIF_TRAP_TYPE_NEIGHBOR_MISS
        };
        const std::set<sai_hostif_trap_type_t> tableTrapIDSet = {
            SAI_HOSTIF_TRAP_TYPE_BGPV6,
            SAI_HOSTIF_TRAP_TYPE_NEIGHBOR_MISS
        };
        EXPECT_TRUE(groupTrapIDSet == getGroupTrapIds(coppOrch, trapGroupName));
        EXPECT_TRUE(tableTrapIDSet == getGenetlinkTableTrapIds(coppOrch));
    }

    // Only arp_req's table entry is rejected, in an entry whose only change adds arp_req.
    TEST_F(CoppOrchTest, Trap_GenetlinkTableFailureOnAddedTrapFailsEntry)
    {
        MockCoppOrch coppOrch;
        addGenetlinkGroupTraps(coppOrch);

        auto kofvt = std::deque<KeyOpFieldsValuesTuple>(
            {
                {
                    "queue2_group1",
                    SET_COMMAND,
                    {
                        { copp_trap_id_list, "arp_req,bgp,bgpv6,neighbor_miss" }
                    }
                }
            }
        );

        _hook_sai_hostif_api(SAI_HOSTIF_TRAP_TYPE_END, SAI_STATUS_INSUFFICIENT_RESOURCES);
        rejected_table_trap_type = SAI_HOSTIF_TRAP_TYPE_ARP_REQUEST;
        auto taskStatus = coppOrch.doProcessCoppRule(kofvt);
        _unhook_sai_hostif_api();

        EXPECT_EQ(taskStatus, task_process_status::task_failed);

        const auto trapIdMap = Portal::CoppOrchInternal::getTrapGroupIdMap(coppOrch.get());
        EXPECT_EQ(trapIdMap.count(SAI_HOSTIF_TRAP_TYPE_ARP_REQUEST), 1);
        const std::set<sai_hostif_trap_type_t> tableTrapIDSet = {
            SAI_HOSTIF_TRAP_TYPE_BGP,
            SAI_HOSTIF_TRAP_TYPE_BGPV6,
            SAI_HOSTIF_TRAP_TYPE_NEIGHBOR_MISS
        };
        EXPECT_TRUE(tableTrapIDSet == getGenetlinkTableTrapIds(coppOrch));
    }

    // bgp's table entry can't be removed, so neither can bgp, and it keeps its record and its counter.
    TEST_F(CoppOrchTest, Trap_RemoveKeepsTrapWhoseTableEntryStays)
    {
        MockCoppOrch coppOrch;
        Portal::FlexCounterOrchInternal::setHostIfTrapCounterState(*gDirectory.get<FlexCounterOrch*>(), true);
        addGenetlinkGroupTraps(coppOrch);

        const auto trapIdMap = Portal::CoppOrchInternal::getTrapGroupIdMap(coppOrch.get());
        const auto bgpTrapOid = trapIdMap.at(SAI_HOSTIF_TRAP_TYPE_BGP).trap_obj;
        ASSERT_TRUE(Portal::CoppOrchInternal::hasTrapCounter(coppOrch.get(), bgpTrapOid));

        auto kofvt = std::deque<KeyOpFieldsValuesTuple>(
            {
                {
                    "queue2_group1",
                    SET_COMMAND,
                    {
                        { copp_trap_id_list, "bgpv6,neighbor_miss" }
                    }
                }
            }
        );

        _hook_sai_hostif_api(SAI_HOSTIF_TRAP_TYPE_END, SAI_STATUS_NOT_IMPLEMENTED);
        auto created = Portal::CoppOrchInternal::createGenetlinkHostIfTable(coppOrch.get(), genetlinkTrapIds);
        rejected_table_entry_oid = Portal::CoppOrchInternal::getTrapIdHostIfTableMap(coppOrch.get())[SAI_HOSTIF_TRAP_TYPE_BGP];
        auto taskStatus = coppOrch.doProcessCoppRule(kofvt);
        _unhook_sai_hostif_api();

        ASSERT_TRUE(created);
        EXPECT_EQ(taskStatus, task_process_status::task_failed);

        const auto trapIdMapAfter = Portal::CoppOrchInternal::getTrapGroupIdMap(coppOrch.get());
        EXPECT_EQ(trapIdMapAfter.count(SAI_HOSTIF_TRAP_TYPE_BGP), 1);
        EXPECT_TRUE(Portal::CoppOrchInternal::hasTrapCounter(coppOrch.get(), bgpTrapOid));
        const std::set<sai_hostif_trap_type_t> tableTrapIDSet(genetlinkTrapIds.begin(), genetlinkTrapIds.end());
        EXPECT_TRUE(tableTrapIDSet == getGenetlinkTableTrapIds(coppOrch));
    }

    TEST_F(CoppOrchTest, TrapGroup_DelKeepsCounterOfTrapItCouldNotRemove)
    {
        const std::string trapGroupName = "queue4_group1";

        MockCoppOrch coppOrch;
        Portal::FlexCounterOrchInternal::setHostIfTrapCounterState(*gDirectory.get<FlexCounterOrch*>(), true);

        auto addKofvt = std::deque<KeyOpFieldsValuesTuple>(
            {
                {
                    trapGroupName,
                    SET_COMMAND,
                    {
                        { copp_trap_action_field,   "trap"      },
                        { copp_trap_priority_field, "4"         },
                        { copp_queue_field,         "4"         },
                        { copp_trap_id_list,        "bgp,bgpv6" }
                    }
                }
            }
        );
        ASSERT_EQ(coppOrch.doProcessCoppRule(addKofvt), task_process_status::task_success);

        const auto trapIdMap = Portal::CoppOrchInternal::getTrapGroupIdMap(coppOrch.get());
        const auto bgpTrapOid = trapIdMap.at(SAI_HOSTIF_TRAP_TYPE_BGP).trap_obj;
        ASSERT_TRUE(Portal::CoppOrchInternal::hasTrapCounter(coppOrch.get(), bgpTrapOid));

        auto delKofvt = std::deque<KeyOpFieldsValuesTuple>(
            { { trapGroupName, DEL_COMMAND, { } } }
        );

        // Only the removal is refused, so the counter can be bound to the trap again.
        _hook_sai_hostif_api(SAI_HOSTIF_TRAP_TYPE_BGP, SAI_STATUS_OBJECT_IN_USE);
        rejected_removal_oid = bgpTrapOid;
        auto taskStatus = coppOrch.doProcessCoppRule(delKofvt);
        _unhook_sai_hostif_api();

        EXPECT_EQ(taskStatus, task_process_status::task_failed);
        EXPECT_TRUE(Portal::CoppOrchInternal::hasTrapCounter(coppOrch.get(), bgpTrapOid));
    }

    // bgp is removed after neighbor_miss and before bgpv6, so the test sees a trap removed on each side of the failure.
    TEST_F(CoppOrchTest, TrapGroup_DelContinuesPastRetryableFailure)
    {
        const std::string trapGroupName = "queue4_group1";

        MockCoppOrch coppOrch;

        auto addKofvt = std::deque<KeyOpFieldsValuesTuple>(
            {
                {
                    trapGroupName,
                    SET_COMMAND,
                    {
                        { copp_trap_action_field,   "trap"                    },
                        { copp_trap_priority_field, "4"                       },
                        { copp_queue_field,         "4"                       },
                        { copp_trap_id_list,        "bgp,bgpv6,neighbor_miss" }
                    }
                }
            }
        );
        ASSERT_EQ(coppOrch.doProcessCoppRule(addKofvt), task_process_status::task_success);

        const auto trapIdMap = Portal::CoppOrchInternal::getTrapGroupIdMap(coppOrch.get());
        ASSERT_EQ(trapIdMap.count(SAI_HOSTIF_TRAP_TYPE_BGP), 1);

        auto delKofvt = std::deque<KeyOpFieldsValuesTuple>(
            { { trapGroupName, DEL_COMMAND, { } } }
        );

        _hook_sai_hostif_api(SAI_HOSTIF_TRAP_TYPE_BGP, SAI_STATUS_OBJECT_IN_USE,
                             trapIdMap.at(SAI_HOSTIF_TRAP_TYPE_BGP).trap_obj);
        auto taskStatus = coppOrch.doProcessCoppRule(delKofvt);
        _unhook_sai_hostif_api();

        EXPECT_EQ(taskStatus, task_process_status::task_failed);

        // The group stays, holding only the trap that could not be removed.
        const std::set<sai_hostif_trap_type_t> trapIDSet = {
            SAI_HOSTIF_TRAP_TYPE_BGP
        };
        EXPECT_TRUE(trapIDSet == getGroupTrapIds(coppOrch, trapGroupName));

        const auto trapIdMapAfter = Portal::CoppOrchInternal::getTrapGroupIdMap(coppOrch.get());
        EXPECT_EQ(trapIdMapAfter.count(SAI_HOSTIF_TRAP_TYPE_BGPV6), 0);
        EXPECT_EQ(trapIdMapAfter.count(SAI_HOSTIF_TRAP_TYPE_NEIGHBOR_MISS), 0);
    }
}
