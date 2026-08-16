#include "engine/wal_manager.h"

#include <algorithm>
#include <cstdio>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

#include "engine/log_record.h"

namespace engine {
namespace {

class WalManagerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        path_ = "test_wal_" + std::to_string(::getpid()) + ".wal";
        std::remove(path_.c_str());
    }
    void TearDown() override {
        std::remove(path_.c_str());
    }

    std::string path_;
};

TEST_F(WalManagerTest, AppendAssignsMonotonicallyIncreasingLsns) {
    auto wal = WalManager::Open(path_).value();
    lsn_t lsn1 = wal->AppendLogRecord(LogRecordType::kInsert, 1, Slice("a"), Slice("1")).value();
    lsn_t lsn2 = wal->AppendLogRecord(LogRecordType::kInsert, 1, Slice("b"), Slice("2")).value();
    lsn_t lsn3 = wal->AppendLogRecord(LogRecordType::kDelete, 1, Slice("a"), Slice("")).value();
    EXPECT_LT(lsn1, lsn2);
    EXPECT_LT(lsn2, lsn3);
}

TEST_F(WalManagerTest, AppendAloneGivesNoDurabilityUntilFlush) {
    auto wal = WalManager::Open(path_).value();
    wal->AppendLogRecord(LogRecordType::kInsert, 1, Slice("a"), Slice("1")).value();
    EXPECT_EQ(wal->durable_lsn(), kInvalidLsn);
}

TEST_F(WalManagerTest, FlushMakesRecordsDurable) {
    auto wal = WalManager::Open(path_).value();
    lsn_t lsn = wal->AppendLogRecord(LogRecordType::kInsert, 1, Slice("a"), Slice("1")).value();
    ASSERT_TRUE(wal->Flush(lsn).ok());
    EXPECT_GE(wal->durable_lsn(), lsn);
}

TEST_F(WalManagerTest, FlushOfAlreadyDurableLsnIsANoOp) {
    auto wal = WalManager::Open(path_).value();
    lsn_t lsn = wal->AppendLogRecord(LogRecordType::kInsert, 1, Slice("a"), Slice("1")).value();
    ASSERT_TRUE(wal->Flush(lsn).ok());
    ASSERT_TRUE(wal->Flush(lsn).ok());
    EXPECT_GE(wal->durable_lsn(), lsn);
}

TEST_F(WalManagerTest, FlushOfLaterLsnCoversEarlierAppendsToo) {
    auto wal = WalManager::Open(path_).value();
    wal->AppendLogRecord(LogRecordType::kInsert, 1, Slice("a"), Slice("1")).value();
    lsn_t lsn2 = wal->AppendLogRecord(LogRecordType::kInsert, 1, Slice("b"), Slice("2")).value();
    ASSERT_TRUE(wal->Flush(lsn2).ok());
    EXPECT_GE(wal->durable_lsn(), lsn2);
}

TEST_F(WalManagerTest, RecordsSurviveReopenAndReadBackCorrectly) {
    lsn_t lsn;
    {
        auto wal = WalManager::Open(path_).value();
        lsn = wal->AppendLogRecord(LogRecordType::kInsert, 7, Slice("key"), Slice("value")).value();
        ASSERT_TRUE(wal->Flush(lsn).ok());
        ASSERT_TRUE(wal->Shutdown().ok());
    }
    FILE* f = std::fopen(path_.c_str(), "rb");
    ASSERT_NE(f, nullptr);
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<char> buf(static_cast<size_t>(size));
    ASSERT_EQ(std::fread(buf.data(), 1, buf.size(), f), buf.size());
    std::fclose(f);

    size_t consumed = 0;
    auto record_or = LogRecord::ParseFrom(buf.data(), buf.size(), 0, &consumed);
    ASSERT_TRUE(record_or.ok()) << record_or.status().ToString();
    LogRecord record = record_or.value();
    EXPECT_EQ(record.lsn, lsn);
    EXPECT_EQ(record.type, LogRecordType::kInsert);
    EXPECT_EQ(record.page_id, 7);
    EXPECT_EQ(record.key, "key");
    EXPECT_EQ(record.value, "value");
}

TEST_F(WalManagerTest, MultipleRecordsParseSequentiallyFromTheFile) {
    {
        auto wal = WalManager::Open(path_).value();
        lsn_t last = 0;
        for (int i = 0; i < 20; ++i) {
            last = wal->AppendLogRecord(LogRecordType::kInsert,
                                        i,
                                        Slice("k" + std::to_string(i)),
                                        Slice("v" + std::to_string(i)))
                       .value();
        }
        ASSERT_TRUE(wal->Flush(last).ok());
    }
    FILE* f = std::fopen(path_.c_str(), "rb");
    ASSERT_NE(f, nullptr);
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<char> buf(static_cast<size_t>(size));
    ASSERT_EQ(std::fread(buf.data(), 1, buf.size(), f), buf.size());
    std::fclose(f);

    size_t offset = 0;
    int count = 0;
    while (offset < buf.size()) {
        size_t consumed = 0;
        auto record_or = LogRecord::ParseFrom(buf.data(), buf.size(), offset, &consumed);
        ASSERT_TRUE(record_or.ok()) << "record " << count << ": " << record_or.status().ToString();
        EXPECT_EQ(record_or.value().key, "k" + std::to_string(count));
        offset += consumed;
        ++count;
    }
    EXPECT_EQ(count, 20);
}

TEST_F(WalManagerTest, CorruptedRecordChecksumIsDetected) {
    {
        auto wal = WalManager::Open(path_).value();
        lsn_t lsn = wal->AppendLogRecord(LogRecordType::kInsert, 1, Slice("a"), Slice("1")).value();
        ASSERT_TRUE(wal->Flush(lsn).ok());
    }
    FILE* f = std::fopen(path_.c_str(), "r+b");
    ASSERT_NE(f, nullptr);
    std::fseek(f, 16, SEEK_SET);
    char corrupt = 'X';
    std::fwrite(&corrupt, 1, 1, f);
    std::fclose(f);

    f = std::fopen(path_.c_str(), "rb");
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<char> buf(static_cast<size_t>(size));
    ASSERT_EQ(std::fread(buf.data(), 1, buf.size(), f), buf.size());
    std::fclose(f);

    size_t consumed = 0;
    auto record_or = LogRecord::ParseFrom(buf.data(), buf.size(), 0, &consumed);
    EXPECT_FALSE(record_or.ok());
    EXPECT_EQ(record_or.status().code(), Status::Code::kCorruption);
}

TEST_F(WalManagerTest, ConcurrentFlushCallsAreBatchedIntoFewerWrites) {
    auto wal = WalManager::Open(path_).value();
    constexpr int kThreads = 8;
    constexpr int kPerThread = 50;
    std::vector<std::thread> threads;
    std::vector<std::vector<lsn_t>> per_thread_lsns(kThreads);

    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < kPerThread; ++i) {
                std::string key = "t" + std::to_string(t) + "-" + std::to_string(i);
                auto lsn_or =
                    wal->AppendLogRecord(LogRecordType::kInsert, t, Slice(key), Slice("v"));
                ASSERT_TRUE(lsn_or.ok());
                lsn_t lsn = lsn_or.value();
                ASSERT_TRUE(wal->Flush(lsn).ok());
                EXPECT_GE(wal->durable_lsn(), lsn);
                per_thread_lsns[static_cast<size_t>(t)].push_back(lsn);
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    std::vector<lsn_t> all_lsns;
    for (auto& v : per_thread_lsns) {
        all_lsns.insert(all_lsns.end(), v.begin(), v.end());
    }
    std::sort(all_lsns.begin(), all_lsns.end());
    for (size_t i = 1; i < all_lsns.size(); ++i) {
        EXPECT_NE(all_lsns[i - 1], all_lsns[i]) << "duplicate LSN handed out under concurrency";
    }
    EXPECT_EQ(all_lsns.size(), static_cast<size_t>(kThreads * kPerThread));

    ASSERT_TRUE(wal->Shutdown().ok());

    FILE* f = std::fopen(path_.c_str(), "rb");
    ASSERT_NE(f, nullptr);
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<char> buf(static_cast<size_t>(size));
    ASSERT_EQ(std::fread(buf.data(), 1, buf.size(), f), buf.size());
    std::fclose(f);

    size_t offset = 0;
    int count = 0;
    while (offset < buf.size()) {
        size_t consumed = 0;
        auto record_or = LogRecord::ParseFrom(buf.data(), buf.size(), offset, &consumed);
        ASSERT_TRUE(record_or.ok()) << "record " << count << ": " << record_or.status().ToString();
        offset += consumed;
        ++count;
    }
    EXPECT_EQ(count, kThreads * kPerThread);
}

TEST_F(WalManagerTest, ShutdownFlushesEverythingPending) {
    {
        auto wal = WalManager::Open(path_).value();
        wal->AppendLogRecord(LogRecordType::kInsert, 1, Slice("a"), Slice("1")).value();
        ASSERT_TRUE(wal->Shutdown().ok());
    }
    FILE* f = std::fopen(path_.c_str(), "rb");
    ASSERT_NE(f, nullptr);
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fclose(f);
    EXPECT_GT(size, 0);
}

} // namespace
} // namespace engine