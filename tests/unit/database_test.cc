#include "engine/database.h"

#include <cstdio>
#include <gtest/gtest.h>

namespace engine {
namespace {

class DatabaseTest : public ::testing::Test {
  protected:
    void SetUp() override {
        path_ = "test_database_" + std::to_string(::getpid()) + ".db";
        std::remove(path_.c_str());
        std::remove((path_ + ".wal").c_str());
    }
    void TearDown() override {
        std::remove(path_.c_str());
        std::remove((path_ + ".wal").c_str());
    }

    Options ValidOptions() {
        Options opts;
        opts.path = path_;
        return opts;
    }

    std::string path_;
};

TEST_F(DatabaseTest, OpenSucceedsWithValidOptions) {
    Database db(ValidOptions());
    Status s = db.Open();
    EXPECT_TRUE(s.ok()) << s.ToString();
    EXPECT_TRUE(db.is_open());
}

TEST_F(DatabaseTest, OpenRejectsEmptyPath) {
    Options opts;
    opts.path = "";
    Database db(opts);
    Status s = db.Open();
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), Status::Code::kInvalidArgument);
    EXPECT_FALSE(db.is_open());
}

TEST_F(DatabaseTest, OpenRejectsNonPowerOfTwoPageSize) {
    Options opts = ValidOptions();
    opts.page_size = 4000;
    Database db(opts);
    Status s = db.Open();
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), Status::Code::kInvalidArgument);
}

TEST_F(DatabaseTest, DoubleOpenFails) {
    Database db(ValidOptions());
    ASSERT_TRUE(db.Open().ok());
    Status s = db.Open();
    EXPECT_FALSE(s.ok());
}

TEST_F(DatabaseTest, CloseThenReopenSucceeds) {
    Database db(ValidOptions());
    ASSERT_TRUE(db.Open().ok());
    ASSERT_TRUE(db.Close().ok());
    EXPECT_FALSE(db.is_open());
    EXPECT_TRUE(db.Open().ok());
}

TEST_F(DatabaseTest, CloseWithoutOpenIsNotAnError) {
    Database db(ValidOptions());
    EXPECT_TRUE(db.Close().ok());
}

TEST_F(DatabaseTest, DestructorClosesAutomatically) {
    Options opts = ValidOptions();
    {
        Database db(opts);
        ASSERT_TRUE(db.Open().ok());
    }
    SUCCEED();
}

} // namespace
} // namespace engine