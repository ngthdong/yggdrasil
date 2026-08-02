#include "engine/database.h"

#include <gtest/gtest.h>

namespace engine {
namespace {

Options ValidOptions() {
    Options opts;
    opts.path = "test.db";
    return opts;
}

TEST(DatabaseTest, OpenSucceedsWithValidOptions) {
    Database db(ValidOptions());
    Status s = db.Open();
    EXPECT_TRUE(s.ok()) << s.ToString();
    EXPECT_TRUE(db.is_open());
}

TEST(DatabaseTest, OpenRejectsEmptyPath) {
    Options opts;
    opts.path = "";
    Database db(opts);
    Status s = db.Open();
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), Status::Code::kInvalidArgument);
    EXPECT_FALSE(db.is_open());
}

TEST(DatabaseTest, OpenRejectsNonPowerOfTwoPageSize) {
    Options opts = ValidOptions();
    opts.page_size = 4000;
    Database db(opts);
    Status s = db.Open();
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), Status::Code::kInvalidArgument);
}

TEST(DatabaseTest, DoubleOpenFails) {
    Database db(ValidOptions());
    ASSERT_TRUE(db.Open().ok());
    Status s = db.Open();
    EXPECT_FALSE(s.ok());
}

TEST(DatabaseTest, CloseThenReopenSucceeds) {
    Database db(ValidOptions());
    ASSERT_TRUE(db.Open().ok());
    ASSERT_TRUE(db.Close().ok());
    EXPECT_FALSE(db.is_open());
    EXPECT_TRUE(db.Open().ok());
}

TEST(DatabaseTest, CloseWithoutOpenIsNotAnError) {
    Database db(ValidOptions());
    EXPECT_TRUE(db.Close().ok());
}

TEST(DatabaseTest, DestructorClosesAutomatically) {
    Options opts = ValidOptions();
    {
        Database db(opts);
        ASSERT_TRUE(db.Open().ok());
    }
    SUCCEED();
}

} // namespace
} // namespace engine
