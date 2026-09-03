#include "engine/database.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <gtest/gtest.h>
#include <map>
#include <random>

namespace engine {
namespace {

class DatabaseApiTest : public ::testing::Test {
  protected:
    void SetUp() override {
        path_ = "test_database_api_" + std::to_string(::getpid()) + ".db";
        std::remove(path_.c_str());
        std::remove((path_ + ".wal").c_str());
    }
    void TearDown() override {
        std::remove(path_.c_str());
        std::remove((path_ + ".wal").c_str());
    }

    Options ValidOptions(uint32_t page_size = 4096, size_t buffer_pool_frames = 32) {
        Options opts;
        opts.path = path_;
        opts.page_size = page_size;
        opts.buffer_pool_frames = buffer_pool_frames;
        return opts;
    }

    std::string path_;
};

TEST_F(DatabaseApiTest, PutGetRemoveRoundTripThroughFacadeOnly) {
    Database db(ValidOptions());
    ASSERT_TRUE(db.Open().ok());

    ASSERT_TRUE(db.Put(Slice("hello"), Slice("world")).ok());
    auto v = db.Get(Slice("hello"));
    ASSERT_TRUE(v.ok());
    EXPECT_EQ(v.value(), "world");

    ASSERT_TRUE(db.Remove(Slice("hello")).ok());
    EXPECT_FALSE(db.Get(Slice("hello")).ok());
}

TEST_F(DatabaseApiTest, PutOnExistingKeyUpsertsInsteadOfRejecting) {
    Database db(ValidOptions());
    ASSERT_TRUE(db.Open().ok());

    ASSERT_TRUE(db.Put(Slice("k"), Slice("original")).ok());
    ASSERT_TRUE(db.Put(Slice("k"), Slice("updated")).ok());
    auto v = db.Get(Slice("k"));
    ASSERT_TRUE(v.ok());
    EXPECT_EQ(v.value(), "updated");
}

TEST_F(DatabaseApiTest, RepeatedUpsertOfSameKeyStaysCorrect) {
    Database db(ValidOptions());
    ASSERT_TRUE(db.Open().ok());
    for (int i = 0; i < 50; ++i) {
        ASSERT_TRUE(db.Put(Slice("k"), Slice("v" + std::to_string(i))).ok());
    }
    auto v = db.Get(Slice("k"));
    ASSERT_TRUE(v.ok());
    EXPECT_EQ(v.value(), "v49");
    ASSERT_TRUE(db.Verify().ok());
}

TEST_F(DatabaseApiTest, OperationsBeforeOpenFail) {
    Database db(ValidOptions());
    EXPECT_FALSE(db.Put(Slice("a"), Slice("1")).ok());
    EXPECT_FALSE(db.Get(Slice("a")).ok());
    EXPECT_FALSE(db.Remove(Slice("a")).ok());
    EXPECT_FALSE(db.NewIterator().ok());
    EXPECT_FALSE(db.GetStats().ok());
    EXPECT_FALSE(db.Verify().ok());
}

TEST_F(DatabaseApiTest, OperationsAfterCloseFail) {
    Database db(ValidOptions());
    ASSERT_TRUE(db.Open().ok());
    ASSERT_TRUE(db.Put(Slice("a"), Slice("1")).ok());
    ASSERT_TRUE(db.Close().ok());

    EXPECT_FALSE(db.Put(Slice("b"), Slice("2")).ok());
    EXPECT_FALSE(db.Get(Slice("a")).ok());
    EXPECT_FALSE(db.Remove(Slice("a")).ok());
    EXPECT_FALSE(db.NewIterator().ok());
    EXPECT_FALSE(db.GetStats().ok());
    EXPECT_FALSE(db.Verify().ok());
}

TEST_F(DatabaseApiTest, FullScanThroughFacadeIteratorMatchesInsertedSet) {
    Database db(ValidOptions(512, 32));
    ASSERT_TRUE(db.Open().ok());

    constexpr int kN = 100;
    std::vector<int> order(kN);
    for (int i = 0; i < kN; ++i) {
        order[static_cast<size_t>(i)] = i;
    }
    std::mt19937 rng(3);
    std::shuffle(order.begin(), order.end(), rng);
    for (int i : order) {
        std::array<char, 16> buf{};
        std::snprintf(buf.data(), buf.size(), "k%06d", i);
        ASSERT_TRUE(db.Put(Slice(std::string(buf.data())), Slice("v" + std::to_string(i))).ok());
    }

    auto it_or = db.NewIterator();
    ASSERT_TRUE(it_or.ok());
    auto it = std::move(it_or.value());
    int count = 0;
    std::string prev;
    while (it.Valid()) {
        std::string k = it.Key().ToString();
        if (!prev.empty()) {
            EXPECT_LT(prev, k);
        }
        prev = k;
        ++count;
        ASSERT_TRUE(it.Next().ok());
    }
    EXPECT_EQ(count, kN);
}

TEST_F(DatabaseApiTest, SeekingIteratorThroughFacadeStartsAtCorrectKey) {
    Database db(ValidOptions(512, 32));
    ASSERT_TRUE(db.Open().ok());
    for (int i = 0; i < 60; ++i) {
        std::array<char, 16> buf{};
        std::snprintf(buf.data(), buf.size(), "k%06d", i);
        ASSERT_TRUE(db.Put(Slice(std::string(buf.data())), Slice("v" + std::to_string(i))).ok());
    }
    auto it_or = db.NewIterator(Slice(std::string("k000030")));
    ASSERT_TRUE(it_or.ok());
    auto it = std::move(it_or.value());
    ASSERT_TRUE(it.Valid());
    EXPECT_EQ(it.Key().ToString(), "k000030");
}

TEST_F(DatabaseApiTest, GetStatsReflectsRealPageCountAndHeight) {
    Database db(ValidOptions(512, 32));
    ASSERT_TRUE(db.Open().ok());

    auto stats0_or = db.GetStats();
    ASSERT_TRUE(stats0_or.ok());
    EXPECT_EQ(stats0_or.value().tree_height, 0);

    for (int i = 0; i < 200; ++i) {
        std::array<char, 16> buf{};
        std::snprintf(buf.data(), buf.size(), "k%06d", i);
        ASSERT_TRUE(db.Put(Slice(std::string(buf.data())), Slice("v" + std::to_string(i))).ok());
    }

    auto stats_or = db.GetStats();
    ASSERT_TRUE(stats_or.ok());
    DBStats stats = stats_or.value();
    EXPECT_GT(stats.page_count, 1u);
    EXPECT_GT(stats.tree_height, 1);
    EXPECT_GT(stats.buffer_pool_hits + stats.buffer_pool_misses, 0u);
    EXPECT_GE(stats.BufferPoolHitRate(), 0.0);
    EXPECT_LE(stats.BufferPoolHitRate(), 1.0);
}

TEST_F(DatabaseApiTest, GetStatsHitRateIncreasesWithRepeatedAccess) {
    Database db(ValidOptions(4096, 32));
    ASSERT_TRUE(db.Open().ok());
    ASSERT_TRUE(db.Put(Slice("a"), Slice("1")).ok());

    for (int i = 0; i < 100; ++i) {
        ASSERT_TRUE(db.Get(Slice("a")).ok());
    }
    auto stats_or = db.GetStats();
    ASSERT_TRUE(stats_or.ok());
    EXPECT_GT(stats_or.value().BufferPoolHitRate(), 0.5);
}

TEST_F(DatabaseApiTest, VerifyThroughFacadeCatchesNothingWrongOnAHealthyTree) {
    Database db(ValidOptions(512, 32));
    ASSERT_TRUE(db.Open().ok());
    for (int i = 0; i < 150; ++i) {
        std::array<char, 16> buf{};
        std::snprintf(buf.data(), buf.size(), "k%06d", i);
        ASSERT_TRUE(db.Put(Slice(std::string(buf.data())), Slice("v" + std::to_string(i))).ok());
    }
    for (int i = 0; i < 150; i += 4) {
        std::array<char, 16> buf{};
        std::snprintf(buf.data(), buf.size(), "k%06d", i);
        ASSERT_TRUE(db.Remove(Slice(std::string(buf.data()))).ok());
    }
    EXPECT_TRUE(db.Verify().ok());
}

TEST_F(DatabaseApiTest, DataPersistsAcrossCloseAndReopenThroughFacadeOnly) {
    {
        Database db(ValidOptions(512, 32));
        ASSERT_TRUE(db.Open().ok());
        for (int i = 0; i < 100; ++i) {
            std::array<char, 16> buf{};
            std::snprintf(buf.data(), buf.size(), "k%06d", i);
            ASSERT_TRUE(
                db.Put(Slice(std::string(buf.data())), Slice("v" + std::to_string(i))).ok());
        }
        for (int i = 0; i < 100; i += 5) {
            std::array<char, 16> buf{};
            std::snprintf(buf.data(), buf.size(), "k%06d", i);
            ASSERT_TRUE(db.Remove(Slice(std::string(buf.data()))).ok());
        }
        ASSERT_TRUE(db.Close().ok());
    }
    {
        Database db(ValidOptions(512, 32));
        ASSERT_TRUE(db.Open().ok());
        ASSERT_TRUE(db.Verify().ok());
        for (int i = 0; i < 100; ++i) {
            std::array<char, 16> buf{};
            std::snprintf(buf.data(), buf.size(), "k%06d", i);
            auto v = db.Get(Slice(std::string(buf.data())));
            if (i % 5 == 0) {
                EXPECT_FALSE(v.ok()) << "k" << i << " should have been removed";
            } else {
                ASSERT_TRUE(v.ok()) << "k" << i;
                EXPECT_EQ(v.value(), "v" + std::to_string(i));
            }
        }
    }
}

TEST_F(DatabaseApiTest, DataSurvivesRelyingOnDestructorInsteadOfExplicitClose) {
    {
        Database db(ValidOptions());
        ASSERT_TRUE(db.Open().ok());
        ASSERT_TRUE(db.Put(Slice("relies-on-dtor"), Slice("still-here")).ok());
    }
    {
        Database db(ValidOptions());
        ASSERT_TRUE(db.Open().ok());
        auto v = db.Get(Slice("relies-on-dtor"));
        ASSERT_TRUE(v.ok());
        EXPECT_EQ(v.value(), "still-here");
    }
}

TEST_F(DatabaseApiTest, LargeRandomizedWorkloadThroughPublicApiOnlyMatchesOracle) {
    Database db(ValidOptions(512, 32));
    ASSERT_TRUE(db.Open().ok());

    std::map<std::string, std::string> oracle;
    std::mt19937 rng(4242);

    for (int round = 0; round < 1000; ++round) {
        int op = static_cast<int>(rng() % 3);
        std::string key = "k" + std::to_string(rng() % 300);
        if (op == 0) {
            std::string value = "v" + std::to_string(round);
            ASSERT_TRUE(db.Put(Slice(key), Slice(value)).ok());
            oracle[key] = value;
        } else if (op == 1) {
            Status s = db.Remove(Slice(key));
            bool present = oracle.contains(key);
            EXPECT_EQ(s.ok(), present);
            if (present) {
                oracle.erase(key);
            }
        } else {
            auto v = db.Get(Slice(key));
            bool present = oracle.contains(key);
            EXPECT_EQ(v.ok(), present);
            if (present) {
                EXPECT_EQ(v.value(), oracle[key]);
            }
        }
    }

    ASSERT_TRUE(db.Verify().ok());
    auto it_or = db.NewIterator();
    ASSERT_TRUE(it_or.ok());
    auto it = std::move(it_or.value());
    auto oracle_it = oracle.begin();
    while (it.Valid()) {
        ASSERT_NE(oracle_it, oracle.end());
        EXPECT_EQ(it.Key().ToString(), oracle_it->first);
        EXPECT_EQ(it.Value().ToString(), oracle_it->second);
        ++oracle_it;
        ASSERT_TRUE(it.Next().ok());
    }
    EXPECT_EQ(oracle_it, oracle.end());
}

TEST_F(DatabaseApiTest, CorruptedPageSurfacesAsCleanErrorNotCrashOrWrongAnswer) {
    {
        Database db(ValidOptions());
        ASSERT_TRUE(db.Open().ok());
        for (int i = 0; i < 50; ++i) {
            ASSERT_TRUE(
                db.Put(Slice("k" + std::to_string(i)), Slice("v" + std::to_string(i))).ok());
        }
        ASSERT_TRUE(db.Checkpoint().ok());
        ASSERT_TRUE(db.Close().ok());
    }
    // Corrupt a byte somewhere in the middle of the data file directly,
    // bypassing the engine entirely.
    {
        FILE* f = std::fopen(path_.c_str(), "r+b");
        ASSERT_NE(f, nullptr);
        std::fseek(f, 600, SEEK_SET); // inside a real data page's slot, past the superblock
        char c = 'X';
        std::fwrite(&c, 1, 1, f);
        std::fclose(f);
    }
    Database db(ValidOptions());
    Status open_s = db.Open();
    if (open_s.ok()) {
        int corruption_reports = 0;
        for (int i = 0; i < 50; ++i) {
            auto v = db.Get(Slice("k" + std::to_string(i)));
            if (!v.ok() && v.status().code() == Status::Code::kCorruption) {
                ++corruption_reports;
            } else if (v.ok()) {
                EXPECT_EQ(v.value(), "v" + std::to_string(i));
            }
        }
        EXPECT_GT(corruption_reports, 0)
            << "the flipped byte should have surfaced as a Corruption on at least one read";
    } else {
        EXPECT_EQ(open_s.code(), Status::Code::kCorruption);
    }
}

} // namespace
} // namespace engine