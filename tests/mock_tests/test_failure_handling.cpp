#include "saihelper.h"
#include "ut_helper.h"
#include <sys/mman.h>

extern sai_switch_api_t *sai_switch_api;

namespace saifailure_test
{
    struct SaiFailureTest : public ::testing::Test
    {
    };
    uint32_t *_sai_syncd_notifications_count;
    int32_t *_sai_syncd_notification_event;
    sai_switch_api_t *pold_sai_switch_api;
    sai_switch_api_t ut_sai_switch_api;

        sai_status_t _ut_stub_sai_set_switch_attribute(
        _In_ sai_object_id_t switch_id,
        _In_ const sai_attribute_t *attr)
    {
        if (attr[0].id == SAI_REDIS_SWITCH_ATTR_NOTIFY_SYNCD)
        {
            *_sai_syncd_notifications_count = *_sai_syncd_notifications_count + 1;
            *_sai_syncd_notification_event = attr[0].value.s32;
        }
        return pold_sai_switch_api->set_switch_attribute(switch_id, attr);
    }

    void _hook_sai_switch_api()
    {
        map<string, string> profile = {
            { "SAI_VS_SWITCH_TYPE", "SAI_VS_SWITCH_TYPE_BCM56850" },
            { "KV_DEVICE_MAC_ADDRESS", "20:03:04:05:06:00" }
        };

        ut_helper::initSaiApi(profile);
        ut_sai_switch_api = *sai_switch_api;
        pold_sai_switch_api = sai_switch_api;
        
        ut_sai_switch_api.set_switch_attribute = _ut_stub_sai_set_switch_attribute;
        sai_switch_api = &ut_sai_switch_api;
    }

    void _unhook_sai_switch_api()
    {
        sai_switch_api = pold_sai_switch_api;
        ut_helper::uninitSaiApi();
    }

    TEST_F(SaiFailureTest, handleSaiFailure)
    {
        _hook_sai_switch_api();
        _sai_syncd_notifications_count = (uint32_t*)mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);

        _sai_syncd_notification_event = (int32_t*)mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        *_sai_syncd_notifications_count = 0;
        uint32_t notif_count = *_sai_syncd_notifications_count;

        handleSaiCreateStatus(SAI_API_FDB, SAI_STATUS_FAILURE);
        ASSERT_EQ(*_sai_syncd_notifications_count, ++notif_count);
        ASSERT_EQ(*_sai_syncd_notification_event, SAI_REDIS_NOTIFY_SYNCD_INVOKE_DUMP);

        handleSaiCreateStatus(SAI_API_HOSTIF, SAI_STATUS_INVALID_PARAMETER);
        ASSERT_EQ(*_sai_syncd_notifications_count, ++notif_count);
        ASSERT_EQ(*_sai_syncd_notification_event, SAI_REDIS_NOTIFY_SYNCD_INVOKE_DUMP);

        handleSaiCreateStatus(SAI_API_PORT, SAI_STATUS_FAILURE);
        ASSERT_EQ(*_sai_syncd_notifications_count, ++notif_count);
        ASSERT_EQ(*_sai_syncd_notification_event, SAI_REDIS_NOTIFY_SYNCD_INVOKE_DUMP);

        handleSaiSetStatus(SAI_API_HOSTIF, SAI_STATUS_FAILURE);
        ASSERT_EQ(*_sai_syncd_notifications_count, ++notif_count);
        ASSERT_EQ(*_sai_syncd_notification_event, SAI_REDIS_NOTIFY_SYNCD_INVOKE_DUMP);

        handleSaiSetStatus(SAI_API_PORT, SAI_STATUS_FAILURE);
        ASSERT_EQ(*_sai_syncd_notifications_count, ++notif_count);
        ASSERT_EQ(*_sai_syncd_notification_event, SAI_REDIS_NOTIFY_SYNCD_INVOKE_DUMP);

        handleSaiSetStatus(SAI_API_TUNNEL, SAI_STATUS_FAILURE);
        ASSERT_EQ(*_sai_syncd_notifications_count, ++notif_count);
        ASSERT_EQ(*_sai_syncd_notification_event, SAI_REDIS_NOTIFY_SYNCD_INVOKE_DUMP);

        handleSaiRemoveStatus(SAI_API_LAG, SAI_STATUS_FAILURE);
        ASSERT_EQ(*_sai_syncd_notifications_count, ++notif_count);
        ASSERT_EQ(*_sai_syncd_notification_event, SAI_REDIS_NOTIFY_SYNCD_INVOKE_DUMP);

        _unhook_sai_switch_api();
    }
<<<<<<< HEAD
=======

    TEST_F(SaiFailureTest, handleSaiFailureNotExecutedDoesNotLatchUnhealthy)
    {
        _hook_sai_switch_api();
        _sai_syncd_notifications_count = (uint32_t*)mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        _sai_syncd_notification_event = (int32_t*)mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        *_sai_syncd_notifications_count = 0;

        // NOT_EXECUTED never reached syncd: it must not latch the sticky
        // unhealthy status, but still requests a SAI failure dump.
        std::string error;
        setSaiFailureStatus(false);
        handleSaiFailure(SAI_API_ROUTE, "create", SAI_STATUS_NOT_EXECUTED, false);
        ASSERT_FALSE(getSaiFailureStatus(error));
        ASSERT_EQ(*_sai_syncd_notifications_count, 1u);

        handleSaiFailure(SAI_API_ROUTE, "create", SAI_STATUS_FAILURE, false);
        ASSERT_TRUE(getSaiFailureStatus(error));

        setSaiFailureStatus(false);
        _unhook_sai_switch_api();
    }

    TEST_F(SaiFailureTest, createSwitchRetrySucceedsAfterTransientFailures)
    {
        _hook_sai_switch_api();
        ut_sai_switch_api.create_switch = _ut_stub_sai_create_switch;

        _create_switch_calls = 0;
        _create_switch_fail_until = 3;  // fail 3 times, succeed on the 4th

        // Inject a small attempt budget with a 0s interval so the test exercises
        // the retry loop without sleeping (production uses the fixed defaults).
        const uint32_t max_attempts = 10;
        const uint32_t retry_interval_sec = 0;

        sai_object_id_t switch_id = SAI_NULL_OBJECT_ID;
        sai_attribute_t attr;
        attr.id = SAI_SWITCH_ATTR_INIT_SWITCH;
        attr.value.booldata = true;

        sai_status_t status = createSwitchWithRetry(&switch_id, 1, &attr,
                                                    max_attempts, retry_interval_sec);
        ASSERT_EQ(status, SAI_STATUS_SUCCESS);
        ASSERT_EQ(_create_switch_calls, _create_switch_fail_until + 1);

        _unhook_sai_switch_api();
    }

    TEST_F(SaiFailureTest, createSwitchRetryExhaustsAndReturnsFailure)
    {
        _hook_sai_switch_api();
        ut_sai_switch_api.create_switch = _ut_stub_sai_create_switch;

        _create_switch_calls = 0;
        _create_switch_fail_until = 1000;  // always fail within the attempt budget

        // Inject a small attempt budget with a 0s interval so the test exercises
        // the retry loop without sleeping (production uses the fixed defaults).
        const uint32_t max_attempts = 4;
        const uint32_t retry_interval_sec = 0;

        sai_object_id_t switch_id = SAI_NULL_OBJECT_ID;
        sai_attribute_t attr;
        attr.id = SAI_SWITCH_ATTR_INIT_SWITCH;
        attr.value.booldata = true;

        sai_status_t status = createSwitchWithRetry(&switch_id, 1, &attr,
                                                    max_attempts, retry_interval_sec);
        ASSERT_EQ(status, SAI_STATUS_FAILURE);
        ASSERT_EQ(_create_switch_calls, max_attempts);

        _unhook_sai_switch_api();
    }

    /*
     * A set that SAI rejects for its parameters is dropped by the caller and
     * latches the unhealthy status once, with the API and status in the
     * recorded error. Later successes on the same API leave it latched, and
     * restating it must not touch syncd again: one failure means one ERROR
     * line and one failure dump, however long the latch stays set.
     */
    TEST_F(SaiFailureTest, rejectedSetLatchesOnceAndRestatingIsSideEffectFree)
    {
        _hook_sai_switch_api();
        _sai_syncd_notifications_count = (uint32_t*)mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        _sai_syncd_notification_event = (int32_t*)mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        *_sai_syncd_notifications_count = 0;

        std::string error;
        setSaiFailureStatus(false);
        ASSERT_EQ(handleSaiSetStatus(SAI_API_SCHEDULER, SAI_STATUS_INVALID_PARAMETER), task_failed);
        ASSERT_TRUE(getSaiFailureStatus(error));
        ASSERT_NE(error.find("set"), std::string::npos);
        ASSERT_NE(error.find("SAI_API_SCHEDULER"), std::string::npos);
        ASSERT_NE(error.find("SAI_STATUS_INVALID_PARAMETER"), std::string::npos);
        ASSERT_EQ(*_sai_syncd_notifications_count, 1u);

        ASSERT_EQ(handleSaiSetStatus(SAI_API_SCHEDULER, SAI_STATUS_SUCCESS), task_success);
        ASSERT_TRUE(getSaiFailureStatus(error));
        ASSERT_EQ(*_sai_syncd_notifications_count, 1u);

        ASSERT_TRUE(logSaiFailureReminder());
        ASSERT_FALSE(logSaiFailureReminder());
        ASSERT_EQ(*_sai_syncd_notifications_count, 1u);

        setSaiFailureStatus(false);
        ASSERT_FALSE(getSaiFailureStatus(error));
        ASSERT_FALSE(logSaiFailureReminder());
        _unhook_sai_switch_api();
    }
>>>>>>> f10e234d (NOS-20131: [orchagent] Restate a latched SAI failure at NOTICE once per 10 min, not at ERROR every second (#1822))
}
