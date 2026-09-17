// Pre-include standard library headers that conflict with
// the #define private public hack (they use 'private' internally).
#include <string>
#include <fstream>
#include <iostream>
#include <sstream>
#include <memory>
#include <deque>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <atomic>
#include "table.h"

#define private public
#include "recorder.h"
#undef private

#include "subscriberstatetable.h"
#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_table.h"
#include "zmqorch.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>
#include <unistd.h>

extern PortsOrch *gPortsOrch;

namespace consumer_test
{
    using namespace std;

    const int UNKNOWN_EXCEPTION_VALUE = 42;

    class TestOrch : public Orch
    {
    public:
        TestOrch(swss::DBConnector *db, string tableName)
            :Orch(db, tableName),
            m_notification_count(0)
        {
        }

        void doTask(Consumer& consumer)
        {
            std::cout << "TestOrch::doTask " << consumer.m_toSync.size() << std::endl;
            m_notification_count += consumer.m_toSync.size();
            consumer.m_toSync.clear();
        }

        long m_notification_count;
    };

    enum class ThrowType
    {
        None,
        InvalidArgument,
        LogicError,
        RuntimeError,
        UnknownException
    };

    class ThrowingOrch : public Orch
    {
    public:
        ThrowingOrch(swss::DBConnector *db, string tableName)
            :Orch(db, tableName),
            m_throwType(ThrowType::None),
            m_doTaskCallCount(0)
        {
        }

        void doTask(Consumer& consumer)
        {
            m_doTaskCallCount++;
            switch (m_throwType)
            {
                case ThrowType::InvalidArgument:
                    throw std::invalid_argument("test invalid argument");
                case ThrowType::LogicError:
                    throw std::logic_error("test logic error");
                case ThrowType::RuntimeError:
                    throw std::runtime_error("test runtime error");
                case ThrowType::UnknownException:
                    throw UNKNOWN_EXCEPTION_VALUE;
                case ThrowType::None:
                default:
                    consumer.m_toSync.clear();
                    break;
            }
        }

        ThrowType m_throwType;
        int m_doTaskCallCount;
    };

    class ThrowingRetryOrch : public Orch
    {
    public:
        ThrowingRetryOrch(swss::DBConnector *db, string tableName)
            :Orch(db, tableName),
            m_throwType(ThrowType::None)
        {
        }

        void doTask(Consumer& consumer)
        {
            consumer.m_toSync.clear();
        }

        size_t retryToSync(const std::string &executorName, size_t quota) override
        {
            switch (m_throwType)
            {
                case ThrowType::InvalidArgument:
                    throw std::invalid_argument("retryToSync invalid argument");
                case ThrowType::LogicError:
                    throw std::logic_error("retryToSync logic error");
                case ThrowType::RuntimeError:
                    throw std::runtime_error("retryToSync runtime error");
                case ThrowType::UnknownException:
                    throw UNKNOWN_EXCEPTION_VALUE;
                case ThrowType::None:
                default:
                    return 0;
            }
        }

        ThrowType m_throwType;
    };

    struct ConsumerTest : public ::testing::Test
    {
        shared_ptr<swss::DBConnector> m_app_db;
        shared_ptr<swss::DBConnector> m_config_db;
        shared_ptr<swss::DBConnector> m_state_db;

        string key = "key";
        string f1 = "field1";
        string v1a = "value1_a";
        string v1b = "value1_b";
        string f2 = "field2";
        string v2a = "value2_a";
        string v2b = "value2_b";
        string f3 = "field3";
        string v3a = "value3_a";
        KeyOpFieldsValuesTuple exp_kofv;

        unique_ptr<Consumer> consumer;
        deque <KeyOpFieldsValuesTuple> kofv_q;

        ConsumerTest()
        {
            // FIXME: move out from constructor
            m_app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
            m_config_db = make_shared<swss::DBConnector>("CONFIG_DB", 0);
            m_state_db = make_shared<swss::DBConnector>("STATE_DB", 0);
            consumer = unique_ptr<Consumer>(new Consumer(
                new swss::ConsumerStateTable(m_config_db.get(), "CFG_TEST_TABLE", 1, 1), gPortsOrch, "CFG_TEST_TABLE"));
        }

        virtual void SetUp() override
        {
            ::testing_db::reset();
            Recorder::Instance().swss.setAsync(false);
        }

        virtual void TearDown() override
        {
            Recorder::Instance().swss.setAsync(false);
            ::testing_db::reset();
        }

        void validate_syncmap(SyncMap &sync, uint16_t exp_sz, std::string exp_key, KeyOpFieldsValuesTuple exp_kofv)
        {
            // verify the content in syncMap
            ASSERT_EQ(sync.size(), exp_sz);
            auto it = sync.begin();
            while (it != sync.end())
            {
                KeyOpFieldsValuesTuple t = it->second;

                string itkey = kfvKey(t);
                if (itkey == exp_key) {
                    ASSERT_EQ(t, exp_kofv);
                    it = sync.erase(it);
                    break;
                } else {
                    it++;
                }
            }
            ASSERT_EQ(sync.size(), exp_sz-1);
        }

        vector<string> waitForRecordedLines(const string& path, const string& marker, size_t expected)
        {
            for (size_t retry = 0; retry < 50; ++retry)
            {
                ifstream ifs(path);
                vector<string> matches;
                string line;

                while (getline(ifs, line))
                {
                    if (line.find(marker) != string::npos)
                    {
                        matches.push_back(line);
                    }
                }

                if (matches.size() >= expected)
                {
                    return matches;
                }

                this_thread::sleep_for(chrono::milliseconds(20));
            }

            return {};
        }
    };

    TEST_F(ConsumerTest, ConsumerAddToSync_Set)
    {

        // Test case, one set_command
        auto entry = KeyOpFieldsValuesTuple(
            { key,
                SET_COMMAND,
                { { f1, v1a },
                    { f2, v2a } } });

        kofv_q.push_back(entry);
        consumer->addToSync(kofv_q);
        exp_kofv = entry;
        validate_syncmap(consumer->m_toSync, 1, key, exp_kofv);
    }

    TEST_F(ConsumerTest, ConsumerAddToSync_Del)
    {
        // Test case, one with del_command
        auto entry = KeyOpFieldsValuesTuple(
            { key,
                DEL_COMMAND,
                { { } } });

        kofv_q.push_back(entry);
        consumer->addToSync(kofv_q);

        exp_kofv = entry;
        validate_syncmap(consumer->m_toSync, 1, key, exp_kofv);

    }

    TEST_F(ConsumerTest, ConsumerAddToSync_Set_Del)
    {
        // Test case, add SET then DEL
        auto entrya = KeyOpFieldsValuesTuple(
            { key,
                SET_COMMAND,
                { { f1, v1a },
                    { f2, v2a } } });

        auto entryb = KeyOpFieldsValuesTuple(
            { key,
                DEL_COMMAND,
                { { } } });

        kofv_q.push_back(entrya);
        kofv_q.push_back(entryb);
        consumer->addToSync(kofv_q);

        // expect only DEL
        exp_kofv = entryb;
        validate_syncmap(consumer->m_toSync, 1, key, exp_kofv);
    }

    TEST_F(ConsumerTest, ConsumerAddToSync_Del_Set)
    {
        auto entrya = KeyOpFieldsValuesTuple(
            { key,
                DEL_COMMAND,
                { { } } });

        auto entryb = KeyOpFieldsValuesTuple(
            { key,
                SET_COMMAND,
                { { f1, v1a },
                    { f2, v2a } } });

        // Test case, add DEL then SET, re-try 100 times, order should be kept
        for (auto x = 0; x < 100; x++)
        {
            kofv_q.push_back(entrya);
            kofv_q.push_back(entryb);
            consumer->addToSync(kofv_q);

            // expect DEL then SET
            exp_kofv = entrya;
            validate_syncmap(consumer->m_toSync, 2, key, exp_kofv);

            exp_kofv = entryb;
            validate_syncmap(consumer->m_toSync, 1, key, exp_kofv);
        }
    }

    TEST_F(ConsumerTest, ConsumerAddToSync_Set_Del_Set_Multi)
    {
        // Test5, add SET, DEL then SET, re-try 100 times , order should be kept
        auto entrya = KeyOpFieldsValuesTuple(
            { key,
                SET_COMMAND,
                { { f1, v1a },
                    { f2, v2a } } });

        auto entryb = KeyOpFieldsValuesTuple(
            { key,
                DEL_COMMAND,
                { { } } });

        auto entryc = KeyOpFieldsValuesTuple(
            { key,
                SET_COMMAND,
                { { f1, v1a },
                    { f2, v2a } } });

        for (auto x = 0; x < 100; x++)
        {
            kofv_q.push_back(entrya);
            kofv_q.push_back(entryb);
            kofv_q.push_back(entryc);
            consumer->addToSync(kofv_q);

            // expect DEL then SET
            exp_kofv = entryb;
            validate_syncmap(consumer->m_toSync, 2, key, exp_kofv);

            exp_kofv = entryc;
            validate_syncmap(consumer->m_toSync, 1, key, exp_kofv);
        }
    }

    TEST_F(ConsumerTest, ConsumerAddToSync_Set_Del_Set_Multi_In_Q)
    {
        // Test5, add SET, DEL then SET, repeat 100 times in queue, final result and order should be kept
        auto entrya = KeyOpFieldsValuesTuple(
            { key,
                SET_COMMAND,
                { { f1, v1a },
                    { f2, v2a } } });

        auto entryb = KeyOpFieldsValuesTuple(
            { key,
                DEL_COMMAND,
                { { } } });

        auto entryc = KeyOpFieldsValuesTuple(
            { key,
                SET_COMMAND,
                { { f1, v1a },
                    { f2, v2a } } });

        for (auto x = 0; x < 100; x++)
        {
            kofv_q.push_back(entrya);
            kofv_q.push_back(entryb);
            kofv_q.push_back(entryc);
        }
        consumer->addToSync(kofv_q);

        // expect DEL then SET
        exp_kofv = entryb;
        validate_syncmap(consumer->m_toSync, 2, key, exp_kofv);

        exp_kofv = entryc;
        validate_syncmap(consumer->m_toSync, 1, key, exp_kofv);
    }

    TEST_F(ConsumerTest, ConsumerAddToSync_Del_Set_Setnew)
    {
        // Test case, DEL, SET, then SET with different value
        auto entrya = KeyOpFieldsValuesTuple(
            { key,
                DEL_COMMAND,
                { { } } });

        auto entryb = KeyOpFieldsValuesTuple(
            { key,
                SET_COMMAND,
                { { f1, v1a },
                    { f2, v2a } } });

        auto entryc = KeyOpFieldsValuesTuple(
            { key,
                SET_COMMAND,
                { { f1, v1b },
                    { f2, v2b } } });

        kofv_q.push_back(entrya);
        kofv_q.push_back(entryb);
        kofv_q.push_back(entryc);
        consumer->addToSync(kofv_q);

        // expect DEL then SET with new values
        exp_kofv = entrya;
        validate_syncmap(consumer->m_toSync, 2, key, exp_kofv);

        exp_kofv = entryc;
        validate_syncmap(consumer->m_toSync, 1, key, exp_kofv);
    }

    TEST_F(ConsumerTest, ConsumerAddToSync_Del_Set_Setnew1)
    {
        // Test case, DEL, SET, then SET with new values and new fields
        auto entrya = KeyOpFieldsValuesTuple(
            { key,
                DEL_COMMAND,
                { { } } });

        auto entryb = KeyOpFieldsValuesTuple(
            { key,
                SET_COMMAND,
                { { f1, v1a },
                    { f2, v2a } } });

        auto entryc = KeyOpFieldsValuesTuple(
            { key,
                SET_COMMAND,
                { { f1, v1b },
                    { f3, v3a } } });

        kofv_q.push_back(entrya);
        kofv_q.push_back(entryb);
        kofv_q.push_back(entryc);
        consumer->addToSync(kofv_q);

        // expect DEL then SET with new values and new fields
        exp_kofv = entrya;
        validate_syncmap(consumer->m_toSync, 2, key, exp_kofv);

        exp_kofv = KeyOpFieldsValuesTuple(
            { key,
                SET_COMMAND,
                { { f2, v2a },
                    { f1, v1b },
                    { f3, v3a } } });

        validate_syncmap(consumer->m_toSync, 1, key, exp_kofv);
    }

    TEST_F(ConsumerTest, ConsumerFullSnapshotSourceFlag)
    {
        // Test case, m_fullSnapshotSource should be set only for consumers
        // backed by SubscriberStateTable
        Consumer sub_consumer(
            new swss::SubscriberStateTable(m_config_db.get(), "CFG_TEST_TABLE", 1, 1),
            gPortsOrch, "CFG_TEST_TABLE");

        ASSERT_TRUE(sub_consumer.m_fullSnapshotSource);
        ASSERT_FALSE(consumer->m_fullSnapshotSource);
    }

    TEST_F(ConsumerTest, SubscriberAddToSync_SnapshotReplacesPendingSet)
    {
        // Test case, SET then SET without f2 and with a new value for f1 on a
        // subscriber consumer, newer snapshot should replace the pending SET
        // so f2 does not survive and f1 carries the new value
        Consumer sub_consumer(
            new swss::SubscriberStateTable(m_config_db.get(), "CFG_TEST_TABLE", 1, 1),
            gPortsOrch, "CFG_TEST_TABLE");

        auto parked = KeyOpFieldsValuesTuple(
            { key,
                SET_COMMAND,
                { { f1, v1a },
                    { f2, v2a },
                    { f3, v3a } } });

        auto snapshot = KeyOpFieldsValuesTuple(
            { key,
                SET_COMMAND,
                { { f1, v1b },
                    { f3, v3a } } });

        sub_consumer.addToSync(parked);
        sub_consumer.addToSync(snapshot);

        // expect the newer snapshot only, f2 should be gone
        exp_kofv = snapshot;
        validate_syncmap(sub_consumer.m_toSync, 1, key, exp_kofv);
    }

    TEST_F(ConsumerTest, SubscriberAddToSync_SnapshotAfterPendingDel)
    {
        // Test case, DEL then SET, SET on a subscriber consumer, order should
        // be kept and the second SET should replace the first
        Consumer sub_consumer(
            new swss::SubscriberStateTable(m_config_db.get(), "CFG_TEST_TABLE", 1, 1),
            gPortsOrch, "CFG_TEST_TABLE");

        auto entry_del = KeyOpFieldsValuesTuple(
            { key,
                DEL_COMMAND,
                { { } } });

        auto entry_set = KeyOpFieldsValuesTuple(
            { key,
                SET_COMMAND,
                { { f1, v1a } } });

        auto entry_set2 = KeyOpFieldsValuesTuple(
            { key,
                SET_COMMAND,
                { { f3, v3a } } });

        sub_consumer.addToSync(entry_del);
        sub_consumer.addToSync(entry_set);
        sub_consumer.addToSync(entry_set2);

        // expect DEL then the newer SET
        exp_kofv = entry_del;
        validate_syncmap(sub_consumer.m_toSync, 2, key, exp_kofv);

        exp_kofv = entry_set2;
        validate_syncmap(sub_consumer.m_toSync, 1, key, exp_kofv);
    }

    TEST_F(ConsumerTest, SubscriberAddToSync_SnapshotReplacesRetryCachedSet)
    {
        // Test case, snapshot replace through the RetryCache path.
        //
        // When a SET for a key is sitting in the RetryCache (not in m_toSync)
        // and a newer SET for that key arrives, addToSync() first evicts the
        // cached SET back into m_toSync and then combines it with the newer
        // SET. On a subscriber consumer that combine must be a replace: the
        // newer snapshot wins as a whole, and f2 (present only in the cached
        // SET) must be gone afterwards.
        //
        // Two cache shapes are covered: (1) only a SET is cached,
        // (2) a DEL and a SET are cached together.
        TestOrch orch(m_config_db.get(), "CFG_RETRY_TABLE");
        orch.createRetryCache("CFG_RETRY_TABLE");
        auto retryCache = orch.getRetryCache("CFG_RETRY_TABLE");
        ASSERT_NE(retryCache, nullptr);

        Consumer sub_consumer(
            new swss::SubscriberStateTable(m_config_db.get(), "CFG_RETRY_TABLE", 1, 1),
            &orch, "CFG_RETRY_TABLE");
        ASSERT_TRUE(sub_consumer.m_fullSnapshotSource);

        auto parked = KeyOpFieldsValuesTuple(
            { key,
                SET_COMMAND,
                { { f1, v1a },
                    { f2, v2a },
                    { f3, v3a } } });

        auto snapshot = KeyOpFieldsValuesTuple(
            { key,
                SET_COMMAND,
                { { f1, v1b },
                    { f3, v3a } } });

        // Shape 1: only a SET is cached. Expect the cache to be empty for the
        // key and m_toSync to hold just the snapshot.
        retryCache->insert(parked, DUMMY_CONSTRAINT);
        sub_consumer.addToSync(snapshot);

        ASSERT_EQ(retryCache->getRetryMap().count(key), 0);
        exp_kofv = snapshot;
        validate_syncmap(sub_consumer.m_toSync, 1, key, exp_kofv);

        // Shape 2: a DEL and a SET are cached. Expect the DEL to stay cached,
        // and m_toSync to hold just the snapshot (the evicted SET was
        // replaced, not merged).
        auto entry_del = KeyOpFieldsValuesTuple(
            { key,
                DEL_COMMAND,
                { { } } });

        retryCache->insert(entry_del, DUMMY_CONSTRAINT);
        retryCache->insert(parked, DUMMY_CONSTRAINT);
        sub_consumer.addToSync(snapshot);

        ASSERT_EQ(retryCache->getRetryMap().count(key), 1);
        ASSERT_EQ(kfvOp(retryCache->getRetryMap().find(key)->second.second), DEL_COMMAND);
        exp_kofv = snapshot;
        validate_syncmap(sub_consumer.m_toSync, 1, key, exp_kofv);
    }

    TEST_F(ConsumerTest, ZmqConsumerAddToSync_KeepsFieldMerge)
    {
        // Test case, ZMQ-backed consumers keep the old field-merge semantics.
        //
        // A ZmqConsumerStateTable delivers deltas, not full snapshots, so
        // m_fullSnapshotSource must be false and SET over SET must still
        // field-merge: f2, which only the older SET carries, must survive.
        swss::ZmqServer zmq_server("tcp://127.0.0.1:18101", "");
        ZmqConsumer zmq_consumer(
            new swss::ZmqConsumerStateTable(m_app_db.get(), "APP_TEST_TABLE", zmq_server,
                                            1, 1, /*dbPersistence=*/false),
            gPortsOrch, "APP_TEST_TABLE");

        ASSERT_FALSE(zmq_consumer.m_fullSnapshotSource);

        auto entrya = KeyOpFieldsValuesTuple(
            { key,
                SET_COMMAND,
                { { f1, v1a },
                    { f2, v2a } } });

        auto entryb = KeyOpFieldsValuesTuple(
            { key,
                SET_COMMAND,
                { { f1, v1b },
                    { f3, v3a } } });

        zmq_consumer.addToSync(entrya);
        zmq_consumer.addToSync(entryb);

        // expect one merged SET: f2 kept from the older SET, f1 updated to
        // the newer value, f3 appended (same field order as
        // ConsumerAddToSync_Del_Set_Setnew1)
        exp_kofv = KeyOpFieldsValuesTuple(
            { key,
                SET_COMMAND,
                { { f2, v2a },
                    { f1, v1b },
                    { f3, v3a } } });
        validate_syncmap(zmq_consumer.m_toSync, 1, key, exp_kofv);
    }

    TEST_F(ConsumerTest, SubscriberRefillToSync_SnapshotReplacesPendingSet)
    {
        // Test case, snapshot replace through the refillToSync path.
        //
        // refillToSync() (used by warm restart and addExistingData) reads
        // whole entries from the table and feeds them through addToSync().
        // On a subscriber consumer the entry read from the table must
        // replace a SET already pending for the same key, not merge into it:
        // f2, present only in the pending SET, must be gone afterwards.
        Consumer sub_consumer(
            new swss::SubscriberStateTable(m_config_db.get(), "CFG_TEST_TABLE", 1, 1),
            gPortsOrch, "CFG_TEST_TABLE");

        auto parked = KeyOpFieldsValuesTuple(
            { key,
                SET_COMMAND,
                { { f1, v1a },
                    { f2, v2a },
                    { f3, v3a } } });
        sub_consumer.addToSync(parked);

        // The table holds the entry without f2 and with a newer f1 value
        swss::Table table(m_config_db.get(), "CFG_TEST_TABLE");
        table.set(key, { { f1, v1b }, { f3, v3a } });

        ASSERT_EQ(sub_consumer.refillToSync(&table), 1);

        // expect the table snapshot only, f2 should be gone
        exp_kofv = KeyOpFieldsValuesTuple(
            { key,
                SET_COMMAND,
                { { f1, v1b },
                    { f3, v3a } } });
        validate_syncmap(sub_consumer.m_toSync, 1, key, exp_kofv);
    }

    TEST_F(ConsumerTest, ConsumerAddToSync_Ind_Set_Del)
    {
        // Test case,  Add individuals by addToSync, SET then DEL
        auto entrya = KeyOpFieldsValuesTuple(
            { key,
                SET_COMMAND,
                { { f1, v1a },
                    { f2, v2a } } });

        auto entryb = KeyOpFieldsValuesTuple(
            { key,
                DEL_COMMAND,
                { { } } });

        consumer->addToSync(entrya);
        consumer->addToSync(entryb);

        // expect only DEL
        exp_kofv = entryb;
        validate_syncmap(consumer->m_toSync, 1, key, exp_kofv);

    }

    TEST_F(ConsumerTest, ConsumerPops_notification_count)
    {
        int consumer_pops_batch_size = 10;
        TestOrch test_orch(m_config_db.get(), "CFG_TEST_TABLE");
        Consumer test_consumer(
                new swss::ConsumerStateTable(m_config_db.get(), "CFG_TEST_TABLE", consumer_pops_batch_size, 1), &test_orch, "CFG_TEST_TABLE");
        swss::ProducerStateTable producer_table(m_config_db.get(), "CFG_TEST_TABLE");

        m_config_db->flushdb();
        for (int notification_count = 0; notification_count< consumer_pops_batch_size*2; notification_count++)
        {
            std::vector<FieldValueTuple> fields;
            FieldValueTuple t("test_field", "test_value");
            fields.push_back(t);
            producer_table.set(std::to_string(notification_count), fields);
            
            cout << "ConsumerPops_notification_count:: add key: " << notification_count << endl;
        }

        // consumer should pops consumer_pops_batch_size notifications 
        test_consumer.execute();
        ASSERT_EQ(test_orch.m_notification_count, consumer_pops_batch_size);

        test_consumer.execute();
        ASSERT_EQ(test_orch.m_notification_count, consumer_pops_batch_size*2);
    }

    TEST_F(ConsumerTest, AsyncSwssRecorderWritesBatchRecords)
    {
        char dir_template[] = "/tmp/swss-consumer-ut-XXXXXX";
        auto dir = mkdtemp(dir_template);
        ASSERT_NE(dir, nullptr);

        const string dirname(dir);
        const string filename = "swss-recorder-ut.rec";
        const string fullpath = dirname + "/" + filename;
        const string key_prefix = "async-swss-recorder-key-";

        Recorder::Instance().swss.setRecord(true);
        Recorder::Instance().swss.setLocation(dirname);
        Recorder::Instance().swss.setFileName(filename);
        Recorder::Instance().swss.setAsync(true);
        Recorder::Instance().swss.startRec(true);

        deque<KeyOpFieldsValuesTuple> entries;
        for (int i = 0; i < 3; ++i)
        {
            entries.push_back(KeyOpFieldsValuesTuple(
                { key_prefix + to_string(i),
                  SET_COMMAND,
                  { { f1, v1a }, { f2, v2a } } }));
        }

        consumer->addToSync(entries);

        const auto lines = waitForRecordedLines(fullpath, key_prefix, entries.size());
        ASSERT_EQ(lines.size(), entries.size());

        for (size_t i = 0; i < lines.size(); ++i)
        {
            EXPECT_NE(lines[i].find(consumer->dumpTuple(entries[i])), string::npos);
            EXPECT_NE(lines[i].find("|field1:value1_a"), string::npos);
            EXPECT_NE(lines[i].find("|field2:value2_a"), string::npos);
        }

        Recorder::Instance().swss.setAsync(false);
        Recorder::Instance().swss.record_ofs.close();
        ASSERT_EQ(remove(fullpath.c_str()), 0);
        ASSERT_EQ(rmdir(dirname.c_str()), 0);
    }

    /*
     * Exception handling tests for Consumer::drain() and Orch::doTask()
     *
     * These tests verify that exceptions thrown inside doTask(Consumer&)
     * are caught gracefully and do not crash the process.
     */

    struct ExceptionHandlingTest : public ::testing::Test
    {
        shared_ptr<swss::DBConnector> m_app_db;
        unique_ptr<ThrowingOrch> m_orch;

        virtual void SetUp() override
        {
            ::testing_db::reset();
            m_app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
            m_orch = make_unique<ThrowingOrch>(m_app_db.get(), "APP_TEST_TABLE");
        }

        virtual void TearDown() override
        {
            m_orch.reset();
            ::testing_db::reset();
        }

        void populateConsumer(Consumer &consumer, int count = 1)
        {
            deque<KeyOpFieldsValuesTuple> entries;
            for (int i = 0; i < count; i++)
            {
                entries.push_back({"key" + to_string(i), SET_COMMAND, {{"field", "value"}}});
            }
            consumer.addToSync(entries);
        }
    };

    TEST_F(ExceptionHandlingTest, DrainCatchesInvalidArgument)
    {
        auto *executor = m_orch->getExecutor("APP_TEST_TABLE");
        auto *consumer = dynamic_cast<Consumer *>(executor);
        ASSERT_NE(consumer, nullptr);

        populateConsumer(*consumer);
        ASSERT_FALSE(consumer->m_toSync.empty());

        m_orch->m_throwType = ThrowType::InvalidArgument;

        // drain() should catch the exception and not crash
        ASSERT_NO_THROW(consumer->drain());
        ASSERT_EQ(m_orch->m_doTaskCallCount, 1);

        // m_toSync is not cleared because the exception prevented it
        ASSERT_FALSE(consumer->m_toSync.empty());
    }

    TEST_F(ExceptionHandlingTest, DrainCatchesLogicError)
    {
        auto *consumer = dynamic_cast<Consumer *>(m_orch->getExecutor("APP_TEST_TABLE"));
        ASSERT_NE(consumer, nullptr);

        populateConsumer(*consumer);
        m_orch->m_throwType = ThrowType::LogicError;

        ASSERT_NO_THROW(consumer->drain());
        ASSERT_EQ(m_orch->m_doTaskCallCount, 1);
        ASSERT_FALSE(consumer->m_toSync.empty());
    }

    TEST_F(ExceptionHandlingTest, DrainCatchesRuntimeError)
    {
        auto *consumer = dynamic_cast<Consumer *>(m_orch->getExecutor("APP_TEST_TABLE"));
        ASSERT_NE(consumer, nullptr);

        populateConsumer(*consumer);
        m_orch->m_throwType = ThrowType::RuntimeError;

        ASSERT_NO_THROW(consumer->drain());
        ASSERT_EQ(m_orch->m_doTaskCallCount, 1);
        ASSERT_FALSE(consumer->m_toSync.empty());
    }

    TEST_F(ExceptionHandlingTest, DrainCatchesUnknownException)
    {
        auto *consumer = dynamic_cast<Consumer *>(m_orch->getExecutor("APP_TEST_TABLE"));
        ASSERT_NE(consumer, nullptr);

        populateConsumer(*consumer);
        m_orch->m_throwType = ThrowType::UnknownException;

        ASSERT_NO_THROW(consumer->drain());
        ASSERT_EQ(m_orch->m_doTaskCallCount, 1);
        ASSERT_FALSE(consumer->m_toSync.empty());
    }

    TEST_F(ExceptionHandlingTest, DrainNoExceptionClearsSync)
    {
        auto *consumer = dynamic_cast<Consumer *>(m_orch->getExecutor("APP_TEST_TABLE"));
        ASSERT_NE(consumer, nullptr);

        populateConsumer(*consumer, 3);
        m_orch->m_throwType = ThrowType::None;

        ASSERT_NO_THROW(consumer->drain());
        ASSERT_EQ(m_orch->m_doTaskCallCount, 1);

        // Normal path: doTask clears m_toSync
        ASSERT_TRUE(consumer->m_toSync.empty());
    }

    TEST_F(ExceptionHandlingTest, OrchDoTaskCatchesExceptionPerConsumer)
    {
        auto *consumer = dynamic_cast<Consumer *>(m_orch->getExecutor("APP_TEST_TABLE"));
        ASSERT_NE(consumer, nullptr);

        populateConsumer(*consumer, 2);
        m_orch->m_throwType = ThrowType::RuntimeError;

        // Orch::doTask() (no-arg) iterates consumers and should not crash
        ASSERT_NO_THROW(static_cast<Orch *>(m_orch.get())->doTask());
        ASSERT_EQ(m_orch->m_doTaskCallCount, 1);
    }

    /*
     * Tests for Orch::doTask() catch blocks via retryToSync().
     *
     * Consumer::drain() has its own catch, so exceptions from doTask(Consumer&)
     * never reach Orch::doTask()'s catch. To exercise Orch::doTask()'s catches
     * directly, we override retryToSync() to throw - it runs before drain()
     * in the Orch::doTask() loop.
     */

    struct OrchDoTaskExceptionTest : public ::testing::Test
    {
        shared_ptr<swss::DBConnector> m_app_db;
        unique_ptr<ThrowingRetryOrch> m_orch;

        virtual void SetUp() override
        {
            ::testing_db::reset();
            m_app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
            m_orch = make_unique<ThrowingRetryOrch>(m_app_db.get(), "APP_TEST_TABLE");
        }

        virtual void TearDown() override
        {
            m_orch.reset();
            ::testing_db::reset();
        }
    };

    TEST_F(OrchDoTaskExceptionTest, DoTaskCatchesInvalidArgumentFromRetry)
    {
        m_orch->m_throwType = ThrowType::InvalidArgument;
        ASSERT_NO_THROW(static_cast<Orch *>(m_orch.get())->doTask());
    }

    TEST_F(OrchDoTaskExceptionTest, DoTaskCatchesLogicErrorFromRetry)
    {
        m_orch->m_throwType = ThrowType::LogicError;
        ASSERT_NO_THROW(static_cast<Orch *>(m_orch.get())->doTask());
    }

    TEST_F(OrchDoTaskExceptionTest, DoTaskCatchesRuntimeErrorFromRetry)
    {
        m_orch->m_throwType = ThrowType::RuntimeError;
        ASSERT_NO_THROW(static_cast<Orch *>(m_orch.get())->doTask());
    }

    TEST_F(OrchDoTaskExceptionTest, DoTaskCatchesUnknownExceptionFromRetry)
    {
        m_orch->m_throwType = ThrowType::UnknownException;
        ASSERT_NO_THROW(static_cast<Orch *>(m_orch.get())->doTask());
    }

    TEST_F(ExceptionHandlingTest, DrainRecoveryAfterException)
    {
        auto *consumer = dynamic_cast<Consumer *>(m_orch->getExecutor("APP_TEST_TABLE"));
        ASSERT_NE(consumer, nullptr);

        populateConsumer(*consumer);

        // First call throws
        m_orch->m_throwType = ThrowType::RuntimeError;
        ASSERT_NO_THROW(consumer->drain());
        ASSERT_EQ(m_orch->m_doTaskCallCount, 1);
        ASSERT_FALSE(consumer->m_toSync.empty());

        // Second call succeeds - orch recovers and processes tasks
        m_orch->m_throwType = ThrowType::None;
        ASSERT_NO_THROW(consumer->drain());
        ASSERT_EQ(m_orch->m_doTaskCallCount, 2);
        ASSERT_TRUE(consumer->m_toSync.empty());
    }

    TEST_F(ConsumerTest, SetRecordableDefaultTrue)
    {
        EXPECT_TRUE(consumer->isRecordable());
    }

    TEST_F(ConsumerTest, SetRecordableToggle)
    {
        consumer->setRecordable(false);
        EXPECT_FALSE(consumer->isRecordable());

        consumer->setRecordable(true);
        EXPECT_TRUE(consumer->isRecordable());
    }

    TEST_F(ConsumerTest, SetRecordableSkipsRecording)
    {
        char dir_template[] = "/tmp/swss-consumer-ut-XXXXXX";
        auto dir = mkdtemp(dir_template);
        ASSERT_NE(dir, nullptr);

        const string dirname(dir);
        const string filename = "swss-recordable-ut.rec";
        const string fullpath = dirname + "/" + filename;
        const string recorded_key = "recorded-key";
        const string skipped_key = "skipped-key";
        const string resumed_key = "resumed-key";

        Recorder::Instance().swss.setRecord(true);
        Recorder::Instance().swss.setLocation(dirname);
        Recorder::Instance().swss.setFileName(filename);
        Recorder::Instance().swss.setAsync(false);
        Recorder::Instance().swss.startRec(true);

        // Record a tuple with recording enabled (default)
        deque<KeyOpFieldsValuesTuple> entries1;
        entries1.push_back(KeyOpFieldsValuesTuple(
            { recorded_key, SET_COMMAND, { { f1, v1a } } }));
        consumer->addToSync(entries1);

        // Disable recording and record another tuple
        consumer->setRecordable(false);
        deque<KeyOpFieldsValuesTuple> entries2;
        entries2.push_back(KeyOpFieldsValuesTuple(
            { skipped_key, SET_COMMAND, { { f2, v2a } } }));
        consumer->addToSync(entries2);

        // Re-enable and record a third tuple
        consumer->setRecordable(true);
        deque<KeyOpFieldsValuesTuple> entries3;
        entries3.push_back(KeyOpFieldsValuesTuple(
            { resumed_key, SET_COMMAND, { { f1, v1a } } }));
        consumer->addToSync(entries3);

        // Verify file contents
        ifstream ifs(fullpath);
        string content((istreambuf_iterator<char>(ifs)),
                        istreambuf_iterator<char>());
        EXPECT_NE(content.find(recorded_key), string::npos);
        EXPECT_EQ(content.find(skipped_key), string::npos);
        EXPECT_NE(content.find(resumed_key), string::npos);

        Recorder::Instance().swss.record_ofs.close();
        ASSERT_EQ(remove(fullpath.c_str()), 0);
        ASSERT_EQ(rmdir(dirname.c_str()), 0);
    }
}
