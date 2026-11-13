#include "table.h"
#include "producerstatetable.h"
#include "consumerstatetable.h"
#include <memory>

#define private public
#define protected public

#include "directory.h"
#include "orch.h"
#include "vxlanorch.h"
#include "portsorch.h"

#undef protected
#undef private

#include "ut_helper.h"
#include "common/vxlan_ut_helpers.h"
#include "mock_orchagent_main.h"
#include "mock_sai_api.h"
#include "mock_sai_tunnel.h"
#include "mock_orch_test.h"

extern Directory<Orch*> gDirectory;
extern PortsOrch *gPortsOrch;
extern string gMySwitchType;

namespace vxlanorch_test
{
    using namespace std;
    using namespace swss;
    using namespace mock_orch_test;
    using ::testing::_;
    using ::testing::Return;
    using ::testing::DoAll;
    using ::testing::SetArgPointee;
    using ::testing::Throw;
    using ::testing::StrictMock;
    using ::testing::Invoke;

    constexpr sai_object_id_t vxlan_tunnel_oid = 0x1232;
    constexpr sai_object_id_t vxlan_tunnel_map_oid = 0x1240;
    constexpr sai_object_id_t vxlan_tunnel_term_table_entry_oid = 0x1248;
    constexpr sai_object_id_t vxlan_tunnel_map_entry_oid = 0x1256;

    class VxlanOrchTest : public ::testing::Test
    {
        public:
            VxlanOrchTest()
            {
            };

            ~VxlanOrchTest()
            {
            };


            void _hook_sai_apis()
            {
                mock_sai_tunnel = &mock_sai_tunnel_;

                saved_create_tunnel = sai_tunnel_api->create_tunnel;
                saved_create_tunnel_map = sai_tunnel_api->create_tunnel_map;
                saved_create_tunnel_map_entry = sai_tunnel_api->create_tunnel_map_entry;
                saved_create_tunnel_term_table_entry = sai_tunnel_api->create_tunnel_term_table_entry;
                saved_remove_tunnel = sai_tunnel_api->remove_tunnel;
                saved_remove_tunnel_map = sai_tunnel_api->remove_tunnel_map;
                saved_remove_tunnel_map_entry = sai_tunnel_api->remove_tunnel_map_entry;
                saved_remove_tunnel_term_table_entry = sai_tunnel_api->remove_tunnel_term_table_entry;

                sai_tunnel_api->create_tunnel = mock_create_tunnel;
                sai_tunnel_api->create_tunnel_map = mock_create_tunnel_map;
                sai_tunnel_api->create_tunnel_map_entry = mock_create_tunnel_map_entry;
                sai_tunnel_api->create_tunnel_term_table_entry = mock_create_tunnel_term_table_entry;
                sai_tunnel_api->remove_tunnel = mock_remove_tunnel;
                sai_tunnel_api->remove_tunnel_map = mock_remove_tunnel_map;
                sai_tunnel_api->remove_tunnel_map_entry = mock_remove_tunnel_map_entry;
                sai_tunnel_api->remove_tunnel_term_table_entry = mock_remove_tunnel_term_table_entry;
            }

            void _unhook_sai_apis()
            {
                if (sai_tunnel_api)
                {
                    sai_tunnel_api->create_tunnel = saved_create_tunnel;
                    sai_tunnel_api->create_tunnel_map = saved_create_tunnel_map;
                    sai_tunnel_api->create_tunnel_map_entry = saved_create_tunnel_map_entry;
                    sai_tunnel_api->create_tunnel_term_table_entry = saved_create_tunnel_term_table_entry;
                    sai_tunnel_api->remove_tunnel = saved_remove_tunnel;
                    sai_tunnel_api->remove_tunnel_map = saved_remove_tunnel_map;
                    sai_tunnel_api->remove_tunnel_map_entry = saved_remove_tunnel_map_entry;
                    sai_tunnel_api->remove_tunnel_term_table_entry = saved_remove_tunnel_term_table_entry;
                }

                mock_sai_tunnel = nullptr;
            }

        protected:
            StrictMock<MockSaiTunnel> mock_sai_tunnel_;
            decltype(sai_tunnel_api->create_tunnel) saved_create_tunnel{};
            decltype(sai_tunnel_api->create_tunnel_map) saved_create_tunnel_map{};
            decltype(sai_tunnel_api->create_tunnel_map_entry) saved_create_tunnel_map_entry{};
            decltype(sai_tunnel_api->create_tunnel_term_table_entry) saved_create_tunnel_term_table_entry{};
            decltype(sai_tunnel_api->remove_tunnel) saved_remove_tunnel{};
            decltype(sai_tunnel_api->remove_tunnel_map) saved_remove_tunnel_map{};
            decltype(sai_tunnel_api->remove_tunnel_map_entry) saved_remove_tunnel_map_entry{};
            decltype(sai_tunnel_api->remove_tunnel_term_table_entry) saved_remove_tunnel_term_table_entry{};

            shared_ptr<swss::DBConnector> m_app_db;
            shared_ptr<swss::DBConnector> m_config_db;
            shared_ptr<swss::DBConnector> m_state_db;
            shared_ptr<swss::DBConnector> m_chassis_app_db;
            VxlanTunnelOrch *m_vxlan_tunnel_orch = nullptr;
            VxlanTunnelMapOrch *m_vxlanTunnelMapOrch = nullptr;
            EvpnNvoOrch *m_evpnNvoOrch = nullptr;
            EvpnRemoteVnip2pOrch *m_evpnRemoteVnip2pOrch = nullptr;

            void SetUp() override
            {
                // Init switch and create dependencies
                m_app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
                m_config_db = make_shared<swss::DBConnector>("CONFIG_DB", 0);
                m_state_db = make_shared<swss::DBConnector>("STATE_DB", 0);
                if(gMySwitchType == "voq")
                {
                    m_chassis_app_db = make_shared<swss::DBConnector>("CHASSIS_APP_DB", 0);
                }

                m_vxlanTunnelMapOrch = new VxlanTunnelMapOrch(m_app_db.get(), APP_VXLAN_TUNNEL_MAP_TABLE_NAME);
                m_evpnNvoOrch = new EvpnNvoOrch(m_app_db.get(), APP_VXLAN_EVPN_NVO_TABLE_NAME);
                m_evpnRemoteVnip2pOrch = new EvpnRemoteVnip2pOrch(m_app_db.get(), APP_VXLAN_REMOTE_VNI_TABLE_NAME);

                gDirectory.set(m_vxlanTunnelMapOrch);
                gDirectory.set(m_evpnNvoOrch);
                gDirectory.set(m_evpnRemoteVnip2pOrch);

                map<string, string> profile = {
                    { "SAI_VS_SWITCH_TYPE", "SAI_VS_SWITCH_TYPE_BCM56850" },
                    { "KV_DEVICE_MAC_ADDRESS", "20:03:04:05:06:00" }
                };

                ut_helper::initSaiApi(profile);
                _hook_sai_apis();

                sai_attribute_t attr;

                attr.id = SAI_SWITCH_ATTR_INIT_SWITCH;
                attr.value.booldata = true;

                auto status = sai_switch_api->create_switch(&gSwitchId, 1, &attr);
                ASSERT_EQ(status, SAI_STATUS_SUCCESS);
            }

            void initSwitchOrch()
            {
                TableConnector stateDbSwitchTable(m_state_db.get(), "SWITCH_CAPABILITY");
                TableConnector conf_asic_sensors(m_config_db.get(), CFG_ASIC_SENSORS_TABLE_NAME);
                TableConnector app_switch_table(m_app_db.get(),  APP_SWITCH_TABLE_NAME);
                TableConnector conf_suppress_asic_sdk_health_categories(m_config_db.get(), CFG_SUPPRESS_ASIC_SDK_HEALTH_EVENT_NAME);

                vector<TableConnector> switch_tables = {
                    conf_asic_sensors,
                    conf_suppress_asic_sdk_health_categories,
                    app_switch_table
                };

                ASSERT_EQ(gSwitchOrch, nullptr);
                gSwitchOrch = new SwitchOrch(m_app_db.get(), switch_tables, stateDbSwitchTable);

                // Create dependencies ...
                const int portsorch_base_pri = 40;

                vector<table_name_with_pri_t> ports_tables = {
                    { APP_PORT_TABLE_NAME, portsorch_base_pri + 5 },
                    { APP_VLAN_TABLE_NAME, portsorch_base_pri + 2 },
                    { APP_VLAN_MEMBER_TABLE_NAME, portsorch_base_pri },
                    { APP_LAG_TABLE_NAME, portsorch_base_pri + 4 },
                    { APP_LAG_MEMBER_TABLE_NAME, portsorch_base_pri }
                };

                ASSERT_EQ(gPortsOrch, nullptr);
                gPortsOrch = new PortsOrch(m_app_db.get(), m_state_db.get(), ports_tables, m_chassis_app_db.get());
}

            void initVxlanOrch()
            {
                m_vxlan_tunnel_orch = new VxlanTunnelOrch(m_state_db.get(), m_app_db.get(), APP_VXLAN_TUNNEL_TABLE_NAME);
                gDirectory.set(m_vxlan_tunnel_orch);
            }

            void TearDown() override
            {
                _unhook_sai_apis();

                delete gPortsOrch;
                gPortsOrch = nullptr;
                if (m_vxlan_tunnel_orch) {
                    m_vxlan_tunnel_orch->vxlan_tunnel_table_.clear();
                    delete m_vxlan_tunnel_orch;
                    m_vxlan_tunnel_orch = nullptr;
                }

                if (m_evpnRemoteVnip2pOrch) {
                    delete m_evpnRemoteVnip2pOrch;
                    m_evpnRemoteVnip2pOrch = nullptr;
                }
                if (m_evpnNvoOrch) {
                    delete m_evpnNvoOrch;
                    m_evpnNvoOrch = nullptr;
                }
                if (m_vxlanTunnelMapOrch) {
                    delete m_vxlanTunnelMapOrch;
                    m_vxlanTunnelMapOrch = nullptr;
                }

                gDirectory.m_values.clear();
                delete gSwitchOrch;
                gSwitchOrch = nullptr;

                auto status = sai_switch_api->remove_switch(gSwitchId);
                ASSERT_EQ(status, SAI_STATUS_SUCCESS);
                gSwitchId = 0;

                ut_helper::uninitSaiApi();
            }

            // Helper function to create a basic VXLAN tunnel
            void CreateBasicVxlanTunnel(const string& tunnel_name, const string& src_ip, const string& dst_ip = "")
            {
                // Create the tunnel entry directly in the orchestrator's internal structures
                // This simulates what would happen after database processing
                IpAddress srcIpAddr(src_ip);
                IpAddress dstIpAddr = dst_ip.empty() ? IpAddress() : IpAddress(dst_ip);

                // Create tunnel object directly
                auto tunnel = std::make_unique<VxlanTunnel>(tunnel_name, srcIpAddr, dstIpAddr, TNL_CREATION_SRC_CLI);
                m_vxlan_tunnel_orch->vxlan_tunnel_table_[tunnel_name] = std::move(tunnel);
            }

            // Helper function to create a VXLAN tunnel map
            void CreateVxlanTunnelMap(const string& tunnel_name, const string& map_name,
                                      const string& vni, const string& vlan)
            {
                // Create the tunnel map entry directly in the orchestrator's internal structures
                string full_key = tunnel_name + ":" + map_name;

                tunnel_map_entry_t map_entry;
                map_entry.vni_id = std::stoi(vni);
                map_entry.vlan_id = std::stoi(vlan.substr(4)); // Remove "Vlan" prefix
                map_entry.map_entry_id = SAI_NULL_OBJECT_ID; // For testing, we don't need actual SAI objects

                m_vxlanTunnelMapOrch->vxlan_tunnel_map_table_[full_key] = map_entry;
            }

            // Helper function to create EVPN NVO
            void CreateEvpnNvo(const string& nvo_name, const string& source_vtep)
            {
                // Set the source VTEP pointer directly
                if (m_vxlan_tunnel_orch->isTunnelExists(source_vtep)) {
                    m_evpnNvoOrch->source_vtep_ptr = m_vxlan_tunnel_orch->getVxlanTunnel(source_vtep);
                }
            }
    };


    TEST_F(VxlanOrchTest, TunnelCreateFailure)
    {
        initSwitchOrch();
        initVxlanOrch();
        VxlanTunnelOrch* vxlan_orch = gDirectory.get<VxlanTunnelOrch*>();
        VxlanTunnel* tunnel = nullptr;

        auto src_ip = IpAddress("10.1.0.1");
        auto dst_ip = IpAddress("20.1.0.1");
        tunnel = new VxlanTunnel("vxlan_tunnel_1", src_ip, dst_ip, TNL_CREATION_SRC_CLI);
        vxlan_orch->addTunnel("vxlan_tunnel_1", tunnel);

        EXPECT_CALL(mock_sai_tunnel_, create_tunnel_map(_, _, _, _))
            .Times(4)
            .WillRepeatedly(DoAll(
                        SetArgPointee<0>(vxlan_tunnel_map_oid),
                        Return(SAI_STATUS_SUCCESS)
                        ));
        EXPECT_CALL(mock_sai_tunnel_, create_tunnel(_, _, _, _))
            .WillOnce(DoAll(
                        SetArgPointee<0>(SAI_NULL_OBJECT_ID),
                        Return(SAI_STATUS_FAILURE)
                        ));
        EXPECT_CALL(mock_sai_tunnel_, remove_tunnel_map(_))
            .Times(4)
            .WillRepeatedly(DoAll(
                        Return(SAI_STATUS_SUCCESS)
                        ));
        EXPECT_NO_THROW({
                bool result = vxlan_orch->createVxlanTunnelMap("vxlan_tunnel_1", TUNNEL_MAP_T_VIRTUAL_ROUTER, 1000, 0x1001, 0x1002, 64);
                EXPECT_FALSE(result);
                });
        vxlan_orch->delTunnel("vxlan_tunnel_1");
    }

    TEST_F(VxlanOrchTest, TunnelMapCreateFailure)
    {
        initSwitchOrch();
        initVxlanOrch();
        VxlanTunnelOrch* vxlan_orch = gDirectory.get<VxlanTunnelOrch*>();
        VxlanTunnel* tunnel = nullptr;

        auto src_ip = IpAddress("10.1.0.1");
        auto dst_ip = IpAddress("20.1.0.1");
        tunnel = new VxlanTunnel("vxlan_tunnel_1", src_ip, dst_ip, TNL_CREATION_SRC_CLI);
        vxlan_orch->addTunnel("vxlan_tunnel_1", tunnel);

        EXPECT_CALL(mock_sai_tunnel_, create_tunnel_map(_, _, _, _))
            .Times(4)
            .WillRepeatedly(DoAll(
                        SetArgPointee<0>(SAI_NULL_OBJECT_ID),
                        Return(SAI_STATUS_FAILURE)
                        ));
        // A tunnel without maps is not created
        EXPECT_CALL(mock_sai_tunnel_, create_tunnel(_, _, _, _))
            .Times(0);
        // None of the maps was created, so none is removed
        EXPECT_CALL(mock_sai_tunnel_, remove_tunnel_map(_))
            .Times(0);

        EXPECT_NO_THROW({
                bool result = vxlan_orch->createVxlanTunnelMap("vxlan_tunnel_1", TUNNEL_MAP_T_VIRTUAL_ROUTER, 1000, 0x1001, 0x1002, 64);
                EXPECT_FALSE(result);
                });
        vxlan_orch->delTunnel("vxlan_tunnel_1");
    }

    /*
     * A switch that supports only some tunnel map types (here: no VLAN maps)
     * still gets a tunnel with the maps it supports. An entry for a missing
     * map is refused without a SAI call, maps that were never created are not
     * removed, and orchagent is not marked unhealthy.
     */
    TEST_F(VxlanOrchTest, TunnelMapTypeNotSupported)
    {
        initSwitchOrch();
        initVxlanOrch();
        VxlanTunnelOrch* vxlan_orch = gDirectory.get<VxlanTunnelOrch*>();

        setSaiFailureStatus(false);

        auto src_ip = IpAddress("10.1.0.1");
        auto dst_ip = IpAddress("20.1.0.1");
        VxlanTunnel* tunnel = new VxlanTunnel("vxlan_tunnel_1", src_ip, dst_ip, TNL_CREATION_SRC_CLI);
        vxlan_orch->addTunnel("vxlan_tunnel_1", tunnel);

        EXPECT_CALL(mock_sai_tunnel_, create_tunnel_map(_, _, _, _))
            .Times(4)
            .WillRepeatedly(Invoke([](sai_object_id_t *tunnel_map_id, sai_object_id_t,
                                      uint32_t attr_count, const sai_attribute_t *attr_list) {
                for (uint32_t i = 0; i < attr_count; i++)
                {
                    if (attr_list[i].id == SAI_TUNNEL_MAP_ATTR_TYPE &&
                        (attr_list[i].value.s32 == SAI_TUNNEL_MAP_TYPE_VNI_TO_VLAN_ID ||
                         attr_list[i].value.s32 == SAI_TUNNEL_MAP_TYPE_VLAN_ID_TO_VNI))
                    {
                        *tunnel_map_id = SAI_NULL_OBJECT_ID;
                        return SAI_STATUS_NOT_SUPPORTED;
                    }
                }
                *tunnel_map_id = vxlan_tunnel_map_oid;
                return SAI_STATUS_SUCCESS;
            }));
        uint32_t decap_map_count = 0;
        uint32_t encap_map_count = 0;
        EXPECT_CALL(mock_sai_tunnel_, create_tunnel(_, _, _, _))
            .WillOnce(Invoke([&](sai_object_id_t *tunnel_id, sai_object_id_t,
                                 uint32_t attr_count, const sai_attribute_t *attr_list) {
                for (uint32_t i = 0; i < attr_count; i++)
                {
                    if (attr_list[i].id == SAI_TUNNEL_ATTR_DECAP_MAPPERS)
                    {
                        decap_map_count = attr_list[i].value.objlist.count;
                    }
                    else if (attr_list[i].id == SAI_TUNNEL_ATTR_ENCAP_MAPPERS)
                    {
                        encap_map_count = attr_list[i].value.objlist.count;
                    }
                }
                *tunnel_id = vxlan_tunnel_oid;
                return SAI_STATUS_SUCCESS;
            }));
        EXPECT_CALL(mock_sai_tunnel_, create_tunnel_term_table_entry(_, _, _, _))
            .WillOnce(DoAll(
                        SetArgPointee<0>(vxlan_tunnel_term_table_entry_oid),
                        Return(SAI_STATUS_SUCCESS)
                        ));
        // Only the two VRF map entries reach the SAI
        EXPECT_CALL(mock_sai_tunnel_, create_tunnel_map_entry(_, _, _, _))
            .Times(2)
            .WillRepeatedly(DoAll(
                        SetArgPointee<0>(vxlan_tunnel_map_entry_oid),
                        Return(SAI_STATUS_SUCCESS)
                        ));
        EXPECT_CALL(mock_sai_tunnel_, remove_tunnel_map_entry(_))
            .Times(2)
            .WillRepeatedly(Return(SAI_STATUS_SUCCESS));
        EXPECT_CALL(mock_sai_tunnel_, remove_tunnel_term_table_entry(_))
            .WillOnce(Return(SAI_STATUS_SUCCESS));
        EXPECT_CALL(mock_sai_tunnel_, remove_tunnel(_))
            .WillOnce(Return(SAI_STATUS_SUCCESS));
        // Only the two VRF maps exist to be removed
        EXPECT_CALL(mock_sai_tunnel_, remove_tunnel_map(vxlan_tunnel_map_oid))
            .Times(2)
            .WillRepeatedly(Return(SAI_STATUS_SUCCESS));
        EXPECT_CALL(mock_sai_tunnel_, remove_tunnel_map(SAI_NULL_OBJECT_ID))
            .Times(0);

        EXPECT_NO_THROW({
                EXPECT_TRUE(vxlan_orch->createVxlanTunnelMap("vxlan_tunnel_1", TUNNEL_MAP_T_VIRTUAL_ROUTER, 1000, 0x1001, 0x1002, 64));
                EXPECT_TRUE(tunnel->isActive());
                EXPECT_EQ(decap_map_count, 1u);
                EXPECT_EQ(encap_map_count, 1u);
                EXPECT_EQ(tunnel->getDecapMapId(TUNNEL_MAP_T_VLAN), SAI_NULL_OBJECT_ID);
                EXPECT_EQ(tunnel->getEncapMapId(TUNNEL_MAP_T_VLAN), SAI_NULL_OBJECT_ID);

                EXPECT_EQ(tunnel->addDecapMapperEntry(0x1003, 2000, TUNNEL_MAP_T_VLAN), SAI_NULL_OBJECT_ID);
                EXPECT_EQ(tunnel->addEncapMapperEntry(0x1003, 2000, TUNNEL_MAP_T_VLAN), SAI_NULL_OBJECT_ID);

                EXPECT_TRUE(vxlan_orch->removeVxlanTunnelMap("vxlan_tunnel_1", 1000));
                });

        std::string error;
        EXPECT_FALSE(getSaiFailureStatus(error)) << error;

        vxlan_orch->delTunnel("vxlan_tunnel_1");
    }

    /*
     * A switch that supports none of the tunnel map types, reporting it as
     * NOT_IMPLEMENTED: no tunnel is created and orchagent is not marked
     * unhealthy.
     */
    TEST_F(VxlanOrchTest, TunnelNoMapTypeSupported)
    {
        initSwitchOrch();
        initVxlanOrch();
        VxlanTunnelOrch* vxlan_orch = gDirectory.get<VxlanTunnelOrch*>();

        setSaiFailureStatus(false);

        auto src_ip = IpAddress("10.1.0.1");
        auto dst_ip = IpAddress("20.1.0.1");
        VxlanTunnel* tunnel = new VxlanTunnel("vxlan_tunnel_1", src_ip, dst_ip, TNL_CREATION_SRC_CLI);
        vxlan_orch->addTunnel("vxlan_tunnel_1", tunnel);

        EXPECT_CALL(mock_sai_tunnel_, create_tunnel_map(_, _, _, _))
            .Times(4)
            .WillRepeatedly(Invoke([](sai_object_id_t *tunnel_map_id, sai_object_id_t,
                                      uint32_t attr_count, const sai_attribute_t *attr_list) {
                *tunnel_map_id = SAI_NULL_OBJECT_ID;
                for (uint32_t i = 0; i < attr_count; i++)
                {
                    if (attr_list[i].id == SAI_TUNNEL_MAP_ATTR_TYPE &&
                        (attr_list[i].value.s32 == SAI_TUNNEL_MAP_TYPE_VNI_TO_VLAN_ID ||
                         attr_list[i].value.s32 == SAI_TUNNEL_MAP_TYPE_VLAN_ID_TO_VNI))
                    {
                        return SAI_STATUS_NOT_IMPLEMENTED;
                    }
                }
                return SAI_STATUS_ATTR_NOT_IMPLEMENTED_0;
            }));
        EXPECT_CALL(mock_sai_tunnel_, create_tunnel(_, _, _, _))
            .Times(0);
        EXPECT_CALL(mock_sai_tunnel_, remove_tunnel_map(_))
            .Times(0);

        EXPECT_NO_THROW({
                EXPECT_FALSE(vxlan_orch->createVxlanTunnelMap("vxlan_tunnel_1", TUNNEL_MAP_T_VIRTUAL_ROUTER, 1000, 0x1001, 0x1002, 64));
                EXPECT_FALSE(tunnel->isActive());
                EXPECT_EQ(tunnel->getTunnelId(), SAI_NULL_OBJECT_ID);
                });

        std::string error;
        EXPECT_FALSE(getSaiFailureStatus(error)) << error;

        vxlan_orch->delTunnel("vxlan_tunnel_1");
    }

    TEST_F(VxlanOrchTest, TunnelTerminationCreateFailure)
    {
        initSwitchOrch();
        initVxlanOrch();
        VxlanTunnelOrch* vxlan_orch = gDirectory.get<VxlanTunnelOrch*>();
        VxlanTunnel* tunnel = nullptr;

        auto src_ip = IpAddress("10.1.0.1");
        auto dst_ip = IpAddress("20.1.0.1");
        tunnel = new VxlanTunnel("vxlan_tunnel_1", src_ip, dst_ip, TNL_CREATION_SRC_CLI);
        vxlan_orch->addTunnel("vxlan_tunnel_1", tunnel);

        EXPECT_CALL(mock_sai_tunnel_, create_tunnel_map(_, _, _, _))
            .Times(4)
            .WillRepeatedly(DoAll(
                        SetArgPointee<0>(vxlan_tunnel_map_oid),
                        Return(SAI_STATUS_SUCCESS)
                        ));
        EXPECT_CALL(mock_sai_tunnel_, create_tunnel(_, _, _, _))
            .WillOnce(DoAll(
                        SetArgPointee<0>(vxlan_tunnel_oid),
                        Return(SAI_STATUS_SUCCESS)
                        ));
        EXPECT_CALL(mock_sai_tunnel_, create_tunnel_term_table_entry(_, _, _, _))
            .WillOnce(DoAll(
                        SetArgPointee<0>(SAI_NULL_OBJECT_ID),
                        Return(SAI_STATUS_FAILURE)
                        ));
        EXPECT_CALL(mock_sai_tunnel_, remove_tunnel(_))
            .WillOnce(DoAll(
                        Return(SAI_STATUS_FAILURE)
                        ));
        EXPECT_CALL(mock_sai_tunnel_, remove_tunnel_map(_))
            .Times(4)
            .WillRepeatedly(DoAll(
                        Return(SAI_STATUS_SUCCESS)
                        ));

        EXPECT_NO_THROW({
                bool result = vxlan_orch->createVxlanTunnelMap("vxlan_tunnel_1", TUNNEL_MAP_T_VIRTUAL_ROUTER, 1000, 0x1001, 0x1002, 64);
                EXPECT_FALSE(result);
                });
        vxlan_orch->delTunnel("vxlan_tunnel_1");
    }

    TEST_F(VxlanOrchTest, TunnelMapEntryCreateFailure)
    {
        initSwitchOrch();
        initVxlanOrch();
        VxlanTunnelOrch* vxlan_orch = gDirectory.get<VxlanTunnelOrch*>();
        VxlanTunnel* tunnel = nullptr;

        auto src_ip = IpAddress("10.1.0.1");
        auto dst_ip = IpAddress("20.1.0.1");
        tunnel = new VxlanTunnel("vxlan_tunnel_1", src_ip, dst_ip, TNL_CREATION_SRC_CLI);
        vxlan_orch->addTunnel("vxlan_tunnel_1", tunnel);

        EXPECT_CALL(mock_sai_tunnel_, create_tunnel_map(_, _, _, _))
            .Times(4)
            .WillRepeatedly(DoAll(
                        SetArgPointee<0>(vxlan_tunnel_map_oid),
                        Return(SAI_STATUS_SUCCESS)
                        ));
        EXPECT_CALL(mock_sai_tunnel_, create_tunnel(_, _, _, _))
            .WillOnce(DoAll(
                        SetArgPointee<0>(vxlan_tunnel_oid),
                        Return(SAI_STATUS_SUCCESS)
                        ));
        EXPECT_CALL(mock_sai_tunnel_, create_tunnel_term_table_entry(_, _, _, _))
            .WillOnce(DoAll(
                        SetArgPointee<0>(vxlan_tunnel_term_table_entry_oid),
                        Return(SAI_STATUS_SUCCESS)
                        ));
        EXPECT_CALL(mock_sai_tunnel_, create_tunnel_map_entry(_, _, _, _))
            .Times(2)
            .WillRepeatedly(DoAll(
                        SetArgPointee<0>(SAI_NULL_OBJECT_ID),
                        Return(SAI_STATUS_FAILURE)
                        ));

        EXPECT_NO_THROW({
                bool result = vxlan_orch->createVxlanTunnelMap("vxlan_tunnel_1", TUNNEL_MAP_T_VIRTUAL_ROUTER, 1000, 0x1001, 0x1002, 64);
                /*
                 * Return value of create_tunnel_map_entry() is intentionally
                 * ignored in createVxlanTunnelMap(), so expect true here
                 */
                EXPECT_TRUE(result);
                });
        vxlan_orch->delTunnel("vxlan_tunnel_1");
    }

    TEST_F(VxlanOrchTest, TunnelAllSuccess)
    {
        initSwitchOrch();
        initVxlanOrch();
        VxlanTunnelOrch* vxlan_orch = gDirectory.get<VxlanTunnelOrch*>();
        VxlanTunnel* tunnel = nullptr;

        auto src_ip = IpAddress("10.1.0.1");
        auto dst_ip = IpAddress("20.1.0.1");
        tunnel = new VxlanTunnel("vxlan_tunnel_1", src_ip, dst_ip, TNL_CREATION_SRC_CLI);
        vxlan_orch->addTunnel("vxlan_tunnel_1", tunnel);

        EXPECT_CALL(mock_sai_tunnel_, create_tunnel_map(_, _, _, _))
            .Times(4)
            .WillRepeatedly(DoAll(
                        SetArgPointee<0>(vxlan_tunnel_map_oid),
                        Return(SAI_STATUS_SUCCESS)
                        ));
        EXPECT_CALL(mock_sai_tunnel_, create_tunnel(_, _, _, _))
            .WillOnce(DoAll(
                        SetArgPointee<0>(vxlan_tunnel_oid),
                        Return(SAI_STATUS_SUCCESS)
                        ));
        EXPECT_CALL(mock_sai_tunnel_, create_tunnel_term_table_entry(_, _, _, _))
            .WillOnce(DoAll(
                        SetArgPointee<0>(vxlan_tunnel_term_table_entry_oid),
                        Return(SAI_STATUS_SUCCESS)
                        ));
        EXPECT_CALL(mock_sai_tunnel_, create_tunnel_map_entry(_, _, _, _))
            .Times(2)
            .WillRepeatedly(DoAll(
                        SetArgPointee<0>(vxlan_tunnel_map_entry_oid),
                        Return(SAI_STATUS_SUCCESS)
                        ));
        EXPECT_CALL(mock_sai_tunnel_, remove_tunnel_map_entry(_))
            .Times(2)
            .WillRepeatedly(DoAll(
                        Return(SAI_STATUS_SUCCESS)
                        ));
        EXPECT_CALL(mock_sai_tunnel_, remove_tunnel_term_table_entry(_))
            .WillOnce(DoAll(
                        Return(SAI_STATUS_SUCCESS)
                        ));
        EXPECT_CALL(mock_sai_tunnel_, remove_tunnel(_))
            .WillOnce(DoAll(
                        Return(SAI_STATUS_SUCCESS)
                        ));
        EXPECT_CALL(mock_sai_tunnel_, remove_tunnel_map(_))
            .Times(4)
            .WillRepeatedly(DoAll(
                        Return(SAI_STATUS_SUCCESS)
                        ));

        EXPECT_NO_THROW({
                bool result1 = vxlan_orch->createVxlanTunnelMap("vxlan_tunnel_1", TUNNEL_MAP_T_VIRTUAL_ROUTER, 1000, 0x1001, 0x1002, 64);
                EXPECT_TRUE(result1);
                bool result2 = vxlan_orch->removeVxlanTunnelMap("vxlan_tunnel_1", 1000);
                EXPECT_TRUE(result2);
                });
        vxlan_orch->delTunnel("vxlan_tunnel_1");
    }

    TEST_F(VxlanOrchTest, TunnelAllRemoveFailure)
    {
        initSwitchOrch();
        initVxlanOrch();
        VxlanTunnelOrch* vxlan_orch = gDirectory.get<VxlanTunnelOrch*>();
        VxlanTunnel* tunnel = nullptr;

        auto src_ip = IpAddress("10.1.0.1");
        auto dst_ip = IpAddress("20.1.0.1");
        tunnel = new VxlanTunnel("vxlan_tunnel_1", src_ip, dst_ip, TNL_CREATION_SRC_CLI);
        vxlan_orch->addTunnel("vxlan_tunnel_1", tunnel);

        EXPECT_CALL(mock_sai_tunnel_, create_tunnel_map(_, _, _, _))
            .Times(4)
            .WillRepeatedly(DoAll(
                        SetArgPointee<0>(vxlan_tunnel_map_oid),
                        Return(SAI_STATUS_SUCCESS)
                        ));
        EXPECT_CALL(mock_sai_tunnel_, create_tunnel(_, _, _, _))
            .WillOnce(DoAll(
                        SetArgPointee<0>(vxlan_tunnel_oid),
                        Return(SAI_STATUS_SUCCESS)
                        ));
        EXPECT_CALL(mock_sai_tunnel_, create_tunnel_term_table_entry(_, _, _, _))
            .WillOnce(DoAll(
                        SetArgPointee<0>(vxlan_tunnel_term_table_entry_oid),
                        Return(SAI_STATUS_SUCCESS)
                        ));
        EXPECT_CALL(mock_sai_tunnel_, create_tunnel_map_entry(_, _, _, _))
            .Times(2)
            .WillRepeatedly(DoAll(
                        SetArgPointee<0>(vxlan_tunnel_map_entry_oid),
                        Return(SAI_STATUS_SUCCESS)
                        ));
        EXPECT_CALL(mock_sai_tunnel_, remove_tunnel_map_entry(_))
            .Times(2)
            .WillRepeatedly(DoAll(
                        Return(SAI_STATUS_FAILURE)
                        ));
        EXPECT_CALL(mock_sai_tunnel_, remove_tunnel_term_table_entry(_))
            .WillOnce(DoAll(
                        Return(SAI_STATUS_FAILURE)
                        ));
        EXPECT_CALL(mock_sai_tunnel_, remove_tunnel(_))
            .WillOnce(DoAll(
                        Return(SAI_STATUS_FAILURE)
                        ));
        EXPECT_CALL(mock_sai_tunnel_, remove_tunnel_map(_))
            .Times(4)
            .WillRepeatedly(DoAll(
                        Return(SAI_STATUS_FAILURE)
                        ));

        EXPECT_NO_THROW({
                bool result1 = vxlan_orch->createVxlanTunnelMap("vxlan_tunnel_1", TUNNEL_MAP_T_VIRTUAL_ROUTER, 1000, 0x1001, 0x1002, 64);
                EXPECT_TRUE(result1);
                bool result2 = vxlan_orch->removeVxlanTunnelMap("vxlan_tunnel_1", 1000);
                EXPECT_TRUE(result2);
                });
        vxlan_orch->delTunnel("vxlan_tunnel_1");
    }

    // Test basic VXLAN tunnel creation
    TEST_F(VxlanOrchTest, BasicTunnelCreation)
    {
        initSwitchOrch();
        initVxlanOrch();

        string tunnel_name = "tunnel1";
        string src_ip = "10.1.1.1";

        CreateBasicVxlanTunnel(tunnel_name, src_ip);

        // Verify tunnel exists
        EXPECT_TRUE(m_vxlan_tunnel_orch->isTunnelExists(tunnel_name));

        // Get tunnel object and verify properties
        VxlanTunnel* tunnel = m_vxlan_tunnel_orch->getVxlanTunnel(tunnel_name);
        ASSERT_NE(tunnel, nullptr);
        EXPECT_EQ(tunnel->getSrcIP().to_string(), src_ip);
        EXPECT_EQ(tunnel->getTunnelName(), tunnel_name);
    }

    // Test VXLAN tunnel deletion
    TEST_F(VxlanOrchTest, TunnelDeletion)
    {
        initSwitchOrch();
        initVxlanOrch();

        string tunnel_name = "tunnel1";
        string src_ip = "10.1.1.1";

        // Create tunnel
        CreateBasicVxlanTunnel(tunnel_name, src_ip);
        EXPECT_TRUE(m_vxlan_tunnel_orch->isTunnelExists(tunnel_name));

        // Delete tunnel directly from internal structures
        m_vxlan_tunnel_orch->vxlan_tunnel_table_.erase(tunnel_name);

        // Verify tunnel is deleted
        EXPECT_FALSE(m_vxlan_tunnel_orch->isTunnelExists(tunnel_name));
    }

    // Test VXLAN tunnel map creation
    TEST_F(VxlanOrchTest, TunnelMapCreation)
    {
        initSwitchOrch();
        initVxlanOrch();

        string tunnel_name = "tunnel1";
        string src_ip = "10.1.1.1";
        string map_name = "map1";
        string vni = "1000";
        string vlan = "Vlan100";

        // First create the tunnel
        CreateBasicVxlanTunnel(tunnel_name, src_ip);
        EXPECT_TRUE(m_vxlan_tunnel_orch->isTunnelExists(tunnel_name));

        // Create tunnel map
        CreateVxlanTunnelMap(tunnel_name, map_name, vni, vlan);

        // Verify tunnel map exists
        string full_key = tunnel_name + ":" + map_name;
        EXPECT_TRUE(m_vxlanTunnelMapOrch->isTunnelMapExists(full_key));
    }

    // Test next hop tunnel creation
    TEST_F(VxlanOrchTest, NextHopTunnelCreation)
    {
        initSwitchOrch();
        initVxlanOrch();

        string tunnel_name = "tunnel1";
        string src_ip = "10.1.1.1";
        string dst_ip = "10.1.1.2";

        CreateBasicVxlanTunnel(tunnel_name, src_ip);
        EXPECT_TRUE(m_vxlan_tunnel_orch->isTunnelExists(tunnel_name));

        VxlanTunnel* tunnel = m_vxlan_tunnel_orch->getVxlanTunnel(tunnel_name);
        ASSERT_NE(tunnel, nullptr);

        // Test tunnel properties
        EXPECT_EQ(tunnel->getSrcIP().to_string(), src_ip);
        EXPECT_EQ(tunnel->getTunnelName(), tunnel_name);

        // Test next hop tunnel methods with proper error handling
        IpAddress nh_ip(dst_ip);
        MacAddress nh_mac("00:11:22:33:44:55");
        uint32_t vni = 1000;

        // In mock environment, createNextHopTunnel will fail because SAI calls fail
        // We test that it handles the failure gracefully by catching the exception
        EXPECT_EQ(m_vxlan_tunnel_orch->createNextHopTunnel(tunnel_name, nh_ip, nh_mac, vni), SAI_NULL_OBJECT_ID);

        // Test that removeNextHopTunnel returns false for non-existent next hop
        bool result = m_vxlan_tunnel_orch->removeNextHopTunnel(tunnel_name, nh_ip, nh_mac, vni);
        EXPECT_FALSE(result);
    }

    // Test dynamic DIP tunnel creation and cleanup
    TEST_F(VxlanOrchTest, DynamicDipTunnelCleanup)
    {
        initSwitchOrch();
        initVxlanOrch();

        string tunnel_name = "tunnel1";
        string src_ip = "10.1.1.1";
        string remote_vtep = "10.1.1.2";

        // Create base tunnel
        CreateBasicVxlanTunnel(tunnel_name, src_ip);
        EXPECT_TRUE(m_vxlan_tunnel_orch->isTunnelExists(tunnel_name));

        VxlanTunnel* tunnel = m_vxlan_tunnel_orch->getVxlanTunnel(tunnel_name);
        ASSERT_NE(tunnel, nullptr);

        // Initially, remote VTEP should not exist in the reference count map
        int initial_ref_count = tunnel->getRemoteEndPointRefCnt(remote_vtep);
        EXPECT_EQ(initial_ref_count, -1); // -1 means not found

        // Manually setup tunnel user tracking for testing
        // This simulates what happens when createDynamicDIPTunnel is called successfully
        tunnel_refcnt_t ref_counts;
        memset(&ref_counts, 0, sizeof(tunnel_refcnt_t));
        ref_counts.imr_refcnt = 1; // Simulate one IMR reference

        // Access the private member to set up the test scenario
        tunnel->tnl_users_[remote_vtep] = ref_counts;

        // Now verify tunnel user count
        int ref_count = tunnel->getRemoteEndPointRefCnt(remote_vtep);
        EXPECT_EQ(ref_count, 1); // Should be 1 (imr_refcnt)

        // Test cleanup when reference count is not zero (should not cleanup)
        // The cleanupDynamicDIPTunnel method only does cleanup when ref count is 0
        tunnel->cleanupDynamicDIPTunnel(remote_vtep);
        int ref_count_after = tunnel->getRemoteEndPointRefCnt(remote_vtep);
        EXPECT_EQ(ref_count_after, ref_count); // Should remain the same

        // Simulate reference count going to zero
        ref_counts.imr_refcnt = 0;
        ref_counts.ip_refcnt = 0;
        tunnel->tnl_users_[remote_vtep] = ref_counts;

        // Verify reference count is zero
        ref_count = tunnel->getRemoteEndPointRefCnt(remote_vtep);
        EXPECT_EQ(ref_count, 0);

        // Note: We don't call cleanupDynamicDIPTunnel with ref_count 0 in mock tests
        // because it requires full SAI/PortsOrch infrastructure for tunnel port cleanup
    }

    // Test tunnel user management
    TEST_F(VxlanOrchTest, TunnelUserManagement)
    {
        initSwitchOrch();
        initVxlanOrch();

        string tunnel_name = "tunnel1";
        string src_ip = "10.1.1.1";
        string remote_vtep = "10.1.1.2";
        uint32_t vni = 1000;
        uint32_t vlan = 100;

        // Create EVPN NVO first
        CreateEvpnNvo("nvo1", tunnel_name);

        // Create base tunnel
        CreateBasicVxlanTunnel(tunnel_name, src_ip);
        EXPECT_TRUE(m_vxlan_tunnel_orch->isTunnelExists(tunnel_name));

        // Set up EVPN VTEP pointer
        VxlanTunnel* tunnel = m_vxlan_tunnel_orch->getVxlanTunnel(tunnel_name);
        ASSERT_NE(tunnel, nullptr);
        m_evpnNvoOrch->source_vtep_ptr = tunnel;

        // Set up tunnel user tracking manually for testing
        // This simulates what would happen when addTunnelUser is called successfully
        tunnel_refcnt_t ref_counts;
        memset(&ref_counts, 0, sizeof(tunnel_refcnt_t));
        ref_counts.imr_refcnt = 1; // Simulate one IMR reference
        ref_counts.ip_refcnt = 1;  // Simulate one IP reference

        // Access the private member to set up the test scenario
        tunnel->tnl_users_[remote_vtep] = ref_counts;

        // Test reference counting functions
        int imr_count = tunnel->getRemoteEndPointIMRRefCnt(remote_vtep);
        int ip_count = tunnel->getRemoteEndPointIPRefCnt(remote_vtep);
        int total_count = tunnel->getRemoteEndPointRefCnt(remote_vtep);

        EXPECT_EQ(imr_count, 1);
        EXPECT_EQ(ip_count, 1);
        EXPECT_EQ(total_count, imr_count + ip_count);

        // Test addTunnelUser interface (may fail due to missing VLAN setup, but we're testing the interface)
        bool added = m_vxlan_tunnel_orch->addTunnelUser(remote_vtep, vni, vlan, TUNNEL_USER_IMR);
        (void)added; // Suppress unused variable warning - result may vary in mock environment
    }

    // Test VXLAN tunnel port operations
    TEST_F(VxlanOrchTest, TunnelPortOperations)
    {
        initSwitchOrch();
        initVxlanOrch();

        string vtep_ip = "10.1.1.2";

        // Test getTunnelPortName
        string local_port_name = m_vxlan_tunnel_orch->getTunnelPortName(vtep_ip, true);
        string remote_port_name = m_vxlan_tunnel_orch->getTunnelPortName(vtep_ip, false);

        EXPECT_FALSE(local_port_name.empty());
        EXPECT_FALSE(remote_port_name.empty());
        EXPECT_NE(local_port_name, remote_port_name);

        // Test tunnel name extraction
        string tunnel_name;
        m_vxlan_tunnel_orch->getTunnelNameFromDIP(vtep_ip, tunnel_name);
        EXPECT_FALSE(tunnel_name.empty());

        // Test port name to tunnel name conversion
        string extracted_tunnel_name;
        m_vxlan_tunnel_orch->getTunnelNameFromPort(remote_port_name, extracted_tunnel_name);
        EXPECT_FALSE(extracted_tunnel_name.empty());
    }

    // Test VXLAN VNI to VLAN mapping
    TEST_F(VxlanOrchTest, VniVlanMapping)
    {
        initSwitchOrch();
        initVxlanOrch();

        uint32_t vni = 1000;
        uint16_t vlan_id = 100;

        // Add VNI to VLAN mapping
        m_vxlan_tunnel_orch->addVlanMappedToVni(vni, vlan_id);

        // Verify mapping
        uint16_t retrieved_vlan = m_vxlan_tunnel_orch->getVlanMappedToVni(vni);
        EXPECT_EQ(retrieved_vlan, vlan_id);

        // Test non-existent VNI
        uint16_t non_existent = m_vxlan_tunnel_orch->getVlanMappedToVni(9999);
        EXPECT_EQ(non_existent, 0);

        // Delete mapping
        m_vxlan_tunnel_orch->delVlanMappedToVni(vni);
        retrieved_vlan = m_vxlan_tunnel_orch->getVlanMappedToVni(vni);
        EXPECT_EQ(retrieved_vlan, 0);
    }

    // Test reference counting edge cases
    TEST_F(VxlanOrchTest, ReferenceCountingEdgeCases)
    {
        initSwitchOrch();
        initVxlanOrch();

        string tunnel_name = "tunnel1";
        string src_ip = "10.1.1.1";
        string remote_vtep = "10.1.1.2";

        CreateBasicVxlanTunnel(tunnel_name, src_ip);
        VxlanTunnel* tunnel = m_vxlan_tunnel_orch->getVxlanTunnel(tunnel_name);
        ASSERT_NE(tunnel, nullptr);

        // Test spurious IMR add/del tracking
        tunnel->increment_spurious_imr_add(remote_vtep);
        tunnel->increment_spurious_imr_del(remote_vtep);

        // Set up tunnel user tracking manually for testing IP reference operations
        tunnel_refcnt_t ref_counts;
        memset(&ref_counts, 0, sizeof(tunnel_refcnt_t));
        ref_counts.ip_refcnt = 1;  // Start with 1 IP reference

        // Access the private member to set up the test scenario
        tunnel->tnl_users_[remote_vtep] = ref_counts;

        // Test IP reference tracking
        tunnel->updateRemoteEndPointIpRef(remote_vtep, true);  // increment
        int ip_count = tunnel->getRemoteEndPointIPRefCnt(remote_vtep);
        EXPECT_EQ(ip_count, 2); // Should be 2 (1 initial + 1 increment)

        tunnel->updateRemoteEndPointIpRef(remote_vtep, false); // decrement
        int ip_count_after = tunnel->getRemoteEndPointIPRefCnt(remote_vtep);
        EXPECT_EQ(ip_count_after, ip_count - 1); // Should be 1 (2 - 1)

        // Test DIP tunnel count
        int dip_count = tunnel->getDipTunnelCnt();
        EXPECT_GE(dip_count, 0);
    }

    // Test error conditions and edge cases
    TEST_F(VxlanOrchTest, ErrorConditions)
    {
        initSwitchOrch();
        initVxlanOrch();

        // Test operations on non-existent tunnel
        EXPECT_FALSE(m_vxlan_tunnel_orch->isTunnelExists("non_existent"));

        // Test tunnel port retrieval for non-existent VTEP
        Port dummy_port;
        bool found = m_vxlan_tunnel_orch->getTunnelPort("192.168.1.1", dummy_port);
        EXPECT_FALSE(found);

        // Test createVxlanTunnelMap with invalid parameters
        bool result = m_vxlan_tunnel_orch->createVxlanTunnelMap("non_existent",
                                                              TUNNEL_MAP_T_VLAN,
                                                              1000,
                                                              SAI_NULL_OBJECT_ID,
                                                              SAI_NULL_OBJECT_ID);
        EXPECT_FALSE(result);

        // Test removeVxlanTunnelMap on non-existent tunnel
        result = m_vxlan_tunnel_orch->removeVxlanTunnelMap("non_existent", 1000);
        EXPECT_FALSE(result);
    }

    // EVPN remote VNI handling driven through the APP_DB tables, with a P2P
    // (per remote VTEP) tunnel as used when the SAI supports DIP tunnels.
    static bool g_failTunnelVlanMember = false;
    static sai_status_t g_tunnelVlanMemberFailStatus = SAI_STATUS_INSUFFICIENT_RESOURCES;
    static int g_tunnelVlanMemberCreates = 0;
    static bool g_failP2pTunnel = false;
    static bool g_failL2mcGroupMember = false;
    static int g_l2mcGroupMemberCreates = 0;
    static decltype(sai_vlan_api->create_vlan_member) g_savedCreateVlanMember = nullptr;
    static decltype(sai_tunnel_api->create_tunnel) g_savedCreateTunnel = nullptr;
    static decltype(sai_l2mc_group_api->create_l2mc_group_member) g_savedCreateL2mcGroupMember = nullptr;

    static sai_status_t stubCreateVlanMember(sai_object_id_t *id, sai_object_id_t sw,
                                             uint32_t n, const sai_attribute_t *attrs)
    {
        g_tunnelVlanMemberCreates++;
        if (g_failTunnelVlanMember)
        {
            return g_tunnelVlanMemberFailStatus;
        }
        return g_savedCreateVlanMember(id, sw, n, attrs);
    }

    static sai_status_t stubCreateL2mcGroupMember(sai_object_id_t *id, sai_object_id_t sw,
                                                  uint32_t n, const sai_attribute_t *attrs)
    {
        g_l2mcGroupMemberCreates++;
        if (g_failL2mcGroupMember)
        {
            return SAI_STATUS_INSUFFICIENT_RESOURCES;
        }
        return g_savedCreateL2mcGroupMember(id, sw, n, attrs);
    }

    static sai_status_t stubCreateTunnel(sai_object_id_t *id, sai_object_id_t sw,
                                         uint32_t n, const sai_attribute_t *attrs)
    {
        for (uint32_t i = 0; i < n; i++)
        {
            if (g_failP2pTunnel && attrs[i].id == SAI_TUNNEL_ATTR_PEER_MODE &&
                attrs[i].value.s32 == SAI_TUNNEL_PEER_MODE_P2P)
            {
                return SAI_STATUS_INSUFFICIENT_RESOURCES;
            }
        }
        return g_savedCreateTunnel(id, sw, n, attrs);
    }

    class EvpnRemoteVniTest : public MockOrchTest
    {
    protected:
        VxlanTunnelMapOrch *m_tunnelMapOrch = nullptr;
        EvpnNvoOrch *m_nvoOrch = nullptr;
        Orch *m_remoteVniOrch = nullptr;

        const string m_remoteVtep = "10.1.1.2";
        const string m_remoteVniKey = "Vlan100:10.1.1.2";

        void ApplyInitialConfigs() override
        {
            Table port_table(m_app_db.get(), APP_PORT_TABLE_NAME);
            auto ports = ut_helper::getInitialSaiPorts();
            for (const auto &it : ports)
            {
                port_table.set(it.first, it.second);
            }
            port_table.set("PortConfigDone", {{"count", to_string(ports.size())}});
            port_table.set("PortInitDone", {{}});
            gPortsOrch->addExistingData(&port_table);
            static_cast<Orch *>(gPortsOrch)->doTask();
        }

        void PostSetUp() override
        {
            g_failTunnelVlanMember = false;
            g_tunnelVlanMemberFailStatus = SAI_STATUS_INSUFFICIENT_RESOURCES;
            g_tunnelVlanMemberCreates = 0;
            g_failP2pTunnel = false;
            g_failL2mcGroupMember = false;
            g_l2mcGroupMemberCreates = 0;
            g_savedCreateVlanMember = sai_vlan_api->create_vlan_member;
            sai_vlan_api->create_vlan_member = stubCreateVlanMember;
            g_savedCreateTunnel = sai_tunnel_api->create_tunnel;
            sai_tunnel_api->create_tunnel = stubCreateTunnel;
            g_savedCreateL2mcGroupMember = sai_l2mc_group_api->create_l2mc_group_member;
            sai_l2mc_group_api->create_l2mc_group_member = stubCreateL2mcGroupMember;

            m_VxlanTunnelOrch->is_dip_tunnel_supported = useDipTunnels();

            m_tunnelMapOrch = new VxlanTunnelMapOrch(m_app_db.get(), APP_VXLAN_TUNNEL_MAP_TABLE_NAME);
            gDirectory.set(m_tunnelMapOrch);
            m_nvoOrch = new EvpnNvoOrch(m_app_db.get(), APP_VXLAN_EVPN_NVO_TABLE_NAME);
            gDirectory.set(m_nvoOrch);
            if (useDipTunnels())
            {
                auto p2pOrch = new EvpnRemoteVnip2pOrch(m_app_db.get(), APP_VXLAN_REMOTE_VNI_TABLE_NAME);
                gDirectory.set(p2pOrch);
                m_remoteVniOrch = p2pOrch;
            }
            else
            {
                // A P2MP flood list is an L2MC group, which needs the combined flood control type.
                gPortsOrch->uuc_sup_flood_control_type.insert(SAI_VLAN_FLOOD_CONTROL_TYPE_COMBINED);
                gPortsOrch->bc_sup_flood_control_type.insert(SAI_VLAN_FLOOD_CONTROL_TYPE_COMBINED);
                auto p2mpOrch = new EvpnRemoteVnip2mpOrch(m_app_db.get(), APP_VXLAN_REMOTE_VNI_TABLE_NAME);
                gDirectory.set(p2mpOrch);
                m_remoteVniOrch = p2mpOrch;
            }

            Table vlan(m_app_db.get(), APP_VLAN_TABLE_NAME);
            vlan.set("Vlan100", {{"admin_status", "up"}, {"mtu", "9100"}});
            gPortsOrch->addExistingData(&vlan);
            static_cast<Orch *>(gPortsOrch)->doTask();

            Table tunnel(m_app_db.get(), APP_VXLAN_TUNNEL_TABLE_NAME);
            tunnel.set("vtep1", {{"src_ip", "10.1.1.1"}});
            m_VxlanTunnelOrch->addExistingData(&tunnel);
            static_cast<Orch *>(m_VxlanTunnelOrch)->doTask();

            Table nvo(m_app_db.get(), APP_VXLAN_EVPN_NVO_TABLE_NAME);
            nvo.set("nvo1", {{"source_vtep", "vtep1"}});
            m_nvoOrch->addExistingData(&nvo);
            static_cast<Orch *>(m_nvoOrch)->doTask();

            Table tunnelMap(m_app_db.get(), APP_VXLAN_TUNNEL_MAP_TABLE_NAME);
            tunnelMap.set("vtep1:map_1000_Vlan100", {{"vni", "1000"}, {"vlan", "Vlan100"}});
            m_tunnelMapOrch->addExistingData(&tunnelMap);
            static_cast<Orch *>(m_tunnelMapOrch)->doTask();

            ASSERT_NE(m_nvoOrch->getEVPNVtep(), nullptr);
            ASSERT_TRUE(m_nvoOrch->getEVPNVtep()->isActive());
        }

        void PreTearDown() override
        {
            delete m_remoteVniOrch;
            m_remoteVniOrch = nullptr;
            delete m_nvoOrch;
            m_nvoOrch = nullptr;
            delete m_tunnelMapOrch;
            m_tunnelMapOrch = nullptr;

            sai_vlan_api->create_vlan_member = g_savedCreateVlanMember;
            sai_tunnel_api->create_tunnel = g_savedCreateTunnel;
            sai_l2mc_group_api->create_l2mc_group_member = g_savedCreateL2mcGroupMember;
            // A non-retryable SAI failure in a test marks orchagent unhealthy.
            setSaiFailureStatus(false);
        }

        virtual bool useDipTunnels() const
        {
            return true;
        }

        Consumer *remoteVniConsumer()
        {
            return dynamic_cast<Consumer *>(m_remoteVniOrch->getExecutor(APP_VXLAN_REMOTE_VNI_TABLE_NAME));
        }

        void setRemoteVni()
        {
            remoteVniConsumer()->addToSync({{m_remoteVniKey, SET_COMMAND, {{"vni", "1000"}}}});
            static_cast<Orch *>(m_remoteVniOrch)->doTask();
        }

        // A DEL carries no attributes, as ConsumerStateTable delivers it.
        void delRemoteVni()
        {
            remoteVniConsumer()->addToSync({{m_remoteVniKey, DEL_COMMAND, {}}});
            static_cast<Orch *>(m_remoteVniOrch)->doTask();
        }

        void retryRemoteVni()
        {
            static_cast<Orch *>(m_remoteVniOrch)->doTask();
        }

        int imrRefCount()
        {
            return m_nvoOrch->getEVPNVtep()->getRemoteEndPointIMRRefCnt(m_remoteVtep);
        }

        bool tunnelPortExists()
        {
            Port tunnelPort;
            return m_VxlanTunnelOrch->getTunnelPort(m_remoteVtep, tunnelPort);
        }

        bool isFloodMember()
        {
            Port vlanPort, tunnelPort;
            if (!gPortsOrch->getVlanByVlanId(100, vlanPort) ||
                !m_VxlanTunnelOrch->getTunnelPort(m_remoteVtep, tunnelPort))
            {
                return false;
            }
            return gPortsOrch->isVlanMember(vlanPort, tunnelPort);
        }

        bool dipTunnelExists()
        {
            string name;
            m_VxlanTunnelOrch->getTunnelNameFromDIP(m_remoteVtep, name);
            return m_VxlanTunnelOrch->isTunnelExists(name);
        }
    };

    // Baseline: the remote VTEP is added to and removed from the VLAN flood list.
    TEST_F(EvpnRemoteVniTest, RemoteVniAddDel)
    {
        setRemoteVni();
        EXPECT_TRUE(remoteVniConsumer()->m_toSync.empty());
        EXPECT_TRUE(isFloodMember());
        EXPECT_EQ(imrRefCount(), 1);

        delRemoteVni();
        EXPECT_TRUE(remoteVniConsumer()->m_toSync.empty());
        EXPECT_FALSE(tunnelPortExists());
        EXPECT_FALSE(dipTunnelExists());
        EXPECT_EQ(imrRefCount(), -1);
    }

    // A refused VLAN member keeps the request queued for retry. The retry adds
    // only the VLAN member: it neither takes another tunnel reference nor
    // rebuilds the tunnel. Once the SAI accepts the member the entry completes,
    // and a later DEL removes everything.
    TEST_F(EvpnRemoteVniTest, VlanMemberFailureIsRetried)
    {
        g_failTunnelVlanMember = true;
        setRemoteVni();

        EXPECT_EQ(remoteVniConsumer()->m_toSync.size(), 1U);
        EXPECT_EQ(g_tunnelVlanMemberCreates, 1);
        EXPECT_FALSE(isFloodMember());
        EXPECT_TRUE(tunnelPortExists());
        EXPECT_EQ(imrRefCount(), 1);

        retryRemoteVni();
        EXPECT_EQ(remoteVniConsumer()->m_toSync.size(), 1U);
        EXPECT_EQ(g_tunnelVlanMemberCreates, 2);
        EXPECT_EQ(imrRefCount(), 1);

        g_failTunnelVlanMember = false;
        retryRemoteVni();
        EXPECT_TRUE(remoteVniConsumer()->m_toSync.empty());
        EXPECT_TRUE(isFloodMember());
        EXPECT_EQ(imrRefCount(), 1);

        delRemoteVni();
        EXPECT_FALSE(tunnelPortExists());
        EXPECT_FALSE(dipTunnelExists());
        EXPECT_EQ(imrRefCount(), -1);
    }

    // A VLAN member add that keeps failing is logged at ERROR once per entry, not
    // on every retry. Success or a DEL ends the entry, and the next failure is
    // logged again.
    TEST_F(EvpnRemoteVniTest, VlanMemberFailureLoggedOnce)
    {
        const string retryLog = "Failed to add remote VTEP " + m_remoteVtep + " to the flood list of vid 100 (vni 1000), will retry";
        auto countRetryErrors = [&retryLog](const string &out) {
            size_t count = 0;
            for (size_t pos = out.find(retryLog); pos != string::npos; pos = out.find(retryLog, pos + 1))
            {
                count++;
            }
            return count;
        };

        swss::Logger::swssOutputNotify("orchagent", "STDERR");
        testing::internal::CaptureStderr();

        g_failTunnelVlanMember = true;
        setRemoteVni();
        retryRemoteVni();
        retryRemoteVni();
        EXPECT_EQ(g_tunnelVlanMemberCreates, 3);
        EXPECT_EQ(remoteVniConsumer()->m_toSync.size(), 1U);
        string out = testing::internal::GetCapturedStderr();
        EXPECT_EQ(countRetryErrors(out), 1U);

        // Once the add succeeds, a later failure of the same entry is logged again.
        testing::internal::CaptureStderr();
        g_failTunnelVlanMember = false;
        retryRemoteVni();
        EXPECT_TRUE(isFloodMember());
        delRemoteVni();
        g_failTunnelVlanMember = true;
        setRemoteVni();
        retryRemoteVni();
        out = testing::internal::GetCapturedStderr();
        EXPECT_EQ(countRetryErrors(out), 1U);

        // A DEL while the add is still failing also resets it.
        testing::internal::CaptureStderr();
        delRemoteVni();
        setRemoteVni();
        out = testing::internal::GetCapturedStderr();
        EXPECT_EQ(countRetryErrors(out), 1U);

        const char *syslogStdout = std::getenv("SWSS_SYSLOG_STDOUT");
        swss::Logger::swssOutputNotify("orchagent",
            (syslogStdout != nullptr && string(syslogStdout) == "1") ? "STDOUT" : "SYSLOG");
    }

    // A DEL for an entry whose VLAN member was never added releases the tunnel
    // taken for it instead of being dropped as spurious.
    TEST_F(EvpnRemoteVniTest, DelAfterVlanMemberFailureReleasesTunnel)
    {
        g_failTunnelVlanMember = true;
        setRemoteVni();
        ASSERT_TRUE(tunnelPortExists());
        ASSERT_TRUE(dipTunnelExists());

        delRemoteVni();
        EXPECT_TRUE(remoteVniConsumer()->m_toSync.empty());
        EXPECT_FALSE(tunnelPortExists());
        EXPECT_FALSE(dipTunnelExists());
        EXPECT_EQ(imrRefCount(), -1);

        // The entry can be added again from scratch.
        g_failTunnelVlanMember = false;
        setRemoteVni();
        EXPECT_TRUE(remoteVniConsumer()->m_toSync.empty());
        EXPECT_TRUE(isFloodMember());
        EXPECT_EQ(imrRefCount(), 1);
    }

    // A VLAN member create that PortsOrch does not retry (a non-resource failure)
    // consumes the entry, but its DEL still releases the tunnel taken for it.
    TEST_F(EvpnRemoteVniTest, VlanMemberTerminalFailureReleasesTunnelOnDel)
    {
        g_failTunnelVlanMember = true;
        g_tunnelVlanMemberFailStatus = SAI_STATUS_FAILURE;
        setRemoteVni();

        EXPECT_TRUE(remoteVniConsumer()->m_toSync.empty());
        EXPECT_EQ(g_tunnelVlanMemberCreates, 1);
        EXPECT_FALSE(isFloodMember());
        EXPECT_TRUE(tunnelPortExists());
        EXPECT_EQ(imrRefCount(), 1);

        delRemoteVni();
        EXPECT_TRUE(remoteVniConsumer()->m_toSync.empty());
        EXPECT_FALSE(tunnelPortExists());
        EXPECT_FALSE(dipTunnelExists());
        EXPECT_EQ(imrRefCount(), -1);
    }

    // A SET of an entry whose VLAN member create was not retried adds only the
    // VLAN member, on the tunnel reference the entry already holds.
    TEST_F(EvpnRemoteVniTest, SetAfterVlanMemberTerminalFailureAddsMember)
    {
        g_failTunnelVlanMember = true;
        g_tunnelVlanMemberFailStatus = SAI_STATUS_FAILURE;
        setRemoteVni();
        ASSERT_TRUE(remoteVniConsumer()->m_toSync.empty());
        ASSERT_FALSE(isFloodMember());

        g_failTunnelVlanMember = false;
        setRemoteVni();
        EXPECT_TRUE(remoteVniConsumer()->m_toSync.empty());
        EXPECT_EQ(g_tunnelVlanMemberCreates, 2);
        EXPECT_TRUE(isFloodMember());
        EXPECT_EQ(imrRefCount(), 1);

        delRemoteVni();
        EXPECT_FALSE(tunnelPortExists());
        EXPECT_FALSE(dipTunnelExists());
        EXPECT_EQ(imrRefCount(), -1);
    }

    // The same, with the P2MP tunnel used when the SAI does not support DIP
    // tunnels: the remote VTEP is an L2MC group member of the VLAN flood list.
    class EvpnRemoteVniP2mpTest : public EvpnRemoteVniTest
    {
    protected:
        bool useDipTunnels() const override
        {
            return false;
        }

        bool isEndpointFloodMember()
        {
            Port vlanPort, tunnelPort;
            if (!gPortsOrch->getVlanByVlanId(100, vlanPort) ||
                !m_VxlanTunnelOrch->getTunnelPort("10.1.1.1", tunnelPort, true))
            {
                return false;
            }
            return gPortsOrch->isVlanMember(vlanPort, tunnelPort, m_remoteVtep);
        }
    };

    // A refused L2MC group member keeps the request queued for retry, and the
    // entry completes once the SAI accepts it.
    TEST_F(EvpnRemoteVniP2mpTest, FloodGroupMemberFailureIsRetried)
    {
        g_failL2mcGroupMember = true;
        setRemoteVni();

        EXPECT_EQ(remoteVniConsumer()->m_toSync.size(), 1U);
        EXPECT_EQ(g_l2mcGroupMemberCreates, 1);
        EXPECT_FALSE(isEndpointFloodMember());

        retryRemoteVni();
        EXPECT_EQ(remoteVniConsumer()->m_toSync.size(), 1U);
        EXPECT_EQ(g_l2mcGroupMemberCreates, 2);
        EXPECT_FALSE(isEndpointFloodMember());

        g_failL2mcGroupMember = false;
        retryRemoteVni();
        EXPECT_TRUE(remoteVniConsumer()->m_toSync.empty());
        EXPECT_EQ(g_l2mcGroupMemberCreates, 3);
        EXPECT_TRUE(isEndpointFloodMember());

        delRemoteVni();
        EXPECT_TRUE(remoteVniConsumer()->m_toSync.empty());
        EXPECT_FALSE(isEndpointFloodMember());
    }

    // A DEL while the L2MC group member add is still failing drops the queued
    // add, and a new SET is added normally.
    TEST_F(EvpnRemoteVniP2mpTest, DelWhileFloodGroupMemberFailing)
    {
        g_failL2mcGroupMember = true;
        setRemoteVni();
        ASSERT_EQ(remoteVniConsumer()->m_toSync.size(), 1U);

        delRemoteVni();
        EXPECT_TRUE(remoteVniConsumer()->m_toSync.empty());
        EXPECT_FALSE(isEndpointFloodMember());

        g_failL2mcGroupMember = false;
        setRemoteVni();
        EXPECT_TRUE(remoteVniConsumer()->m_toSync.empty());
        EXPECT_TRUE(isEndpointFloodMember());
    }

    // If the P2P tunnel to the remote VTEP cannot be created, nothing is
    // recorded for it and the request is retried.
    TEST_F(EvpnRemoteVniTest, DipTunnelFailureIsRetried)
    {
        g_failP2pTunnel = true;
        setRemoteVni();

        EXPECT_EQ(remoteVniConsumer()->m_toSync.size(), 1U);
        EXPECT_FALSE(dipTunnelExists());
        EXPECT_FALSE(tunnelPortExists());
        EXPECT_EQ(imrRefCount(), -1);
        EXPECT_EQ(g_tunnelVlanMemberCreates, 0);

        g_failP2pTunnel = false;
        retryRemoteVni();
        EXPECT_TRUE(remoteVniConsumer()->m_toSync.empty());
        EXPECT_TRUE(isFloodMember());
        EXPECT_EQ(imrRefCount(), 1);
    }


    static decltype(sai_tunnel_api->create_tunnel_map) g_savedCreateTunnelMap = nullptr;
    static int g_tunnelCreates = 0;

    static sai_status_t stubTunnelMapNotSupported(sai_object_id_t *id, sai_object_id_t,
                                                  uint32_t, const sai_attribute_t *)
    {
        *id = SAI_NULL_OBJECT_ID;
        return SAI_STATUS_NOT_SUPPORTED;
    }

    static sai_status_t stubCountCreateTunnel(sai_object_id_t *id, sai_object_id_t sw,
                                              uint32_t n, const sai_attribute_t *attrs)
    {
        g_tunnelCreates++;
        return g_savedCreateTunnel(id, sw, n, attrs);
    }

    // A VRF-VNI map on a switch that supports no tunnel map type, with the
    // P2MP tunnel port used when the SAI does not support DIP tunnels.
    class VxlanVrfMapNoTunnelMapTest : public MockOrchTest
    {
    protected:
        VxlanTunnelMapOrch *m_tunnelMapOrch = nullptr;
        VxlanVrfMapOrch *m_vrfMapOrch = nullptr;

        void ApplyInitialConfigs() override
        {
            Table port_table(m_app_db.get(), APP_PORT_TABLE_NAME);
            auto ports = ut_helper::getInitialSaiPorts();
            for (const auto &it : ports)
            {
                port_table.set(it.first, it.second);
            }
            port_table.set("PortConfigDone", {{"count", to_string(ports.size())}});
            port_table.set("PortInitDone", {{}});
            gPortsOrch->addExistingData(&port_table);
            static_cast<Orch *>(gPortsOrch)->doTask();
        }

        void PostSetUp() override
        {
            g_tunnelCreates = 0;
            g_savedCreateTunnelMap = sai_tunnel_api->create_tunnel_map;
            sai_tunnel_api->create_tunnel_map = stubTunnelMapNotSupported;
            g_savedCreateTunnel = sai_tunnel_api->create_tunnel;
            sai_tunnel_api->create_tunnel = stubCountCreateTunnel;

            m_VxlanTunnelOrch->is_dip_tunnel_supported = false;

            m_tunnelMapOrch = new VxlanTunnelMapOrch(m_app_db.get(), APP_VXLAN_TUNNEL_MAP_TABLE_NAME);
            gDirectory.set(m_tunnelMapOrch);
            m_vrfMapOrch = new VxlanVrfMapOrch(m_app_db.get(), APP_VXLAN_VRF_TABLE_NAME);
            gDirectory.set(m_vrfMapOrch);

            Table vrf(m_app_db.get(), APP_VRF_TABLE_NAME);
            vrf.set("Vrf1", {{"v4", "true"}});
            gVrfOrch->addExistingData(&vrf);
            static_cast<Orch *>(gVrfOrch)->doTask();
            ASSERT_TRUE(gVrfOrch->isVRFexists("Vrf1"));

            Table tunnel(m_app_db.get(), APP_VXLAN_TUNNEL_TABLE_NAME);
            tunnel.set("vtep1", {{"src_ip", "10.1.1.1"}});
            m_VxlanTunnelOrch->addExistingData(&tunnel);
            static_cast<Orch *>(m_VxlanTunnelOrch)->doTask();
            ASSERT_TRUE(m_VxlanTunnelOrch->isTunnelExists("vtep1"));
        }

        void PreTearDown() override
        {
            delete m_vrfMapOrch;
            m_vrfMapOrch = nullptr;
            delete m_tunnelMapOrch;
            m_tunnelMapOrch = nullptr;

            sai_tunnel_api->create_tunnel_map = g_savedCreateTunnelMap;
            sai_tunnel_api->create_tunnel = g_savedCreateTunnel;
            setSaiFailureStatus(false);
        }

        Consumer *vrfMapConsumer()
        {
            return dynamic_cast<Consumer *>(m_vrfMapOrch->getExecutor(APP_VXLAN_VRF_TABLE_NAME));
        }
    };

    // The tunnel cannot be created, so the map is not applied and the request
    // is retried. No tunnel port is added with a null tunnel id.
    TEST_F(VxlanVrfMapNoTunnelMapTest, VrfMapWaitsForTunnel)
    {
        setSaiFailureStatus(false);

        vrfMapConsumer()->addToSync({{"vtep1:evpn_map_1000_Vrf1", SET_COMMAND,
                                      {{"vni", "1000"}, {"vrf", "Vrf1"}}}});
        static_cast<Orch *>(m_vrfMapOrch)->doTask();

        EXPECT_EQ(vrfMapConsumer()->m_toSync.size(), 1U);
        EXPECT_FALSE(m_vrfMapOrch->isVrfMapExists("vtep1:evpn_map_1000_Vrf1"));
        EXPECT_FALSE(m_VxlanTunnelOrch->getVxlanTunnel("vtep1")->isActive());
        EXPECT_EQ(g_tunnelCreates, 0);

        Port tunnelPort;
        EXPECT_FALSE(m_VxlanTunnelOrch->getTunnelPort("10.1.1.1", tunnelPort, true));

        std::string error;
        EXPECT_FALSE(getSaiFailureStatus(error)) << error;
    }

    class VxlanOrchRemoteVniTest : public VxlanOrchTest
    {
        protected:
        // A remote VNI DEL carries no fields. Sets up what the DEL path looks up (the
        // EVPN source VTEP, the remote end point, the VLAN and the remote's tunnel port,
        // which is not a VLAN member) and feeds the given entries to the orch, so the
        // DEL ends on the "spurious delete" path that counts it on the end point.
        uint32_t spuriousRemoteVniDels(Orch *remote_vni_orch, const deque<KeyOpFieldsValuesTuple> &entries,
                                       const string &remote_vtep, const string &tunnel_port)
        {
            CreateBasicVxlanTunnel("vtep1", "10.1.1.1");
            VxlanTunnel *vtep = m_vxlan_tunnel_orch->getVxlanTunnel("vtep1");
            m_evpnNvoOrch->source_vtep_ptr = vtep;

            tunnel_refcnt_t ref_counts;
            memset(&ref_counts, 0, sizeof(tunnel_refcnt_t));
            ref_counts.imr_refcnt = 1;
            vtep->tnl_users_[remote_vtep] = ref_counts;

            Port vlan("Vlan100", Port::VLAN);
            vlan.m_vlan_info.vlan_id = 100;
            gPortsOrch->m_portList["Vlan100"] = vlan;
            gPortsOrch->m_portList[tunnel_port] = Port(tunnel_port, Port::TUNNEL);

            auto consumer = dynamic_cast<Consumer *>(remote_vni_orch->getExecutor(APP_VXLAN_REMOTE_VNI_TABLE_NAME));
            consumer->addToSync(entries);
            remote_vni_orch->doTask();
            EXPECT_TRUE(consumer->m_toSync.empty());

            return vtep->tnl_users_[remote_vtep].spurious_del_imr_refcnt;
        }
    };

    // The first remote VNI request an orchagent sees can be a DEL (e.g. on a cold start
    // with EVPN peers up). It must be handled, not thrown away by the request parser.
    TEST_F(VxlanOrchRemoteVniTest, RemoteVniP2pDelAsFirstRequest)
    {
        initSwitchOrch();
        initVxlanOrch();

        deque<KeyOpFieldsValuesTuple> entries = {
            { "Vlan100:10.1.1.2", DEL_COMMAND, {} }
        };
        EXPECT_EQ(spuriousRemoteVniDels(m_evpnRemoteVnip2pOrch, entries, "10.1.1.2", "Port_EVPN_10.1.1.2"), 1u);
    }

    // A DEL must not act on the VNI of whatever request the orch parsed before it.
    TEST_F(VxlanOrchRemoteVniTest, RemoteVniP2pDelIgnoresEarlierVni)
    {
        initSwitchOrch();
        initVxlanOrch();

        // An out-of-range VNI: the SET is dropped, but the parser has seen its VNI.
        auto consumer = dynamic_cast<Consumer *>(m_evpnRemoteVnip2pOrch->getExecutor(APP_VXLAN_REMOTE_VNI_TABLE_NAME));
        consumer->addToSync(deque<KeyOpFieldsValuesTuple>{
            { "Vlan200:10.1.1.3", SET_COMMAND, { { "vni", to_string(MAX_VNI_ID) } } }
        });
        static_cast<Orch *>(m_evpnRemoteVnip2pOrch)->doTask();
        ASSERT_TRUE(consumer->m_toSync.empty());

        deque<KeyOpFieldsValuesTuple> entries = {
            { "Vlan100:10.1.1.2", DEL_COMMAND, {} }
        };
        EXPECT_EQ(spuriousRemoteVniDels(m_evpnRemoteVnip2pOrch, entries, "10.1.1.2", "Port_EVPN_10.1.1.2"), 1u);
    }

    TEST_F(VxlanOrchRemoteVniTest, RemoteVniP2mpDelAsFirstRequest)
    {
        initSwitchOrch();
        initVxlanOrch();

        auto p2mp_orch = make_unique<EvpnRemoteVnip2mpOrch>(m_app_db.get(), APP_VXLAN_REMOTE_VNI_TABLE_NAME);
        deque<KeyOpFieldsValuesTuple> entries = {
            { "Vlan100:10.1.1.2", DEL_COMMAND, {} }
        };
        EXPECT_EQ(spuriousRemoteVniDels(p2mp_orch.get(), entries, "10.1.1.2", "Port_SRC_VTEP_10.1.1.1"), 1u);
    }

<<<<<<< HEAD
=======

    // A remote VTEP whose best route is a connected subnet gets its neighbor
    // resolved through NeighOrch while it has none.
    class RemoteVtepNeighborTest : public EvpnRemoteVniTest
    {
    protected:
        // RouteOrch reads the global, which MockOrchTest leaves unset.
        void PostSetUp() override
        {
            EvpnRemoteVniTest::PostSetUp();
            gTunneldecapOrch = m_TunnelDecapOrch;
        }

        void PreTearDown() override
        {
            gTunneldecapOrch = nullptr;
            EvpnRemoteVniTest::PreTearDown();
        }

        void addInterface(const string &alias, const string &prefix)
        {
            auto consumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_INTF_TABLE_NAME));
            ASSERT_NE(consumer, nullptr);
            consumer->addToSync({{alias, SET_COMMAND, {{"mtu", "9100"}}},
                                 {alias + ":" + prefix, SET_COMMAND, {{"scope", "global"}, {"family", "IPv4"}}}});
            static_cast<Orch *>(gIntfsOrch)->doTask(*consumer);
            ASSERT_TRUE(gIntfsOrch->getSyncdIntfses().count(alias));
            ASSERT_TRUE(gIntfsOrch->getSyncdIntfses().at(alias).ip_addresses.count(IpPrefix(prefix)));
        }

        void routeOp(const string &prefix, const string &op, const string &nexthop = "0.0.0.0",
                     const string &ifname = "Ethernet0")
        {
            auto consumer = dynamic_cast<Consumer *>(gRouteOrch->getExecutor(APP_ROUTE_TABLE_NAME));
            ASSERT_NE(consumer, nullptr);
            vector<FieldValueTuple> fvs;
            if (op == SET_COMMAND)
            {
                fvs = {{"nexthop", nexthop}, {"ifname", ifname}};
            }
            consumer->addToSync({{prefix, op, fvs}});
            static_cast<Orch *>(gRouteOrch)->doTask(*consumer);
        }

        void neighborOp(const string &alias, const string &ip, const string &op)
        {
            auto consumer = dynamic_cast<Consumer *>(gNeighOrch->getExecutor(APP_NEIGH_TABLE_NAME));
            ASSERT_NE(consumer, nullptr);
            vector<FieldValueTuple> fvs;
            if (op == SET_COMMAND)
            {
                fvs = {{"neigh", "00:00:0a:01:02:05"}, {"family", "IPv4"}};
            }
            consumer->addToSync({{alias + ":" + ip, op, fvs}});
            // doTask() also flushes the bulked neighbor operations.
            static_cast<Orch *>(gNeighOrch)->doTask();
            ASSERT_EQ(gNeighOrch->hasNextHop(NextHopKey(IpAddress(ip), alias)), op == SET_COMMAND);
        }

        void remoteVniOp(const string &vtep, const string &op)
        {
            vector<FieldValueTuple> fvs;
            if (op == SET_COMMAND)
            {
                fvs = {{"vni", "1000"}};
            }
            remoteVniConsumer()->addToSync({{"Vlan100:" + vtep, op, fvs}});
            static_cast<Orch *>(m_remoteVniOrch)->doTask();
            ASSERT_TRUE(remoteVniConsumer()->m_toSync.empty());
        }

        bool resolveRequested(const string &alias, const string &ip)
        {
            Table resolve(m_app_db.get(), APP_NEIGH_RESOLVE_TABLE_NAME);
            vector<FieldValueTuple> fvs;
            return resolve.get(alias + resolve.getTableNameSeparator() + ip, fvs);
        }

        bool observesRoute(const string &ip)
        {
            return gRouteOrch->m_nextHopObservers.count(make_pair(gVirtualRouterId, IpAddress(ip))) != 0;
        }

        // Ethernet0 10.1.2.1/24 with its connected route; the VTEP 10.1.2.5 is on it.
        void addSubnet()
        {
            addInterface("Ethernet0", "10.1.2.1/24");
            routeOp("10.1.2.0/24", SET_COMMAND);
        }

        void addRouteUser()
        {
            ASSERT_TRUE(m_VxlanTunnelOrch->addTunnelUser(m_vtep, 1000, 0, TUNNEL_USER_IP));
        }

        void delRouteUser()
        {
            ASSERT_TRUE(m_VxlanTunnelOrch->delTunnelUser(m_vtep, 1000, 0, TUNNEL_USER_IP));
        }

        // L2NhgOrch references a VTEP only once its P2P tunnel port exists, so only P2P tests use these two.
        void addL2NhgRef()
        {
            m_nvoOrch->getEVPNVtep()->updateRemoteEndPointIpRef(m_vtep, true);
        }

        // L2NhgOrch releases a reference and then cleans up the P2P tunnel that the release may have left unused.
        void delL2NhgRef()
        {
            auto vtep = m_nvoOrch->getEVPNVtep();
            vtep->updateRemoteEndPointIpRef(m_vtep, false);
            vtep->cleanupDynamicDIPTunnel(m_vtep);
        }

        // The tunnel to the VTEP, its resolve request, its route observer and its tracking entry are all gone.
        void expectVtepReleased()
        {
            EXPECT_FALSE(dipTunnelExists(m_vtep));
            Port tunnelPort;
            EXPECT_FALSE(m_VxlanTunnelOrch->getTunnelPort(m_vtep, tunnelPort));
            EXPECT_FALSE(resolveRequested("Ethernet0", m_vtep));
            EXPECT_FALSE(observesRoute(m_vtep));
            EXPECT_TRUE(m_VxlanTunnelOrch->m_remoteVtepNeighbors.empty());
        }

        // P2P: an IMET holds the VTEP while a RouteOrch IP user and an L2 NHG reference come and go in every order.
        // The request stays until the IMET goes, and then nothing of the VTEP's tracking is left.
        void imetWithRouteAndL2NhgUsers()
        {
            ASSERT_NO_FATAL_FAILURE(addSubnet());
            auto vtep = m_nvoOrch->getEVPNVtep();
            for (bool route_first : {true, false})
            {
                for (bool route_released_first : {true, false})
                {
                    SCOPED_TRACE(string("route user added ") + (route_first ? "first" : "second") + ", released " +
                                 (route_released_first ? "first" : "second"));
                    ASSERT_NO_FATAL_FAILURE(remoteVniOp(m_vtep, SET_COMMAND));
                    EXPECT_TRUE(dipTunnelExists(m_vtep));
                    EXPECT_TRUE(resolveRequested("Ethernet0", m_vtep));

                    if (route_first)
                    {
                        ASSERT_NO_FATAL_FAILURE(addRouteUser());
                        addL2NhgRef();
                    }
                    else
                    {
                        addL2NhgRef();
                        ASSERT_NO_FATAL_FAILURE(addRouteUser());
                    }
                    EXPECT_EQ(vtep->getRemoteEndPointIPRefCnt(m_vtep), 2);
                    if (route_released_first)
                    {
                        ASSERT_NO_FATAL_FAILURE(delRouteUser());
                    }
                    else
                    {
                        delL2NhgRef();
                    }
                    EXPECT_EQ(vtep->getRemoteEndPointIPRefCnt(m_vtep), 1);
                    EXPECT_TRUE(resolveRequested("Ethernet0", m_vtep));
                    if (route_released_first)
                    {
                        delL2NhgRef();
                    }
                    else
                    {
                        ASSERT_NO_FATAL_FAILURE(delRouteUser());
                    }
                    // The IMET keeps the entry, and the tunnel, with no IP reference left.
                    EXPECT_EQ(vtep->getRemoteEndPointIPRefCnt(m_vtep), 0);
                    EXPECT_TRUE(dipTunnelExists(m_vtep));
                    EXPECT_TRUE(resolveRequested("Ethernet0", m_vtep));
                    EXPECT_TRUE(observesRoute(m_vtep));

                    ASSERT_NO_FATAL_FAILURE(remoteVniOp(m_vtep, DEL_COMMAND));
                    EXPECT_EQ(vtep->getRemoteEndPointIPRefCnt(m_vtep), -1);
                    expectVtepReleased();
                    if (HasFailure())
                    {
                        return;
                    }
                }
            }
        }

        const string m_vtep = "10.1.2.5";
    };

    TEST_F(RemoteVtepNeighborTest, RouteAndL2NhgUsersOnImetVtep)
    {
        imetWithRouteAndL2NhgUsers();
    }

    // No IMET: a RouteOrch IP user and an L2 NHG reference hold the VTEP, and the last of them to go releases its P2P
    // tunnel, through delTunnelUser() for the route user and cleanupDynamicDIPTunnel() for the L2 NHG.
    TEST_F(RemoteVtepNeighborTest, LastRouteOrL2NhgUserReleasesTunnel)
    {
        ASSERT_NO_FATAL_FAILURE(addSubnet());
        auto vtep = m_nvoOrch->getEVPNVtep();
        for (bool route_released_first : {true, false})
        {
            SCOPED_TRACE(string("route user released ") + (route_released_first ? "first" : "second"));
            ASSERT_NO_FATAL_FAILURE(addRouteUser());
            addL2NhgRef();
            EXPECT_EQ(vtep->getRemoteEndPointIPRefCnt(m_vtep), 2);
            EXPECT_EQ(vtep->getRemoteEndPointRefCnt(m_vtep), 2);
            EXPECT_TRUE(dipTunnelExists(m_vtep));
            EXPECT_TRUE(resolveRequested("Ethernet0", m_vtep));
            EXPECT_TRUE(observesRoute(m_vtep));

            if (route_released_first)
            {
                ASSERT_NO_FATAL_FAILURE(delRouteUser());
            }
            else
            {
                delL2NhgRef();
            }
            EXPECT_EQ(vtep->getRemoteEndPointIPRefCnt(m_vtep), 1);
            EXPECT_TRUE(dipTunnelExists(m_vtep));
            Port tunnelPort;
            EXPECT_TRUE(m_VxlanTunnelOrch->getTunnelPort(m_vtep, tunnelPort));
            EXPECT_TRUE(resolveRequested("Ethernet0", m_vtep));

            if (route_released_first)
            {
                delL2NhgRef();
            }
            else
            {
                ASSERT_NO_FATAL_FAILURE(delRouteUser());
            }
            EXPECT_EQ(vtep->getRemoteEndPointIPRefCnt(m_vtep), -1);
            expectVtepReleased();
            if (HasFailure())
            {
                return;
            }
        }
    }

    // Releasing an IP reference the VTEP does not hold changes nothing: the IMET keeps the VTEP and its request.
    TEST_F(RemoteVtepNeighborTest, IpReleaseWithoutIpReferenceIsIgnored)
    {
        addSubnet();
        auto vtep = m_nvoOrch->getEVPNVtep();
        vtep->updateRemoteEndPointIpRef(m_vtep, false);
        EXPECT_EQ(vtep->getRemoteEndPointIPRefCnt(m_vtep), -1);
        EXPECT_TRUE(m_VxlanTunnelOrch->m_remoteVtepNeighbors.empty());

        remoteVniOp(m_vtep, SET_COMMAND);
        vtep->updateRemoteEndPointIpRef(m_vtep, false);
        EXPECT_EQ(vtep->getRemoteEndPointIPRefCnt(m_vtep), 0);
        EXPECT_EQ(vtep->getRemoteEndPointIMRRefCnt(m_vtep), 1);
        EXPECT_TRUE(dipTunnelExists(m_vtep));
        EXPECT_EQ(m_VxlanTunnelOrch->m_remoteVtepNeighbors.size(), 1u);
        EXPECT_TRUE(resolveRequested("Ethernet0", m_vtep));

        remoteVniOp(m_vtep, DEL_COMMAND);
        expectVtepReleased();
    }

    // A source VTEP deleted while it still tracks a remote VTEP releases its own use, and the EVPN VTEP's IMET keeps
    // the remote VTEP tracked.
    TEST_F(RemoteVtepNeighborTest, DeletedVtepReleasesTrackedVteps)
    {
        addSubnet();
        Table tunnel(m_app_db.get(), APP_VXLAN_TUNNEL_TABLE_NAME);
        tunnel.set("vtep2", {{"src_ip", "10.1.1.9"}});
        m_VxlanTunnelOrch->addExistingData(&tunnel);
        static_cast<Orch *>(m_VxlanTunnelOrch)->doTask();
        ASSERT_TRUE(m_VxlanTunnelOrch->isTunnelExists("vtep2"));

        remoteVniOp(m_vtep, SET_COMMAND);
        m_VxlanTunnelOrch->getVxlanTunnel("vtep2")->updateRemoteEndPointIpRef(m_vtep, true);
        ASSERT_EQ(m_VxlanTunnelOrch->m_remoteVtepNeighbors.size(), 1u);
        EXPECT_EQ(m_VxlanTunnelOrch->m_remoteVtepNeighbors.begin()->second.users, 2u);
        EXPECT_TRUE(resolveRequested("Ethernet0", m_vtep));
        EXPECT_TRUE(observesRoute(m_vtep));

        auto consumer = dynamic_cast<Consumer *>(m_VxlanTunnelOrch->getExecutor(APP_VXLAN_TUNNEL_TABLE_NAME));
        ASSERT_NE(consumer, nullptr);
        consumer->addToSync({{"vtep2", DEL_COMMAND, {}}});
        static_cast<Orch *>(m_VxlanTunnelOrch)->doTask(*consumer);
        ASSERT_FALSE(m_VxlanTunnelOrch->isTunnelExists("vtep2"));
        ASSERT_EQ(m_VxlanTunnelOrch->m_remoteVtepNeighbors.size(), 1u);
        EXPECT_EQ(m_VxlanTunnelOrch->m_remoteVtepNeighbors.begin()->second.users, 1u);
        EXPECT_TRUE(resolveRequested("Ethernet0", m_vtep));
        EXPECT_TRUE(observesRoute(m_vtep));

        remoteVniOp(m_vtep, DEL_COMMAND);
        expectVtepReleased();
    }

    // Added, resolved, lost and re-resolved, then removed with its request outstanding.
    TEST_F(RemoteVtepNeighborTest, ResolvesVtepOnConnectedSubnet)
    {
        addSubnet();
        remoteVniOp(m_vtep, SET_COMMAND);
        EXPECT_TRUE(observesRoute(m_vtep));
        EXPECT_TRUE(resolveRequested("Ethernet0", m_vtep));

        neighborOp("Ethernet0", m_vtep, SET_COMMAND);
        EXPECT_TRUE(gNeighOrch->hasNextHop(NextHopKey(IpAddress(m_vtep), "Ethernet0")));
        EXPECT_FALSE(resolveRequested("Ethernet0", m_vtep));

        neighborOp("Ethernet0", m_vtep, DEL_COMMAND);
        EXPECT_FALSE(gNeighOrch->hasNextHop(NextHopKey(IpAddress(m_vtep), "Ethernet0")));
        EXPECT_TRUE(resolveRequested("Ethernet0", m_vtep));

        remoteVniOp(m_vtep, DEL_COMMAND);
        EXPECT_FALSE(dipTunnelExists(m_vtep));
        EXPECT_FALSE(resolveRequested("Ethernet0", m_vtep));
        EXPECT_FALSE(observesRoute(m_vtep));
        EXPECT_TRUE(m_VxlanTunnelOrch->m_remoteVtepNeighbors.empty());
    }

    // A neighbor that already exists needs no request; losing it brings one.
    TEST_F(RemoteVtepNeighborTest, ExistingNeighborNeedsNoRequest)
    {
        addSubnet();
        neighborOp("Ethernet0", m_vtep, SET_COMMAND);
        remoteVniOp(m_vtep, SET_COMMAND);
        EXPECT_TRUE(observesRoute(m_vtep));
        EXPECT_FALSE(resolveRequested("Ethernet0", m_vtep));

        neighborOp("Ethernet0", m_vtep, DEL_COMMAND);
        EXPECT_TRUE(resolveRequested("Ethernet0", m_vtep));
        remoteVniOp(m_vtep, DEL_COMMAND);
        EXPECT_FALSE(resolveRequested("Ethernet0", m_vtep));
    }

    // A loopback VTEP outside every connected subnet neither observes its route nor asks for a neighbor.
    TEST_F(RemoteVtepNeighborTest, VtepOutsideConnectedSubnetsIsLeftAlone)
    {
        addSubnet();
        remoteVniOp("10.9.9.9", SET_COMMAND);
        EXPECT_FALSE(observesRoute("10.9.9.9"));
        EXPECT_FALSE(resolveRequested("Ethernet0", "10.9.9.9"));
        remoteVniOp("10.9.9.9", DEL_COMMAND);
        EXPECT_TRUE(m_VxlanTunnelOrch->m_remoteVtepNeighbors.empty());
    }

    // A more specific route through another router withdraws the request, and its removal
    // brings the request back; a neighbor lost meanwhile is not asked for.
    TEST_F(RemoteVtepNeighborTest, MoreSpecificRouteWithdrawsRequest)
    {
        addSubnet();
        addInterface("Ethernet4", "10.1.3.1/24");
        // RouteOrch skips a route whose next hop is on a port that is down.
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet4", port));
        port.m_oper_status = SAI_PORT_OPER_STATUS_UP;
        gPortsOrch->setPort("Ethernet4", port);
        neighborOp("Ethernet4", "10.1.3.2", SET_COMMAND);

        remoteVniOp(m_vtep, SET_COMMAND);
        EXPECT_TRUE(resolveRequested("Ethernet0", m_vtep));

        routeOp(m_vtep + "/32", SET_COMMAND, "10.1.3.2", "Ethernet4");
        EXPECT_TRUE(gRouteOrch->m_syncdRoutes.at(gVirtualRouterId).count(IpPrefix(m_vtep + "/32")));
        EXPECT_FALSE(resolveRequested("Ethernet0", m_vtep));

        neighborOp("Ethernet0", m_vtep, SET_COMMAND);
        neighborOp("Ethernet0", m_vtep, DEL_COMMAND);
        EXPECT_FALSE(resolveRequested("Ethernet0", m_vtep));

        routeOp(m_vtep + "/32", DEL_COMMAND);
        EXPECT_TRUE(resolveRequested("Ethernet0", m_vtep));

        remoteVniOp(m_vtep, DEL_COMMAND);
        EXPECT_FALSE(resolveRequested("Ethernet0", m_vtep));
    }

    // A subnet configured after the VTEP, and one removed and added back, are both picked up.
    TEST_F(RemoteVtepNeighborTest, SubnetAddedAfterVtep)
    {
        remoteVniOp(m_vtep, SET_COMMAND);
        EXPECT_FALSE(observesRoute(m_vtep));
        EXPECT_FALSE(resolveRequested("Ethernet0", m_vtep));

        addInterface("Ethernet0", "10.1.2.1/24");
        EXPECT_TRUE(observesRoute(m_vtep));
        EXPECT_FALSE(resolveRequested("Ethernet0", m_vtep));
        routeOp("10.1.2.0/24", SET_COMMAND);
        EXPECT_TRUE(resolveRequested("Ethernet0", m_vtep));

        routeOp("10.1.2.0/24", DEL_COMMAND);
        EXPECT_FALSE(resolveRequested("Ethernet0", m_vtep));
        routeOp("10.1.2.0/24", SET_COMMAND);
        EXPECT_TRUE(resolveRequested("Ethernet0", m_vtep));

        remoteVniOp(m_vtep, DEL_COMMAND);
        EXPECT_FALSE(resolveRequested("Ethernet0", m_vtep));
    }

    // P2MP: the remote endpoint has no tunnel of its own, and is tracked all the same.
    class RemoteVtepNeighborP2mpTest : public RemoteVtepNeighborTest
    {
    protected:
        bool useDipTunnels() const override
        {
            return false;
        }
    };

    // P2MP has route users only (no L2 NHG references, no IMET count in tnl_users_): the IMET keeps the VTEP tracked
    // after the route user goes, whose release erases the entry.
    TEST_F(RemoteVtepNeighborP2mpTest, RouteUserOnImetVtep)
    {
        ASSERT_NO_FATAL_FAILURE(addSubnet());
        auto vtep = m_nvoOrch->getEVPNVtep();
        ASSERT_NO_FATAL_FAILURE(remoteVniOp(m_vtep, SET_COMMAND));
        ASSERT_NO_FATAL_FAILURE(addRouteUser());
        EXPECT_EQ(vtep->getRemoteEndPointIPRefCnt(m_vtep), 1);
        ASSERT_EQ(m_VxlanTunnelOrch->m_remoteVtepNeighbors.size(), 1u);
        EXPECT_EQ(m_VxlanTunnelOrch->m_remoteVtepNeighbors.begin()->second.users, 2u);

        ASSERT_NO_FATAL_FAILURE(delRouteUser());
        EXPECT_EQ(vtep->getRemoteEndPointIPRefCnt(m_vtep), -1);
        EXPECT_TRUE(resolveRequested("Ethernet0", m_vtep));
        EXPECT_TRUE(observesRoute(m_vtep));

        ASSERT_NO_FATAL_FAILURE(remoteVniOp(m_vtep, DEL_COMMAND));
        expectVtepReleased();
    }

    // A route user alone holds the VTEP; its release ends the tracking.
    TEST_F(RemoteVtepNeighborP2mpTest, LastRouteUserReleasesTracking)
    {
        ASSERT_NO_FATAL_FAILURE(addSubnet());
        auto vtep = m_nvoOrch->getEVPNVtep();
        ASSERT_NO_FATAL_FAILURE(addRouteUser());
        EXPECT_EQ(vtep->getRemoteEndPointIPRefCnt(m_vtep), 1);
        EXPECT_TRUE(resolveRequested("Ethernet0", m_vtep));
        EXPECT_TRUE(observesRoute(m_vtep));

        ASSERT_NO_FATAL_FAILURE(delRouteUser());
        EXPECT_EQ(vtep->getRemoteEndPointIPRefCnt(m_vtep), -1);
        expectVtepReleased();
    }

    TEST_F(RemoteVtepNeighborP2mpTest, ResolvesVtepOnConnectedSubnet)
    {
        addSubnet();
        remoteVniOp(m_vtep, SET_COMMAND);
        EXPECT_TRUE(resolveRequested("Ethernet0", m_vtep));

        neighborOp("Ethernet0", m_vtep, SET_COMMAND);
        EXPECT_FALSE(resolveRequested("Ethernet0", m_vtep));
        neighborOp("Ethernet0", m_vtep, DEL_COMMAND);
        EXPECT_TRUE(resolveRequested("Ethernet0", m_vtep));

        remoteVniOp(m_vtep, DEL_COMMAND);
        EXPECT_FALSE(resolveRequested("Ethernet0", m_vtep));
        EXPECT_TRUE(m_VxlanTunnelOrch->m_remoteVtepNeighbors.empty());
    }

    // A cancellable request can be withdrawn and made again; one that any caller made without cancellable cannot.
    TEST_F(RemoteVtepNeighborTest, CancelOnlyWithdrawsCancellableRequests)
    {
        NeighborEntry entry(IpAddress(m_vtep), "Ethernet0");
        gNeighOrch->resolveNeighbor(entry, true);
        EXPECT_TRUE(resolveRequested("Ethernet0", m_vtep));
        gNeighOrch->cancelResolveNeighbor(entry);
        EXPECT_FALSE(resolveRequested("Ethernet0", m_vtep));
        gNeighOrch->cancelResolveNeighbor(entry);
        gNeighOrch->resolveNeighbor(entry, true);
        EXPECT_TRUE(resolveRequested("Ethernet0", m_vtep));

        gNeighOrch->resolveNeighbor(entry);
        gNeighOrch->cancelResolveNeighbor(entry);
        EXPECT_TRUE(resolveRequested("Ethernet0", m_vtep));

        NeighborEntry other(IpAddress("10.1.2.6"), "Ethernet0");
        gNeighOrch->resolveNeighbor(other);
        gNeighOrch->resolveNeighbor(other, true);
        gNeighOrch->cancelResolveNeighbor(other);
        EXPECT_TRUE(resolveRequested("Ethernet0", "10.1.2.6"));
    }

    // Two cancellable requesters: the request stays until both have cancelled, and extra cancels do nothing.
    TEST_F(RemoteVtepNeighborTest, CancellableRequestersAreCounted)
    {
        NeighborEntry entry(IpAddress(m_vtep), "Ethernet0");
        gNeighOrch->resolveNeighbor(entry, true);
        gNeighOrch->resolveNeighbor(entry, true);
        EXPECT_EQ(gNeighOrch->m_cancellableResolves.at(entry), 2u);

        gNeighOrch->cancelResolveNeighbor(entry);
        EXPECT_TRUE(resolveRequested("Ethernet0", m_vtep));
        EXPECT_EQ(gNeighOrch->m_neighborToResolve.count(entry), 1u);

        gNeighOrch->cancelResolveNeighbor(entry);
        EXPECT_FALSE(resolveRequested("Ethernet0", m_vtep));
        EXPECT_EQ(gNeighOrch->m_neighborToResolve.count(entry), 0u);
        EXPECT_EQ(gNeighOrch->m_cancellableResolves.count(entry), 0u);

        gNeighOrch->cancelResolveNeighbor(entry);
        gNeighOrch->resolveNeighbor(entry, true);
        EXPECT_EQ(gNeighOrch->m_cancellableResolves.at(entry), 1u);
        gNeighOrch->cancelResolveNeighbor(entry);
        EXPECT_FALSE(resolveRequested("Ethernet0", m_vtep));
    }

    // Adding the neighbor ends every request: a cancel afterwards neither underflows nor withdraws
    // a request made after the neighbor is lost.
    TEST_F(RemoteVtepNeighborTest, ResolvedNeighborEndsCancellableRequests)
    {
        addSubnet();
        NeighborEntry entry(IpAddress(m_vtep), "Ethernet0");
        gNeighOrch->resolveNeighbor(entry, true);
        gNeighOrch->resolveNeighbor(entry, true);

        neighborOp("Ethernet0", m_vtep, SET_COMMAND);
        EXPECT_FALSE(resolveRequested("Ethernet0", m_vtep));
        EXPECT_EQ(gNeighOrch->m_neighborToResolve.count(entry), 0u);
        EXPECT_EQ(gNeighOrch->m_cancellableResolves.count(entry), 0u);
        gNeighOrch->cancelResolveNeighbor(entry);
        EXPECT_EQ(gNeighOrch->m_cancellableResolves.count(entry), 0u);

        neighborOp("Ethernet0", m_vtep, DEL_COMMAND);
        gNeighOrch->resolveNeighbor(entry, true);
        EXPECT_EQ(gNeighOrch->m_cancellableResolves.at(entry), 1u);
        gNeighOrch->cancelResolveNeighbor(entry);
        EXPECT_FALSE(resolveRequested("Ethernet0", m_vtep));
    }

    // The VTEP's request is one of two: removing the VTEP leaves the other caller's, also when
    // the neighbor was resolved and lost before the other caller asked.
    TEST_F(RemoteVtepNeighborTest, VtepCancelLeavesOtherCancellableRequest)
    {
        addSubnet();
        NeighborEntry entry(IpAddress(m_vtep), "Ethernet0");
        remoteVniOp(m_vtep, SET_COMMAND);
        gNeighOrch->resolveNeighbor(entry, true);
        EXPECT_EQ(gNeighOrch->m_cancellableResolves.at(entry), 2u);
        remoteVniOp(m_vtep, DEL_COMMAND);
        EXPECT_EQ(gNeighOrch->m_cancellableResolves.at(entry), 1u);
        EXPECT_TRUE(resolveRequested("Ethernet0", m_vtep));
        gNeighOrch->cancelResolveNeighbor(entry);
        EXPECT_FALSE(resolveRequested("Ethernet0", m_vtep));

        remoteVniOp(m_vtep, SET_COMMAND);
        neighborOp("Ethernet0", m_vtep, SET_COMMAND);
        EXPECT_TRUE(m_VxlanTunnelOrch->m_remoteVtepNeighbors.at(IpAddress(m_vtep)).requested_alias.empty());
        neighborOp("Ethernet0", m_vtep, DEL_COMMAND);
        gNeighOrch->resolveNeighbor(entry, true);
        EXPECT_EQ(gNeighOrch->m_cancellableResolves.at(entry), 2u);
        remoteVniOp(m_vtep, DEL_COMMAND);
        EXPECT_TRUE(resolveRequested("Ethernet0", m_vtep));
        gNeighOrch->cancelResolveNeighbor(entry);
        EXPECT_FALSE(resolveRequested("Ethernet0", m_vtep));
    }

    // Withdrawing the last request leaves a neighbor refresh on the same key in place.
    TEST_F(RemoteVtepNeighborTest, CancelKeepsPendingRefresh)
    {
        NeighborEntry entry(IpAddress(m_vtep), "Ethernet0");
        gNeighOrch->resolveNeighbor(entry, true);
        gNeighOrch->m_neighborToRefresh.insert(entry);
        gNeighOrch->cancelResolveNeighbor(entry);
        EXPECT_EQ(gNeighOrch->m_neighborToResolve.count(entry), 0u);
        EXPECT_TRUE(resolveRequested("Ethernet0", m_vtep));

        gNeighOrch->clearNeighborRefresh(entry);
        EXPECT_FALSE(resolveRequested("Ethernet0", m_vtep));
    }

    // Another orch's request for the VTEP's neighbor outlives the VTEP.
    TEST_F(RemoteVtepNeighborTest, OtherRequesterKeepsItsRequest)
    {
        addSubnet();
        gNeighOrch->resolveNeighbor(NeighborEntry(IpAddress(m_vtep), "Ethernet0"));
        remoteVniOp(m_vtep, SET_COMMAND);
        EXPECT_TRUE(resolveRequested("Ethernet0", m_vtep));
        remoteVniOp(m_vtep, DEL_COMMAND);
        EXPECT_TRUE(m_VxlanTunnelOrch->m_remoteVtepNeighbors.empty());
        EXPECT_TRUE(resolveRequested("Ethernet0", m_vtep));
    }

    // A request without the cancellable flag after a cancellable one outlives the cancel.
    TEST_F(RemoteVtepNeighborTest, LaterPlainRequestOutlivesCancel)
    {
        NeighborEntry entry(IpAddress(m_vtep), "Ethernet0");
        gNeighOrch->resolveNeighbor(entry, true);
        gNeighOrch->resolveNeighbor(entry);
        EXPECT_EQ(gNeighOrch->m_cancellableResolves.count(entry), 0u);
        gNeighOrch->cancelResolveNeighbor(entry);
        EXPECT_TRUE(resolveRequested("Ethernet0", m_vtep));
        EXPECT_EQ(gNeighOrch->m_neighborToResolve.count(entry), 1u);
    }



    sai_remove_bridge_port_fn g_savedRemoveBridgePort = nullptr;
    decltype(sai_tunnel_api->create_tunnel_map_entry) g_savedCreateTunnelMapEntry = nullptr;
    sai_status_t g_tunnelMapEntryCreateStatus = SAI_STATUS_SUCCESS;
    // The output of a failed create is no id the SAI created: a non-null value catches its use.
    const sai_object_id_t g_unknownTunnelMapEntryId = 0x3b00000000dead;
    sai_status_t stubCreateTunnelMapEntryFails(sai_object_id_t *id, sai_object_id_t, uint32_t,
                                               const sai_attribute_t *)
    {
        *id = g_unknownTunnelMapEntryId;
        return g_tunnelMapEntryCreateStatus;
    }
    decltype(sai_vlan_api->remove_vlan) g_savedRemoveVlan = nullptr;
    decltype(sai_hostif_api->remove_hostif) g_savedRemoveHostif = nullptr;
    decltype(sai_hostif_api->create_hostif) g_savedCreateHostif = nullptr;
    const sai_object_id_t g_vlanHostifId = 0xd00000000beef;
    int g_vlanRemoveFailures = 0;
    int g_vlanHostifRemoves = 0;
    int g_vlanHostifCreates = 0;
    sai_status_t stubCreateVlanHostif(sai_object_id_t *id, sai_object_id_t, uint32_t, const sai_attribute_t *)
    {
        g_vlanHostifCreates++;
        *id = g_vlanHostifId;
        return SAI_STATUS_SUCCESS;
    }
    sai_status_t stubRemoveVlanFailsFirst(sai_object_id_t vlan_oid)
    {
        if (g_vlanRemoveFailures-- > 0)
        {
            return SAI_STATUS_OBJECT_IN_USE;
        }
        return g_savedRemoveVlan(vlan_oid);
    }
    sai_status_t stubRemoveVlanHostif(sai_object_id_t id)
    {
        if (id != g_vlanHostifId)
        {
            return g_savedRemoveHostif(id);
        }
        return g_vlanHostifRemoves++ ? SAI_STATUS_INVALID_OBJECT_ID : SAI_STATUS_SUCCESS;
    }
    int g_removeBridgePortInUseCalls = 0;
    sai_status_t stubRemoveBridgePortInUse(sai_object_id_t)
    {
        g_removeBridgePortInUseCalls++;
        return SAI_STATUS_OBJECT_IN_USE;
    }
    decltype(sai_tunnel_api->remove_tunnel_map_entry) g_savedRemoveTunnelMapEntry = nullptr;
    sai_object_id_t g_refusedTunnelMapEntryId = SAI_NULL_OBJECT_ID;
    int g_refusedTunnelMapEntryRemoves = 0;
    sai_status_t stubRemoveTunnelMapEntryRefused(sai_object_id_t id)
    {
        if (id != g_refusedTunnelMapEntryId)
        {
            return g_savedRemoveTunnelMapEntry(id);
        }
        g_refusedTunnelMapEntryRemoves++;
        return SAI_STATUS_OBJECT_IN_USE;
    }

    // The EVPN NVO must outlive the remote VTEP users of its source VTEP: their DELs release
    // the VTEP through EvpnNvoOrch::getEVPNVtep(). Each step is the APPL_DB change vxlanmgrd
    // or fdbsyncd makes, then the DEL as ConsumerStateTable delivers it.
    class EvpnNvoDelTest : public EvpnRemoteVniTest
    {
    protected:
        const string m_mapKey = "vtep1:map_1000_Vlan100";
        StpOrch *m_stpOrch = nullptr;

        bool m_countVrSrcMacSets = false;

        const string m_newMapKey = "vtep1:map_2000_Vlan200";
        const string m_vrfMapKey = "vtep1:evpn_map_2000_Vrf1";
        VxlanVrfMapOrch *m_vrfMapOrch = nullptr;

        void PreTearDown() override
        {
            if (m_stpOrch)
            {
                delete m_stpOrch;
                m_stpOrch = nullptr;
                gStpOrch = nullptr;
            }
            if (g_savedCreateHostif)
            {
                sai_hostif_api->create_hostif = g_savedCreateHostif;
                g_savedCreateHostif = nullptr;
            }
            if (g_savedRemoveBridgePort)
            {
                sai_bridge_api->remove_bridge_port = g_savedRemoveBridgePort;
                g_savedRemoveBridgePort = nullptr;
            }
            if (m_countVrSrcMacSets)
            {
                sai_virtual_router_api->set_virtual_router_attribute = g_savedSetVirtualRouterAttr;
            }
            if (g_savedCreateTunnelMapEntry)
            {
                sai_tunnel_api->create_tunnel_map_entry = g_savedCreateTunnelMapEntry;
                g_savedCreateTunnelMapEntry = nullptr;
            }
            if (g_savedRemoveTunnelMapEntry)
            {
                sai_tunnel_api->remove_tunnel_map_entry = g_savedRemoveTunnelMapEntry;
                g_savedRemoveTunnelMapEntry = nullptr;
                g_refusedTunnelMapEntryId = SAI_NULL_OBJECT_ID;
            }
            if (g_savedRemoveVlan)
            {
                sai_vlan_api->remove_vlan = g_savedRemoveVlan;
                g_savedRemoveVlan = nullptr;
            }
            if (g_savedRemoveHostif)
            {
                sai_hostif_api->remove_hostif = g_savedRemoveHostif;
                g_savedRemoveHostif = nullptr;
            }
            Table(m_app_db.get(), APP_STP_VLAN_INSTANCE_TABLE_NAME).del("Vlan200");
            Table(m_app_db.get(), APP_STP_PORT_STATE_TABLE_NAME).del("Ethernet0:1");
            Table(m_app_db.get(), APP_INTF_TABLE_NAME).del("Vlan200");
            Table(m_app_db.get(), APP_VLAN_MEMBER_TABLE_NAME).del("Vlan200:Ethernet0");
            Table(m_app_db.get(), APP_VLAN_TABLE_NAME).del("Vlan200");
            Table(m_app_db.get(), APP_VXLAN_TUNNEL_MAP_TABLE_NAME).del("vtep1:map_2000_Vlan200");
            Table(m_app_db.get(), APP_VXLAN_EVPN_NVO_TABLE_NAME).del("nvo1");
            Table(m_app_db.get(), APP_VXLAN_TUNNEL_MAP_TABLE_NAME).del(m_mapKey);
            Table(m_app_db.get(), APP_VXLAN_TUNNEL_MAP_TABLE_NAME).del(m_newMapKey);
            Table(m_app_db.get(), APP_VXLAN_VRF_TABLE_NAME).del(m_vrfMapKey);
            Table(m_app_db.get(), APP_VXLAN_TUNNEL_TABLE_NAME).del("vtep1");
            delete m_vrfMapOrch;
            m_vrfMapOrch = nullptr;
            EvpnRemoteVniTest::PreTearDown();
        }

        static Consumer *consumerOf(Orch *orch, const string &table)
        {
            return dynamic_cast<Consumer *>(orch->getExecutor(table));
        }

        void apply(Orch *orch, const string &table, const KeyOpFieldsValuesTuple &kfv)
        {
            Table t(m_app_db.get(), table);
            if (kfvOp(kfv) == DEL_COMMAND)
            {
                t.del(kfvKey(kfv));
            }
            else
            {
                t.set(kfvKey(kfv), kfvFieldsValues(kfv));
            }
            consumerOf(orch, table)->addToSync(deque<KeyOpFieldsValuesTuple>{kfv});
            static_cast<Orch *>(orch)->doTask();
        }

        void nvoDel() { apply(m_nvoOrch, APP_VXLAN_EVPN_NVO_TABLE_NAME, {"nvo1", DEL_COMMAND, {}}); }
        void nvoSet() { apply(m_nvoOrch, APP_VXLAN_EVPN_NVO_TABLE_NAME, {"nvo1", SET_COMMAND, {{"source_vtep", "vtep1"}}}); }
        void mapDel() { apply(m_tunnelMapOrch, APP_VXLAN_TUNNEL_MAP_TABLE_NAME, {m_mapKey, DEL_COMMAND, {}}); }
        void mapSet()
        {
            apply(m_tunnelMapOrch, APP_VXLAN_TUNNEL_MAP_TABLE_NAME,
                  {m_mapKey, SET_COMMAND, {{"vni", "1000"}, {"vlan", "Vlan100"}}});
        }
        void tunnelDel() { apply(m_VxlanTunnelOrch, APP_VXLAN_TUNNEL_TABLE_NAME, {"vtep1", DEL_COMMAND, {}}); }
        void retryNvo() { static_cast<Orch *>(m_nvoOrch)->doTask(); }
        // The bridge port removal retry interval has passed.
        void expireBridgePortRetry() { m_nvoOrch->m_bridgePortRetryAt = {}; }

        bool nvoHeld() { return !consumerOf(m_nvoOrch, APP_VXLAN_EVPN_NVO_TABLE_NAME)->m_toSync.empty(); }
        bool mapHeld() { return !consumerOf(m_tunnelMapOrch, APP_VXLAN_TUNNEL_MAP_TABLE_NAME)->m_toSync.empty(); }
        VxlanTunnel *vtep() { return m_VxlanTunnelOrch->getVxlanTunnel("vtep1"); }

        bool remoteVtepIsFloodMember()
        {
            Port vlanPort, tunnelPort;
            if (!gPortsOrch->getVlanByVlanId(100, vlanPort))
            {
                return false;
            }
            if (useDipTunnels())
            {
                return m_VxlanTunnelOrch->getTunnelPort(m_remoteVtep, tunnelPort) &&
                       gPortsOrch->isVlanMember(vlanPort, tunnelPort);
            }
            return m_VxlanTunnelOrch->getTunnelPort("10.1.1.1", tunnelPort, true) &&
                   gPortsOrch->isVlanMember(vlanPort, tunnelPort, m_remoteVtep);
        }

        // The NVO and its map are configured again and a remote VTEP joins the VLAN.
        void expectRecreateWorks()
        {
            nvoSet();
            EXPECT_EQ(m_nvoOrch->getEVPNVtep(), vtep());
            mapSet();
            EXPECT_FALSE(mapHeld());
            EXPECT_TRUE(vtep()->isActive());
            setRemoteVni();
            EXPECT_TRUE(remoteVniConsumer()->m_toSync.empty());
            EXPECT_TRUE(remoteVtepIsFloodMember());
        }

        // The released VTEP: hw delete done, NVO gone, nothing held or logged as held.
        void expectReleased()
        {
            EXPECT_FALSE(vtep()->del_tnl_hw_pending);
            EXPECT_FALSE(vtep()->isActive());
            EXPECT_EQ(m_nvoOrch->getEVPNVtep(), nullptr);
            EXPECT_FALSE(nvoHeld());
            EXPECT_TRUE(m_nvoOrch->m_heldDeletes.empty());
        }

        // The NVO DEL reaches orchagent before the last map DEL, then the remote VNI DEL comes
        // from fdbsyncd. vxlanmgrd writes the map DEL first, but orchagent serves the readiest
        // least recently used consumer first, so the NVO DEL can win.
        void nvoBeforeLastMap()
        {
            setRemoteVni();
            ASSERT_TRUE(remoteVtepIsFloodMember());

            nvoDel();
            EXPECT_TRUE(nvoHeld());
            EXPECT_EQ(m_nvoOrch->getEVPNVtep(), vtep());
            retryNvo();
            retryNvo();
            EXPECT_EQ(m_nvoOrch->m_heldDeletes.size(), 1u);

            mapDel();
            EXPECT_TRUE(vtep()->del_tnl_hw_pending);
            EXPECT_TRUE(nvoHeld());

            delRemoteVni();
            EXPECT_TRUE(remoteVniConsumer()->m_toSync.empty());
            EXPECT_FALSE(remoteVtepIsFloodMember());
            EXPECT_FALSE(vtep()->del_tnl_hw_pending);

            retryNvo();
            expectReleased();
            expectRecreateWorks();
        }

        // The usual CLI order: the map DEL first marks the hw delete pending, the NVO DEL waits,
        // the remote VNI DEL finishes the delete.
        void lastMapBeforeNvo()
        {
            setRemoteVni();
            mapDel();
            EXPECT_TRUE(vtep()->del_tnl_hw_pending);

            nvoDel();
            EXPECT_TRUE(nvoHeld());
            EXPECT_EQ(m_nvoOrch->getEVPNVtep(), vtep());

            delRemoteVni();
            EXPECT_FALSE(vtep()->del_tnl_hw_pending);
            retryNvo();
            expectReleased();
            expectRecreateWorks();
        }

        // A VTEP with no remote user is deleted at once, whichever of map and NVO comes first.
        void idleVtep(bool nvo_first)
        {
            if (nvo_first)
            {
                nvoDel();
                EXPECT_FALSE(nvoHeld());
                EXPECT_EQ(m_nvoOrch->getEVPNVtep(), nullptr);
                mapDel();
            }
            else
            {
                mapDel();
                nvoDel();
            }
            expectReleased();
        }

        // VXLAN_TUNNEL DEL waits while the NVO's source VTEP has users, which release it through the NVO. Once
        // they are gone the NVO lets go of the tunnel, so its pointer never dangles, and waits for the tunnel to be
        // configured again until the NVO itself is deleted.
        void tunnelWaitsForNvo()
        {
            setRemoteVni();
            mapDel();
            tunnelDel();
            retryTunnel();
            EXPECT_TRUE(tunnelHeld());
            EXPECT_EQ(m_VxlanTunnelOrch->m_heldDeletes.size(), 1u);
            EXPECT_EQ(m_nvoOrch->getEVPNVtep(), vtep());

            delRemoteVni();
            EXPECT_FALSE(vtep()->del_tnl_hw_pending);
            retryTunnel();
            EXPECT_FALSE(tunnelHeld());
            EXPECT_FALSE(m_VxlanTunnelOrch->isTunnelExists("vtep1"));
            EXPECT_TRUE(m_VxlanTunnelOrch->m_heldDeletes.empty());
            EXPECT_EQ(m_nvoOrch->getEVPNVtep(), nullptr);
            EXPECT_TRUE(m_nvoOrch->isBindPending());

            nvoDel();
            EXPECT_FALSE(nvoHeld());
            EXPECT_FALSE(m_nvoOrch->isBindPending());
        }

        // A held DEL must not clear the pointer of an NVO configured again behind it.
        void staleDelKeepsRecreatedNvo()
        {
            setRemoteVni();
            nvoDel();
            ASSERT_TRUE(nvoHeld());

            // The DEL is still queued ahead of the SET.
            nvoSet();
            EXPECT_EQ(m_nvoOrch->getEVPNVtep(), vtep());

            delRemoteVni();
            retryNvo();
            EXPECT_FALSE(nvoHeld());
            EXPECT_EQ(m_nvoOrch->getEVPNVtep(), vtep());
            EXPECT_TRUE(m_nvoOrch->m_heldDeletes.empty());
        }

        void tunnelSet(const string &src_ip)
        {
            apply(m_VxlanTunnelOrch, APP_VXLAN_TUNNEL_TABLE_NAME, {"vtep1", SET_COMMAND, {{"src_ip", src_ip}}});
        }
        void retryTunnel() { static_cast<Orch *>(m_VxlanTunnelOrch)->doTask(); }
        bool tunnelHeld() { return !consumerOf(m_VxlanTunnelOrch, APP_VXLAN_TUNNEL_TABLE_NAME)->m_toSync.empty(); }

        // A teardown leaves the NVO and tunnel DELs held, and the same config is created again. The tunnel DEL is
        // dropped as stale, so a later NVO-only delete leaves the tunnel in place.
        void staleDelKeepsRecreatedTunnel()
        {
            setRemoteVni();
            nvoDel();
            tunnelDel();
            ASSERT_TRUE(nvoHeld());
            ASSERT_TRUE(tunnelHeld());

            tunnelSet("10.1.1.1");
            nvoSet();
            retryTunnel();
            EXPECT_FALSE(tunnelHeld());
            EXPECT_TRUE(m_VxlanTunnelOrch->m_heldDeletes.empty());

            delRemoteVni();
            retryNvo();
            EXPECT_FALSE(nvoHeld());
            nvoDel();
            EXPECT_FALSE(nvoHeld());
            EXPECT_EQ(m_nvoOrch->getEVPNVtep(), nullptr);
            retryTunnel();
            EXPECT_TRUE(m_VxlanTunnelOrch->isTunnelExists("vtep1"));
        }

        // A tunnel row back with another source IP is not the tunnel the DEL is for: the DEL stays held, the SET
        // waits behind it, and the tunnel is created with the new source IP once the old one is released.
        void heldTunnelDelKeptForOtherSourceIp()
        {
            setRemoteVni();
            mapDel();
            nvoDel();
            tunnelDel();
            tunnelSet("10.1.1.9");
            retryTunnel();
            EXPECT_TRUE(tunnelHeld());
            EXPECT_EQ(vtep()->getSrcIP(), IpAddress("10.1.1.1"));

            delRemoteVni();
            retryNvo();
            EXPECT_FALSE(nvoHeld());
            retryTunnel();
            EXPECT_FALSE(tunnelHeld());
            ASSERT_TRUE(m_VxlanTunnelOrch->isTunnelExists("vtep1"));
            EXPECT_EQ(vtep()->getSrcIP(), IpAddress("10.1.1.9"));
            EXPECT_TRUE(m_VxlanTunnelOrch->m_heldDeletes.empty());
        }

        // The full teardown (map, NVO, tunnel) is held on the remote VTEP, and the same config comes back while the
        // hw delete is pending: the tunnel DEL is dropped, the remote VNI DEL finishes the pending delete, and the
        // map that waited for it is created on the same tunnel.
        void staleTunnelDelDuringPendingHwDelete()
        {
            setRemoteVni();
            mapDel();
            nvoDel();
            tunnelDel();
            ASSERT_TRUE(vtep()->del_tnl_hw_pending);
            ASSERT_TRUE(tunnelHeld());

            tunnelSet("10.1.1.1");
            nvoSet();
            mapSet();
            retryTunnel();
            EXPECT_FALSE(tunnelHeld());

            delRemoteVni();
            static_cast<Orch *>(m_tunnelMapOrch)->doTask();
            EXPECT_FALSE(mapHeld());
            EXPECT_FALSE(vtep()->del_tnl_hw_pending);
            EXPECT_TRUE(vtep()->isActive());
            EXPECT_EQ(m_nvoOrch->getEVPNVtep(), vtep());
        }

        // An NVO configured again on another source VTEP behind its held DEL waits: the old VTEP's users still
        // release through the NVO, and the NVO moves once they are gone.
        void nvoRecreatedOnAnotherVtepWaits()
        {
            struct RowCleanup
            {
                Table t;
                ~RowCleanup() { t.del("vtep2"); }
            } cleanup{Table(m_app_db.get(), APP_VXLAN_TUNNEL_TABLE_NAME)};
            setRemoteVni();
            nvoDel();
            ASSERT_TRUE(nvoHeld());
            apply(m_VxlanTunnelOrch, APP_VXLAN_TUNNEL_TABLE_NAME, {"vtep2", SET_COMMAND, {{"src_ip", "10.1.1.2"}}});
            ASSERT_TRUE(m_VxlanTunnelOrch->isTunnelExists("vtep2"));

            apply(m_nvoOrch, APP_VXLAN_EVPN_NVO_TABLE_NAME, {"nvo1", SET_COMMAND, {{"source_vtep", "vtep2"}}});
            EXPECT_TRUE(nvoHeld());
            EXPECT_EQ(m_nvoOrch->getEVPNVtep(), vtep());

            delRemoteVni();
            EXPECT_TRUE(remoteVniConsumer()->m_toSync.empty());
            EXPECT_FALSE(remoteVtepIsFloodMember());
            retryNvo();
            EXPECT_FALSE(nvoHeld());
            EXPECT_EQ(m_nvoOrch->getEVPNVtep(), m_VxlanTunnelOrch->getVxlanTunnel("vtep2"));
            EXPECT_TRUE(m_nvoOrch->m_heldDeletes.empty());
        }

        // The tunnel comes back with another source IP while its DEL and the NVO's are held on a pending hw delete,
        // and the NVO is configured again on it: the NVO waits for the old tunnel to go and binds the new one.
        void nvoRecreatedBehindHeldTunnelDelWithNewSourceIp()
        {
            setRemoteVni();
            mapDel();
            nvoDel();
            tunnelDel();
            ASSERT_TRUE(vtep()->del_tnl_hw_pending);
            tunnelSet("10.1.1.9");
            retryTunnel();
            ASSERT_TRUE(tunnelHeld());

            nvoSet();
            EXPECT_TRUE(nvoHeld());

            delRemoteVni();
            EXPECT_FALSE(vtep()->del_tnl_hw_pending);
            retryNvo();
            retryTunnel();
            EXPECT_FALSE(tunnelHeld());
            ASSERT_TRUE(m_VxlanTunnelOrch->isTunnelExists("vtep1"));
            EXPECT_EQ(vtep()->getSrcIP(), IpAddress("10.1.1.9"));
            retryNvo();
            EXPECT_FALSE(nvoHeld());
            EXPECT_EQ(m_nvoOrch->getEVPNVtep(), vtep());
            EXPECT_TRUE(m_nvoOrch->m_heldDeletes.empty());
            EXPECT_TRUE(m_VxlanTunnelOrch->m_heldDeletes.empty());
        }

        // A held NVO DEL keeps the NVO router MAC on the default VRF: the switch MAC returns only
        // when the delete finishes.
        void heldDelKeepsRouterMac()
        {
            const MacAddress rmac("02:00:00:00:00:01");
            g_savedSetVirtualRouterAttr = sai_virtual_router_api->set_virtual_router_attribute;
            sai_virtual_router_api->set_virtual_router_attribute = countSetVirtualRouterAttr;
            m_countVrSrcMacSets = true;
            g_vrSrcMacSets = 0;
            apply(m_nvoOrch, APP_VXLAN_EVPN_NVO_TABLE_NAME,
                  {"nvo1", SET_COMMAND, {{"source_vtep", "vtep1"}, {"router_mac", rmac.to_string()}}});
            ASSERT_EQ(defaultVrfSrcMac(), rmac);
            ASSERT_EQ(g_vrSrcMacSets, 1);

            setRemoteVni();
            nvoDel();
            ASSERT_TRUE(nvoHeld());
            retryNvo();
            mapDel();
            ASSERT_TRUE(vtep()->del_tnl_hw_pending);
            retryNvo();
            EXPECT_TRUE(nvoHeld());
            EXPECT_EQ(defaultVrfSrcMac(), rmac);
            EXPECT_EQ(g_vrSrcMacSets, 1);

            delRemoteVni();
            retryNvo();
            expectReleased();
            EXPECT_EQ(defaultVrfSrcMac(), gMacAddress);
            EXPECT_EQ(g_vrSrcMacSets, 2);
        }

        bool saiTunnelExists(sai_object_id_t id)
        {
            sai_attribute_t attr;
            attr.id = SAI_TUNNEL_ATTR_TYPE;
            return sai_tunnel_api->get_tunnel_attribute(id, 1, &attr) == SAI_STATUS_SUCCESS;
        }

        // A VTEP without an NVO: vxlanmgrd writes the map DEL, then the VXLAN_TUNNEL DEL, and orchagent can take
        // the tunnel DEL first (two consumers ready at one select). The tunnel DEL waits for the map, so the last
        // map DEL still removes the SAI tunnel, and the VTEP can be configured again.
        void tunnelDelWaitsForItsMaps()
        {
            nvoDel();
            ASSERT_EQ(m_nvoOrch->getEVPNVtep(), nullptr);
            const sai_object_id_t tunnel_id = vtep()->getTunnelId();
            ASSERT_TRUE(saiTunnelExists(tunnel_id));

            tunnelDel();
            ASSERT_TRUE(m_VxlanTunnelOrch->isTunnelExists("vtep1"));
            EXPECT_TRUE(tunnelHeld());
            retryTunnel();
            EXPECT_EQ(m_VxlanTunnelOrch->m_heldDeletes.size(), 1u);

            mapDel();
            EXPECT_FALSE(mapHeld());
            EXPECT_FALSE(vtep()->isActive());
            EXPECT_FALSE(saiTunnelExists(tunnel_id));

            retryTunnel();
            EXPECT_FALSE(m_VxlanTunnelOrch->isTunnelExists("vtep1"));
            EXPECT_FALSE(tunnelHeld());
            EXPECT_TRUE(m_VxlanTunnelOrch->m_heldDeletes.empty());

            tunnelSet("10.1.1.1");
            mapSet();
            EXPECT_FALSE(mapHeld());
            ASSERT_TRUE(m_VxlanTunnelOrch->isTunnelExists("vtep1"));
            EXPECT_TRUE(vtep()->isActive());
            EXPECT_EQ(vtep()->vlan_vrf_vni_count, 1u);
        }

        // As above with the NVO still bound and idle: the tunnel DEL waits for the map and the NVO stays bound,
        // since a DEL dropped as stale later would leave nothing to bind it again. The last map DEL removes the
        // SAI tunnel, then the DEL goes and the NVO waits for the tunnel to be configured again.
        void tunnelDelWithNvoWaitsForItsMaps()
        {
            ASSERT_EQ(m_nvoOrch->getEVPNVtep(), vtep());
            const sai_object_id_t tunnel_id = vtep()->getTunnelId();
            ASSERT_TRUE(saiTunnelExists(tunnel_id));
            ASSERT_EQ(vtep()->vlan_vrf_vni_count, 1u);

            tunnelDel();
            retryTunnel();
            ASSERT_TRUE(m_VxlanTunnelOrch->isTunnelExists("vtep1"));
            EXPECT_TRUE(tunnelHeld());
            EXPECT_EQ(m_nvoOrch->getEVPNVtep(), vtep());
            EXPECT_FALSE(m_nvoOrch->isBindPending());
            EXPECT_TRUE(saiTunnelExists(tunnel_id));

            mapDel();
            EXPECT_FALSE(mapHeld());
            EXPECT_EQ(vtep()->vlan_vrf_vni_count, 0u);
            EXPECT_FALSE(vtep()->isActive());
            EXPECT_FALSE(saiTunnelExists(tunnel_id));
            Port tunnelPort;
            EXPECT_FALSE(m_VxlanTunnelOrch->getTunnelPort("10.1.1.1", tunnelPort, true));

            retryTunnel();
            EXPECT_FALSE(tunnelHeld());
            EXPECT_FALSE(m_VxlanTunnelOrch->isTunnelExists("vtep1"));
            EXPECT_TRUE(m_VxlanTunnelOrch->m_heldDeletes.empty());
            EXPECT_EQ(m_nvoOrch->getEVPNVtep(), nullptr);
            EXPECT_TRUE(m_nvoOrch->isBindPending());

            tunnelSet("10.1.1.1");
            EXPECT_EQ(m_nvoOrch->getEVPNVtep(), vtep());
            mapSet();
            EXPECT_FALSE(mapHeld());
            EXPECT_TRUE(vtep()->isActive());
            EXPECT_EQ(vtep()->vlan_vrf_vni_count, 1u);
        }

        // The VTEP is configured again with another ttl_mode. The tunnel's DEL and SET reach orchagent apart, so
        // the DEL is held for the map and the SET waits behind it, but the map's DEL and SET merged into one SET.
        // Once the new tunnel row is there, that SET removes the map, so the held DEL goes, and the map goes on the
        // tunnel created next, with the VNI it now carries.
        void mergedMapSetReattachesToTheNewTunnel(bool tunnel_set_first, const string &vni)
        {
            const sai_object_id_t tunnel_id = vtep()->getTunnelId();
            const KeyOpFieldsValuesTuple map{m_mapKey, SET_COMMAND, {{"vni", vni}, {"vlan", "Vlan100"}}};
            tunnelDel();
            ASSERT_TRUE(tunnelHeld());
            ASSERT_EQ(m_nvoOrch->getEVPNVtep(), vtep());
            if (!tunnel_set_first)
            {
                apply(m_tunnelMapOrch, APP_VXLAN_TUNNEL_MAP_TABLE_NAME, map);
                EXPECT_TRUE(mapHeld());
                EXPECT_TRUE(m_tunnelMapOrch->isTunnelMapExists(m_mapKey));
                EXPECT_TRUE(saiTunnelExists(tunnel_id));
            }
            apply(m_VxlanTunnelOrch, APP_VXLAN_TUNNEL_TABLE_NAME,
                  {"vtep1", SET_COMMAND, {{"src_ip", "10.1.1.1"}, {"ttl_mode", "pipe"}}});
            ASSERT_TRUE(tunnelHeld());

            if (tunnel_set_first)
            {
                apply(m_tunnelMapOrch, APP_VXLAN_TUNNEL_MAP_TABLE_NAME, map);
            }
            else
            {
                static_cast<Orch *>(m_tunnelMapOrch)->doTask();
            }
            EXPECT_TRUE(mapHeld());
            EXPECT_FALSE(m_tunnelMapOrch->isTunnelMapExists(m_mapKey));
            EXPECT_EQ(vtep()->vlan_vrf_vni_count, 0u);
            EXPECT_FALSE(saiTunnelExists(tunnel_id));

            retryTunnel();
            EXPECT_FALSE(tunnelHeld());
            ASSERT_TRUE(m_VxlanTunnelOrch->isTunnelExists("vtep1"));
            EXPECT_TRUE(vtep()->hasConfig(IpAddress("10.1.1.1"), IpAddress("0.0.0.0"), VxlanTunnelTTLMode::PIPE));
            EXPECT_EQ(m_nvoOrch->getEVPNVtep(), vtep());
            EXPECT_TRUE(m_VxlanTunnelOrch->m_heldDeletes.empty());

            static_cast<Orch *>(m_tunnelMapOrch)->doTask();
            EXPECT_FALSE(mapHeld());
            EXPECT_TRUE(vtep()->isActive());
            EXPECT_EQ(vtep()->vlan_vrf_vni_count, 1u);
            EXPECT_EQ(vlanVni(100), static_cast<uint32_t>(stoul(vni)));
        }

        // As above with the tunnel back as it was: the held DEL is stale, so the merged map SET waits for it to be
        // dropped and then keeps the map and the SAI tunnel as they are, also when it comes before the tunnel SET.
        void mergedMapSetKeepsTheSameTunnel(bool tunnel_set_first)
        {
            const sai_object_id_t tunnel_id = vtep()->getTunnelId();
            tunnelDel();
            ASSERT_TRUE(tunnelHeld());
            if (tunnel_set_first)
            {
                Table(m_app_db.get(), APP_VXLAN_TUNNEL_TABLE_NAME).set("vtep1", {{"src_ip", "10.1.1.1"}});
            }

            mapSet();
            EXPECT_TRUE(mapHeld());
            EXPECT_TRUE(m_tunnelMapOrch->isTunnelMapExists(m_mapKey));
            EXPECT_TRUE(saiTunnelExists(tunnel_id));

            if (tunnel_set_first)
            {
                retryTunnel();
            }
            else
            {
                tunnelSet("10.1.1.1");
            }
            EXPECT_FALSE(tunnelHeld());
            static_cast<Orch *>(m_tunnelMapOrch)->doTask();
            EXPECT_FALSE(mapHeld());
            EXPECT_EQ(vtep()->getTunnelId(), tunnel_id);
            EXPECT_TRUE(saiTunnelExists(tunnel_id));
            EXPECT_EQ(vtep()->vlan_vrf_vni_count, 1u);
            EXPECT_EQ(m_nvoOrch->getEVPNVtep(), vtep());
        }

        // The SAI refuses the map entry remove of a merged map SET behind a held tunnel DEL: it is logged, not
        // retried on the SET's later passes, and the held DEL finishes with the map going on the new tunnel.
        void mergedMapSetWithRefusedMapEntryRemove()
        {
            const KeyOpFieldsValuesTuple map{m_mapKey, SET_COMMAND, {{"vni", "1000"}, {"vlan", "Vlan100"}}};
            tunnelDel();
            ASSERT_TRUE(tunnelHeld());
            apply(m_VxlanTunnelOrch, APP_VXLAN_TUNNEL_TABLE_NAME,
                  {"vtep1", SET_COMMAND, {{"src_ip", "10.1.1.1"}, {"ttl_mode", "pipe"}}});
            ASSERT_TRUE(tunnelHeld());

            g_refusedTunnelMapEntryId = m_tunnelMapOrch->vxlan_tunnel_map_table_[m_mapKey].map_entry_id;
            ASSERT_NE(g_refusedTunnelMapEntryId, SAI_NULL_OBJECT_ID);
            g_refusedTunnelMapEntryRemoves = 0;
            g_savedRemoveTunnelMapEntry = sai_tunnel_api->remove_tunnel_map_entry;
            sai_tunnel_api->remove_tunnel_map_entry = stubRemoveTunnelMapEntryRefused;

            apply(m_tunnelMapOrch, APP_VXLAN_TUNNEL_MAP_TABLE_NAME, map);
            static_cast<Orch *>(m_tunnelMapOrch)->doTask();
            static_cast<Orch *>(m_tunnelMapOrch)->doTask();
            EXPECT_EQ(g_refusedTunnelMapEntryRemoves, 1);
            EXPECT_TRUE(mapHeld());
            EXPECT_FALSE(m_tunnelMapOrch->isTunnelMapExists(m_mapKey));
            EXPECT_EQ(vtep()->vlan_vrf_vni_count, 0u);

            retryTunnel();
            EXPECT_FALSE(tunnelHeld());
            ASSERT_TRUE(m_VxlanTunnelOrch->isTunnelExists("vtep1"));
            EXPECT_TRUE(vtep()->hasConfig(IpAddress("10.1.1.1"), IpAddress("0.0.0.0"), VxlanTunnelTTLMode::PIPE));
            EXPECT_TRUE(m_VxlanTunnelOrch->m_heldDeletes.empty());

            static_cast<Orch *>(m_tunnelMapOrch)->doTask();
            EXPECT_FALSE(mapHeld());
            EXPECT_TRUE(vtep()->isActive());
            EXPECT_EQ(vtep()->vlan_vrf_vni_count, 1u);
            EXPECT_EQ(vlanVni(100), 1000U);
            EXPECT_EQ(g_refusedTunnelMapEntryRemoves, 1);
        }

        // A tunnel DEL held for its maps is dropped when the same tunnel is configured again behind it: the
        // tunnel and its map stay, and a later map DEL alone removes only the hw.
        void heldForMapsDroppedOnRecreate()
        {
            nvoDel();
            tunnelDel();
            ASSERT_TRUE(tunnelHeld());

            tunnelSet("10.1.1.1");
            retryTunnel();
            EXPECT_FALSE(tunnelHeld());
            EXPECT_TRUE(m_VxlanTunnelOrch->m_heldDeletes.empty());
            ASSERT_TRUE(m_VxlanTunnelOrch->isTunnelExists("vtep1"));
            EXPECT_TRUE(vtep()->isActive());

            mapDel();
            EXPECT_FALSE(vtep()->isActive());
            EXPECT_TRUE(m_VxlanTunnelOrch->isTunnelExists("vtep1"));
        }

        // The VLAN DEL can also come before the map DEL: the map DEL still removes the tunnel hw, so the held
        // tunnel DEL then goes.
        void vlanDel()
        {
            Table(m_app_db.get(), APP_VLAN_TABLE_NAME).del("Vlan100");
            consumerOf(gPortsOrch, APP_VLAN_TABLE_NAME)->addToSync(
                deque<KeyOpFieldsValuesTuple>{{"Vlan100", DEL_COMMAND, {}}});
            static_cast<Orch *>(gPortsOrch)->doTask();
        }

        bool vlanHeld() { return !consumerOf(gPortsOrch, APP_VLAN_TABLE_NAME)->m_toSync.empty(); }

        void vlanSet()
        {
            const vector<FieldValueTuple> fvs = {{"admin_status", "up"}, {"mtu", "9100"}};
            Table(m_app_db.get(), APP_VLAN_TABLE_NAME).set("Vlan100", fvs);
            consumerOf(gPortsOrch, APP_VLAN_TABLE_NAME)->addToSync(
                deque<KeyOpFieldsValuesTuple>{{"Vlan100", SET_COMMAND, fvs}});
            static_cast<Orch *>(gPortsOrch)->doTask();
        }

        // What orchagent saw in evpn test_l2_config's parent-first case on hardware: the VLAN DEL arrives before the
        // map DEL and is held, the map's DEL and re-add reach orchagent merged into one SET (so the map is never
        // removed), the tunnel is re-added the same way, and the VLAN is re-created. The case's teardown then deletes
        // the map, which must not take the re-created VLAN with it.
        void parentFirstDeleteMergedWithRecreate()
        {
            nvoDel();
            vlanDel();
            ASSERT_TRUE(vlanHeld());

            tunnelSet("10.1.1.1");
            nvoSet();
            mapSet();
            vlanSet();
            EXPECT_FALSE(vlanHeld());
            Port vlanPort;
            ASSERT_TRUE(gPortsOrch->getVlanByVlanId(100, vlanPort));
            EXPECT_EQ(vlanPort.m_vnid, 1000U);
            EXPECT_TRUE(vtep()->isActive());

            mapDel();
            static_cast<Orch *>(gPortsOrch)->doTask();
            EXPECT_FALSE(vlanHeld());
            ASSERT_TRUE(gPortsOrch->getVlanByVlanId(100, vlanPort));
            EXPECT_EQ(vlanPort.m_vnid, VNID_NONE);
        }

        // A VLAN DEL held for its map is dropped when the VLAN is re-created, so the later map DEL does not take
        // the re-created VLAN with it.
        void heldVlanDelDroppedOnRecreate()
        {
            vlanDel();
            ASSERT_TRUE(vlanHeld());

            vlanSet();
            EXPECT_FALSE(vlanHeld());
            Port vlanPort;
            ASSERT_TRUE(gPortsOrch->getVlanByVlanId(100, vlanPort));
            EXPECT_EQ(vlanPort.m_vnid, 1000U);

            mapDel();
            EXPECT_FALSE(mapHeld());
            static_cast<Orch *>(gPortsOrch)->doTask();
            ASSERT_TRUE(gPortsOrch->getVlanByVlanId(100, vlanPort));
            EXPECT_EQ(vlanPort.m_vnid, VNID_NONE);
        }

        // A second VLAN-VNI map whose SAI entry create fails: the map is still recorded and counted, so its DEL
        // undoes the count, and the held tunnel DEL goes once the last map DEL has removed the tunnel hw.
        void failedMapEntryCreateKeepsMapCount(sai_status_t status)
        {
            const string key2 = "vtep1:map_2000_Vlan200";
            const vector<FieldValueTuple> vlanFvs = {{"admin_status", "up"}, {"mtu", "9100"}};
            Table(m_app_db.get(), APP_VLAN_TABLE_NAME).set("Vlan200", vlanFvs);
            consumerOf(gPortsOrch, APP_VLAN_TABLE_NAME)->addToSync(
                deque<KeyOpFieldsValuesTuple>{{"Vlan200", SET_COMMAND, vlanFvs}});
            static_cast<Orch *>(gPortsOrch)->doTask();

            nvoDel();
            const sai_object_id_t tunnel_id = vtep()->getTunnelId();
            ASSERT_EQ(vtep()->vlan_vrf_vni_count, 1u);

            g_tunnelMapEntryCreateStatus = status;
            g_savedCreateTunnelMapEntry = sai_tunnel_api->create_tunnel_map_entry;
            sai_tunnel_api->create_tunnel_map_entry = stubCreateTunnelMapEntryFails;
            apply(m_tunnelMapOrch, APP_VXLAN_TUNNEL_MAP_TABLE_NAME,
                  {key2, SET_COMMAND, {{"vni", "2000"}, {"vlan", "Vlan200"}}});
            static_cast<Orch *>(m_tunnelMapOrch)->doTask();
            sai_tunnel_api->create_tunnel_map_entry = g_savedCreateTunnelMapEntry;
            g_savedCreateTunnelMapEntry = nullptr;
            EXPECT_FALSE(mapHeld());
            EXPECT_EQ(vtep()->vlan_vrf_vni_count, 2u);
            ASSERT_TRUE(m_tunnelMapOrch->isTunnelMapExists(key2));
            EXPECT_EQ(m_tunnelMapOrch->vxlan_tunnel_map_table_[key2].map_entry_id, SAI_NULL_OBJECT_ID);

            tunnelDel();
            ASSERT_TRUE(tunnelHeld());
            apply(m_tunnelMapOrch, APP_VXLAN_TUNNEL_MAP_TABLE_NAME, {key2, DEL_COMMAND, {}});
            EXPECT_FALSE(mapHeld());
            EXPECT_EQ(vtep()->vlan_vrf_vni_count, 1u);
            EXPECT_TRUE(vtep()->isActive());
            retryTunnel();
            EXPECT_TRUE(tunnelHeld());

            mapDel();
            EXPECT_FALSE(vtep()->isActive());
            EXPECT_FALSE(saiTunnelExists(tunnel_id));
            retryTunnel();
            EXPECT_FALSE(tunnelHeld());
            EXPECT_FALSE(m_VxlanTunnelOrch->isTunnelExists("vtep1"));
            EXPECT_TRUE(m_VxlanTunnelOrch->m_heldDeletes.empty());
        }

        uint32_t vlanVni(sai_vlan_id_t vlan_id)
        {
            Port vlanPort;
            EXPECT_TRUE(gPortsOrch->getVlanByVlanId(vlan_id, vlanPort));
            return vlanPort.m_vnid;
        }

        // The decap entries orchagent holds for a VNI.
        size_t decapEntries(uint32_t vni)
        {
            size_t n = 0;
            for (const auto &map : m_tunnelMapOrch->vxlan_tunnel_map_table_)
            {
                n += map.second.vni_id == vni && map.second.map_entry_id != SAI_NULL_OBJECT_ID;
            }
            return n;
        }

        void addVlan200()
        {
            const vector<FieldValueTuple> vlanFvs = {{"admin_status", "up"}, {"mtu", "9100"}};
            apply(gPortsOrch, APP_VLAN_TABLE_NAME, {"Vlan200", SET_COMMAND, vlanFvs});
        }

        // A VNI change whose new map arrives before the old one's DEL: the new map waits for the VLAN, then applies.
        void vniChangeWaitsForTheOldMap()
        {
            const string newKey = "vtep1:map_2000_Vlan100";
            apply(m_tunnelMapOrch, APP_VXLAN_TUNNEL_MAP_TABLE_NAME,
                  {newKey, SET_COMMAND, {{"vni", "2000"}, {"vlan", "Vlan100"}}});
            EXPECT_TRUE(mapHeld());
            EXPECT_FALSE(m_tunnelMapOrch->isTunnelMapExists(newKey));
            EXPECT_EQ(vlanVni(100), 1000U);
            EXPECT_EQ(m_VxlanTunnelOrch->getVlanMappedToVni(2000), 0);

            mapDel();
            EXPECT_FALSE(mapHeld());
            EXPECT_EQ(vlanVni(100), 2000U);
            EXPECT_EQ(m_VxlanTunnelOrch->getVlanMappedToVni(2000), 100);
            EXPECT_EQ(m_VxlanTunnelOrch->getVlanMappedToVni(1000), 0);

            apply(m_tunnelMapOrch, APP_VXLAN_TUNNEL_MAP_TABLE_NAME, {newKey, DEL_COMMAND, {}});
            EXPECT_EQ(vlanVni(100), VNID_NONE);
        }

        // A VNI change reverted before orchagent catches up: the waiting create is replaced by its DEL, and the VLAN
        // keeps the older map's VNI, so its DEL still waits for that map.
        void revertedVniChangeKeepsTheOldVni()
        {
            const string newKey = "vtep1:map_2000_Vlan100";
            apply(m_tunnelMapOrch, APP_VXLAN_TUNNEL_MAP_TABLE_NAME,
                  {newKey, SET_COMMAND, {{"vni", "2000"}, {"vlan", "Vlan100"}}});
            ASSERT_TRUE(mapHeld());

            apply(m_tunnelMapOrch, APP_VXLAN_TUNNEL_MAP_TABLE_NAME, {newKey, DEL_COMMAND, {}});
            EXPECT_FALSE(mapHeld());
            EXPECT_EQ(vlanVni(100), 1000U);
            vlanDel();
            EXPECT_TRUE(vlanHeld());

            mapDel();
            EXPECT_EQ(vlanVni(100), VNID_NONE);
            static_cast<Orch *>(gPortsOrch)->doTask();
            EXPECT_FALSE(vlanHeld());
            Port vlanPort;
            EXPECT_FALSE(gPortsOrch->getVlanByVlanId(100, vlanPort));
        }

        // A remap in one batch whose new key sorts before the old one: pass 1 removes the old entry and keeps the
        // create, pass 2 adds it.
        void remapInOneBatch()
        {
            const string newKey = "vtep1:map_10000_Vlan100";
            ASSERT_LT(newKey, m_mapKey);
            queue(m_tunnelMapOrch, APP_VXLAN_TUNNEL_MAP_TABLE_NAME, {m_mapKey, DEL_COMMAND, {}});
            queue(m_tunnelMapOrch, APP_VXLAN_TUNNEL_MAP_TABLE_NAME,
                  {newKey, SET_COMMAND, {{"vni", "10000"}, {"vlan", "Vlan100"}}});

            static_cast<Orch *>(m_tunnelMapOrch)->doTask();
            EXPECT_TRUE(mapHeld());
            EXPECT_FALSE(m_tunnelMapOrch->isTunnelMapExists(m_mapKey));
            EXPECT_FALSE(m_tunnelMapOrch->isTunnelMapExists(newKey));
            EXPECT_EQ(vlanVni(100), VNID_NONE);

            static_cast<Orch *>(m_tunnelMapOrch)->doTask();
            EXPECT_FALSE(mapHeld());
            EXPECT_EQ(m_tunnelMapOrch->vxlan_tunnel_map_table_.size(), 1u);
            EXPECT_EQ(decapEntries(10000), 1u);
            EXPECT_EQ(decapEntries(1000), 0u);
            EXPECT_EQ(vlanVni(100), 10000U);
            EXPECT_EQ(m_VxlanTunnelOrch->getVlanMappedToVni(10000), 100);
            EXPECT_EQ(m_VxlanTunnelOrch->getVlanMappedToVni(1000), 0);
            EXPECT_EQ(vtep()->vlan_vrf_vni_count, 1u);

            apply(m_tunnelMapOrch, APP_VXLAN_TUNNEL_MAP_TABLE_NAME, {newKey, DEL_COMMAND, {}});
        }

        // Vlan100 and Vlan200 swap VNIs 1000 and 2000 in one batch. Each create sorts between the two DELs, so
        // pass 1 keeps both creates (one waits for its VLAN, the other for its VNI) and removes the old entries;
        // pass 2 adds both, and the DEL of map_2000_Vlan200 does not unbind VNI 2000 from Vlan100.
        void swapInOneBatch()
        {
            const string key2 = "vtep1:map_2000_Vlan200";
            const string swap1 = "vtep1:map_2000_Vlan100";
            const string swap2 = "vtep1:map_1000_Vlan200";
            addVlan200();
            apply(m_tunnelMapOrch, APP_VXLAN_TUNNEL_MAP_TABLE_NAME,
                  {key2, SET_COMMAND, {{"vni", "2000"}, {"vlan", "Vlan200"}}});
            ASSERT_FALSE(mapHeld());
            ASSERT_EQ(vlanVni(200), 2000U);

            queue(m_tunnelMapOrch, APP_VXLAN_TUNNEL_MAP_TABLE_NAME, {m_mapKey, DEL_COMMAND, {}});
            queue(m_tunnelMapOrch, APP_VXLAN_TUNNEL_MAP_TABLE_NAME, {key2, DEL_COMMAND, {}});
            queue(m_tunnelMapOrch, APP_VXLAN_TUNNEL_MAP_TABLE_NAME,
                  {swap1, SET_COMMAND, {{"vni", "2000"}, {"vlan", "Vlan100"}}});
            queue(m_tunnelMapOrch, APP_VXLAN_TUNNEL_MAP_TABLE_NAME,
                  {swap2, SET_COMMAND, {{"vni", "1000"}, {"vlan", "Vlan200"}}});

            static_cast<Orch *>(m_tunnelMapOrch)->doTask();
            EXPECT_EQ(consumerOf(m_tunnelMapOrch, APP_VXLAN_TUNNEL_MAP_TABLE_NAME)->m_toSync.size(), 2u);
            EXPECT_TRUE(m_tunnelMapOrch->vxlan_tunnel_map_table_.empty());

            static_cast<Orch *>(m_tunnelMapOrch)->doTask();
            EXPECT_FALSE(mapHeld());
            EXPECT_EQ(m_tunnelMapOrch->vxlan_tunnel_map_table_.size(), 2u);
            EXPECT_EQ(decapEntries(1000), 1u);
            EXPECT_EQ(decapEntries(2000), 1u);
            EXPECT_EQ(m_tunnelMapOrch->vxlan_tunnel_map_table_[swap1].vlan_id, 100);
            EXPECT_EQ(m_tunnelMapOrch->vxlan_tunnel_map_table_[swap2].vlan_id, 200);
            EXPECT_EQ(m_VxlanTunnelOrch->getVlanMappedToVni(2000), 100);
            EXPECT_EQ(m_VxlanTunnelOrch->getVlanMappedToVni(1000), 200);
            EXPECT_EQ(vlanVni(100), 2000U);
            EXPECT_EQ(vlanVni(200), 1000U);
            EXPECT_EQ(vtep()->vlan_vrf_vni_count, 2u);

            apply(m_tunnelMapOrch, APP_VXLAN_TUNNEL_MAP_TABLE_NAME, {swap1, DEL_COMMAND, {}});
            apply(m_tunnelMapOrch, APP_VXLAN_TUNNEL_MAP_TABLE_NAME, {swap2, DEL_COMMAND, {}});
        }

        // A map key not named map_<vni>_<vlan> still releases its VNI on delete, so a later map of that VNI applies.
        void freeFormKeyReleasesItsVni()
        {
            const string entryKey = "vtep1:entry_1";
            const string nextKey = "vtep1:entry_2";
            addVlan200();
            apply(m_tunnelMapOrch, APP_VXLAN_TUNNEL_MAP_TABLE_NAME,
                  {entryKey, SET_COMMAND, {{"vni", "3000"}, {"vlan", "Vlan200"}}});
            ASSERT_EQ(m_VxlanTunnelOrch->getVlanMappedToVni(3000), 200);

            apply(m_tunnelMapOrch, APP_VXLAN_TUNNEL_MAP_TABLE_NAME, {entryKey, DEL_COMMAND, {}});
            EXPECT_EQ(m_VxlanTunnelOrch->getVlanMappedToVni(3000), 0);
            EXPECT_EQ(vlanVni(200), VNID_NONE);

            mapDel();
            apply(m_tunnelMapOrch, APP_VXLAN_TUNNEL_MAP_TABLE_NAME,
                  {nextKey, SET_COMMAND, {{"vni", "3000"}, {"vlan", "Vlan100"}}});
            EXPECT_FALSE(mapHeld());
            EXPECT_EQ(m_VxlanTunnelOrch->getVlanMappedToVni(3000), 100);
            EXPECT_EQ(decapEntries(3000), 1u);

            apply(m_tunnelMapOrch, APP_VXLAN_TUNNEL_MAP_TABLE_NAME, {nextKey, DEL_COMMAND, {}});
        }

        // Writes the APPL_DB row as the consumer's pop does and queues the entry, without processing it.
        void queue(Orch *orch, const string &table, const KeyOpFieldsValuesTuple &kfv)
        {
            Table t(m_app_db.get(), table);
            if (kfvOp(kfv) == DEL_COMMAND)
            {
                t.del(kfvKey(kfv));
            }
            else
            {
                t.set(kfvKey(kfv), kfvFieldsValues(kfv));
            }
            consumerOf(orch, table)->addToSync(deque<KeyOpFieldsValuesTuple>{kfv});
        }

        const KeyOpFieldsValuesTuple m_vlan200Set = {
            "Vlan200", SET_COMMAND, {{"admin_status", "up"}, {"mtu", "9100"}, {"host_ifname", "Vlan200"}}};

        // Vlan200 with a host interface, in STP instance 1 with Ethernet0 forwarding.
        void addStpVlan200()
        {
            vector<string> stpTables = {APP_STP_VLAN_INSTANCE_TABLE_NAME, APP_STP_PORT_STATE_TABLE_NAME};
            Table(m_state_db.get(), STATE_STP_TABLE_NAME).del("BPDU_GUARD");
            m_stpOrch = new StpOrch(m_app_db.get(), m_state_db.get(), stpTables);
            gStpOrch = m_stpOrch;
            // The VS SAI has no SAI_SWITCH_ATTR_MAX_STP_INSTANCE; the default instance is still read.
            sai_attribute_t stpAttr;
            stpAttr.id = SAI_SWITCH_ATTR_DEFAULT_STP_INST_ID;
            ASSERT_EQ(sai_switch_api->get_switch_attribute(gSwitchId, 1, &stpAttr), SAI_STATUS_SUCCESS);
            ASSERT_EQ(m_stpOrch->m_defaultStpId, stpAttr.value.oid);
            g_vlanHostifCreates = 0;
            g_vlanHostifRemoves = 0;
            g_savedCreateHostif = sai_hostif_api->create_hostif;
            sai_hostif_api->create_hostif = stubCreateVlanHostif;
            g_savedRemoveHostif = sai_hostif_api->remove_hostif;
            sai_hostif_api->remove_hostif = stubRemoveVlanHostif;

            apply(gPortsOrch, APP_VLAN_TABLE_NAME, m_vlan200Set);
            apply(gStpOrch, APP_STP_VLAN_INSTANCE_TABLE_NAME, {"Vlan200", SET_COMMAND, {{"stp_instance", "1"}}});
            apply(gStpOrch, APP_STP_PORT_STATE_TABLE_NAME, {"Ethernet0:1", SET_COMMAND, {{"state", "4"}}});
            Port vlan, port;
            ASSERT_TRUE(gPortsOrch->getPort("Vlan200", vlan));
            ASSERT_EQ(vlan.m_vlan_info.host_intf_id, g_vlanHostifId);
            ASSERT_EQ(vlan.m_stp_id, 1);
            ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));
            ASSERT_EQ(port.m_stp_port_ids.count(1), 1u);
        }

        // The SAI VLAN remove fails after the VLAN's host interface and STP membership are gone. A VLAN-VNI map
        // and a router interface then attach to the VLAN, and the VLAN and a member are configured again. The
        // retried DEL is stale: the VLAN stays with the map, the router interface and the member, and gets its
        // host interface and STP state back.
        void partlyRemovedVlanKeptOnRecreate()
        {
            ASSERT_NO_FATAL_FAILURE(addStpVlan200());
            Port vlan, port;
            ASSERT_TRUE(gPortsOrch->getPort("Vlan200", vlan));
            const sai_object_id_t vlan_oid = vlan.m_vlan_info.vlan_oid;

            g_vlanRemoveFailures = 1;
            g_savedRemoveVlan = sai_vlan_api->remove_vlan;
            sai_vlan_api->remove_vlan = stubRemoveVlanFailsFirst;
            auto *vlanConsumer = consumerOf(gPortsOrch, APP_VLAN_TABLE_NAME);
            apply(gPortsOrch, APP_VLAN_TABLE_NAME, {"Vlan200", DEL_COMMAND, {}});
            EXPECT_EQ(vlanConsumer->m_toSync.size(), 1u);
            EXPECT_EQ(g_vlanHostifRemoves, 1);
            ASSERT_TRUE(gPortsOrch->getPort("Vlan200", vlan));
            EXPECT_EQ(vlan.m_vlan_info.host_intf_id, SAI_NULL_OBJECT_ID);
            EXPECT_EQ(vlan.m_stp_id, -1);

            apply(m_tunnelMapOrch, APP_VXLAN_TUNNEL_MAP_TABLE_NAME,
                  {"vtep1:map_2000_Vlan200", SET_COMMAND, {{"vni", "2000"}, {"vlan", "Vlan200"}}});
            apply(gIntfsOrch, APP_INTF_TABLE_NAME, {"Vlan200", SET_COMMAND, {{"mtu", "9100"}}});
            static_cast<Orch *>(gPortsOrch)->doTask();
            EXPECT_EQ(vlanConsumer->m_toSync.size(), 1u);

            queue(gPortsOrch, APP_VLAN_TABLE_NAME, m_vlan200Set);
            queue(gPortsOrch, APP_VLAN_MEMBER_TABLE_NAME,
                  {"Vlan200:Ethernet0", SET_COMMAND, {{"tagging_mode", "untagged"}}});
            static_cast<Orch *>(gPortsOrch)->doTask();
            static_cast<Orch *>(gStpOrch)->doTask();
            EXPECT_TRUE(vlanConsumer->m_toSync.empty());
            EXPECT_TRUE(consumerOf(gPortsOrch, APP_VLAN_MEMBER_TABLE_NAME)->m_toSync.empty());
            EXPECT_TRUE(consumerOf(gStpOrch, APP_STP_VLAN_INSTANCE_TABLE_NAME)->m_toSync.empty());
            EXPECT_TRUE(consumerOf(gStpOrch, APP_STP_PORT_STATE_TABLE_NAME)->m_toSync.empty());

            ASSERT_TRUE(gPortsOrch->getPort("Vlan200", vlan));
            EXPECT_EQ(vlan.m_vlan_info.vlan_oid, vlan_oid);
            EXPECT_EQ(vlan.m_vnid, 2000U);
            EXPECT_NE(vlan.m_rif_id, SAI_NULL_OBJECT_ID);
            EXPECT_EQ(vlan.m_members.count("Ethernet0"), 1u);
            EXPECT_EQ(vlan.m_vlan_info.host_intf_id, g_vlanHostifId);
            EXPECT_EQ(g_vlanHostifCreates, 2);
            EXPECT_EQ(vlan.m_stp_id, 1);
            ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));
            EXPECT_EQ(port.m_stp_port_ids.count(1), 1u);
            EXPECT_TRUE(gPortsOrch->m_partlyRemovedVlans.empty());

            // stpd sends the instance and port state again for the re-created VLAN: nothing changes.
            const sai_object_id_t stp_oid = m_stpOrch->getStpInstanceOid(1);
            const sai_object_id_t stp_port_oid = port.m_stp_port_ids.at(1);
            apply(gStpOrch, APP_STP_VLAN_INSTANCE_TABLE_NAME, {"Vlan200", SET_COMMAND, {{"stp_instance", "1"}}});
            apply(gStpOrch, APP_STP_PORT_STATE_TABLE_NAME, {"Ethernet0:1", SET_COMMAND, {{"state", "4"}}});
            EXPECT_TRUE(consumerOf(gStpOrch, APP_STP_VLAN_INSTANCE_TABLE_NAME)->m_toSync.empty());
            EXPECT_TRUE(consumerOf(gStpOrch, APP_STP_PORT_STATE_TABLE_NAME)->m_toSync.empty());
            EXPECT_EQ(m_stpOrch->getStpInstanceOid(1), stp_oid);
            EXPECT_EQ(m_stpOrch->m_stpInstToOid.size(), 1u);
            EXPECT_EQ(m_stpOrch->m_vlanAliasToStpInstanceMap.at(1).stp_inst_vlan_list, set<string>{"Vlan200"});
            ASSERT_TRUE(gPortsOrch->getPort("Vlan200", vlan));
            EXPECT_EQ(vlan.m_stp_id, 1);
            sai_attribute_t attr;
            attr.id = SAI_VLAN_ATTR_STP_INSTANCE;
            ASSERT_EQ(sai_vlan_api->get_vlan_attribute(vlan.m_vlan_info.vlan_oid, 1, &attr), SAI_STATUS_SUCCESS);
            EXPECT_EQ(attr.value.oid, stp_oid);
            ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));
            EXPECT_EQ(port.m_stp_port_ids.size(), 1u);
            EXPECT_EQ(port.m_stp_port_ids.at(1), stp_port_oid);
        }

        // The SAI VLAN remove fails once after the VLAN left its STP instance, and the retried DEL removes it.
        // Nothing stays marked partly removed, so a stale DEL of the VLAN configured again restores no STP state.
        void partlyRemovedVlanGoneOnRetry()
        {
            ASSERT_NO_FATAL_FAILURE(addStpVlan200());
            g_vlanRemoveFailures = 1;
            g_savedRemoveVlan = sai_vlan_api->remove_vlan;
            sai_vlan_api->remove_vlan = stubRemoveVlanFailsFirst;
            auto *vlanConsumer = consumerOf(gPortsOrch, APP_VLAN_TABLE_NAME);
            apply(gPortsOrch, APP_VLAN_TABLE_NAME, {"Vlan200", DEL_COMMAND, {}});
            ASSERT_EQ(vlanConsumer->m_toSync.size(), 1u);
            ASSERT_EQ(gPortsOrch->m_partlyRemovedVlans.count("Vlan200"), 1u);

            static_cast<Orch *>(gPortsOrch)->doTask();
            EXPECT_TRUE(vlanConsumer->m_toSync.empty());
            Port vlan;
            EXPECT_FALSE(gPortsOrch->getPort("Vlan200", vlan));
            EXPECT_TRUE(gPortsOrch->m_partlyRemovedVlans.empty());

            apply(gPortsOrch, APP_VLAN_TABLE_NAME, m_vlan200Set);
            ASSERT_TRUE(gPortsOrch->getPort("Vlan200", vlan));
            EXPECT_EQ(vlan.m_stp_id, -1);
            queue(gPortsOrch, APP_VLAN_TABLE_NAME, {"Vlan200", DEL_COMMAND, {}});
            queue(gPortsOrch, APP_VLAN_TABLE_NAME, m_vlan200Set);
            static_cast<Orch *>(gPortsOrch)->doTask();
            EXPECT_TRUE(vlanConsumer->m_toSync.empty());
            EXPECT_TRUE(consumerOf(gStpOrch, APP_STP_VLAN_INSTANCE_TABLE_NAME)->m_toSync.empty());
            EXPECT_TRUE(consumerOf(gStpOrch, APP_STP_PORT_STATE_TABLE_NAME)->m_toSync.empty());
            ASSERT_TRUE(gPortsOrch->getPort("Vlan200", vlan));
            EXPECT_EQ(vlan.m_stp_id, -1);
        }

        // The VLAN DEL waits for the VLAN's map; the map DEL then releases the tunnel with the VLAN still there.
        void vlanDelWaitsForMap()
        {
            nvoDel();
            const sai_object_id_t tunnel_id = vtep()->getTunnelId();
            tunnelDel();
            ASSERT_TRUE(tunnelHeld());

            vlanDel();
            EXPECT_TRUE(vlanHeld());
            Port vlanPort;
            ASSERT_TRUE(gPortsOrch->getVlanByVlanId(100, vlanPort));
            EXPECT_EQ(vlanPort.m_vnid, 1000U);

            mapDel();
            EXPECT_FALSE(mapHeld());
            EXPECT_FALSE(vtep()->isActive());
            EXPECT_FALSE(saiTunnelExists(tunnel_id));
            ASSERT_TRUE(gPortsOrch->getVlanByVlanId(100, vlanPort));
            EXPECT_EQ(vlanPort.m_vnid, VNID_NONE);

            static_cast<Orch *>(gPortsOrch)->doTask();
            EXPECT_FALSE(vlanHeld());
            EXPECT_FALSE(gPortsOrch->getVlanByVlanId(100, vlanPort));
            retryTunnel();
            EXPECT_FALSE(m_VxlanTunnelOrch->isTunnelExists("vtep1"));
            EXPECT_TRUE(m_VxlanTunnelOrch->m_heldDeletes.empty());
        }

        void vlanGoneBeforeMapDel()
        {
            nvoDel();
            const sai_object_id_t tunnel_id = vtep()->getTunnelId();
            tunnelDel();
            ASSERT_TRUE(tunnelHeld());

            // The map holds the VLAN; drop that hold to remove the VLAN first.
            Port vlanPort;
            ASSERT_TRUE(gPortsOrch->getVlanByVlanId(100, vlanPort));
            vlanPort.m_vnid = VNID_NONE;
            gPortsOrch->setPort(vlanPort.m_alias, vlanPort);
            vlanDel();
            ASSERT_FALSE(gPortsOrch->getVlanByVlanId(100, vlanPort));

            mapDel();
            EXPECT_FALSE(mapHeld());
            EXPECT_FALSE(vtep()->isActive());
            EXPECT_FALSE(saiTunnelExists(tunnel_id));
            retryTunnel();
            EXPECT_FALSE(m_VxlanTunnelOrch->isTunnelExists("vtep1"));
            EXPECT_TRUE(m_VxlanTunnelOrch->m_heldDeletes.empty());
        }

        // The NVO is configured again on the same source VTEP before orchagent sees the tunnel's DEL, so its held
        // DEL is dropped as stale. The tunnel DEL that follows waits while the remote VTEP uses the tunnel, then the
        // NVO lets go of it and binds the tunnel created with the new source IP, whether that SET is already queued
        // behind the tunnel DEL or comes after the old tunnel is gone.
        void nvoRecreatedBeforeTunnelDel(bool tunnel_set_queued)
        {
            setRemoteVni();
            nvoDel();
            ASSERT_TRUE(nvoHeld());
            nvoSet();
            ASSERT_FALSE(nvoHeld());
            ASSERT_EQ(m_nvoOrch->getEVPNVtep(), vtep());

            mapDel();
            ASSERT_TRUE(vtep()->del_tnl_hw_pending);
            tunnelDel();
            if (tunnel_set_queued)
            {
                tunnelSet("10.1.1.9");
            }
            retryTunnel();
            EXPECT_TRUE(tunnelHeld());
            EXPECT_EQ(m_nvoOrch->getEVPNVtep(), vtep());

            delRemoteVni();
            EXPECT_TRUE(remoteVniConsumer()->m_toSync.empty());
            EXPECT_FALSE(remoteVtepIsFloodMember());
            EXPECT_FALSE(vtep()->del_tnl_hw_pending);
            retryTunnel();
            EXPECT_FALSE(tunnelHeld());
            if (!tunnel_set_queued)
            {
                EXPECT_FALSE(m_VxlanTunnelOrch->isTunnelExists("vtep1"));
                EXPECT_EQ(m_nvoOrch->getEVPNVtep(), nullptr);
                EXPECT_TRUE(m_nvoOrch->isBindPending());
                tunnelSet("10.1.1.9");
            }
            ASSERT_TRUE(m_VxlanTunnelOrch->isTunnelExists("vtep1"));
            EXPECT_EQ(vtep()->getSrcIP(), IpAddress("10.1.1.9"));
            EXPECT_EQ(m_nvoOrch->getEVPNVtep(), vtep());
            EXPECT_FALSE(m_nvoOrch->isBindPending());
            EXPECT_FALSE(nvoHeld());
            EXPECT_TRUE(m_nvoOrch->m_heldDeletes.empty());
            EXPECT_TRUE(m_VxlanTunnelOrch->m_heldDeletes.empty());
            mapSet();
            EXPECT_FALSE(mapHeld());
            EXPECT_TRUE(vtep()->isActive());
        }

        // The tunnel DEL reaches orchagent while its map still carries the remote VTEP, so the DEL is held with no
        // hw delete pending. A VLAN or VRF map configured then waits for the tunnel configured next instead of
        // going on the old one.
        void mapWaitsForHeldTunnelDel(bool vrf_map)
        {
            Orch *orch = m_tunnelMapOrch;
            string table = APP_VXLAN_TUNNEL_MAP_TABLE_NAME;
            KeyOpFieldsValuesTuple mapKfv{m_newMapKey, SET_COMMAND, {{"vni", "2000"}, {"vlan", "Vlan200"}}};
            if (vrf_map)
            {
                m_vrfMapOrch = new VxlanVrfMapOrch(m_app_db.get(), APP_VXLAN_VRF_TABLE_NAME);
                gDirectory.set(m_vrfMapOrch);
                consumerOf(gVrfOrch, APP_VRF_TABLE_NAME)->addToSync({{"Vrf1", SET_COMMAND, {}}});
                static_cast<Orch *>(gVrfOrch)->doTask();
                ASSERT_TRUE(gVrfOrch->isVRFexists("Vrf1"));
                orch = m_vrfMapOrch;
                table = APP_VXLAN_VRF_TABLE_NAME;
                mapKfv = KeyOpFieldsValuesTuple{m_vrfMapKey, SET_COMMAND, {{"vni", "2000"}, {"vrf", "Vrf1"}}};
            }
            else
            {
                consumerOf(gPortsOrch, APP_VLAN_TABLE_NAME)
                    ->addToSync({{"Vlan200", SET_COMMAND, {{"admin_status", "up"}, {"mtu", "9100"}}}});
                static_cast<Orch *>(gPortsOrch)->doTask();
            }
            auto newMapHeld = [&]() { return !consumerOf(orch, table)->m_toSync.empty(); };

            setRemoteVni();
            tunnelDel();
            tunnelSet("10.1.1.9");
            ASSERT_TRUE(tunnelHeld());
            ASSERT_FALSE(vtep()->del_tnl_hw_pending);

            apply(orch, table, mapKfv);
            EXPECT_TRUE(newMapHeld());

            mapDel();
            delRemoteVni();
            retryTunnel();
            EXPECT_FALSE(tunnelHeld());
            ASSERT_TRUE(m_VxlanTunnelOrch->isTunnelExists("vtep1"));
            EXPECT_EQ(vtep()->getSrcIP(), IpAddress("10.1.1.9"));
            EXPECT_EQ(m_nvoOrch->getEVPNVtep(), vtep());

            orch->doTask();
            EXPECT_FALSE(newMapHeld());
            EXPECT_TRUE(vtep()->isActive());
        }

        // The last map DEL cannot remove the P2MP source tunnel's bridge port (the SAI refuses it): the hw delete
        // stays pending, so the held tunnel DEL waits, and the DEL's retry finishes it once the bridge port can go.
        void lastMapDelWithRefusedBridgePortLeavesHwDeletePending()
        {
            nvoDel();
            const sai_object_id_t tunnel_id = vtep()->getTunnelId();
            tunnelDel();
            ASSERT_TRUE(tunnelHeld());

            g_savedRemoveBridgePort = sai_bridge_api->remove_bridge_port;
            sai_bridge_api->remove_bridge_port = stubRemoveBridgePortInUse;
            mapDel();
            EXPECT_FALSE(mapHeld());
            EXPECT_EQ(vtep()->vlan_vrf_vni_count, 0u);
            EXPECT_TRUE(vtep()->del_tnl_hw_pending);
            EXPECT_TRUE(saiTunnelExists(tunnel_id));
            retryTunnel();
            EXPECT_TRUE(tunnelHeld());
            ASSERT_TRUE(m_VxlanTunnelOrch->isTunnelExists("vtep1"));

            sai_bridge_api->remove_bridge_port = g_savedRemoveBridgePort;
            g_savedRemoveBridgePort = nullptr;
            retryTunnel();
            EXPECT_FALSE(m_VxlanTunnelOrch->isTunnelExists("vtep1"));
            EXPECT_FALSE(saiTunnelExists(tunnel_id));
            Port tunnelPort;
            EXPECT_FALSE(m_VxlanTunnelOrch->getTunnelPort("10.1.1.1", tunnelPort, true));
            EXPECT_FALSE(m_VxlanTunnelOrch->isTunnelExists("vtep1"));
            EXPECT_TRUE(m_VxlanTunnelOrch->m_heldDeletes.empty());
        }

        // A VRF map on the VLAN map's VNI takes the VLAN map's entry over. When the VLAN map is removed first, the
        // VRF map DEL must not restore an entry for it: no map is left behind for that VLAN.
        void vrfMapDelDoesNotRestoreARemovedVlanMap()
        {
            m_vrfMapOrch = new VxlanVrfMapOrch(m_app_db.get(), APP_VXLAN_VRF_TABLE_NAME);
            gDirectory.set(m_vrfMapOrch);
            consumerOf(gVrfOrch, APP_VRF_TABLE_NAME)->addToSync({{"Vrf1", SET_COMMAND, {}}});
            static_cast<Orch *>(gVrfOrch)->doTask();
            ASSERT_TRUE(gVrfOrch->isVRFexists("Vrf1"));

            apply(m_vrfMapOrch, APP_VXLAN_VRF_TABLE_NAME,
                  {m_vrfMapKey, SET_COMMAND, {{"vni", "1000"}, {"vrf", "Vrf1"}}});
            ASSERT_TRUE(m_vrfMapOrch->isVrfMapExists(m_vrfMapKey));
            ASSERT_TRUE(m_tunnelMapOrch->isTunnelMapExists(m_mapKey));

            mapDel();
            ASSERT_FALSE(m_tunnelMapOrch->isTunnelMapExists(m_mapKey));

            apply(m_vrfMapOrch, APP_VXLAN_VRF_TABLE_NAME, {m_vrfMapKey, DEL_COMMAND, {}});
            EXPECT_FALSE(m_vrfMapOrch->isVrfMapExists(m_vrfMapKey));
            EXPECT_FALSE(m_tunnelMapOrch->isTunnelMapExists(m_mapKey));
            EXPECT_EQ(vtep()->vlan_vrf_vni_count, 0u);
        }

        // While the NVO waits for its source VTEP to be configured again, a remote VNI is kept rather than dropped,
        // and is added once the NVO binds the new tunnel.
        void remoteVniWaitsForNvoBind()
        {
            mapDel();
            tunnelDel();
            ASSERT_FALSE(m_VxlanTunnelOrch->isTunnelExists("vtep1"));
            ASSERT_TRUE(m_nvoOrch->isBindPending());

            setRemoteVni();
            EXPECT_EQ(remoteVniConsumer()->m_toSync.size(), 1u);

            tunnelSet("10.1.1.1");
            ASSERT_TRUE(m_VxlanTunnelOrch->isTunnelExists("vtep1"));
            EXPECT_EQ(m_nvoOrch->getEVPNVtep(), vtep());
            mapSet();
            retryRemoteVni();
            EXPECT_TRUE(remoteVniConsumer()->m_toSync.empty());
            EXPECT_TRUE(remoteVtepIsFloodMember());
        }

        // A tunnel row back with the same source IP but another dst_ip or ttl_mode is not the tunnel the held DEL
        // is for: the SET waits behind the DEL, and the tunnel is created again with the new row once the NVO lets
        // go of the old one.
        void heldTunnelDelKeptForOtherAttributes(const vector<FieldValueTuple> &row, const IpAddress &dst_ip,
                                                 VxlanTunnelTTLMode ttl_mode)
        {
            setRemoteVni();
            nvoDel();
            tunnelDel();
            apply(m_VxlanTunnelOrch, APP_VXLAN_TUNNEL_TABLE_NAME, {"vtep1", SET_COMMAND, row});
            retryTunnel();
            EXPECT_TRUE(tunnelHeld());
            EXPECT_TRUE(vtep()->hasConfig(IpAddress("10.1.1.1"), IpAddress("0.0.0.0"), VxlanTunnelTTLMode::NOT_SET));

            delRemoteVni();
            retryNvo();
            EXPECT_FALSE(nvoHeld());
            retryTunnel();
            EXPECT_TRUE(tunnelHeld());
            EXPECT_TRUE(vtep()->isActive());
            mapDel();
            retryTunnel();
            EXPECT_FALSE(tunnelHeld());
            ASSERT_TRUE(m_VxlanTunnelOrch->isTunnelExists("vtep1"));
            EXPECT_TRUE(vtep()->hasConfig(IpAddress("10.1.1.1"), dst_ip, ttl_mode));
            EXPECT_TRUE(m_VxlanTunnelOrch->m_heldDeletes.empty());
        }
    };

    class EvpnNvoDelP2mpTest : public EvpnNvoDelTest
    {
    protected:
        bool useDipTunnels() const override
        {
            return false;
        }
    };

    TEST_F(EvpnNvoDelTest, NvoDelBeforeLastMapWaitsForRemoteVniDel) { nvoBeforeLastMap(); }
    TEST_F(EvpnNvoDelP2mpTest, NvoDelBeforeLastMapWaitsForRemoteVniDel) { nvoBeforeLastMap(); }
    TEST_F(EvpnNvoDelTest, LastMapDelBeforeNvoWaitsForRemoteVniDel) { lastMapBeforeNvo(); }
    TEST_F(EvpnNvoDelP2mpTest, LastMapDelBeforeNvoWaitsForRemoteVniDel) { lastMapBeforeNvo(); }
    TEST_F(EvpnNvoDelTest, IdleVtepNvoFirstIsDeletedAtOnce) { idleVtep(true); }
    TEST_F(EvpnNvoDelP2mpTest, IdleVtepNvoFirstIsDeletedAtOnce) { idleVtep(true); }
    TEST_F(EvpnNvoDelTest, IdleVtepMapFirstIsDeletedAtOnce) { idleVtep(false); }
    TEST_F(EvpnNvoDelP2mpTest, IdleVtepMapFirstIsDeletedAtOnce) { idleVtep(false); }
    TEST_F(EvpnNvoDelTest, TunnelDeleteWaitsForTheNvo) { tunnelWaitsForNvo(); }
    TEST_F(EvpnNvoDelP2mpTest, TunnelDeleteWaitsForTheNvo) { tunnelWaitsForNvo(); }
    TEST_F(EvpnNvoDelTest, StaleDeleteDoesNotDropARecreatedNvo) { staleDelKeepsRecreatedNvo(); }
    TEST_F(EvpnNvoDelP2mpTest, StaleDeleteDoesNotDropARecreatedNvo) { staleDelKeepsRecreatedNvo(); }
    TEST_F(EvpnNvoDelTest, StaleTunnelDeleteDoesNotDropARecreatedTunnel) { staleDelKeepsRecreatedTunnel(); }
    TEST_F(EvpnNvoDelP2mpTest, StaleTunnelDeleteDoesNotDropARecreatedTunnel) { staleDelKeepsRecreatedTunnel(); }
    TEST_F(EvpnNvoDelTest, HeldTunnelDeleteKeptForOtherSourceIp) { heldTunnelDelKeptForOtherSourceIp(); }
    TEST_F(EvpnNvoDelP2mpTest, HeldTunnelDeleteKeptForOtherSourceIp) { heldTunnelDelKeptForOtherSourceIp(); }
    TEST_F(EvpnNvoDelTest, StaleTunnelDeleteDuringPendingHwDelete) { staleTunnelDelDuringPendingHwDelete(); }
    TEST_F(EvpnNvoDelP2mpTest, StaleTunnelDeleteDuringPendingHwDelete) { staleTunnelDelDuringPendingHwDelete(); }
    TEST_F(EvpnNvoDelTest, NvoRecreatedOnAnotherVtepWaits) { nvoRecreatedOnAnotherVtepWaits(); }
    TEST_F(EvpnNvoDelP2mpTest, NvoRecreatedOnAnotherVtepWaits) { nvoRecreatedOnAnotherVtepWaits(); }
    TEST_F(EvpnNvoDelTest, NvoRecreatedBehindHeldTunnelDelWithNewSourceIp) { nvoRecreatedBehindHeldTunnelDelWithNewSourceIp(); }
    TEST_F(EvpnNvoDelP2mpTest, NvoRecreatedBehindHeldTunnelDelWithNewSourceIp) { nvoRecreatedBehindHeldTunnelDelWithNewSourceIp(); }
    TEST_F(EvpnNvoDelTest, HeldDeleteKeepsRouterMac) { heldDelKeepsRouterMac(); }
    TEST_F(EvpnNvoDelP2mpTest, HeldDeleteKeepsRouterMac) { heldDelKeepsRouterMac(); }
    TEST_F(EvpnNvoDelTest, TunnelDeleteWaitsForItsMaps) { tunnelDelWaitsForItsMaps(); }
    TEST_F(EvpnNvoDelP2mpTest, TunnelDeleteWaitsForItsMaps) { tunnelDelWaitsForItsMaps(); }
    TEST_F(EvpnNvoDelTest, TunnelDeleteWithTheNvoWaitsForItsMaps) { tunnelDelWithNvoWaitsForItsMaps(); }
    TEST_F(EvpnNvoDelP2mpTest, TunnelDeleteWithTheNvoWaitsForItsMaps) { tunnelDelWithNvoWaitsForItsMaps(); }
    TEST_F(EvpnNvoDelTest, MergedMapSetReattachesToTheNewTunnel) { mergedMapSetReattachesToTheNewTunnel(true, "1000"); }
    TEST_F(EvpnNvoDelTest, MergedMapSetBeforeTunnelSetReattaches) { mergedMapSetReattachesToTheNewTunnel(false, "1000"); }
    TEST_F(EvpnNvoDelTest, MergedMapSetWithNewVniReattaches) { mergedMapSetReattachesToTheNewTunnel(true, "2000"); }
    TEST_F(EvpnNvoDelTest, MergedMapSetKeepsTheSameTunnel) { mergedMapSetKeepsTheSameTunnel(true); }
    TEST_F(EvpnNvoDelTest, MergedMapSetBeforeTunnelSetKeepsTheSameTunnel) { mergedMapSetKeepsTheSameTunnel(false); }
    TEST_F(EvpnNvoDelTest, MergedMapSetWithRefusedMapEntryRemove) { mergedMapSetWithRefusedMapEntryRemove(); }
    TEST_F(EvpnNvoDelP2mpTest, MergedMapSetReattachesToTheNewTunnel) { mergedMapSetReattachesToTheNewTunnel(true, "1000"); }
    TEST_F(EvpnNvoDelP2mpTest, MergedMapSetBeforeTunnelSetReattaches) { mergedMapSetReattachesToTheNewTunnel(false, "1000"); }
    TEST_F(EvpnNvoDelP2mpTest, MergedMapSetWithNewVniReattaches) { mergedMapSetReattachesToTheNewTunnel(true, "2000"); }
    TEST_F(EvpnNvoDelP2mpTest, MergedMapSetKeepsTheSameTunnel) { mergedMapSetKeepsTheSameTunnel(true); }
    TEST_F(EvpnNvoDelP2mpTest, MergedMapSetBeforeTunnelSetKeepsTheSameTunnel) { mergedMapSetKeepsTheSameTunnel(false); }
    TEST_F(EvpnNvoDelP2mpTest, MergedMapSetWithRefusedMapEntryRemove) { mergedMapSetWithRefusedMapEntryRemove(); }
    TEST_F(EvpnNvoDelTest, TunnelDeleteHeldForMapsDroppedOnRecreate) { heldForMapsDroppedOnRecreate(); }
    TEST_F(EvpnNvoDelP2mpTest, TunnelDeleteHeldForMapsDroppedOnRecreate) { heldForMapsDroppedOnRecreate(); }
    TEST_F(EvpnNvoDelTest, VlanRemovedBeforeMapDelStillReleasesTheTunnel) { vlanGoneBeforeMapDel(); }
    TEST_F(EvpnNvoDelP2mpTest, VlanRemovedBeforeMapDelStillReleasesTheTunnel) { vlanGoneBeforeMapDel(); }
    TEST_F(EvpnNvoDelTest, FailedMapEntryCreateKeepsMapCount)
    {
        failedMapEntryCreateKeepsMapCount(SAI_STATUS_FAILURE);
    }
    TEST_F(EvpnNvoDelP2mpTest, FailedMapEntryCreateKeepsMapCount)
    {
        failedMapEntryCreateKeepsMapCount(SAI_STATUS_FAILURE);
    }
    TEST_F(EvpnNvoDelTest, MapEntryCreateAlreadyExistsKeepsMapCount)
    {
        failedMapEntryCreateKeepsMapCount(SAI_STATUS_ITEM_ALREADY_EXISTS);
    }
    TEST_F(EvpnNvoDelP2mpTest, MapEntryCreateAlreadyExistsKeepsMapCount)
    {
        failedMapEntryCreateKeepsMapCount(SAI_STATUS_ITEM_ALREADY_EXISTS);
    }
    TEST_F(EvpnNvoDelTest, VlanDelWaitsForItsMap) { vlanDelWaitsForMap(); }
    TEST_F(EvpnNvoDelTest, VniChangeWaitsForTheOldMap) { vniChangeWaitsForTheOldMap(); }
    TEST_F(EvpnNvoDelP2mpTest, VniChangeWaitsForTheOldMap) { vniChangeWaitsForTheOldMap(); }
    TEST_F(EvpnNvoDelTest, RevertedVniChangeKeepsTheOldVni) { revertedVniChangeKeepsTheOldVni(); }
    TEST_F(EvpnNvoDelP2mpTest, RevertedVniChangeKeepsTheOldVni) { revertedVniChangeKeepsTheOldVni(); }
    TEST_F(EvpnNvoDelTest, RemapInOneBatch) { remapInOneBatch(); }
    TEST_F(EvpnNvoDelP2mpTest, RemapInOneBatch) { remapInOneBatch(); }
    TEST_F(EvpnNvoDelTest, SwapInOneBatch) { swapInOneBatch(); }
    TEST_F(EvpnNvoDelP2mpTest, SwapInOneBatch) { swapInOneBatch(); }
    TEST_F(EvpnNvoDelTest, FreeFormKeyReleasesItsVni) { freeFormKeyReleasesItsVni(); }
    TEST_F(EvpnNvoDelTest, PartlyRemovedVlanKeptOnRecreate) { partlyRemovedVlanKeptOnRecreate(); }
    TEST_F(EvpnNvoDelP2mpTest, PartlyRemovedVlanKeptOnRecreate) { partlyRemovedVlanKeptOnRecreate(); }
    TEST_F(EvpnNvoDelTest, PartlyRemovedVlanGoneOnRetry) { partlyRemovedVlanGoneOnRetry(); }
    TEST_F(EvpnNvoDelP2mpTest, PartlyRemovedVlanGoneOnRetry) { partlyRemovedVlanGoneOnRetry(); }
    TEST_F(EvpnNvoDelP2mpTest, VlanDelWaitsForItsMap) { vlanDelWaitsForMap(); }
    TEST_F(EvpnNvoDelTest, HeldVlanDelDroppedOnRecreate) { heldVlanDelDroppedOnRecreate(); }
    TEST_F(EvpnNvoDelP2mpTest, HeldVlanDelDroppedOnRecreate) { heldVlanDelDroppedOnRecreate(); }
    TEST_F(EvpnNvoDelTest, ParentFirstDeleteMergedWithRecreate) { parentFirstDeleteMergedWithRecreate(); }
    TEST_F(EvpnNvoDelP2mpTest, ParentFirstDeleteMergedWithRecreate) { parentFirstDeleteMergedWithRecreate(); }
    TEST_F(EvpnNvoDelTest, NvoRecreatedBeforeTunnelDelBindsTheNewTunnel) { nvoRecreatedBeforeTunnelDel(true); }
    TEST_F(EvpnNvoDelP2mpTest, NvoRecreatedBeforeTunnelDelBindsTheNewTunnel) { nvoRecreatedBeforeTunnelDel(true); }
    TEST_F(EvpnNvoDelTest, NvoRecreatedBeforeTunnelDelWaitsForTheNewTunnel) { nvoRecreatedBeforeTunnelDel(false); }
    TEST_F(EvpnNvoDelP2mpTest, NvoRecreatedBeforeTunnelDelWaitsForTheNewTunnel) { nvoRecreatedBeforeTunnelDel(false); }
    TEST_F(EvpnNvoDelTest, MapWaitsForHeldTunnelDelete) { mapWaitsForHeldTunnelDel(false); }
    TEST_F(EvpnNvoDelP2mpTest, MapWaitsForHeldTunnelDelete) { mapWaitsForHeldTunnelDel(false); }
    TEST_F(EvpnNvoDelTest, VrfMapWaitsForHeldTunnelDelete) { mapWaitsForHeldTunnelDel(true); }
    TEST_F(EvpnNvoDelP2mpTest, VrfMapWaitsForHeldTunnelDelete) { mapWaitsForHeldTunnelDel(true); }
    TEST_F(EvpnNvoDelP2mpTest, LastMapDelWithRefusedBridgePortLeavesHwDeletePending)
    {
        lastMapDelWithRefusedBridgePortLeavesHwDeletePending();
    }
    TEST_F(EvpnNvoDelTest, VrfMapDelDoesNotRestoreARemovedVlanMap) { vrfMapDelDoesNotRestoreARemovedVlanMap(); }
    TEST_F(EvpnNvoDelP2mpTest, VrfMapDelDoesNotRestoreARemovedVlanMap) { vrfMapDelDoesNotRestoreARemovedVlanMap(); }
    TEST_F(EvpnNvoDelTest, RemoteVniWaitsForTheNvoToBind) { remoteVniWaitsForNvoBind(); }
    TEST_F(EvpnNvoDelP2mpTest, RemoteVniWaitsForTheNvoToBind) { remoteVniWaitsForNvoBind(); }
    TEST_F(EvpnNvoDelTest, HeldTunnelDeleteKeptForOtherTtlMode)
    {
        heldTunnelDelKeptForOtherAttributes({{"src_ip", "10.1.1.1"}, {"ttl_mode", "pipe"}}, IpAddress("0.0.0.0"),
                                            VxlanTunnelTTLMode::PIPE);
    }
    TEST_F(EvpnNvoDelP2mpTest, HeldTunnelDeleteKeptForOtherTtlMode)
    {
        heldTunnelDelKeptForOtherAttributes({{"src_ip", "10.1.1.1"}, {"ttl_mode", "pipe"}}, IpAddress("0.0.0.0"),
                                            VxlanTunnelTTLMode::PIPE);
    }
    TEST_F(EvpnNvoDelTest, HeldTunnelDeleteKeptForOtherDstIp)
    {
        heldTunnelDelKeptForOtherAttributes({{"src_ip", "10.1.1.1"}, {"dst_ip", "10.1.1.20"}}, IpAddress("10.1.1.20"),
                                            VxlanTunnelTTLMode::NOT_SET);
    }
    TEST_F(EvpnNvoDelP2mpTest, HeldTunnelDeleteKeptForOtherDstIp)
    {
        heldTunnelDelKeptForOtherAttributes({{"src_ip", "10.1.1.1"}, {"dst_ip", "10.1.1.20"}}, IpAddress("10.1.1.20"),
                                            VxlanTunnelTTLMode::NOT_SET);
    }

    // P2MP: the remote VNI DEL that releases the last user cannot remove the source tunnel's
    // bridge port, so the hw delete stays pending with no user left. The NVO delete retries it
    // until the bridge port goes, then finishes the delete.
    TEST_F(EvpnNvoDelP2mpTest, PendingHwDeleteIsFinishedByTheNvoDelete)
    {
        setRemoteVni();
        mapDel();
        ASSERT_TRUE(vtep()->del_tnl_hw_pending);

        g_savedRemoveBridgePort = sai_bridge_api->remove_bridge_port;
        sai_bridge_api->remove_bridge_port = stubRemoveBridgePortInUse;
        delRemoteVni();
        EXPECT_TRUE(remoteVniConsumer()->m_toSync.empty());
        EXPECT_FALSE(remoteVtepIsFloodMember());
        EXPECT_TRUE(vtep()->del_tnl_hw_pending);

        // The refused removal is retried on an interval, not on every pass.
        g_removeBridgePortInUseCalls = 0;
        nvoDel();
        retryNvo();
        retryNvo();
        EXPECT_EQ(g_removeBridgePortInUseCalls, 1);
        expireBridgePortRetry();
        retryNvo();
        EXPECT_EQ(g_removeBridgePortInUseCalls, 2);
        EXPECT_TRUE(nvoHeld());
        EXPECT_TRUE(vtep()->del_tnl_hw_pending);
        EXPECT_EQ(m_nvoOrch->m_heldDeletes.size(), 1u);

        sai_bridge_api->remove_bridge_port = g_savedRemoveBridgePort;
        g_savedRemoveBridgePort = nullptr;
        expireBridgePortRetry();
        retryNvo();
        expectReleased();
        Port tunnelPort;
        EXPECT_FALSE(m_VxlanTunnelOrch->getTunnelPort("10.1.1.1", tunnelPort, true));
        expectRecreateWorks();
    }

    // P2MP: the NVO is configured again while its held DEL is still retrying a pending hw delete that the
    // bridge port blocks. The DEL keeps retrying the cleanup, and is dropped as stale only once it is done, so
    // the tunnel map that waits for it is created.
    TEST_F(EvpnNvoDelP2mpTest, PendingHwDeleteIsFinishedForARecreatedNvo)
    {
        setRemoteVni();
        mapDel();
        ASSERT_TRUE(vtep()->del_tnl_hw_pending);

        g_savedRemoveBridgePort = sai_bridge_api->remove_bridge_port;
        sai_bridge_api->remove_bridge_port = stubRemoveBridgePortInUse;
        delRemoteVni();
        EXPECT_TRUE(vtep()->del_tnl_hw_pending);

        nvoDel();
        EXPECT_TRUE(nvoHeld());
        nvoSet();
        EXPECT_TRUE(nvoHeld());
        EXPECT_EQ(m_nvoOrch->getEVPNVtep(), vtep());
        mapSet();
        EXPECT_TRUE(mapHeld());

        sai_bridge_api->remove_bridge_port = g_savedRemoveBridgePort;
        g_savedRemoveBridgePort = nullptr;
        expireBridgePortRetry();
        retryNvo();
        EXPECT_FALSE(nvoHeld());
        EXPECT_FALSE(vtep()->del_tnl_hw_pending);
        EXPECT_EQ(m_nvoOrch->getEVPNVtep(), vtep());
        EXPECT_TRUE(m_nvoOrch->m_heldDeletes.empty());

        static_cast<Orch *>(m_tunnelMapOrch)->doTask();
        EXPECT_FALSE(mapHeld());
        EXPECT_TRUE(vtep()->isActive());
        setRemoteVni();
        EXPECT_TRUE(remoteVniConsumer()->m_toSync.empty());
        EXPECT_TRUE(remoteVtepIsFloodMember());
    }

    // P2MP: a remote MAC on the source VTEP that comes while the NVO waits for that VTEP to be configured again
    // is kept rather than dropped, and is added once the NVO binds it.
    TEST_F(EvpnNvoDelP2mpTest, RemoteMacWaitsForTheNvoToBind)
    {
        mapDel();
        nvoDel();
        tunnelDel();
        ASSERT_FALSE(m_VxlanTunnelOrch->isTunnelExists("vtep1"));
        nvoSet();
        ASSERT_TRUE(nvoHeld());
        ASSERT_TRUE(m_nvoOrch->isBindPending());

        auto *fdbConsumer = consumerOf(gFdbOrch, APP_VXLAN_FDB_TABLE_NAME);
        fdbConsumer->addToSync(
            {{"Vlan100:00:11:22:33:44:55", SET_COMMAND, {{"remote_vtep", m_remoteVtep}, {"type", "dynamic"}, {"vni", "1000"}}}});
        static_cast<Orch *>(gFdbOrch)->doTask();
        EXPECT_EQ(fdbConsumer->m_toSync.size(), 1u);

        tunnelSet("10.1.1.1");
        EXPECT_EQ(m_nvoOrch->getEVPNVtep(), vtep());
        retryNvo();
        EXPECT_FALSE(nvoHeld());
        mapSet();
        setRemoteVni();
        ASSERT_TRUE(remoteVtepIsFloodMember());

        static_cast<Orch *>(gFdbOrch)->doTask();
        EXPECT_TRUE(fdbConsumer->m_toSync.empty());
        Port vlanPort;
        ASSERT_TRUE(gPortsOrch->getVlanByVlanId(100, vlanPort));
        EXPECT_EQ(gFdbOrch->m_entries.count({MacAddress("00:11:22:33:44:55"), vlanPort.m_vlan_info.vlan_oid, ""}), 1u);
    }

    // P2MP, NVO DEL first: the remote VNI DEL cannot remove the bridge port, and FdbOrch's
    // notification that the tunnel port has no MAC left finishes the delete through the NVO's VTEP.
    TEST_F(EvpnNvoDelP2mpTest, NvoDelBeforeLastMapWaitsForTunnelPortRelease)
    {
        setRemoteVni();
        nvoDel();
        mapDel();
        ASSERT_TRUE(vtep()->del_tnl_hw_pending);

        Port tunnelPort;
        ASSERT_TRUE(m_VxlanTunnelOrch->getTunnelPort("10.1.1.1", tunnelPort, true));
        g_savedRemoveBridgePort = sai_bridge_api->remove_bridge_port;
        sai_bridge_api->remove_bridge_port = stubRemoveBridgePortInUse;
        delRemoteVni();
        EXPECT_TRUE(vtep()->del_tnl_hw_pending);
        sai_bridge_api->remove_bridge_port = g_savedRemoveBridgePort;
        g_savedRemoveBridgePort = nullptr;

        ASSERT_TRUE(m_VxlanTunnelOrch->getTunnelPort("10.1.1.1", tunnelPort, true));
        ASSERT_EQ(tunnelPort.m_fdb_count, 0u);
        gFdbOrch->notifyTunnelOrch(tunnelPort);
        EXPECT_FALSE(vtep()->del_tnl_hw_pending);
        retryNvo();
        expectReleased();
    }

>>>>>>> 791ab704 (NOS-22828: [stporch] read the default STP instance on its own (#1855))
} // namespace vxlanorch_test
