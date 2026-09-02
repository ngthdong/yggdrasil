#include "engine/database.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <gtest/gtest.h>
#include <thread>

namespace engine {
namespace {
using namespace std::chrono_literals;

class DeadlockDetectionIntegrationTest : public ::testing::Test {
  protected:
    void SetUp() override {
        path_ = "test_deadlock_" + std::to_string(::getpid()) + ".db";
        std::remove(path_.c_str());
        std::remove((path_ + ".wal").c_str());
    }
    void TearDown() override {
        std::remove(path_.c_str());
        std::remove((path_ + ".wal").c_str());
    }
    Options DetectionOptions() {
        Options o;
        o.path = path_;
        o.page_size = 512;
        o.buffer_pool_frames = 32;
        o.deadlock_policy = DeadlockPolicy::kDetection;
        o.deadlock_detection_interval = 15ms;
        return o;
    }
    std::string path_;
};

TEST_F(DeadlockDetectionIntegrationTest, DetectionPolicyResolvesRealCircularWaitEndToEnd) {
    Database db(DetectionOptions());
    ASSERT_TRUE(db.Open().ok());
    ASSERT_TRUE(db.Put(Slice("A"), Slice("a0")).ok());
    ASSERT_TRUE(db.Put(Slice("B"), Slice("b0")).ok());

    std::atomic<int> old_ok{-1};
    std::atomic<int> young_ok{-1};
    std::thread t_old([&]() {
        auto txn = std::move(db.BeginTransaction().value());
        ASSERT_TRUE(txn.Put(Slice("A"), Slice("old")).ok());
        std::this_thread::sleep_for(150ms);
        Status s = txn.Put(Slice("B"), Slice("old"));
        old_ok = s.ok() ? 1 : 0;
        if (s.ok()) {
            ASSERT_TRUE(txn.Commit().ok());
        }
    });
    std::this_thread::sleep_for(30ms);
    std::thread t_young([&]() {
        auto txn = std::move(db.BeginTransaction().value());
        ASSERT_TRUE(txn.Put(Slice("B"), Slice("young")).ok());
        std::this_thread::sleep_for(150ms);
        Status s = txn.Put(Slice("A"), Slice("young"));
        young_ok = s.ok() ? 1 : 0;
        if (s.ok()) {
            ASSERT_TRUE(txn.Commit().ok());
        }
    });
    t_old.join();
    t_young.join();

    EXPECT_EQ(old_ok.load() + young_ok.load(), 1);
    EXPECT_TRUE(db.Verify().ok());
    std::string a = db.Get(Slice("A")).value();
    std::string b = db.Get(Slice("B")).value();
    EXPECT_EQ(a, b);
}

TEST_F(DeadlockDetectionIntegrationTest, NonConflictingConcurrentTransactionsUnaffectedByDetector) {
    Database db(DetectionOptions());
    ASSERT_TRUE(db.Open().ok());
    std::atomic<int> both_done{0};
    std::thread t1([&]() {
        auto txn = std::move(db.BeginTransaction().value());
        ASSERT_TRUE(txn.Put(Slice("x"), Slice("1")).ok());
        ASSERT_TRUE(txn.Commit().ok());
        ++both_done;
    });
    std::thread t2([&]() {
        auto txn = std::move(db.BeginTransaction().value());
        ASSERT_TRUE(txn.Put(Slice("y"), Slice("2")).ok());
        ASSERT_TRUE(txn.Commit().ok());
        ++both_done;
    });
    t1.join();
    t2.join();
    EXPECT_EQ(both_done.load(), 2);
    EXPECT_EQ(db.Get(Slice("x")).value(), "1");
    EXPECT_EQ(db.Get(Slice("y")).value(), "2");
}

TEST_F(DeadlockDetectionIntegrationTest, DetectionModeSurvivesCloseWhileNoContention) {
    Database db(DetectionOptions());
    ASSERT_TRUE(db.Open().ok());
    ASSERT_TRUE(db.Put(Slice("a"), Slice("1")).ok());
    ASSERT_TRUE(db.Close().ok()); // must cleanly stop the background detector thread
    SUCCEED();
}

} // namespace
} // namespace engine
