#include "engine/database.h"

#include <cstdio>
#include <gtest/gtest.h>
#include <unistd.h>

namespace engine {
namespace {

class SnapshotTest : public ::testing::Test {
  protected:
    void SetUp() override {
        path_ = "test_snapshot_" + std::to_string(::getpid()) + ".db";
        std::remove(path_.c_str());
        std::remove((path_ + ".wal").c_str());
    }
    void TearDown() override {
        std::remove(path_.c_str());
        std::remove((path_ + ".wal").c_str());
        // Snapshot files are removed by ~Snapshot(), but sweep a generous
        // range in case a test left one behind (e.g. via ASSERT failure).
        for (int i = 1; i <= 10; ++i) {
            std::remove((path_ + ".snapshot." + std::to_string(i)).c_str());
        }
    }

    Options ValidOptions(uint32_t page_size = 4096, size_t buffer_pool_frames = 32) {
        Options opts;
        opts.path = path_;
        opts.page_size = page_size;
        opts.buffer_pool_frames = buffer_pool_frames;
        return opts;
    }

    static bool FileExists(const std::string& path) {
        return ::access(path.c_str(), F_OK) == 0;
    }

    std::string path_;
};

TEST_F(SnapshotTest, CreateSnapshotBeforeOpenFails) {
    Database db(ValidOptions());
    StatusOr<Snapshot> snap_or = db.CreateSnapshot();
    EXPECT_FALSE(snap_or.ok());
}

TEST_F(SnapshotTest, SnapshotOfAnEmptyDatabaseIsValidAndEmpty) {
    Database db(ValidOptions());
    ASSERT_TRUE(db.Open().ok());

    StatusOr<Snapshot> snap_or = db.CreateSnapshot();
    ASSERT_TRUE(snap_or.ok());
    Snapshot snap = std::move(snap_or.value());

    EXPECT_TRUE(snap.is_valid());
    EXPECT_FALSE(snap.Get(Slice("missing")).ok());
}

TEST_F(SnapshotTest, SnapshotSeesDataWrittenBeforeItWasCreated) {
    Database db(ValidOptions());
    ASSERT_TRUE(db.Open().ok());
    ASSERT_TRUE(db.Put(Slice("a"), Slice("1")).ok());
    ASSERT_TRUE(db.Put(Slice("b"), Slice("2")).ok());

    StatusOr<Snapshot> snap_or = db.CreateSnapshot();
    ASSERT_TRUE(snap_or.ok());
    Snapshot snap = std::move(snap_or.value());

    EXPECT_EQ(snap.Get(Slice("a")).value(), "1");
    EXPECT_EQ(snap.Get(Slice("b")).value(), "2");
}

TEST_F(SnapshotTest, SnapshotDoesNotSeeKeysWrittenAfterItWasCreated) {
    Database db(ValidOptions());
    ASSERT_TRUE(db.Open().ok());
    ASSERT_TRUE(db.Put(Slice("a"), Slice("1")).ok());

    StatusOr<Snapshot> snap_or = db.CreateSnapshot();
    ASSERT_TRUE(snap_or.ok());
    Snapshot snap = std::move(snap_or.value());

    ASSERT_TRUE(db.Put(Slice("b"), Slice("2")).ok());

    EXPECT_EQ(snap.Get(Slice("a")).value(), "1");
    EXPECT_FALSE(snap.Get(Slice("b")).ok());
    EXPECT_EQ(db.Get(Slice("b")).value(), "2")
        << "the live database must still see the post-snapshot write";
}

TEST_F(SnapshotTest, SnapshotIsUnaffectedByALaterUpdateToAnExistingKey) {
    Database db(ValidOptions());
    ASSERT_TRUE(db.Open().ok());
    ASSERT_TRUE(db.Put(Slice("k"), Slice("original")).ok());

    StatusOr<Snapshot> snap_or = db.CreateSnapshot();
    ASSERT_TRUE(snap_or.ok());
    Snapshot snap = std::move(snap_or.value());

    ASSERT_TRUE(db.Put(Slice("k"), Slice("updated")).ok());

    EXPECT_EQ(snap.Get(Slice("k")).value(), "original");
    EXPECT_EQ(db.Get(Slice("k")).value(), "updated");
}

TEST_F(SnapshotTest, SnapshotIsUnaffectedByALaterRemovalOfAKey) {
    Database db(ValidOptions());
    ASSERT_TRUE(db.Open().ok());
    ASSERT_TRUE(db.Put(Slice("k"), Slice("v")).ok());

    StatusOr<Snapshot> snap_or = db.CreateSnapshot();
    ASSERT_TRUE(snap_or.ok());
    Snapshot snap = std::move(snap_or.value());

    ASSERT_TRUE(db.Remove(Slice("k")).ok());

    EXPECT_EQ(snap.Get(Slice("k")).value(), "v")
        << "removing the key from the live database must not affect the snapshot's copy";
    EXPECT_FALSE(db.Get(Slice("k")).ok());
}

TEST_F(SnapshotTest, MultipleSnapshotsEachCaptureTheirOwnPointInTime) {
    Database db(ValidOptions());
    ASSERT_TRUE(db.Open().ok());
    ASSERT_TRUE(db.Put(Slice("k"), Slice("v1")).ok());

    Snapshot snap1 = std::move(db.CreateSnapshot().value());

    ASSERT_TRUE(db.Put(Slice("k"), Slice("v2")).ok());

    Snapshot snap2 = std::move(db.CreateSnapshot().value());

    ASSERT_TRUE(db.Put(Slice("k"), Slice("v3")).ok());

    EXPECT_EQ(snap1.Get(Slice("k")).value(), "v1");
    EXPECT_EQ(snap2.Get(Slice("k")).value(), "v2");
    EXPECT_EQ(db.Get(Slice("k")).value(), "v3");
}

TEST_F(SnapshotTest, SnapshotSurvivesCloseOfTheLiveDatabase) {
    Database db(ValidOptions());
    ASSERT_TRUE(db.Open().ok());
    ASSERT_TRUE(db.Put(Slice("k"), Slice("v")).ok());

    Snapshot snap = std::move(db.CreateSnapshot().value());

    ASSERT_TRUE(db.Close().ok());

    EXPECT_TRUE(snap.is_valid());
    EXPECT_EQ(snap.Get(Slice("k")).value(), "v")
        << "the snapshot owns an independent storage stack and copy of the file, "
           "so it must remain readable after the live database closes";
}

TEST_F(SnapshotTest, SnapshotFileIsRemovedWhenTheSnapshotIsDestroyed) {
    Database db(ValidOptions());
    ASSERT_TRUE(db.Open().ok());
    ASSERT_TRUE(db.Put(Slice("k"), Slice("v")).ok());

    std::string snapshot_path = path_ + ".snapshot.1";
    {
        Snapshot snap = std::move(db.CreateSnapshot().value());
        EXPECT_TRUE(FileExists(snapshot_path));
    }
    EXPECT_FALSE(FileExists(snapshot_path))
        << "~Snapshot() should remove its backing copy of the database file";
}

TEST_F(SnapshotTest, SequentialSnapshotsUseDistinctBackingFiles) {
    Database db(ValidOptions());
    ASSERT_TRUE(db.Open().ok());
    ASSERT_TRUE(db.Put(Slice("k"), Slice("v")).ok());

    Snapshot snap1 = std::move(db.CreateSnapshot().value());
    Snapshot snap2 = std::move(db.CreateSnapshot().value());

    EXPECT_TRUE(FileExists(path_ + ".snapshot.1"));
    EXPECT_TRUE(FileExists(path_ + ".snapshot.2"));

    EXPECT_EQ(snap1.Get(Slice("k")).value(), "v");
    EXPECT_EQ(snap2.Get(Slice("k")).value(), "v");
}

TEST_F(SnapshotTest, SnapshotReflectsALargerWrittenDataset) {
    Database db(ValidOptions());
    ASSERT_TRUE(db.Open().ok());
    for (int i = 0; i < 200; ++i) {
        ASSERT_TRUE(
            db.Put(Slice("key" + std::to_string(i)), Slice("val" + std::to_string(i))).ok());
    }

    Snapshot snap = std::move(db.CreateSnapshot().value());

    for (int i = 0; i < 200; ++i) {
        StatusOr<std::string> v = snap.Get(Slice("key" + std::to_string(i)));
        ASSERT_TRUE(v.ok()) << "missing key" << i;
        EXPECT_EQ(v.value(), "val" + std::to_string(i));
    }
}

} // namespace
} // namespace engine
