#define private public // make Directory::m_values available to clean it.
#include "directory.h"
#undef private
#define protected public
#include "orch.h"
#undef protected
#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_table.h"
#include "mock_response_publisher.h"
#include "dash/dashresulthelper.h"
#include "saihelper.h"
#include <fstream>
#include <map>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

extern bool gSyncMode;
extern sai_redis_communication_mode_t gRedisCommunicationMode;
extern std::map<std::string, std::string> gProfileMap;

namespace saihelper_test
{
    using namespace std;
    using ::testing::_;
    using ::testing::Throw;
    using namespace testing_db;

    sai_switch_api_t ut_sai_switch_api;
    sai_switch_api_t *old_sai_switch_api;

    shared_ptr<swss::DBConnector> m_app_db;
    shared_ptr<swss::DBConnector> m_config_db;
    shared_ptr<swss::DBConnector> m_state_db;

    bool set_comm_mode_not_supported;
    bool comm_mode_was_set;
    int32_t comm_mode_set_value;
    bool use_pipeline_was_set;
    bool use_pipeline_not_supported;
    bool record_output_dir_failure;
    bool record_filename_failure;
    bool record_failure;
    bool syncd_dump_failure;
    bool response_timeout_failure;
    uint32_t *_sai_syncd_notifications_count;
    int32_t *_sai_syncd_notification_event;

    sai_status_t _ut_stub_sai_set_switch_attribute(
        _In_ sai_object_id_t switch_id,
        _In_ const sai_attribute_t *attr)
    {
        switch (attr[0].id)
        {
            case SAI_REDIS_SWITCH_ATTR_REDIS_COMMUNICATION_MODE:
                comm_mode_was_set = true;
                comm_mode_set_value = attr[0].value.s32;
                if (set_comm_mode_not_supported)
                {
                    return SAI_STATUS_NOT_SUPPORTED;
                }
                break;
            case SAI_REDIS_SWITCH_ATTR_USE_PIPELINE:
                use_pipeline_was_set = true;
                if (use_pipeline_not_supported)
                {
                    return SAI_STATUS_NOT_SUPPORTED;
                }
                break;
            case SAI_REDIS_SWITCH_ATTR_RECORDING_OUTPUT_DIR:
                if (record_output_dir_failure)
                {
                    return SAI_STATUS_FAILURE;
                }
                break;
            case SAI_REDIS_SWITCH_ATTR_RECORDING_FILENAME:
                if (record_filename_failure)
                {
                    return SAI_STATUS_FAILURE;
                }
                break;
            case SAI_REDIS_SWITCH_ATTR_RECORD:
                if (record_failure)
                {
                    return SAI_STATUS_FAILURE;
                }
                break;
            case SAI_REDIS_SWITCH_ATTR_SYNC_OPERATION_RESPONSE_TIMEOUT:
                if (response_timeout_failure)
                {
                    return SAI_STATUS_FAILURE;
                }
                break;
            case SAI_REDIS_SWITCH_ATTR_NOTIFY_SYNCD:
                if (syncd_dump_failure)
                {
                    return SAI_STATUS_FAILURE;
                }
                *_sai_syncd_notifications_count = *_sai_syncd_notifications_count + 1;
                *_sai_syncd_notification_event = attr[0].value.s32;
                break;
            default:
                break;
        }
        return SAI_STATUS_SUCCESS;
    }

    void _hook_sai_apis()
    {
        ut_sai_switch_api = *sai_switch_api;
        old_sai_switch_api = sai_switch_api;
        ut_sai_switch_api.set_switch_attribute = _ut_stub_sai_set_switch_attribute;
        sai_switch_api = &ut_sai_switch_api;
    }

    void _unhook_sai_apis()
    {
        sai_switch_api = old_sai_switch_api;
    }

    class MockDBTable : public Table {
        public:
            MockDBTable(swss::DBConnector* db, const std::string& tableName) : Table(db, tableName) {}

            MOCK_METHOD(void, set, (const std::string &key, const std::vector<FieldValueTuple> &values, const std::string &op, const std::string &prefix), (override));
            MOCK_METHOD(void, del, (const std::string &key, const std::string &op, const std::string &prefix), (override));
    };

    class SaihelperTest : public ::testing::Test
    {
        public:

            SaihelperTest()
            {
            };

            ~SaihelperTest()
            {
            };

            void SetUp() override
            {
                // Init switch and create dependencies
                m_app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
                m_config_db = make_shared<swss::DBConnector>("CONFIG_DB", 0);
                m_state_db = make_shared<swss::DBConnector>("STATE_DB", 0);

                set_comm_mode_not_supported = false;
                comm_mode_was_set = false;
                comm_mode_set_value = -1;
                use_pipeline_was_set = false;
                use_pipeline_not_supported = false;
                record_output_dir_failure = false;
                record_filename_failure = false;
                record_failure = false;
                syncd_dump_failure = false;
                response_timeout_failure = false;

                map<string, string> profile = {
                    { "SAI_VS_SWITCH_TYPE", "SAI_VS_SWITCH_TYPE_BCM56850" },
                    { "KV_DEVICE_MAC_ADDRESS", "20:03:04:05:06:00" }
                };

                ut_helper::initSaiApi(profile);

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
            }

            void TearDown() override
            {
                ::testing_db::reset();

                gDirectory.m_values.clear();

                delete gSwitchOrch;
                gSwitchOrch = nullptr;

                ut_helper::uninitSaiApi();
            }
    };

    TEST_F(SaihelperTest, TestSetCommunicationModeFailure) {
        set_comm_mode_not_supported = true;
        _hook_sai_apis();
        initSwitchOrch();

        _sai_syncd_notifications_count = (uint32_t*)mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        _sai_syncd_notification_event = (int32_t*)mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        *_sai_syncd_notifications_count = 0;

        ASSERT_DEATH({initSaiRedis();}, "");

        set_comm_mode_not_supported = false;
        _unhook_sai_apis();
    }

    TEST_F(SaihelperTest, TestSetRedisPipelineFailure) {
        use_pipeline_not_supported = true;
        _hook_sai_apis();
        initSwitchOrch();

        _sai_syncd_notifications_count = (uint32_t*)mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        _sai_syncd_notification_event = (int32_t*)mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        *_sai_syncd_notifications_count = 0;

        ASSERT_DEATH({initSaiRedis();}, "");

        use_pipeline_not_supported = false;
        _unhook_sai_apis();
    }

    TEST_F(SaihelperTest, TestSetRecordingOutputDirFailure) {
        record_output_dir_failure = true;
        _hook_sai_apis();
        initSwitchOrch();

        _sai_syncd_notifications_count = (uint32_t*)mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        _sai_syncd_notification_event = (int32_t*)mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        *_sai_syncd_notifications_count = 0;

        ASSERT_DEATH({initSaiRedis();}, "");

        record_output_dir_failure = false;
        _unhook_sai_apis();
    }

    TEST_F(SaihelperTest, TestSetRecordingFilenameFailure) {
        record_filename_failure = true;
        _hook_sai_apis();
        initSwitchOrch();

        _sai_syncd_notifications_count = (uint32_t*)mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        _sai_syncd_notification_event = (int32_t*)mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        *_sai_syncd_notifications_count = 0;

        ASSERT_DEATH({initSaiRedis();}, "");

        record_filename_failure = false;
        _unhook_sai_apis();
    }

    TEST_F(SaihelperTest, TestSetRecordFailure) {
        record_failure = true;
        _hook_sai_apis();
        initSwitchOrch();

        _sai_syncd_notifications_count = (uint32_t*)mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        _sai_syncd_notification_event = (int32_t*)mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        *_sai_syncd_notifications_count = 0;

        ASSERT_DEATH({initSaiRedis();}, "");

        record_failure = false;
        _unhook_sai_apis();
    }

    TEST_F(SaihelperTest, TestSetResponseTimeoutFailure) {
        response_timeout_failure = true;
        _hook_sai_apis();
        initSwitchOrch();

        _sai_syncd_notifications_count = (uint32_t*)mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        _sai_syncd_notification_event = (int32_t*)mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        *_sai_syncd_notifications_count = 0;
        (void) setenv("platform", "mellanox", 1);

        ASSERT_DEATH({initSaiRedis();}, "");

        response_timeout_failure = false;
        (void) unsetenv("platform");
        _unhook_sai_apis();
    }

    // -s still rewrites a Redis mode and a plain -z zmq_sync when the context
    // file did not pin ZMQ. A pin leaves ZMQ_SYNC alone, including the
    // -s -z zmq_sync combination where the mode was already ZMQ. The
    // deprecation warning is logged on every -s path, including that one.
    TEST_F(SaihelperTest, SyncModeFlagDoesNotOverrideZmqPin)
    {
        _sai_syncd_notifications_count = (uint32_t*)mmap(NULL, sizeof(uint32_t), PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        _sai_syncd_notification_event = (int32_t*)mmap(NULL, sizeof(int32_t), PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        ASSERT_NE(static_cast<void *>(_sai_syncd_notifications_count), MAP_FAILED);
        ASSERT_NE(static_cast<void *>(_sai_syncd_notification_event), MAP_FAILED);
        *_sai_syncd_notifications_count = 0;

        _hook_sai_apis();

        const bool savedSyncMode = gSyncMode;
        const bool savedPinned = gContextConfigPinsZmq;
        const sai_redis_communication_mode_t savedMode = gRedisCommunicationMode;

        gSyncMode = true;

        // File pin, mode already ZMQ_SYNC (-s -z zmq_sync): -s must not rewrite.
        comm_mode_was_set = false;
        use_pipeline_was_set = false;
        gContextConfigPinsZmq = true;
        gRedisCommunicationMode = SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC;
        initSaiRedis();
        EXPECT_EQ(gRedisCommunicationMode, SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC);
        EXPECT_TRUE(comm_mode_was_set);
        EXPECT_EQ(comm_mode_set_value, SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC);
        EXPECT_FALSE(use_pipeline_was_set);

        // No pin: -s still beats a command-line -z zmq_sync.
        comm_mode_was_set = false;
        gContextConfigPinsZmq = false;
        gRedisCommunicationMode = SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC;
        initSaiRedis();
        EXPECT_EQ(gRedisCommunicationMode, SAI_REDIS_COMMUNICATION_MODE_REDIS_SYNC);
        EXPECT_TRUE(comm_mode_was_set);
        EXPECT_EQ(comm_mode_set_value, SAI_REDIS_COMMUNICATION_MODE_REDIS_SYNC);

        comm_mode_was_set = false;
        gContextConfigPinsZmq = false;
        gRedisCommunicationMode = SAI_REDIS_COMMUNICATION_MODE_REDIS_ASYNC;
        initSaiRedis();
        EXPECT_EQ(gRedisCommunicationMode, SAI_REDIS_COMMUNICATION_MODE_REDIS_SYNC);
        EXPECT_TRUE(comm_mode_was_set);
        EXPECT_EQ(comm_mode_set_value, SAI_REDIS_COMMUNICATION_MODE_REDIS_SYNC);

        gSyncMode = savedSyncMode;
        gContextConfigPinsZmq = savedPinned;
        gRedisCommunicationMode = savedMode;
        _unhook_sai_apis();
        munmap(_sai_syncd_notifications_count, sizeof(uint32_t));
        munmap(_sai_syncd_notification_event, sizeof(int32_t));
        _sai_syncd_notifications_count = nullptr;
        _sai_syncd_notification_event = nullptr;
    }

    // NOS-23003: swss_zmq is off, so orchagent starts in Redis, and -s is set.
    // The context file pins guid 0 to ZMQ. The file read must promote the mode
    // and set the pin, and initSaiRedis must send ZMQ_SYNC rather than let -s
    // rewrite it.
    TEST_F(SaihelperTest, ContextConfigPinSurvivesSyncMode)
    {
        _sai_syncd_notifications_count = (uint32_t*)mmap(NULL, sizeof(uint32_t), PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        _sai_syncd_notification_event = (int32_t*)mmap(NULL, sizeof(int32_t), PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        ASSERT_NE(static_cast<void *>(_sai_syncd_notifications_count), MAP_FAILED);
        ASSERT_NE(static_cast<void *>(_sai_syncd_notification_event), MAP_FAILED);
        *_sai_syncd_notifications_count = 0;

        char path[] = "/tmp/swss-context-config-XXXXXX";
        int fd = mkstemp(path);
        ASSERT_GE(fd, 0);
        ASSERT_EQ(close(fd), 0);
        {
            std::ofstream out(path);
            out << R"({"CONTEXTS":[{"guid":0,"name":"syncd","dbAsic":"ASIC_DB","dbCounters":"COUNTERS_DB","dbFlex":"FLEX_COUNTER_DB","dbState":"STATE_DB","switches":[{"index":0,"hwinfo":""}],"zmq_enable":true}]})";
            if (!out)
            {
                unlink(path);
                FAIL() << "failed to write " << path;
            }
        }

        _hook_sai_apis();

        const bool savedSyncMode = gSyncMode;
        const bool savedPinned = gContextConfigPinsZmq;
        const sai_redis_communication_mode_t savedMode = gRedisCommunicationMode;

        gSyncMode = true;
        gContextConfigPinsZmq = false;
        gRedisCommunicationMode = SAI_REDIS_COMMUNICATION_MODE_REDIS_ASYNC;
        comm_mode_was_set = false;
        use_pipeline_was_set = false;

        applyContextConfigCommunicationMode(path);
        gProfileMap.erase(SAI_REDIS_KEY_CONTEXT_CONFIG);
        EXPECT_TRUE(gContextConfigPinsZmq);
        EXPECT_EQ(gRedisCommunicationMode, SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC);

        initSaiRedis();
        EXPECT_EQ(gRedisCommunicationMode, SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC);
        EXPECT_TRUE(comm_mode_was_set);
        EXPECT_EQ(comm_mode_set_value, SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC);
        EXPECT_FALSE(use_pipeline_was_set);

        gSyncMode = savedSyncMode;
        gContextConfigPinsZmq = savedPinned;
        gRedisCommunicationMode = savedMode;
        _unhook_sai_apis();
        munmap(_sai_syncd_notifications_count, sizeof(uint32_t));
        munmap(_sai_syncd_notification_event, sizeof(int32_t));
        _sai_syncd_notifications_count = nullptr;
        _sai_syncd_notification_event = nullptr;
        unlink(path);
    }

    TEST_F(SaihelperTest, TestCreateFailure) {
        _hook_sai_apis();
        initSwitchOrch();

        _sai_syncd_notifications_count = (uint32_t*)mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        _sai_syncd_notification_event = (int32_t*)mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        *_sai_syncd_notifications_count = 0;
        uint32_t notif_count = *_sai_syncd_notifications_count;
        task_process_status status;

        status = handleSaiCreateStatus(SAI_API_ROUTE, SAI_STATUS_FAILURE);
        ASSERT_EQ(*_sai_syncd_notifications_count, ++notif_count);
        ASSERT_EQ(*_sai_syncd_notification_event, SAI_REDIS_NOTIFY_SYNCD_INVOKE_DUMP);
        ASSERT_EQ(status, task_failed);

        status = handleSaiCreateStatus(SAI_API_PORT, SAI_STATUS_FAILURE);
        ASSERT_EQ(*_sai_syncd_notifications_count, ++notif_count);
        ASSERT_EQ(*_sai_syncd_notification_event, SAI_REDIS_NOTIFY_SYNCD_INVOKE_DUMP);
        ASSERT_EQ(status, task_failed);

        status = handleSaiCreateStatus(SAI_API_TUNNEL, SAI_STATUS_NOT_SUPPORTED);
        ASSERT_EQ(*_sai_syncd_notifications_count, ++notif_count);
        ASSERT_EQ(*_sai_syncd_notification_event, SAI_REDIS_NOTIFY_SYNCD_INVOKE_DUMP);
        ASSERT_EQ(status, task_failed);

        status = handleSaiCreateStatus(SAI_API_TUNNEL, SAI_STATUS_NOT_IMPLEMENTED);
        ASSERT_EQ(*_sai_syncd_notifications_count, ++notif_count);
        ASSERT_EQ(*_sai_syncd_notification_event, SAI_REDIS_NOTIFY_SYNCD_INVOKE_DUMP);
        ASSERT_EQ(status, task_failed);

        status = handleSaiCreateStatus(SAI_API_NEXT_HOP_GROUP, SAI_STATUS_INVALID_PARAMETER);
        ASSERT_EQ(*_sai_syncd_notifications_count, ++notif_count);
        ASSERT_EQ(*_sai_syncd_notification_event, SAI_REDIS_NOTIFY_SYNCD_INVOKE_DUMP);
        ASSERT_EQ(status, task_failed);

        status = handleSaiCreateStatus(SAI_API_SWITCH, SAI_STATUS_UNINITIALIZED);
        ASSERT_EQ(*_sai_syncd_notifications_count, ++notif_count);
        ASSERT_EQ(*_sai_syncd_notification_event, SAI_REDIS_NOTIFY_SYNCD_INVOKE_DUMP);
        ASSERT_EQ(status, task_failed);

        status = handleSaiCreateStatus((sai_api_t) SAI_API_DASH_OUTBOUND_ROUTING, SAI_STATUS_INVALID_OBJECT_ID);
        ASSERT_EQ(*_sai_syncd_notifications_count, ++notif_count);
        ASSERT_EQ(*_sai_syncd_notification_event, SAI_REDIS_NOTIFY_SYNCD_INVOKE_DUMP);
        ASSERT_EQ(status, task_failed);

        status = handleSaiCreateStatus(SAI_API_ACL, SAI_STATUS_MANDATORY_ATTRIBUTE_MISSING);
        ASSERT_EQ(*_sai_syncd_notifications_count, ++notif_count);
        ASSERT_EQ(*_sai_syncd_notification_event, SAI_REDIS_NOTIFY_SYNCD_INVOKE_DUMP);
        ASSERT_EQ(status, task_failed);

        status = handleSaiCreateStatus(SAI_API_QOS_MAP, SAI_STATUS_INVALID_ATTR_VALUE_MAX);
        ASSERT_EQ(*_sai_syncd_notifications_count, ++notif_count);
        ASSERT_EQ(*_sai_syncd_notification_event, SAI_REDIS_NOTIFY_SYNCD_INVOKE_DUMP);
        ASSERT_EQ(status, task_failed);

        status = handleSaiCreateStatus(SAI_API_TUNNEL, SAI_STATUS_ATTR_NOT_IMPLEMENTED_6);
        ASSERT_EQ(*_sai_syncd_notifications_count, ++notif_count);
        ASSERT_EQ(*_sai_syncd_notification_event, SAI_REDIS_NOTIFY_SYNCD_INVOKE_DUMP);
        ASSERT_EQ(status, task_failed);

        status = handleSaiCreateStatus(SAI_API_ROUTER_INTERFACE, SAI_STATUS_UNKNOWN_ATTRIBUTE_0);
        ASSERT_EQ(*_sai_syncd_notifications_count, ++notif_count);
        ASSERT_EQ(*_sai_syncd_notification_event, SAI_REDIS_NOTIFY_SYNCD_INVOKE_DUMP);
        ASSERT_EQ(status, task_failed);

        status = handleSaiCreateStatus(SAI_API_ROUTER_INTERFACE, SAI_STATUS_ATTR_NOT_SUPPORTED_0);
        ASSERT_EQ(*_sai_syncd_notifications_count, ++notif_count);
        ASSERT_EQ(*_sai_syncd_notification_event, SAI_REDIS_NOTIFY_SYNCD_INVOKE_DUMP);
        ASSERT_EQ(status, task_failed);

        status = handleSaiCreateStatus(SAI_API_LAG, SAI_STATUS_INVALID_PORT_NUMBER);
        ASSERT_EQ(*_sai_syncd_notifications_count, ++notif_count);
        ASSERT_EQ(*_sai_syncd_notification_event, SAI_REDIS_NOTIFY_SYNCD_INVOKE_DUMP);
        ASSERT_EQ(status, task_failed);

        _unhook_sai_apis();
    }

    TEST_F(SaihelperTest, TestSetFailure) {
        _hook_sai_apis();
        initSwitchOrch();

        _sai_syncd_notifications_count = (uint32_t*)mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        _sai_syncd_notification_event = (int32_t*)mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        *_sai_syncd_notifications_count = 0;
        uint32_t notif_count = *_sai_syncd_notifications_count;
        task_process_status status;

        status = handleSaiSetStatus(SAI_API_ROUTE, SAI_STATUS_FAILURE);
        ASSERT_EQ(*_sai_syncd_notifications_count, ++notif_count);
        ASSERT_EQ(*_sai_syncd_notification_event, SAI_REDIS_NOTIFY_SYNCD_INVOKE_DUMP);
        ASSERT_EQ(status, task_failed);

        status = handleSaiSetStatus(SAI_API_ROUTE, SAI_STATUS_NOT_EXECUTED);
        ASSERT_EQ(*_sai_syncd_notifications_count, ++notif_count);
        ASSERT_EQ(*_sai_syncd_notification_event, SAI_REDIS_NOTIFY_SYNCD_INVOKE_DUMP);
        ASSERT_EQ(status, task_failed);

        status = handleSaiSetStatus(SAI_API_PORT, SAI_STATUS_FAILURE);
        ASSERT_EQ(*_sai_syncd_notifications_count, ++notif_count);
        ASSERT_EQ(*_sai_syncd_notification_event, SAI_REDIS_NOTIFY_SYNCD_INVOKE_DUMP);
        ASSERT_EQ(status, task_failed);

        status = handleSaiSetStatus(SAI_API_TUNNEL, SAI_STATUS_NOT_IMPLEMENTED);
        ASSERT_EQ(*_sai_syncd_notifications_count, ++notif_count);
        ASSERT_EQ(*_sai_syncd_notification_event, SAI_REDIS_NOTIFY_SYNCD_INVOKE_DUMP);
        ASSERT_EQ(status, task_failed);

        status = handleSaiSetStatus(SAI_API_HOSTIF, SAI_STATUS_INVALID_PARAMETER);
        ASSERT_EQ(*_sai_syncd_notifications_count, ++notif_count);
        ASSERT_EQ(*_sai_syncd_notification_event, SAI_REDIS_NOTIFY_SYNCD_INVOKE_DUMP);
        ASSERT_EQ(status, task_failed);

        status = handleSaiSetStatus(SAI_API_PORT, SAI_STATUS_ATTR_NOT_SUPPORTED_0);
        ASSERT_EQ(*_sai_syncd_notifications_count, ++notif_count);
        ASSERT_EQ(*_sai_syncd_notification_event, SAI_REDIS_NOTIFY_SYNCD_INVOKE_DUMP);
        ASSERT_EQ(status, task_failed);

        status = handleSaiSetStatus(SAI_API_LAG, SAI_STATUS_INVALID_PORT_NUMBER);
        ASSERT_EQ(*_sai_syncd_notifications_count, ++notif_count);
        ASSERT_EQ(*_sai_syncd_notification_event, SAI_REDIS_NOTIFY_SYNCD_INVOKE_DUMP);
        ASSERT_EQ(status, task_failed);

        _unhook_sai_apis();
    }

    TEST_F(SaihelperTest, TestGetFailure) {
        _hook_sai_apis();
        initSwitchOrch();

        _sai_syncd_notifications_count = (uint32_t*)mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        *_sai_syncd_notifications_count = 0;
        task_process_status status;

        status = handleSaiGetStatus(SAI_API_FDB, SAI_STATUS_INVALID_PARAMETER);
        ASSERT_EQ(*_sai_syncd_notifications_count, 0);
        ASSERT_EQ(status, task_failed);
        _unhook_sai_apis();
    }

    TEST_F(SaihelperTest, TestAllSuccess) {
        _hook_sai_apis();
        initSwitchOrch();

        _sai_syncd_notifications_count = (uint32_t*)mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        *_sai_syncd_notifications_count = 0;
        task_process_status status;

        status = handleSaiCreateStatus(SAI_API_NEXT_HOP_GROUP, SAI_STATUS_SUCCESS);
        ASSERT_EQ(*_sai_syncd_notifications_count, 0);
        ASSERT_EQ(status, task_success);

        status = handleSaiSetStatus(SAI_API_ROUTE, SAI_STATUS_SUCCESS);
        ASSERT_EQ(*_sai_syncd_notifications_count, 0);
        ASSERT_EQ(status, task_success);

        status = handleSaiRemoveStatus(SAI_API_NEXT_HOP, SAI_STATUS_SUCCESS);
        ASSERT_EQ(*_sai_syncd_notifications_count, 0);
        ASSERT_EQ(status, task_success);

        status = handleSaiCreateStatus(SAI_API_VLAN, SAI_STATUS_ITEM_ALREADY_EXISTS);
        ASSERT_EQ(*_sai_syncd_notifications_count, 0);
        ASSERT_EQ(status, task_success);

        status = handleSaiSetStatus(SAI_API_ROUTE, SAI_STATUS_ITEM_ALREADY_EXISTS);
        ASSERT_EQ(*_sai_syncd_notifications_count, 0);
        ASSERT_EQ(status, task_success);

        status = handleSaiCreateStatus(SAI_API_MIRROR, SAI_STATUS_ITEM_NOT_FOUND);
        ASSERT_EQ(*_sai_syncd_notifications_count, 0);
        ASSERT_EQ(status, task_success);

        status = handleSaiSetStatus(SAI_API_NEIGHBOR, SAI_STATUS_ITEM_NOT_FOUND);
        ASSERT_EQ(*_sai_syncd_notifications_count, 0);
        ASSERT_EQ(status, task_success);

        status = handleSaiSetStatus(SAI_API_NEXT_HOP, SAI_STATUS_OBJECT_IN_USE);
        ASSERT_EQ(*_sai_syncd_notifications_count, 0);
        ASSERT_EQ(status, task_success);

        status = handleSaiRemoveStatus(SAI_API_LAG, SAI_STATUS_ITEM_NOT_FOUND);
        ASSERT_EQ(*_sai_syncd_notifications_count, 0);
        ASSERT_EQ(status, task_success);

        status = handleSaiRemoveStatus(SAI_API_PORT, SAI_STATUS_ITEM_ALREADY_EXISTS);
        ASSERT_EQ(*_sai_syncd_notifications_count, 0);
        ASSERT_EQ(status, task_success);

        status = handleSaiRemoveStatus((sai_api_t) SAI_API_DASH_OUTBOUND_ROUTING, SAI_STATUS_ITEM_NOT_FOUND);
        ASSERT_EQ(*_sai_syncd_notifications_count, 0);
        ASSERT_EQ(status, task_success);

        status = handleSaiRemoveStatus((sai_api_t) SAI_API_DASH_INBOUND_ROUTING, SAI_STATUS_ITEM_NOT_FOUND);
        ASSERT_EQ(*_sai_syncd_notifications_count, 0);
        ASSERT_EQ(status, task_success);

        _unhook_sai_apis();
    }

    TEST_F(SaihelperTest, TestCreateSetResourceFailure) {
        _hook_sai_apis();
        initSwitchOrch();

        _sai_syncd_notifications_count = (uint32_t*)mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        *_sai_syncd_notifications_count = 0;
        task_process_status status;

        status = handleSaiCreateStatus(SAI_API_ACL, SAI_STATUS_INSUFFICIENT_RESOURCES);
        ASSERT_EQ(*_sai_syncd_notifications_count, 0);
        ASSERT_EQ(status, task_need_retry);

        status = handleSaiSetStatus(SAI_API_PORT, SAI_STATUS_INSUFFICIENT_RESOURCES);
        ASSERT_EQ(*_sai_syncd_notifications_count, 0);
        ASSERT_EQ(status, task_need_retry);

        status = handleSaiCreateStatus(SAI_API_TUNNEL, SAI_STATUS_TABLE_FULL);
        ASSERT_EQ(*_sai_syncd_notifications_count, 0);
        ASSERT_EQ(status, task_need_retry);

        status = handleSaiSetStatus(SAI_API_ROUTER_INTERFACE, SAI_STATUS_TABLE_FULL);
        ASSERT_EQ(*_sai_syncd_notifications_count, 0);
        ASSERT_EQ(status, task_need_retry);

        status = handleSaiCreateStatus(SAI_API_NEXT_HOP_GROUP, SAI_STATUS_NO_MEMORY);
        ASSERT_EQ(*_sai_syncd_notifications_count, 0);
        ASSERT_EQ(status, task_need_retry);

        status = handleSaiSetStatus(SAI_API_NEXT_HOP_GROUP, SAI_STATUS_NO_MEMORY);
        ASSERT_EQ(*_sai_syncd_notifications_count, 0);
        ASSERT_EQ(status, task_need_retry);

        _unhook_sai_apis();
    }

    TEST_F(SaihelperTest, TestRemoveObjectInUse) {
        _hook_sai_apis();
        initSwitchOrch();

        _sai_syncd_notifications_count = (uint32_t*)mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        *_sai_syncd_notifications_count = 0;
        task_process_status status;

        status = handleSaiRemoveStatus(SAI_API_NEXT_HOP_GROUP, SAI_STATUS_OBJECT_IN_USE);
        ASSERT_EQ(*_sai_syncd_notifications_count, 0);
        ASSERT_EQ(status, task_need_retry);

        _unhook_sai_apis();
    }

    TEST(WriteToDBTest, SetThrowsException) {
        auto db = std::make_shared<swss::DBConnector>("DPU_APPL_STATE_DB", 0);
        std::unique_ptr<swss::Table> mockTable = std::make_unique<MockDBTable>(db.get(), "APP_DASH_APPLIANCE_TABLE_NAME");
        std::string key = "testKey";
        uint32_t res = 1;
        std::string version = "v1";

        auto* mock = dynamic_cast<MockDBTable*>(mockTable.get());
        ASSERT_NE(mock, nullptr);

        // Simulate set() throwing an exception
        EXPECT_CALL(*mock, set(_, _, _, _))
            .WillOnce(Throw(std::runtime_error("Set failed")));

        writeResultToDB(mockTable, key, res, version);
    }

    TEST(RemoveFromDBTest, DelThrowsException) {
        auto db = std::make_shared<swss::DBConnector>("DPU_APPL_STATE_DB", 0);
        std::unique_ptr<swss::Table> mockTable = std::make_unique<MockDBTable>(db.get(), "APP_DASH_APPLIANCE_TABLE_NAME");
        std::string key = "testKey";

        auto* mock = dynamic_cast<MockDBTable*>(mockTable.get());
        ASSERT_NE(mock, nullptr);

        // Simulate del() throwing an exception
        EXPECT_CALL(*mock, del(_, _, _))
            .WillOnce(Throw(std::runtime_error("Del failed")));

        removeResultFromDB(mockTable, key);
    }

    TEST_F(SaihelperTest, TestSaiDumpFailure)
    {
        syncd_dump_failure = true;
        _hook_sai_apis();
        initSwitchOrch();

        // Call with abort_on_fail = false
        handleSaiFailure(SAI_API_SWITCH, "set", SAI_STATUS_FAILURE, false);
        ASSERT_EQ(*_sai_syncd_notifications_count, 0);

        _unhook_sai_apis();
        syncd_dump_failure = false;
    }

    TEST(FlushToDBTest, NullTable) {
        std::unique_ptr<swss::Table> nullTable;
        // Should not crash when table is null
        flushResultsToDB(nullTable);
    }

    TEST(FlushToDBTest, FlushValidTable) {
        auto db = std::make_shared<swss::DBConnector>("DPU_APPL_STATE_DB", 0);
        std::unique_ptr<swss::Table> table = std::make_unique<swss::Table>(db.get(), "TEST_TABLE");

        // Should not crash or throw with a valid table
        flushResultsToDB(table);
    }
}

