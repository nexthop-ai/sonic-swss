#include "ut_helper.h"
#include "saihelper.h"
#include "mock_table.h"
#include "mock_sai_capability_wrap.h"

#include <cstdint>
#include <memory>
#include <initializer_list>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>
<<<<<<< HEAD
=======
#include <atomic>
#include <map>
#include <thread>
#include <vector>
>>>>>>> ff4e0da2 (NOS-23003: [orchagent] promote the Redis mode to ZMQ when context config pins zmq_enable (#1899))

extern std::unique_ptr<swss::DBConnector> gHealthStateDb;
extern std::unique_ptr<swss::Table> gOrchHealthTable;
extern bool gOrchUnhealthyCached;
extern std::string gLastSaiError;

namespace saihelper_test
{
    struct CommModeCase
    {
        const char *name;
        std::string json;
        sai_redis_communication_mode_t input;
        sai_redis_communication_mode_t expected;
        bool pinnedToZmq;
    };

    void PrintTo(const CommModeCase &c, std::ostream *os)
    {
        *os << c.name;
    }

    class ResolveCommunicationModeTest : public ::testing::TestWithParam<CommModeCase>
    {
    };

    TEST_P(ResolveCommunicationModeTest, Resolves)
    {
        const CommModeCase &c = GetParam();
        std::istringstream iss(c.json);
        const CommunicationModeResolution resolved =
            resolveCommunicationModeFromContextConfig(iss, c.input);
        EXPECT_EQ(c.expected, resolved.mode);
        EXPECT_EQ(c.pinnedToZmq, resolved.pinnedToZmq);
    }

    // Fields ContextConfigContainer::loadFromFile requires on every context.
    // zmqLiteral is a JSON literal ("true", "false", "\"true\"", "1") or null
    // to omit zmq_enable.
    std::string validContext(unsigned guid, const char *zmqLiteral)
    {
        std::string id = std::to_string(guid);
        std::string body = std::string("{\"guid\":") + id +
            ",\"name\":\"syncd" + id +
            "\",\"dbAsic\":\"ASIC_DB" + id +
            "\",\"dbCounters\":\"COUNTERS_DB" + id +
            "\",\"dbFlex\":\"FLEX_COUNTER_DB" + id +
            "\",\"dbState\":\"STATE_DB\",\"switches\":[{\"index\":0,\"hwinfo\":\"\"}]";
        if (zmqLiteral != nullptr)
        {
            body += std::string(",\"zmq_enable\":") + zmqLiteral;
        }
        body += "}";
        return body;
    }

    std::string contextMissingDbAsic(unsigned guid)
    {
        std::string id = std::to_string(guid);
        return std::string("{\"guid\":") + id +
            ",\"name\":\"syncd" + id +
            "\",\"dbCounters\":\"COUNTERS_DB" + id +
            "\",\"dbFlex\":\"FLEX_COUNTER_DB" + id +
            "\",\"dbState\":\"STATE_DB\",\"switches\":[{\"index\":0,\"hwinfo\":\"\"}],\"zmq_enable\":false}";
    }

    std::string contexts(std::initializer_list<std::string> items)
    {
        std::string body = "{\"CONTEXTS\":[";
        const char *sep = "";
        for (const auto &item : items)
        {
            body += sep;
            body += item;
            sep = ",";
        }
        body += "]}";
        return body;
    }

    // guid 0's zmq_enable is applied only when every context would load in
    // sairedis. A non-bool zmq_enable, or a sibling missing a required field,
    // keeps the starting mode and does not pin, on the promote and demote paths.
    std::vector<CommModeCase> makeCommModeCases()
    {
        const auto redisAsync = SAI_REDIS_COMMUNICATION_MODE_REDIS_ASYNC;
        const auto redisSync = SAI_REDIS_COMMUNICATION_MODE_REDIS_SYNC;
        const auto zmqSync = SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC;
        return {
            {"ZmqDisabledKeepsRedisAsync",
             contexts({validContext(0, "false")}), redisAsync, redisAsync, false},
            {"ZmqDisabledKeepsRedisSync",
             contexts({validContext(0, "false")}), redisSync, redisSync, false},
            {"ZmqDisabledDemotesZmqSync",
             contexts({validContext(0, "false")}), zmqSync, redisSync, false},
            {"ZmqEnabledKeepsZmqSync",
             contexts({validContext(0, "true")}), zmqSync, zmqSync, true},
            {"ZmqEnabledPromotesRedisSync",
             contexts({validContext(0, "true")}), redisSync, zmqSync, true},
            {"ZmqEnabledPromotesRedisAsync",
             contexts({validContext(0, "true")}), redisAsync, zmqSync, true},
            {"MissingZmqEnableKeepsZmqSync",
             contexts({validContext(0, nullptr)}), zmqSync, zmqSync, false},
            {"MissingZmqEnableKeepsRedisAsync",
             contexts({validContext(0, nullptr)}), redisAsync, redisAsync, false},
            {"MissingZmqEnableKeepsRedisSync",
             contexts({validContext(0, nullptr)}), redisSync, redisSync, false},
            {"NonZeroGuidIgnored",
             contexts({validContext(1, "false")}), zmqSync, zmqSync, false},
            {"NonZeroGuidPinIgnored",
             contexts({validContext(1, "true")}), redisAsync, redisAsync, false},
            {"GuidZeroDemotesAmongMultiple",
             contexts({validContext(1, "true"), validContext(0, "false")}), zmqSync, redisSync, false},
            {"GuidZeroFalseKeepsRedisAmongMultiple",
             contexts({validContext(1, "true"), validContext(0, "false")}), redisAsync, redisAsync, false},
            {"SiblingMissingDbAsicDoesNotPromote",
             contexts({validContext(0, "true"), contextMissingDbAsic(1)}), redisAsync, redisAsync, false},
            {"SiblingMissingDbAsicDoesNotDemote",
             contexts({validContext(0, "false"), contextMissingDbAsic(1)}), zmqSync, zmqSync, false},
            {"ZmqEnableStringKeepsRedis",
             contexts({validContext(0, "\"true\"")}), redisAsync, redisAsync, false},
            {"ZmqEnableNumberKeepsZmq",
             contexts({validContext(0, "1")}), zmqSync, zmqSync, false},
            {"MalformedJsonKeepsZmqSync",
             "not valid json", zmqSync, zmqSync, false},
            {"MalformedJsonKeepsRedisSync",
             "not valid json", redisSync, redisSync, false},
            {"MissingContextsKeyKeepsZmqSync",
             R"({"OTHER_KEY":[]})", zmqSync, zmqSync, false},
            {"MissingContextsKeyKeepsRedisAsync",
             R"({"OTHER_KEY":[]})", redisAsync, redisAsync, false},
        };
    }

    const std::vector<CommModeCase> kCommModeCases = makeCommModeCases();

<<<<<<< HEAD
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
=======
    INSTANTIATE_TEST_SUITE_P(
        ResolveCommunicationModeFromContextConfig,
        ResolveCommunicationModeTest,
        ::testing::ValuesIn(kCommModeCases),
        [](const ::testing::TestParamInfo<CommModeCase> &info) -> std::string {
            return info.param.name;
        });

    TEST(SortSystemPortConfigList, SameListForAnyKeyOrder)
    {
        // {port_id, switch_id, core_index, core_port_index, speed, num_voq}, in one CONFIG_DB key order
        std::vector<sai_system_port_config_t> keyOrderA = {
            {39, 0, 2, 25, 400000, 8},
            {0, 0, 0, 0, 10000, 8},
            {2, 0, 3, 5, 400000, 8},
            {32, 0, 6, 5, 400000, 8},
            {1, 0, 3, 1, 400000, 8},
        };
        std::vector<sai_system_port_config_t> keyOrderB(keyOrderA.rbegin(), keyOrderA.rend());
        std::swap(keyOrderB[0], keyOrderB[2]);

        sortSystemPortConfigList(keyOrderA);
        sortSystemPortConfigList(keyOrderB);

        ASSERT_EQ(keyOrderA.size(), keyOrderB.size());
        for (size_t i = 0; i < keyOrderA.size(); i++)
        {
            EXPECT_EQ(keyOrderA[i].port_id, keyOrderB[i].port_id);
            EXPECT_EQ(keyOrderA[i].attached_switch_id, keyOrderB[i].attached_switch_id);
            EXPECT_EQ(keyOrderA[i].attached_core_index, keyOrderB[i].attached_core_index);
            EXPECT_EQ(keyOrderA[i].attached_core_port_index, keyOrderB[i].attached_core_port_index);
            EXPECT_EQ(keyOrderA[i].speed, keyOrderB[i].speed);
            EXPECT_EQ(keyOrderA[i].num_voq, keyOrderB[i].num_voq);
        }
        EXPECT_EQ(0u, keyOrderA.front().port_id);
        EXPECT_EQ(39u, keyOrderA.back().port_id);
>>>>>>> ff4e0da2 (NOS-23003: [orchagent] promote the Redis mode to ZMQ when context config pins zmq_enable (#1899))
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
    }
}
