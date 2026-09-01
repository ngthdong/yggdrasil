#include "engine/b_plus_tree.h"
#include "engine/recovery_manager.h"

#include <cstdio>
#include <memory>
#include <string>

#include <gtest/gtest.h>

namespace engine {
namespace {

class RecoveryManagerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        path_ = "test_recovery_" + std::to_string(::getpid()) + ".db";
        wal_path_ = path_ + ".wal";
        std::remove(path_.c_str());
        std::remove(wal_path_.c_str());
        disk_manager_ = DiskManager::Open(path_, 512, true).value();
        wal_manager_ = WalManager::Open(wal_path_).value();
        bpm_ = std::make_unique<BufferPoolManager>(disk_manager_.get(), 32);
        bpm_->SetWalManager(wal_manager_.get());
        fpm_ = std::make_unique<FreePageManager>(disk_manager_.get(), bpm_.get());
        tree_ = std::make_unique<BPlusTree>(disk_manager_.get(), bpm_.get(), fpm_.get());
    }

    void TearDown() override {
        tree_.reset();
        fpm_.reset();
        bpm_.reset();
        wal_manager_.reset();
        disk_manager_.reset();
        std::remove(path_.c_str());
        std::remove(wal_path_.c_str());
    }

    // Appends a record to the WAL and blocks until it is durable, so a
    // freshly-opened RecoveryManager reading the file from disk sees it.
    lsn_t LogAndFlush(LogRecordType type,
                      const std::string& key,
                      const std::string& value,
                      txn_id_t txn_id = kInvalidTxnId) {
        lsn_t lsn =
            wal_manager_->AppendLogRecord(type, kInvalidPageId, Slice(key), Slice(value), txn_id)
                .value();
        EXPECT_TRUE(wal_manager_->Flush(lsn).ok());
        return lsn;
    }

    std::string path_, wal_path_;
    std::unique_ptr<DiskManager> disk_manager_;
    std::unique_ptr<WalManager> wal_manager_;
    std::unique_ptr<BufferPoolManager> bpm_;
    std::unique_ptr<FreePageManager> fpm_;
    std::unique_ptr<BPlusTree> tree_;
};

TEST_F(RecoveryManagerTest, RecoverOnMissingWalFileIsOk) {
    std::remove(wal_path_.c_str());
    RecoveryManager recovery(bpm_.get(), tree_.get(), wal_path_);
    EXPECT_TRUE(recovery.Recover().ok());
    EXPECT_TRUE(tree_->IsEmpty());
}

TEST_F(RecoveryManagerTest, RecoverOnEmptyWalFileIsOk) {
    RecoveryManager recovery(bpm_.get(), tree_.get(), wal_path_);
    EXPECT_TRUE(recovery.Recover().ok());
    EXPECT_TRUE(tree_->IsEmpty());
}

TEST_F(RecoveryManagerTest, RedoAppliesAutoCommitInsertMissingFromTree) {
    LogAndFlush(LogRecordType::kInsert, "a", "1");

    RecoveryManager recovery(bpm_.get(), tree_.get(), wal_path_);
    ASSERT_TRUE(recovery.Recover().ok());

    ASSERT_TRUE(tree_->Get(Slice("a")).ok());
    EXPECT_EQ(tree_->Get(Slice("a")).value(), "1");
}

TEST_F(RecoveryManagerTest, RedoAppliesAutoCommitDeleteToExistingKey) {
    ASSERT_TRUE(tree_->Insert(Slice("b"), Slice("old")).ok());
    LogAndFlush(LogRecordType::kDelete, "b", "old");

    RecoveryManager recovery(bpm_.get(), tree_.get(), wal_path_);
    ASSERT_TRUE(recovery.Recover().ok());

    EXPECT_FALSE(tree_->Get(Slice("b")).ok());
}

TEST_F(RecoveryManagerTest, RedoOfInsertAlreadyReflectedInTreeIsANoop) {
    ASSERT_TRUE(tree_->Insert(Slice("c"), Slice("2")).ok());
    LogAndFlush(LogRecordType::kInsert, "c", "2");

    RecoveryManager recovery(bpm_.get(), tree_.get(), wal_path_);
    ASSERT_TRUE(recovery.Recover().ok());

    ASSERT_TRUE(tree_->Get(Slice("c")).ok());
    EXPECT_EQ(tree_->Get(Slice("c")).value(), "2");
}

TEST_F(RecoveryManagerTest, RedoOfDeleteAlreadyAbsentFromTreeIsANoop) {
    LogAndFlush(LogRecordType::kDelete, "missing", "whatever");

    RecoveryManager recovery(bpm_.get(), tree_.get(), wal_path_);
    EXPECT_TRUE(recovery.Recover().ok());
    EXPECT_FALSE(tree_->Get(Slice("missing")).ok());
}

TEST_F(RecoveryManagerTest, UncommittedInsertIsUndone) {
    LogAndFlush(LogRecordType::kBegin, "", "", /*txn_id=*/5);
    LogAndFlush(LogRecordType::kInsert, "u", "1", /*txn_id=*/5);
    // No kCommit for txn 5.

    RecoveryManager recovery(bpm_.get(), tree_.get(), wal_path_);
    ASSERT_TRUE(recovery.Recover().ok());

    EXPECT_FALSE(tree_->Get(Slice("u")).ok())
        << "insert from an uncommitted transaction must be rolled back";
}

TEST_F(RecoveryManagerTest, CommittedInsertSurvivesRecovery) {
    LogAndFlush(LogRecordType::kBegin, "", "", /*txn_id=*/7);
    LogAndFlush(LogRecordType::kInsert, "c", "2", /*txn_id=*/7);
    LogAndFlush(LogRecordType::kCommit, "", "", /*txn_id=*/7);

    RecoveryManager recovery(bpm_.get(), tree_.get(), wal_path_);
    ASSERT_TRUE(recovery.Recover().ok());

    ASSERT_TRUE(tree_->Get(Slice("c")).ok());
    EXPECT_EQ(tree_->Get(Slice("c")).value(), "2");
}

TEST_F(RecoveryManagerTest, UncommittedDeleteIsUndoneByReinsertingOldValue) {
    ASSERT_TRUE(tree_->Insert(Slice("d"), Slice("orig")).ok());
    LogAndFlush(LogRecordType::kBegin, "", "", /*txn_id=*/6);
    LogAndFlush(LogRecordType::kDelete, "d", "orig", /*txn_id=*/6);
    // No kCommit for txn 6.

    RecoveryManager recovery(bpm_.get(), tree_.get(), wal_path_);
    ASSERT_TRUE(recovery.Recover().ok());

    ASSERT_TRUE(tree_->Get(Slice("d")).ok())
        << "delete from an uncommitted transaction must be rolled back";
    EXPECT_EQ(tree_->Get(Slice("d")).value(), "orig");
}

TEST_F(RecoveryManagerTest, CommittedDeleteStaysDeletedAfterRecovery) {
    ASSERT_TRUE(tree_->Insert(Slice("e"), Slice("x")).ok());
    LogAndFlush(LogRecordType::kBegin, "", "", /*txn_id=*/8);
    LogAndFlush(LogRecordType::kDelete, "e", "x", /*txn_id=*/8);
    LogAndFlush(LogRecordType::kCommit, "", "", /*txn_id=*/8);

    RecoveryManager recovery(bpm_.get(), tree_.get(), wal_path_);
    ASSERT_TRUE(recovery.Recover().ok());

    EXPECT_FALSE(tree_->Get(Slice("e")).ok());
}

TEST_F(RecoveryManagerTest, AbortedTransactionIsUndoneJustLikeUncommitted) {
    LogAndFlush(LogRecordType::kBegin, "", "", /*txn_id=*/9);
    LogAndFlush(LogRecordType::kInsert, "f", "1", /*txn_id=*/9);
    LogAndFlush(LogRecordType::kAbort, "", "", /*txn_id=*/9);

    RecoveryManager recovery(bpm_.get(), tree_.get(), wal_path_);
    ASSERT_TRUE(recovery.Recover().ok());

    EXPECT_FALSE(tree_->Get(Slice("f")).ok());
}

TEST_F(RecoveryManagerTest, RecoveringTwiceProducesTheSameResult) {
    LogAndFlush(LogRecordType::kBegin, "", "", /*txn_id=*/1);
    LogAndFlush(LogRecordType::kInsert, "g", "1", /*txn_id=*/1);
    LogAndFlush(LogRecordType::kCommit, "", "", /*txn_id=*/1);
    LogAndFlush(LogRecordType::kInsert, "h", "2");

    RecoveryManager first(bpm_.get(), tree_.get(), wal_path_);
    ASSERT_TRUE(first.Recover().ok());

    // Recovery does not truncate the WAL; re-running it against the
    // already-recovered tree must be a safe no-op.
    RecoveryManager second(bpm_.get(), tree_.get(), wal_path_);
    ASSERT_TRUE(second.Recover().ok());

    ASSERT_TRUE(tree_->Get(Slice("g")).ok());
    EXPECT_EQ(tree_->Get(Slice("g")).value(), "1");
    ASSERT_TRUE(tree_->Get(Slice("h")).ok());
    EXPECT_EQ(tree_->Get(Slice("h")).value(), "2");
}

TEST_F(RecoveryManagerTest, MultipleTransactionsAreResolvedIndependently) {
    LogAndFlush(LogRecordType::kBegin, "", "", /*txn_id=*/1);
    LogAndFlush(LogRecordType::kInsert, "k1", "committed", /*txn_id=*/1);
    LogAndFlush(LogRecordType::kCommit, "", "", /*txn_id=*/1);

    LogAndFlush(LogRecordType::kBegin, "", "", /*txn_id=*/2);
    LogAndFlush(LogRecordType::kInsert, "k2", "uncommitted", /*txn_id=*/2);
    // txn 2 never commits.

    LogAndFlush(LogRecordType::kInsert, "k3", "auto-commit");

    RecoveryManager recovery(bpm_.get(), tree_.get(), wal_path_);
    ASSERT_TRUE(recovery.Recover().ok());

    ASSERT_TRUE(tree_->Get(Slice("k1")).ok());
    EXPECT_EQ(tree_->Get(Slice("k1")).value(), "committed");
    EXPECT_FALSE(tree_->Get(Slice("k2")).ok());
    ASSERT_TRUE(tree_->Get(Slice("k3")).ok());
    EXPECT_EQ(tree_->Get(Slice("k3")).value(), "auto-commit");
}

// End-to-end simulation of a real crash: writes reach the WAL and are
// fsynced, but the buffer pool holding the modified pages is destroyed
// before it ever flushes them to disk. A fresh buffer pool/tree opened
// against the same file must recover the lost writes purely from the WAL.
TEST_F(RecoveryManagerTest, DataSurvivesSimulatedCrashViaWalRedo) {
    std::vector<std::pair<std::string, std::string>> kvs;
    kvs.reserve(30);
    for (int i = 0; i < 30; ++i) {
        kvs.emplace_back("key" + std::to_string(i), "value" + std::to_string(i));
    }

    lsn_t last_lsn = kInvalidLsn;
    for (const auto& [k, v] : kvs) {
        lsn_t lsn =
            wal_manager_
                ->AppendLogRecord(LogRecordType::kInsert, kInvalidPageId, Slice(k), Slice(v))
                .value();
        ASSERT_TRUE(tree_->Insert(Slice(k), Slice(v), lsn).ok());
        last_lsn = lsn;
    }
    // Simulate the fsync that happened right before the crash: the WAL is
    // durable, but the dirty pages in bpm_ are not.
    ASSERT_TRUE(wal_manager_->Flush(last_lsn).ok());

    // "Crash": drop the buffer pool (and everything built on top of it)
    // without ever calling FlushAllPages, discarding the dirty pages.
    tree_.reset();
    fpm_.reset();
    bpm_.reset();

    // "Restart": reopen a fresh buffer pool over the same on-disk file.
    auto bpm2 = std::make_unique<BufferPoolManager>(disk_manager_.get(), 32);
    auto fpm2 = std::make_unique<FreePageManager>(disk_manager_.get(), bpm2.get());
    auto tree2 = std::make_unique<BPlusTree>(disk_manager_.get(), bpm2.get(), fpm2.get());

    RecoveryManager recovery(bpm2.get(), tree2.get(), wal_path_);
    ASSERT_TRUE(recovery.Recover().ok());

    for (const auto& [k, v] : kvs) {
        StatusOr<std::string> got = tree2->Get(Slice(k));
        ASSERT_TRUE(got.ok()) << "missing key: " << k;
        EXPECT_EQ(got.value(), v);
    }
    EXPECT_TRUE(tree2->Verify().ok());
}

} // namespace
} // namespace engine
