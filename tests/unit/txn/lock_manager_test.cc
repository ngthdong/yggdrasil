#include "engine/lock_manager.h"

#include <chrono>
#include <future>
#include <thread>

#include <gtest/gtest.h>

namespace engine {
namespace {

class LockManagerTest : public ::testing::Test {
  protected:
    // Waits up to `timeout` for `fut` to become ready and reports whether the
    // request is still blocked. Never joins or detaches `t` — callers decide
    // what to do next based on the returned state.
    static bool StillBlocked(std::future<Status>& fut,
                             std::chrono::milliseconds timeout = std::chrono::milliseconds(150)) {
        return fut.wait_for(timeout) == std::future_status::timeout;
    }

    // Waits (generously, but boundedly) for `fut` to become ready and joins
    // `t`. If the request never completes, a LockManager bug has caused a
    // permanent deadlock; rather than hang the whole test binary, this fails
    // the test and detaches the thread instead of joining it.
    static Status WaitForResult(std::future<Status>& fut,
                                std::thread& t,
                                std::chrono::seconds timeout = std::chrono::seconds(5)) {
        if (fut.wait_for(timeout) != std::future_status::ready) {
            ADD_FAILURE() << "AcquireLock did not complete within the timeout; "
                             "likely a deadlock in LockManager";
            t.detach();
            return Status::IOError("timed out waiting for AcquireLock");
        }
        t.join();
        return fut.get();
    }

    LockManager lm_;
};

TEST_F(LockManagerTest, SharedLockIsAcquiredImmediatelyWhenUncontended) {
    EXPECT_TRUE(lm_.AcquireLock(1, "k", LockMode::kShared).ok());
}

TEST_F(LockManagerTest, ExclusiveLockIsAcquiredImmediatelyWhenUncontended) {
    EXPECT_TRUE(lm_.AcquireLock(1, "k", LockMode::kExclusive).ok());
}

TEST_F(LockManagerTest, ReacquiringTheSameModeIsANoop) {
    ASSERT_TRUE(lm_.AcquireLock(1, "k", LockMode::kExclusive).ok());
    EXPECT_TRUE(lm_.AcquireLock(1, "k", LockMode::kExclusive).ok());
}

TEST_F(LockManagerTest, RequestingSharedWhileAlreadyHoldingExclusiveIsANoop) {
    ASSERT_TRUE(lm_.AcquireLock(1, "k", LockMode::kExclusive).ok());
    EXPECT_TRUE(lm_.AcquireLock(1, "k", LockMode::kShared).ok());
}

TEST_F(LockManagerTest, SoleSharedHolderCanUpgradeToExclusiveWithoutBlocking) {
    ASSERT_TRUE(lm_.AcquireLock(1, "k", LockMode::kShared).ok());
    EXPECT_TRUE(lm_.AcquireLock(1, "k", LockMode::kExclusive).ok());
}

TEST_F(LockManagerTest, CompatibleSharedLocksFromDifferentTransactionsBothSucceed) {
    EXPECT_TRUE(lm_.AcquireLock(1, "k", LockMode::kShared).ok());
    EXPECT_TRUE(lm_.AcquireLock(2, "k", LockMode::kShared).ok());
}

TEST_F(LockManagerTest, LocksOnDifferentResourcesDoNotConflict) {
    EXPECT_TRUE(lm_.AcquireLock(1, "a", LockMode::kExclusive).ok());
    EXPECT_TRUE(lm_.AcquireLock(2, "b", LockMode::kExclusive).ok());
}

TEST_F(LockManagerTest, IsAbortedIsFalseForATransactionThatNeverConflicted) {
    EXPECT_FALSE(lm_.IsAborted(1));
    ASSERT_TRUE(lm_.AcquireLock(1, "k", LockMode::kShared).ok());
    EXPECT_FALSE(lm_.IsAborted(1));
}

TEST_F(LockManagerTest, ReleaseAllLocksOnATransactionHoldingNothingIsHarmless) {
    lm_.ReleaseAllLocks(42);
    EXPECT_FALSE(lm_.IsAborted(42));
}

TEST_F(LockManagerTest, ReleaseAllLocksFreesTheResourceForOtherTransactions) {
    ASSERT_TRUE(lm_.AcquireLock(1, "k", LockMode::kExclusive).ok());
    lm_.ReleaseAllLocks(1);
    EXPECT_TRUE(lm_.AcquireLock(2, "k", LockMode::kExclusive).ok());
}

// --- Real blocking / wound-wait behavior, exercised across threads ---

TEST_F(LockManagerTest, YoungerRequesterWaitsForOlderHolderRatherThanWoundingIt) {
    constexpr txn_id_t kOlder = 1;
    constexpr txn_id_t kYounger = 5;

    ASSERT_TRUE(lm_.AcquireLock(kOlder, "k", LockMode::kExclusive).ok());

    std::promise<Status> promise;
    std::future<Status> fut = promise.get_future();
    std::thread requester(
        [&] { promise.set_value(lm_.AcquireLock(kYounger, "k", LockMode::kExclusive)); });

    EXPECT_TRUE(StillBlocked(fut)) << "younger requester should wait, not proceed";
    EXPECT_FALSE(lm_.IsAborted(kOlder)) << "younger requester must not wound the older holder";

    lm_.ReleaseAllLocks(kOlder);

    Status result = WaitForResult(fut, requester);
    EXPECT_TRUE(result.ok());
}

TEST_F(LockManagerTest, OlderRequesterWoundsYoungerHolderAndWaitsForItToRelease) {
    constexpr txn_id_t kOlder = 1;
    constexpr txn_id_t kYounger = 5;

    ASSERT_TRUE(lm_.AcquireLock(kYounger, "k", LockMode::kExclusive).ok());

    std::promise<Status> promise;
    std::future<Status> fut = promise.get_future();
    std::thread requester(
        [&] { promise.set_value(lm_.AcquireLock(kOlder, "k", LockMode::kExclusive)); });

    bool wounded = false;
    for (int i = 0; i < 50 && !wounded; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        wounded = lm_.IsAborted(kYounger);
    }
    EXPECT_TRUE(wounded) << "older requester should wound the younger lock holder";
    EXPECT_TRUE(StillBlocked(fut)) << "older requester still waits for the victim to release";

    // Simulate the wounded transaction noticing the abort and rolling back,
    // the way Transaction::SelfRollbackAfterWound does.
    lm_.ReleaseAllLocks(kYounger);

    Status result = WaitForResult(fut, requester);
    EXPECT_TRUE(result.ok());
}

TEST_F(LockManagerTest, OlderExclusiveRequestWoundsAllYoungerSharedHolders) {
    constexpr txn_id_t kOlder = 1;
    constexpr txn_id_t kYoungerA = 5;
    constexpr txn_id_t kYoungerB = 6;

    ASSERT_TRUE(lm_.AcquireLock(kYoungerA, "k", LockMode::kShared).ok());
    ASSERT_TRUE(lm_.AcquireLock(kYoungerB, "k", LockMode::kShared).ok());

    std::promise<Status> promise;
    std::future<Status> fut = promise.get_future();
    std::thread requester(
        [&] { promise.set_value(lm_.AcquireLock(kOlder, "k", LockMode::kExclusive)); });

    bool both_wounded = false;
    for (int i = 0; i < 50 && !both_wounded; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        both_wounded = lm_.IsAborted(kYoungerA) && lm_.IsAborted(kYoungerB);
    }
    EXPECT_TRUE(both_wounded) << "both younger shared holders should be wounded";

    lm_.ReleaseAllLocks(kYoungerA);
    EXPECT_TRUE(StillBlocked(fut)) << "kYoungerB still holds the lock";

    lm_.ReleaseAllLocks(kYoungerB);

    Status result = WaitForResult(fut, requester);
    EXPECT_TRUE(result.ok());
}

TEST_F(LockManagerTest, RetryByAnAlreadyWoundedTransactionFailsImmediatelyWithoutBlocking) {
    constexpr txn_id_t kOlder = 1;
    constexpr txn_id_t kYounger = 5;

    ASSERT_TRUE(lm_.AcquireLock(kYounger, "k", LockMode::kExclusive).ok());

    std::promise<Status> promise;
    std::future<Status> fut = promise.get_future();
    std::thread requester(
        [&] { promise.set_value(lm_.AcquireLock(kOlder, "k", LockMode::kExclusive)); });

    bool wounded = false;
    for (int i = 0; i < 50 && !wounded; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        wounded = lm_.IsAborted(kYounger);
    }
    ASSERT_TRUE(wounded);

    // A retry on a completely unrelated resource must fail immediately
    // rather than block, since the early kAborted check runs before any
    // lock-table lookup.
    Status retry_s = lm_.AcquireLock(kYounger, "unrelated-resource", LockMode::kShared);
    EXPECT_FALSE(retry_s.ok());
    EXPECT_EQ(retry_s.code(), Status::Code::kAborted);

    lm_.ReleaseAllLocks(kYounger);
    Status result = WaitForResult(fut, requester);
    EXPECT_TRUE(result.ok());
}

} // namespace
} // namespace engine
