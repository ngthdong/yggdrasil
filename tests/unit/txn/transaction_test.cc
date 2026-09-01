#include "engine/database.h"

#include <cstdio>
#include <gtest/gtest.h>

namespace engine {
namespace {

class TransactionTest : public ::testing::Test {
  protected:
    void SetUp() override {
        path_ = "test_txn_" + std::to_string(::getpid()) + ".db";
        std::remove(path_.c_str());
        std::remove((path_ + ".wal").c_str());

        Options opts;
        opts.path = path_;
        db_ = std::make_unique<Database>(opts);
        ASSERT_TRUE(db_->Open().ok());
    }

    void TearDown() override {
        db_.reset();
        std::remove(path_.c_str());
        std::remove((path_ + ".wal").c_str());
    }

    std::string path_;
    std::unique_ptr<Database> db_;
};

TEST_F(TransactionTest, BeginTransactionFailsWhenDatabaseNotOpen) {
    Database closed_db(Options{.path = path_ + "_unopened"});
    StatusOr<Transaction> txn_or = closed_db.BeginTransaction();
    EXPECT_FALSE(txn_or.ok());
}

TEST_F(TransactionTest, BeginTransactionSucceedsAndMarksDatabaseActive) {
    EXPECT_FALSE(db_->has_active_transaction());

    StatusOr<Transaction> txn_or = db_->BeginTransaction();
    ASSERT_TRUE(txn_or.ok());
    EXPECT_TRUE(txn_or.value().is_active());
    EXPECT_TRUE(db_->has_active_transaction());
}

TEST_F(TransactionTest, MultipleTransactionsCanBeActiveConcurrently) {
    StatusOr<Transaction> first_or = db_->BeginTransaction();
    ASSERT_TRUE(first_or.ok());
    Transaction first = std::move(first_or.value());
    EXPECT_TRUE(first.is_active());

    StatusOr<Transaction> second_or = db_->BeginTransaction();
    ASSERT_TRUE(second_or.ok());
    Transaction second = std::move(second_or.value());
    EXPECT_TRUE(second.is_active());

    EXPECT_TRUE(db_->has_active_transaction());

    ASSERT_TRUE(first.Commit().ok());
    EXPECT_TRUE(db_->has_active_transaction()) << "second transaction is still active";

    ASSERT_TRUE(second.Commit().ok());
    EXPECT_FALSE(db_->has_active_transaction());
}

TEST_F(TransactionTest, DirectPutIsRejectedWhileTransactionIsActive) {
    StatusOr<Transaction> txn_or = db_->BeginTransaction();
    ASSERT_TRUE(txn_or.ok());

    EXPECT_FALSE(db_->Put(Slice("a"), Slice("1")).ok());
}

TEST_F(TransactionTest, DirectRemoveIsRejectedWhileTransactionIsActive) {
    ASSERT_TRUE(db_->Put(Slice("a"), Slice("1")).ok());

    StatusOr<Transaction> txn_or = db_->BeginTransaction();
    ASSERT_TRUE(txn_or.ok());

    EXPECT_FALSE(db_->Remove(Slice("a")).ok());
}

TEST_F(TransactionTest, CommittedPutOfNewKeyIsVisibleAfterCommit) {
    StatusOr<Transaction> txn_or = db_->BeginTransaction();
    ASSERT_TRUE(txn_or.ok());
    Transaction txn = std::move(txn_or.value());

    ASSERT_TRUE(txn.Put(Slice("a"), Slice("1")).ok());
    ASSERT_TRUE(txn.Commit().ok());

    StatusOr<std::string> v = db_->Get(Slice("a"));
    ASSERT_TRUE(v.ok());
    EXPECT_EQ(v.value(), "1");
}

TEST_F(TransactionTest, CommittedPutUpdatesExistingKey) {
    ASSERT_TRUE(db_->Put(Slice("a"), Slice("1")).ok());

    Transaction txn = std::move(db_->BeginTransaction().value());
    ASSERT_TRUE(txn.Put(Slice("a"), Slice("2")).ok());
    ASSERT_TRUE(txn.Commit().ok());

    StatusOr<std::string> v = db_->Get(Slice("a"));
    ASSERT_TRUE(v.ok());
    EXPECT_EQ(v.value(), "2");
}

TEST_F(TransactionTest, CommittedRemoveDeletesKey) {
    ASSERT_TRUE(db_->Put(Slice("a"), Slice("1")).ok());

    Transaction txn = std::move(db_->BeginTransaction().value());
    ASSERT_TRUE(txn.Remove(Slice("a")).ok());
    ASSERT_TRUE(txn.Commit().ok());

    EXPECT_FALSE(db_->Get(Slice("a")).ok());
}

TEST_F(TransactionTest, RemovingMissingKeyFailsButDoesNotPoisonTheTransaction) {
    Transaction txn = std::move(db_->BeginTransaction().value());

    Status remove_s = txn.Remove(Slice("missing"));
    EXPECT_FALSE(remove_s.ok());
    EXPECT_EQ(remove_s.code(), Status::Code::kNotFound);

    // A NotFound on Remove is not an internal error; the transaction should
    // still be committable.
    EXPECT_TRUE(txn.Commit().ok());
}

TEST_F(TransactionTest, MultiKeyTransactionCommitsAllChangesAtomically) {
    ASSERT_TRUE(db_->Put(Slice("keep"), Slice("0")).ok());
    ASSERT_TRUE(db_->Put(Slice("del"), Slice("0")).ok());

    Transaction txn = std::move(db_->BeginTransaction().value());
    ASSERT_TRUE(txn.Put(Slice("new"), Slice("1")).ok());
    ASSERT_TRUE(txn.Put(Slice("keep"), Slice("updated")).ok());
    ASSERT_TRUE(txn.Remove(Slice("del")).ok());
    ASSERT_TRUE(txn.Commit().ok());

    EXPECT_EQ(db_->Get(Slice("new")).value(), "1");
    EXPECT_EQ(db_->Get(Slice("keep")).value(), "updated");
    EXPECT_FALSE(db_->Get(Slice("del")).ok());
}

TEST_F(TransactionTest, CommitClearsActiveTransactionAllowingDirectOpsAgain) {
    Transaction txn = std::move(db_->BeginTransaction().value());
    ASSERT_TRUE(txn.Put(Slice("a"), Slice("1")).ok());
    ASSERT_TRUE(txn.Commit().ok());

    EXPECT_FALSE(db_->has_active_transaction());
    EXPECT_TRUE(db_->Put(Slice("b"), Slice("2")).ok());
}

TEST_F(TransactionTest, RollbackUndoesInsertOfNewKey) {
    Transaction txn = std::move(db_->BeginTransaction().value());
    ASSERT_TRUE(txn.Put(Slice("x"), Slice("1")).ok());
    ASSERT_TRUE(txn.Rollback().ok());

    EXPECT_FALSE(db_->Get(Slice("x")).ok());
}

TEST_F(TransactionTest, RollbackRestoresPreviousValueOnUpdate) {
    ASSERT_TRUE(db_->Put(Slice("x"), Slice("orig")).ok());

    Transaction txn = std::move(db_->BeginTransaction().value());
    ASSERT_TRUE(txn.Put(Slice("x"), Slice("new")).ok());
    ASSERT_TRUE(txn.Rollback().ok());

    StatusOr<std::string> v = db_->Get(Slice("x"));
    ASSERT_TRUE(v.ok());
    EXPECT_EQ(v.value(), "orig");
}

TEST_F(TransactionTest, RollbackRestoresRemovedKey) {
    ASSERT_TRUE(db_->Put(Slice("x"), Slice("orig")).ok());

    Transaction txn = std::move(db_->BeginTransaction().value());
    ASSERT_TRUE(txn.Remove(Slice("x")).ok());
    ASSERT_TRUE(txn.Rollback().ok());

    StatusOr<std::string> v = db_->Get(Slice("x"));
    ASSERT_TRUE(v.ok());
    EXPECT_EQ(v.value(), "orig");
}

TEST_F(TransactionTest, RollbackClearsActiveTransactionAllowingDirectOpsAgain) {
    Transaction txn = std::move(db_->BeginTransaction().value());
    ASSERT_TRUE(txn.Put(Slice("x"), Slice("1")).ok());
    ASSERT_TRUE(txn.Rollback().ok());

    EXPECT_FALSE(db_->has_active_transaction());
    EXPECT_TRUE(db_->Put(Slice("y"), Slice("2")).ok());
}

TEST_F(TransactionTest, UnfinishedTransactionIsRolledBackWhenDestroyed) {
    {
        Transaction txn = std::move(db_->BeginTransaction().value());
        ASSERT_TRUE(txn.Put(Slice("x"), Slice("1")).ok());
        // txn goes out of scope here without Commit() or Rollback().
    }

    EXPECT_FALSE(db_->has_active_transaction());
    EXPECT_FALSE(db_->Get(Slice("x")).ok());
}

TEST_F(TransactionTest, UnfinishedTransactionRollbackSurvivesCloseAndReopen) {
    {
        Transaction txn = std::move(db_->BeginTransaction().value());
        ASSERT_TRUE(txn.Put(Slice("x"), Slice("1")).ok());
    }

    ASSERT_TRUE(db_->Close().ok());
    ASSERT_TRUE(db_->Open().ok());

    EXPECT_FALSE(db_->Get(Slice("x")).ok());
}

TEST_F(TransactionTest, CommitTwiceFailsOnSecondCall) {
    Transaction txn = std::move(db_->BeginTransaction().value());
    ASSERT_TRUE(txn.Put(Slice("a"), Slice("1")).ok());
    ASSERT_TRUE(txn.Commit().ok());

    EXPECT_FALSE(txn.Commit().ok());
}

TEST_F(TransactionTest, RollbackAfterCommitFails) {
    Transaction txn = std::move(db_->BeginTransaction().value());
    ASSERT_TRUE(txn.Put(Slice("a"), Slice("1")).ok());
    ASSERT_TRUE(txn.Commit().ok());

    EXPECT_FALSE(txn.Rollback().ok());
}

TEST_F(TransactionTest, CommitAfterRollbackFails) {
    Transaction txn = std::move(db_->BeginTransaction().value());
    ASSERT_TRUE(txn.Put(Slice("a"), Slice("1")).ok());
    ASSERT_TRUE(txn.Rollback().ok());

    EXPECT_FALSE(txn.Commit().ok());
}

TEST_F(TransactionTest, IsActiveReflectsLifecycleState) {
    Transaction txn = std::move(db_->BeginTransaction().value());
    EXPECT_TRUE(txn.is_active());

    ASSERT_TRUE(txn.Commit().ok());
    EXPECT_FALSE(txn.is_active());
}

TEST_F(TransactionTest, MoveConstructionTransfersActiveState) {
    Transaction original = std::move(db_->BeginTransaction().value());
    ASSERT_TRUE(original.Put(Slice("a"), Slice("1")).ok());

    Transaction moved(std::move(original));
    // Checking the moved-from object's state is the point of this test;
    // Transaction's move constructor leaves it well-defined (moved_from_ = true).
    // NOLINTNEXTLINE(bugprone-use-after-move)
    EXPECT_FALSE(original.is_active());
    EXPECT_TRUE(moved.is_active());

    ASSERT_TRUE(moved.Commit().ok());
    EXPECT_EQ(db_->Get(Slice("a")).value(), "1");
}

TEST_F(TransactionTest, MoveAssignmentOverAnActiveTransactionRollsItBack) {
    Transaction txn = std::move(db_->BeginTransaction().value());
    ASSERT_TRUE(txn.Put(Slice("m"), Slice("1")).ok());

    // Overwriting a still-active transaction must roll back its pending
    // work rather than silently discard it.
    txn = Transaction();

    EXPECT_FALSE(db_->has_active_transaction());
    EXPECT_FALSE(db_->Get(Slice("m")).ok());
}

} // namespace
} // namespace engine
