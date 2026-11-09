#include "ut_helper.h"
#include "saihelper.h"
#include "mock_table.h"
#include "mock_sai_capability_wrap.h"

<<<<<<< HEAD
#include <cstdint>
#include <memory>
#include <sstream>
#include <vector>
=======
#include <atomic>
#include <map>
#include <memory>
#include <sstream>
#include <thread>
>>>>>>> f10e234d (NOS-20131: [orchagent] Restate a latched SAI failure at NOTICE once per 10 min, not at ERROR every second (#1822))

extern std::unique_ptr<swss::DBConnector> gHealthStateDb;
extern std::unique_ptr<swss::Table> gOrchHealthTable;
extern bool gOrchUnhealthyCached;
extern std::string gLastSaiError;

namespace saihelper_test
{
    TEST(ResolveCommunicationModeFromContextConfig, NonZmqInputUnchanged)
    {
        std::istringstream iss(R"({"CONTEXTS":[{"guid":0,"zmq_enable":false}]})");
        EXPECT_EQ(SAI_REDIS_COMMUNICATION_MODE_REDIS_ASYNC,
                  resolveCommunicationModeFromContextConfig(
                      iss, SAI_REDIS_COMMUNICATION_MODE_REDIS_ASYNC));
    }

    TEST(ResolveCommunicationModeFromContextConfig, RedisSyncInputUnchanged)
    {
        std::istringstream iss(R"({"CONTEXTS":[{"guid":0,"zmq_enable":false}]})");
        EXPECT_EQ(SAI_REDIS_COMMUNICATION_MODE_REDIS_SYNC,
                  resolveCommunicationModeFromContextConfig(
                      iss, SAI_REDIS_COMMUNICATION_MODE_REDIS_SYNC));
    }

    TEST(ResolveCommunicationModeFromContextConfig, ZmqDisabledForGuidZeroDemotes)
    {
        std::istringstream iss(R"({"CONTEXTS":[{"guid":0,"zmq_enable":false}]})");
        EXPECT_EQ(SAI_REDIS_COMMUNICATION_MODE_REDIS_SYNC,
                  resolveCommunicationModeFromContextConfig(
                      iss, SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC));
    }

    TEST(ResolveCommunicationModeFromContextConfig, ZmqEnabledForGuidZeroUnchanged)
    {
        std::istringstream iss(R"({"CONTEXTS":[{"guid":0,"zmq_enable":true}]})");
        EXPECT_EQ(SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC,
                  resolveCommunicationModeFromContextConfig(
                      iss, SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC));
    }

    TEST(ResolveCommunicationModeFromContextConfig, MissingZmqEnableUnchanged)
    {
        std::istringstream iss(R"({"CONTEXTS":[{"guid":0}]})");
        EXPECT_EQ(SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC,
                  resolveCommunicationModeFromContextConfig(
                      iss, SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC));
    }

    TEST(ResolveCommunicationModeFromContextConfig, NonZeroGuidSkipped)
    {
        // Only guid=0 (the default context) is consulted; non-zero guids are
        // ignored even if they disable zmq.
        std::istringstream iss(R"({"CONTEXTS":[{"guid":1,"zmq_enable":false}]})");
        EXPECT_EQ(SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC,
                  resolveCommunicationModeFromContextConfig(
                      iss, SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC));
    }

    TEST(ResolveCommunicationModeFromContextConfig, FindsGuidZeroAmongMultiple)
    {
        std::istringstream iss(R"({"CONTEXTS":[{"guid":1,"zmq_enable":true},{"guid":0,"zmq_enable":false}]})");
        EXPECT_EQ(SAI_REDIS_COMMUNICATION_MODE_REDIS_SYNC,
                  resolveCommunicationModeFromContextConfig(
                      iss, SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC));
    }

    TEST(ResolveCommunicationModeFromContextConfig, MalformedJsonUnchanged)
    {
        std::istringstream iss("not valid json");
        EXPECT_EQ(SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC,
                  resolveCommunicationModeFromContextConfig(
                      iss, SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC));
    }

    TEST(ResolveCommunicationModeFromContextConfig, MissingContextsKeyUnchanged)
    {
        std::istringstream iss(R"({"OTHER_KEY":[]})");
        EXPECT_EQ(SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC,
                  resolveCommunicationModeFromContextConfig(
                      iss, SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC));
    }
}

namespace sai_failure_status_test
{
    struct SaiFailureStatusTest : public ::testing::Test
    {
        void SetUp() override
        {
            testing_db::reset();
            gLastSaiError.clear();
            initSaiFailureTable();
        }

        void TearDown() override
        {
            testing_db::reset();
            gLastSaiError.clear();
        }
    };

    TEST_F(SaiFailureStatusTest, InitiallyHealthy)
    {
        setSaiFailureStatus(false);
        std::string error;
        EXPECT_FALSE(getSaiFailureStatus(error));
    }

    TEST_F(SaiFailureStatusTest, SetUnhealthyReturnsTrueWithError)
    {
        std::string errorMsg = "Encountered failure in set operation, SAI API: SAI_API_SWITCH, status: -1";
        setSaiFailureStatus(true, errorMsg);

        std::string error;
        EXPECT_TRUE(getSaiFailureStatus(error));
        EXPECT_EQ(error, errorMsg);
    }

    TEST_F(SaiFailureStatusTest, ResetToHealthyAfterFailure)
    {
        setSaiFailureStatus(true, "some SAI error");

        std::string error;
        EXPECT_TRUE(getSaiFailureStatus(error));
        EXPECT_EQ(error, "some SAI error");

        // Reset
        setSaiFailureStatus(false);
        EXPECT_FALSE(getSaiFailureStatus(error));
    }

    TEST_F(SaiFailureStatusTest, ExternalResetDetected)
    {
        // Simulate a SAI failure
        setSaiFailureStatus(true, "SAI failure");

        std::string error;
        EXPECT_TRUE(getSaiFailureStatus(error));

        // Simulate an external operator clearing the flag in STATE_DB
        // by writing "false" directly via the mock DB
        swss::DBConnector db("STATE_DB", 0);
        swss::Table tbl(&db, "PROCESS_HEALTH");
        std::vector<swss::FieldValueTuple> fvs;
        fvs.emplace_back("unhealthy", "false");
        fvs.emplace_back("error", "");
        tbl.set("orchagent", fvs);

        // getSaiFailureStatus should detect the external reset
        EXPECT_FALSE(getSaiFailureStatus(error));

        // Subsequent calls should also return false (cache updated)
        EXPECT_FALSE(getSaiFailureStatus(error));
    }

    TEST_F(SaiFailureStatusTest, MultipleFailuresOverwrite)
    {
        setSaiFailureStatus(true, "first error");
        setSaiFailureStatus(true, "second error");

        std::string error;
        EXPECT_TRUE(getSaiFailureStatus(error));
        EXPECT_EQ(error, "second error");
    }

    TEST_F(SaiFailureStatusTest, KeyMissingKeepsUnhealthy)
    {
        // Mark unhealthy so the cache is set
        setSaiFailureStatus(true, "SAI failure");

        // Delete the key from mock DB to simulate missing key
        testing_db::reset();

        // getSaiFailureStatus should still report unhealthy with cached error
        std::string error;
        EXPECT_TRUE(getSaiFailureStatus(error));
        EXPECT_EQ(error, "SAI failure");
    }

    TEST_F(SaiFailureStatusTest, NullTableReturnsUnhealthy)
    {
        // Null out the table directly
        gOrchHealthTable.reset();
        gHealthStateDb.reset();

        // Set cache to unhealthy (table is null, so DB write is skipped)
        setSaiFailureStatus(true, "SAI failure");

        // getSaiFailureStatus should hit the null table guard with cached error
        std::string error;
        EXPECT_TRUE(getSaiFailureStatus(error));
        EXPECT_EQ(error, "SAI failure");

        // Re-init for TearDown safety
        initSaiFailureTable();
    }

<<<<<<< HEAD
    /*
     * querySupportedAclTableGroupType() must preserve the historical PARALLEL
     * default on every outcome except a *successful* enum-values query that
     * reports PARALLEL unsupported while SEQUENTIAL is supported. The enum-values
     * SAI call is driven through the shared --wrap override (sai_enum_cap_ut).
     */
    namespace
    {
        sai_enum_cap_ut::EnumValuesCapabilityOverride makeAclGroupTypeOverride(
            sai_status_t status, std::vector<int32_t> supported_values)
        {
            return [status, supported_values](
                       sai_object_id_t /*switch_id*/,
                       sai_object_type_t object_type,
                       sai_attr_id_t attr_id,
                       sai_s32_list_t *cap) -> sai_status_t
            {
                // Only intercept the ACL table group TYPE query.
                if (object_type != SAI_OBJECT_TYPE_ACL_TABLE_GROUP ||
                    attr_id != SAI_ACL_TABLE_GROUP_ATTR_TYPE)
                {
                    if (cap)
                    {
                        cap->count = 0;
                    }
                    return SAI_STATUS_SUCCESS;
                }

                if (status != SAI_STATUS_SUCCESS)
                {
                    if (cap)
                    {
                        cap->count = 0;
                    }
                    return status;
                }

                // The helper pre-sizes the buffer to the enum's total value
                // count, so a supported subset always fits.
                if (!cap || !cap->list ||
                    cap->count < supported_values.size())
                {
                    if (cap)
                    {
                        cap->count = static_cast<uint32_t>(supported_values.size());
                    }
                    return SAI_STATUS_BUFFER_OVERFLOW;
                }
                for (size_t i = 0; i < supported_values.size(); i++)
                {
                    cap->list[i] = supported_values[i];
                }
                cap->count = static_cast<uint32_t>(supported_values.size());
                return SAI_STATUS_SUCCESS;
            };
        }
    }

    TEST(QuerySupportedAclTableGroupType, ParallelSupportedReturnsParallel)
    {
        sai_enum_cap_ut::EnumValuesCapabilityOverrideGuard guard(
            makeAclGroupTypeOverride(SAI_STATUS_SUCCESS,
                {SAI_ACL_TABLE_GROUP_TYPE_SEQUENTIAL,
                 SAI_ACL_TABLE_GROUP_TYPE_PARALLEL}));
        EXPECT_EQ(SAI_ACL_TABLE_GROUP_TYPE_PARALLEL,
                  querySupportedAclTableGroupType(SAI_NULL_OBJECT_ID));
    }

    TEST(QuerySupportedAclTableGroupType, OnlySequentialReturnsSequential)
    {
        sai_enum_cap_ut::EnumValuesCapabilityOverrideGuard guard(
            makeAclGroupTypeOverride(SAI_STATUS_SUCCESS,
                {SAI_ACL_TABLE_GROUP_TYPE_SEQUENTIAL}));
        EXPECT_EQ(SAI_ACL_TABLE_GROUP_TYPE_SEQUENTIAL,
                  querySupportedAclTableGroupType(SAI_NULL_OBJECT_ID));
    }

    TEST(QuerySupportedAclTableGroupType, EmptyListPreservesParallel)
    {
        sai_enum_cap_ut::EnumValuesCapabilityOverrideGuard guard(
            makeAclGroupTypeOverride(SAI_STATUS_SUCCESS, {}));
        EXPECT_EQ(SAI_ACL_TABLE_GROUP_TYPE_PARALLEL,
                  querySupportedAclTableGroupType(SAI_NULL_OBJECT_ID));
    }

    TEST(QuerySupportedAclTableGroupType, NotSupportedPreservesParallel)
    {
        sai_enum_cap_ut::EnumValuesCapabilityOverrideGuard guard(
            makeAclGroupTypeOverride(SAI_STATUS_NOT_SUPPORTED, {}));
        EXPECT_EQ(SAI_ACL_TABLE_GROUP_TYPE_PARALLEL,
                  querySupportedAclTableGroupType(SAI_NULL_OBJECT_ID));
    }

    TEST(QuerySupportedAclTableGroupType, NotImplementedPreservesParallel)
    {
        sai_enum_cap_ut::EnumValuesCapabilityOverrideGuard guard(
            makeAclGroupTypeOverride(SAI_STATUS_NOT_IMPLEMENTED, {}));
        EXPECT_EQ(SAI_ACL_TABLE_GROUP_TYPE_PARALLEL,
                  querySupportedAclTableGroupType(SAI_NULL_OBJECT_ID));
    }

    TEST(QuerySupportedAclTableGroupType, GenuineFailurePreservesParallel)
    {
        sai_enum_cap_ut::EnumValuesCapabilityOverrideGuard guard(
            makeAclGroupTypeOverride(SAI_STATUS_FAILURE, {}));
        EXPECT_EQ(SAI_ACL_TABLE_GROUP_TYPE_PARALLEL,
                  querySupportedAclTableGroupType(SAI_NULL_OBJECT_ID));
=======
    static std::map<std::string, std::string> healthRow()
    {
        swss::DBConnector db("STATE_DB", 0);
        swss::Table tbl(&db, "PROCESS_HEALTH");
        std::vector<swss::FieldValueTuple> fvs;
        std::map<std::string, std::string> row;
        if (tbl.get("orchagent", fvs))
        {
            for (const auto& fv : fvs)
            {
                row[fvField(fv)] = fvValue(fv);
            }
        }
        return row;
    }

    static void clearHealthRowExternally()
    {
        swss::DBConnector db("STATE_DB", 0);
        swss::Table tbl(&db, "PROCESS_HEALTH");
        std::vector<swss::FieldValueTuple> fvs;
        fvs.emplace_back("unhealthy", "false");
        fvs.emplace_back("error", "");
        tbl.set("orchagent", fvs);
    }

    /*
     * The row records when the current unhealthy episode started and how many
     * failures have hit it, so a reader can tell a historical latch from a
     * live stream of failures without counting syslog lines.
     */
    TEST_F(SaiFailureStatusTest, LatchRecordsEpisodeStartAndCount)
    {
        setSaiFailureStatus(false);
        auto row = healthRow();
        EXPECT_EQ(row["unhealthy"], "false");
        EXPECT_EQ(row["count"], "0");
        EXPECT_EQ(row["since"], "");

        setSaiFailureStatus(true, "first error");
        row = healthRow();
        EXPECT_EQ(row["unhealthy"], "true");
        EXPECT_EQ(row["count"], "1");
        EXPECT_FALSE(row["since"].empty());
        const std::string since = row["since"];

        // A further failure while already unhealthy is counted against the same
        // episode and reports the latest error, not a new start time.
        setSaiFailureStatus(true, "second error");
        row = healthRow();
        EXPECT_EQ(row["count"], "2");
        EXPECT_EQ(row["since"], since);
        EXPECT_EQ(row["error"], "second error");

        setSaiFailureStatus(false);
        row = healthRow();
        EXPECT_EQ(row["unhealthy"], "false");
        EXPECT_EQ(row["count"], "0");
        EXPECT_EQ(row["since"], "");
    }

    TEST_F(SaiFailureStatusTest, ReminderSilentWhileHealthy)
    {
        setSaiFailureStatus(false);
        const auto now = std::chrono::steady_clock::now();
        EXPECT_FALSE(logSaiFailureReminder(now));
        EXPECT_FALSE(logSaiFailureReminder(now + SAI_FAILURE_REMINDER_INTERVAL));
    }

    /*
     * The reminder cadence. OrchDaemon::start() calls this once per select
     * timeout; a latched failure must be restated on the first pass and then
     * once per interval, never once per pass.
     */
    TEST_F(SaiFailureStatusTest, ReminderRestatesOncePerInterval)
    {
        setSaiFailureStatus(false);
        setSaiFailureStatus(true, "SAI failure");
        const auto t0 = std::chrono::steady_clock::now();

        EXPECT_TRUE(logSaiFailureReminder(t0));

        // One-second select passes for the rest of the interval stay silent.
        for (auto t = t0 + std::chrono::seconds(1);
             t < t0 + SAI_FAILURE_REMINDER_INTERVAL;
             t += std::chrono::seconds(1))
        {
            ASSERT_FALSE(logSaiFailureReminder(t));
        }

        // Once the interval has elapsed it is restated exactly once more.
        EXPECT_TRUE(logSaiFailureReminder(t0 + SAI_FAILURE_REMINDER_INTERVAL));
        EXPECT_FALSE(logSaiFailureReminder(t0 + SAI_FAILURE_REMINDER_INTERVAL + std::chrono::seconds(1)));

        setSaiFailureStatus(false);
    }

    /*
     * An operator reset through STATE_DB ends the episode: the reminder goes
     * quiet, and a fresh failure afterwards is restated immediately with a new
     * count rather than waiting out the previous interval.
     */
    TEST_F(SaiFailureStatusTest, ReminderFollowsExternalReset)
    {
        setSaiFailureStatus(false);
        setSaiFailureStatus(true, "SAI failure");
        const auto t0 = std::chrono::steady_clock::now();
        EXPECT_TRUE(logSaiFailureReminder(t0));

        clearHealthRowExternally();
        EXPECT_FALSE(logSaiFailureReminder(t0 + std::chrono::seconds(1)));
        // Detecting the reset normalises the row the operator only half-cleared.
        auto row = healthRow();
        EXPECT_EQ(row["unhealthy"], "false");
        EXPECT_EQ(row["error"], "");
        EXPECT_EQ(row["since"], "");
        EXPECT_EQ(row["count"], "0");

        setSaiFailureStatus(true, "later SAI failure");
        EXPECT_TRUE(logSaiFailureReminder(t0 + std::chrono::seconds(2)));
        row = healthRow();
        EXPECT_EQ(row["count"], "1");
        EXPECT_EQ(row["error"], "later SAI failure");

        setSaiFailureStatus(false);
    }

    /*
     * Ring mode splits the callers across threads: handleSaiFailure() latches
     * from the ring thread while OrchDaemon::start() reads, restates and
     * normalises from the main thread. Each round clears the row the way an
     * operator would, then lets one latch from another thread overlap the
     * main thread's read-decide-write. Whichever order they land in, the
     * latch must still be in the row afterwards; an unserialised normalise
     * would wipe it when the latch falls between the read and the write.
     * The operator write itself runs while the latcher is idle, because the
     * mock table is not thread-safe and the real one is a Redis server.
     */
    TEST_F(SaiFailureStatusTest, LatchFromAnotherThreadSurvivesOperatorReset)
    {
        setSaiFailureStatus(false);
        std::atomic<int> rounds{0};
        std::atomic<int> latched{0};
        std::atomic<bool> stop{false};
        std::thread latcher([&]() {
            while (!stop.load())
            {
                if (latched.load() == rounds.load())
                {
                    std::this_thread::yield();
                    continue;
                }
                setSaiFailureStatus(true, "ring thread failure");
                latched++;
            }
        });

        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 1000; i++)
        {
            clearHealthRowExternally();
            rounds++;
            std::string error;
            if (i % 2 == 0)
            {
                getSaiFailureStatus(error);
            }
            else
            {
                logSaiFailureReminder(t0 + std::chrono::seconds(i));
            }
            while (latched.load() < rounds.load())
            {
                std::this_thread::yield();
            }
            auto row = healthRow();
            ASSERT_EQ(row["unhealthy"], "true") << "latch lost in round " << i;
            ASSERT_EQ(row["error"], "ring thread failure");
            ASSERT_NE(row["count"], "0");
            ASSERT_TRUE(getSaiFailureStatus(error));
        }
        stop = true;
        latcher.join();
        setSaiFailureStatus(false);
>>>>>>> f10e234d (NOS-20131: [orchagent] Restate a latched SAI failure at NOTICE once per 10 min, not at ERROR every second (#1822))
    }
}
