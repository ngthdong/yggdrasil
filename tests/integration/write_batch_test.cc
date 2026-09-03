#include "engine/database.h"
#include "engine/write_batch.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <gtest/gtest.h>
#include <thread>

namespace engine {
namespace {

using ::std::chrono::milliseconds;

class WriteBatchTest : public ::testing::Test {
  protected:
    void SetUp() override {
        path_ = "test_write_batch_" + std::to_string(::getpid()) + ".db";
        std::remove(path_.c_str());
        std::remove((path_ + ".wal").c_str());
    }
    void TearDown() override {
        std::remove(path_.c_str());
        std::remove((path_ + ".wal").c_str());
    }

    Options ValidOptions(DeadlockPolicy policy = DeadlockPolicy::kWoundWait) {
        Options opts;
        opts.path = path_;
        opts.page_size = 4096;
        opts.buffer_pool_frames = 32;
        opts.deadlock_policy = policy;
        return opts;
    }

    std::string path_;
};

TEST_F(WriteBatchTest, EmptyBatchIsANoopAndSucceeds) {
    Database db(ValidOptions());
    ASSERT_TRUE(db.Open().ok());

    WriteBatch batch;
    EXPECT_TRUE(batch.empty());
    EXPECT_TRUE(db.Write(batch).ok());
}

TEST_F(WriteBatchTest, AllPutsInABatchAreVisibleAfterWrite) {
    Database db(ValidOptions());
    ASSERT_TRUE(db.Open().ok());

    WriteBatch batch;
    batch.Put(Slice("a"), Slice("1"));
    batch.Put(Slice("b"), Slice("2"));
    batch.Put(Slice("c"), Slice("3"));
    ASSERT_EQ(batch.size(), 3u);

    ASSERT_TRUE(db.Write(batch).ok());

    EXPECT_EQ(db.Get(Slice("a")).value(), "1");
    EXPECT_EQ(db.Get(Slice("b")).value(), "2");
    EXPECT_EQ(db.Get(Slice("c")).value(), "3");
    EXPECT_TRUE(db.Verify().ok());
}

TEST_F(WriteBatchTest, DeletesInABatchRemoveKeys) {
    Database db(ValidOptions());
    ASSERT_TRUE(db.Open().ok());
    ASSERT_TRUE(db.Put(Slice("a"), Slice("1")).ok());
    ASSERT_TRUE(db.Put(Slice("b"), Slice("2")).ok());

    WriteBatch batch;
    batch.Delete(Slice("a"));
    ASSERT_TRUE(db.Write(batch).ok());

    EXPECT_FALSE(db.Get(Slice("a")).ok());
    EXPECT_EQ(db.Get(Slice("b")).value(), "2");
}

TEST_F(WriteBatchTest, DeletingAKeyThatDoesNotExistIsANoopNotAFailure) {
    Database db(ValidOptions());
    ASSERT_TRUE(db.Open().ok());
    ASSERT_TRUE(db.Put(Slice("a"), Slice("1")).ok());

    WriteBatch batch;
    batch.Delete(Slice("missing"));
    batch.Put(Slice("b"), Slice("2"));

    ASSERT_TRUE(db.Write(batch).ok())
        << "a missing key in a Delete op must not fail the rest of the batch";
    EXPECT_EQ(db.Get(Slice("a")).value(), "1");
    EXPECT_EQ(db.Get(Slice("b")).value(), "2");
}

TEST_F(WriteBatchTest, MultiplePutsOnTheSameKeyKeepTheLastOne) {
    Database db(ValidOptions());
    ASSERT_TRUE(db.Open().ok());

    WriteBatch batch;
    batch.Put(Slice("k"), Slice("first"));
    batch.Put(Slice("k"), Slice("second"));
    batch.Put(Slice("k"), Slice("third"));

    ASSERT_TRUE(db.Write(batch).ok())
        << "BPlusTree::Insert rejects duplicate keys; Write must upsert like Database::Put does";
    EXPECT_EQ(db.Get(Slice("k")).value(), "third");
}

TEST_F(WriteBatchTest, PutThenDeleteOfTheSameKeyInOneBatchAppliesInSubmissionOrder) {
    Database db(ValidOptions());
    ASSERT_TRUE(db.Open().ok());

    WriteBatch batch;
    batch.Put(Slice("k"), Slice("v"));
    batch.Delete(Slice("k"));

    ASSERT_TRUE(db.Write(batch).ok());
    EXPECT_FALSE(db.Get(Slice("k")).ok())
        << "ops must apply in insertion order, not key-sorted lock-acquisition order";
}

TEST_F(WriteBatchTest, DeleteThenPutOfTheSameKeyInOneBatchLeavesItPresent) {
    Database db(ValidOptions());
    ASSERT_TRUE(db.Open().ok());
    ASSERT_TRUE(db.Put(Slice("k"), Slice("old")).ok());

    WriteBatch batch;
    batch.Delete(Slice("k"));
    batch.Put(Slice("k"), Slice("new"));

    ASSERT_TRUE(db.Write(batch).ok());
    EXPECT_EQ(db.Get(Slice("k")).value(), "new");
}

TEST_F(WriteBatchTest, ClearedBatchCanBeReusedForAnotherWrite) {
    Database db(ValidOptions());
    ASSERT_TRUE(db.Open().ok());

    WriteBatch batch;
    batch.Put(Slice("a"), Slice("1"));
    ASSERT_TRUE(db.Write(batch).ok());

    batch.Clear();
    EXPECT_TRUE(batch.empty());
    batch.Put(Slice("b"), Slice("2"));
    ASSERT_TRUE(db.Write(batch).ok());

    EXPECT_EQ(db.Get(Slice("a")).value(), "1");
    EXPECT_EQ(db.Get(Slice("b")).value(), "2");
}

TEST_F(WriteBatchTest, WriteBeforeOpenFails) {
    Database db(ValidOptions());
    WriteBatch batch;
    batch.Put(Slice("a"), Slice("1"));
    EXPECT_FALSE(db.Write(batch).ok());
}

TEST_F(WriteBatchTest, WriteFailsWhileATransactionIsActive) {
    Database db(ValidOptions());
    ASSERT_TRUE(db.Open().ok());

    auto txn_or = db.BeginTransaction();
    ASSERT_TRUE(txn_or.ok());
    Transaction txn = std::move(txn_or.value());

    WriteBatch batch;
    batch.Put(Slice("a"), Slice("1"));
    EXPECT_FALSE(db.Write(batch).ok());

    ASSERT_TRUE(txn.Commit().ok());
}

TEST_F(WriteBatchTest, WrittenDataPersistsAcrossCloseAndReopen) {
    {
        Database db(ValidOptions());
        ASSERT_TRUE(db.Open().ok());
        WriteBatch batch;
        batch.Put(Slice("a"), Slice("1"));
        batch.Put(Slice("b"), Slice("2"));
        batch.Delete(Slice("a"));
        ASSERT_TRUE(db.Write(batch).ok());
        ASSERT_TRUE(db.Close().ok());
    }

    Database db(ValidOptions());
    ASSERT_TRUE(db.Open().ok());
    EXPECT_FALSE(db.Get(Slice("a")).ok());
    EXPECT_EQ(db.Get(Slice("b")).value(), "2");
    EXPECT_TRUE(db.Verify().ok());
}

// Regression test: Transaction::Commit() used to finalize the transaction
// (satisfying EnsureNoActiveTransaction) without ever calling
// LockManager::ReleaseAllLocks(). A later Write() touching the same key
// would then wait forever on a lock nobody would ever release -- and,
// because Write() held engine_mutex_ across that wait, it would freeze
// every other engine_mutex_-using operation on the database with it.
TEST_F(WriteBatchTest, WriteOnAKeyFromACommittedTransactionDoesNotHang) {
    Database db(ValidOptions());
    ASSERT_TRUE(db.Open().ok());

    auto txn_or = db.BeginTransaction();
    ASSERT_TRUE(txn_or.ok());
    Transaction txn = std::move(txn_or.value());
    ASSERT_TRUE(txn.Put(Slice("k"), Slice("from_txn")).ok());
    ASSERT_TRUE(txn.Commit().ok());

    WriteBatch batch;
    batch.Put(Slice("k"), Slice("from_batch"));

    std::promise<Status> promise;
    std::future<Status> fut = promise.get_future();
    std::thread writer([&] { promise.set_value(db.Write(batch)); });

    ASSERT_EQ(fut.wait_for(std::chrono::seconds(5)), std::future_status::ready)
        << "Write() did not complete; the committed transaction's lock on \"k\" was "
           "likely never released";
    writer.join();

    EXPECT_TRUE(fut.get().ok());
    EXPECT_EQ(db.Get(Slice("k")).value(), "from_batch");
}

// Same regression, via Rollback() instead of Commit().
TEST_F(WriteBatchTest, WriteOnAKeyFromARolledBackTransactionDoesNotHang) {
    Database db(ValidOptions());
    ASSERT_TRUE(db.Open().ok());
    ASSERT_TRUE(db.Put(Slice("k"), Slice("initial")).ok());

    auto txn_or = db.BeginTransaction();
    ASSERT_TRUE(txn_or.ok());
    Transaction txn = std::move(txn_or.value());
    ASSERT_TRUE(txn.Put(Slice("k"), Slice("from_txn")).ok());
    ASSERT_TRUE(txn.Rollback().ok());

    WriteBatch batch;
    batch.Put(Slice("k"), Slice("from_batch"));

    std::promise<Status> promise;
    std::future<Status> fut = promise.get_future();
    std::thread writer([&] { promise.set_value(db.Write(batch)); });

    ASSERT_EQ(fut.wait_for(std::chrono::seconds(5)), std::future_status::ready)
        << "Write() did not complete; the rolled-back transaction's lock on \"k\" was "
           "likely never released";
    writer.join();

    EXPECT_TRUE(fut.get().ok());
    EXPECT_EQ(db.Get(Slice("k")).value(), "from_batch");
}

TEST_F(WriteBatchTest, ConcurrentWritesToDisjointKeysBothSucceed) {
    Database db(ValidOptions(DeadlockPolicy::kDetection));
    ASSERT_TRUE(db.Open().ok());

    WriteBatch batch1;
    batch1.Put(Slice("x"), Slice("1"));
    WriteBatch batch2;
    batch2.Put(Slice("y"), Slice("2"));

    std::atomic<bool> ok1{false};
    std::atomic<bool> ok2{false};
    std::thread t1([&] { ok1 = db.Write(batch1).ok(); });
    std::thread t2([&] { ok2 = db.Write(batch2).ok(); });
    t1.join();
    t2.join();

    EXPECT_TRUE(ok1.load());
    EXPECT_TRUE(ok2.load());
    EXPECT_EQ(db.Get(Slice("x")).value(), "1");
    EXPECT_EQ(db.Get(Slice("y")).value(), "2");
}

} // namespace
} // namespace engine
