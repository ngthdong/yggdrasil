#include "engine/b_plus_tree.h"
#include "engine/checkpoint_manager.h"
#include <cstdio>
#include <cstring>
#include <gtest/gtest.h>
namespace engine {
namespace {
class CheckpointManagerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        path_ = "test_ckpt_" + std::to_string(::getpid()) + ".db";
        wal_path_ = path_ + ".wal";
        std::remove(path_.c_str());
        std::remove(wal_path_.c_str());
        disk_manager_ = DiskManager::Open(path_, 512, true).value();
        wal_manager_ = WalManager::Open(wal_path_).value();
        bpm_ = std::make_unique<BufferPoolManager>(disk_manager_.get(), 32);
        bpm_->SetWalManager(wal_manager_.get());
        fpm_ = std::make_unique<FreePageManager>(disk_manager_.get(), bpm_.get());
        tree_ = std::make_unique<BPlusTree>(disk_manager_.get(), bpm_.get(), fpm_.get());
        ckpt_ = std::make_unique<CheckpointManager>(
            disk_manager_.get(), bpm_.get(), wal_manager_.get());
    }
    void TearDown() override {
        ckpt_.reset();
        tree_.reset();
        fpm_.reset();
        bpm_.reset();
        wal_manager_.reset();
        disk_manager_.reset();
        std::remove(path_.c_str());
        std::remove(wal_path_.c_str());
    }
    long WalFileSize() {
        FILE* f = std::fopen(wal_path_.c_str(), "rb");
        if (f == nullptr) {
            return -1;
        }
        std::fseek(f, 0, SEEK_END);
        long size = std::ftell(f);
        std::fclose(f);
        return size;
    }
    std::string path_, wal_path_;
    std::unique_ptr<DiskManager> disk_manager_;
    std::unique_ptr<WalManager> wal_manager_;
    std::unique_ptr<BufferPoolManager> bpm_;
    std::unique_ptr<FreePageManager> fpm_;
    std::unique_ptr<BPlusTree> tree_;
    std::unique_ptr<CheckpointManager> ckpt_;
};

TEST_F(CheckpointManagerTest, CheckpointOnFreshDatabaseSucceeds) {
    EXPECT_TRUE(ckpt_->TakeCheckpoint().ok());
}

TEST_F(CheckpointManagerTest, CheckpointFlushesAllDirtyPages) {
    lsn_t lsn =
        wal_manager_
            ->AppendLogRecord(LogRecordType::kInsert, kInvalidPageId, Slice("k"), Slice("v"))
            .value();
    ASSERT_TRUE(tree_->Insert(Slice("a"), Slice("1"), lsn).ok());
    page_id_t root = disk_manager_->GetRootPageId();
    EXPECT_GT(bpm_->GetPageLSN(root), kInvalidLsn);

    ASSERT_TRUE(ckpt_->TakeCheckpoint().ok());

    ASSERT_TRUE(bpm_->FlushAllPages().ok());
    auto dm2 = DiskManager::Open(path_, 512, false).value();
    std::vector<char> buf(512);
    ASSERT_TRUE(dm2->ReadPage(root, buf.data()).ok());
    std::string page_str(buf.data(), buf.size());
    EXPECT_NE(page_str.find('a'), std::string::npos);
}

TEST_F(CheckpointManagerTest, CheckpointPersistsLastCheckpointLsnAcrossReopen) {
    lsn_t lsn =
        wal_manager_
            ->AppendLogRecord(LogRecordType::kInsert, kInvalidPageId, Slice("k"), Slice("v"))
            .value();
    ASSERT_TRUE(tree_->Insert(Slice("a"), Slice("1"), lsn).ok());
    ASSERT_TRUE(ckpt_->TakeCheckpoint().ok());
    lsn_t recorded = disk_manager_->GetLastCheckpointLsn();
    EXPECT_GT(recorded, kInvalidLsn);

    ASSERT_TRUE(bpm_->FlushAllPages().ok());
    ASSERT_TRUE(disk_manager_->Shutdown().ok());
    auto dm2 = DiskManager::Open(path_, 512, false).value();
    EXPECT_EQ(dm2->GetLastCheckpointLsn(), recorded);
}

TEST_F(CheckpointManagerTest, CheckpointRecyclesWalFile) {
    lsn_t last_lsn = kInvalidLsn;
    for (int i = 0; i < 20; ++i) {
        last_lsn = wal_manager_
                       ->AppendLogRecord(LogRecordType::kInsert,
                                         kInvalidPageId,
                                         Slice("k" + std::to_string(i)),
                                         Slice("v"))
                       .value();
        ASSERT_TRUE(
            tree_->Insert(Slice("k" + std::to_string(i)), Slice("v" + std::to_string(i)), last_lsn)
                .ok());
    }
    ASSERT_TRUE(wal_manager_->Flush(last_lsn).ok());
    long before = WalFileSize();
    EXPECT_GT(before, 0);
    ASSERT_TRUE(ckpt_->TakeCheckpoint().ok());
    long after = WalFileSize();
    EXPECT_EQ(after, 0) << "WAL file should be truncated to empty after a checkpoint";
}

TEST_F(CheckpointManagerTest, DataCorrectAfterCheckpointAndMoreWrites) {
    for (int i = 0; i < 50; ++i) {
        lsn_t lsn = wal_manager_
                        ->AppendLogRecord(LogRecordType::kInsert,
                                          kInvalidPageId,
                                          Slice("k" + std::to_string(i)),
                                          Slice("v"))
                        .value();
        ASSERT_TRUE(
            tree_->Insert(Slice("k" + std::to_string(i)), Slice("v" + std::to_string(i)), lsn)
                .ok());
    }
    ASSERT_TRUE(ckpt_->TakeCheckpoint().ok());
    for (int i = 50; i < 100; ++i) {
        lsn_t lsn = wal_manager_
                        ->AppendLogRecord(LogRecordType::kInsert,
                                          kInvalidPageId,
                                          Slice("k" + std::to_string(i)),
                                          Slice("v"))
                        .value();
        ASSERT_TRUE(
            tree_->Insert(Slice("k" + std::to_string(i)), Slice("v" + std::to_string(i)), lsn)
                .ok());
    }
    ASSERT_TRUE(ckpt_->TakeCheckpoint().ok());
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(tree_->Get(Slice("k" + std::to_string(i))).value(), "v" + std::to_string(i));
    }
    ASSERT_TRUE(tree_->Verify().ok());
}

TEST_F(CheckpointManagerTest, RepeatedCheckpointsWithNoActivityAreHarmless) {
    ASSERT_TRUE(ckpt_->TakeCheckpoint().ok());
    ASSERT_TRUE(ckpt_->TakeCheckpoint().ok());
    ASSERT_TRUE(ckpt_->TakeCheckpoint().ok());
    EXPECT_EQ(WalFileSize(), 0);
}

TEST_F(CheckpointManagerTest, WalFileSizeStaysBoundedUnderSteadyCheckpointedChurn) {
    for (int batch = 0; batch < 10; ++batch) {
        for (int i = 0; i < 20; ++i) {
            std::string key =
                "k" + std::to_string((batch * 20 + i) % 30); // small working set, lots of churn
            lsn_t lsn = wal_manager_
                            ->AppendLogRecord(
                                LogRecordType::kInsert, kInvalidPageId, Slice(key), Slice("v"))
                            .value();
            tree_->Remove(Slice(key)); // may fail if not present, fine either way
            ASSERT_TRUE(tree_->Insert(Slice(key), Slice("v"), lsn).ok());
        }
        ASSERT_TRUE(ckpt_->TakeCheckpoint().ok());
    }

    EXPECT_EQ(WalFileSize(), 0);
}

} // namespace
} // namespace engine
