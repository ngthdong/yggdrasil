#include "engine/disk_manager.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <gtest/gtest.h>
#include <random>
#include <vector>

namespace engine {
namespace {

class DiskManagerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        path_ = "test_diskmanager_" + std::to_string(::getpid()) + ".db";
        std::remove(path_.c_str());
    }
    void TearDown() override {
        std::remove(path_.c_str());
    }

    std::string path_;
};

TEST_F(DiskManagerTest, OpenCreatesFreshSuperblock) {
    auto dm_or = DiskManager::Open(path_, 4096, /*create_if_missing=*/true);
    ASSERT_TRUE(dm_or.ok()) << dm_or.status().ToString();
    auto dm = std::move(dm_or.value());
    EXPECT_EQ(dm->GetNumPages(), 1u);
    EXPECT_EQ(dm->page_size(), 4096u);
}

TEST_F(DiskManagerTest, OpenWithoutCreateFailsOnMissingFile) {
    auto dm_or = DiskManager::Open(path_, 4096, /*create_if_missing=*/false);
    EXPECT_FALSE(dm_or.ok());
    EXPECT_EQ(dm_or.status().code(), Status::Code::kNotFound);
}

TEST_F(DiskManagerTest, WriteThenReadPageRoundTrips) {
    auto dm_or = DiskManager::Open(path_, 4096, true);
    ASSERT_TRUE(dm_or.ok()) << dm_or.status().ToString();

    auto dm = std::move(dm_or.value());

    auto id_or = dm->AllocatePage();
    ASSERT_TRUE(id_or.ok()) << id_or.status().ToString();
    page_id_t id = id_or.value();

    std::vector<char> write_buf(4096, 0);
    std::memcpy(write_buf.data(), "hello page", 10);

    ASSERT_TRUE(dm->WritePage(id, write_buf.data()).ok());

    std::vector<char> read_buf(4096, 0);
    ASSERT_TRUE(dm->ReadPage(id, read_buf.data()).ok());

    EXPECT_EQ(std::memcmp(write_buf.data(), read_buf.data(), 4096), 0);
}

TEST_F(DiskManagerTest, AllocatePageIsMonotonic) {
    auto dm_or = DiskManager::Open(path_, 4096, true);
    ASSERT_TRUE(dm_or.ok()) << dm_or.status().ToString();

    auto dm = std::move(dm_or.value());

    auto a_or = dm->AllocatePage();
    ASSERT_TRUE(a_or.ok()) << a_or.status().ToString();
    page_id_t a = a_or.value();

    auto b_or = dm->AllocatePage();
    ASSERT_TRUE(b_or.ok()) << b_or.status().ToString();
    page_id_t b = b_or.value();

    auto c_or = dm->AllocatePage();
    ASSERT_TRUE(c_or.ok()) << c_or.status().ToString();
    page_id_t c = c_or.value();

    EXPECT_EQ(a, 1); // page 0 is the superblock
    EXPECT_EQ(b, 2);
    EXPECT_EQ(c, 3);
    EXPECT_EQ(dm->GetNumPages(), 4u);
}
TEST_F(DiskManagerTest, WriteToPageZeroIsRejected) {
    auto dm_or = DiskManager::Open(path_, 4096, true);
    ASSERT_TRUE(dm_or.ok()) << dm_or.status().ToString();

    auto dm = std::move(dm_or.value());

    std::vector<char> buf(4096, 'x');

    Status s = dm->WritePage(kSuperblockPageId, buf.data());

    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), Status::Code::kInvalidArgument);
}

TEST_F(DiskManagerTest, ReadOutOfRangePageFails) {
    auto dm_or = DiskManager::Open(path_, 4096, true);
    ASSERT_TRUE(dm_or.ok()) << dm_or.status().ToString();

    auto dm = std::move(dm_or.value());

    std::vector<char> buf(4096);

    Status s = dm->ReadPage(99, buf.data());

    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), Status::Code::kInvalidArgument);
}

TEST_F(DiskManagerTest, StatePersistsAcrossReopen) {
    page_id_t id;
    {
        auto dm_or = DiskManager::Open(path_, 4096, true);
        ASSERT_TRUE(dm_or.ok()) << dm_or.status().ToString();
        auto dm = std::move(dm_or.value());
        id = dm->AllocatePage().value();
        std::vector<char> buf(4096, 0);
        std::memcpy(buf.data(), "persisted", 9);
        ASSERT_TRUE(dm->WritePage(id, buf.data()).ok());
        ASSERT_TRUE(dm->Shutdown().ok());
    }
    {
        auto dm_or = DiskManager::Open(path_, 4096, false);
        ASSERT_TRUE(dm_or.ok()) << dm_or.status().ToString();
        auto dm = std::move(dm_or.value());
        EXPECT_EQ(dm->GetNumPages(), 2u);
        std::vector<char> buf(4096, 0);
        ASSERT_TRUE(dm->ReadPage(id, buf.data()).ok());
        EXPECT_EQ(std::memcmp(buf.data(), "persisted", 9), 0);
    }
}

TEST_F(DiskManagerTest, RandomWriteReadMatchesShadowModel) {
    auto dm_or = DiskManager::Open(path_, 4096, true);
    ASSERT_TRUE(dm_or.ok()) << dm_or.status().ToString();

    auto dm = std::move(dm_or.value());

    constexpr int kNumPages = 20;
    std::vector<page_id_t> ids;

    for (int i = 0; i < kNumPages; ++i) {
        auto page_or = dm->AllocatePage();
        ASSERT_TRUE(page_or.ok()) << page_or.status().ToString();
        ids.push_back(page_or.value());
    }

    std::mt19937 rng(42);
    std::vector<std::vector<char>> shadow(ids.size(), std::vector<char>(4096, 0));

    for (int round = 0; round < 100; ++round) {
        size_t idx = rng() % ids.size();
        char fill = static_cast<char>(rng() % 256);

        std::vector<char> buf(4096, fill);
        ASSERT_TRUE(dm->WritePage(ids[idx], buf.data()).ok());

        shadow[idx] = buf;
    }

    for (size_t i = 0; i < ids.size(); ++i) {
        std::vector<char> read_buf(4096);

        ASSERT_TRUE(dm->ReadPage(ids[i], read_buf.data()).ok());

        EXPECT_EQ(std::memcmp(read_buf.data(), shadow[i].data(), 4096), 0)
            << "mismatch at page index " << i;
    }
}

TEST_F(DiskManagerTest, TruncatedFileIsDetectedAsCorruption) {
    {
        auto dm_or = DiskManager::Open(path_, 4096, true);
        ASSERT_TRUE(dm_or.ok()) << dm_or.status().ToString();

        auto dm = std::move(dm_or.value());

        auto page_or = dm->AllocatePage();
        ASSERT_TRUE(page_or.ok()) << page_or.status().ToString();

        ASSERT_TRUE(dm->Shutdown().ok());
    }

    int fd = ::open(path_.c_str(), O_RDWR);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(::ftruncate(fd, 4096 + 2000), 0); // was 8192, now a partial 2nd page
    ::close(fd);

    auto dm_or = DiskManager::Open(path_, 4096, false);
    ASSERT_FALSE(dm_or.ok());
    EXPECT_EQ(dm_or.status().code(), Status::Code::kCorruption);
}

TEST_F(DiskManagerTest, ForeignFileIsRejectedAsCorruption) {
    FILE* f = std::fopen(path_.c_str(), "wb");
    ASSERT_NE(f, nullptr);
    std::vector<char> junk(4096, 'Z');
    std::fwrite(junk.data(), 1, junk.size(), f);
    std::fclose(f);

    auto dm_or = DiskManager::Open(path_, 4096, false);
    ASSERT_FALSE(dm_or.ok());
    EXPECT_EQ(dm_or.status().code(), Status::Code::kCorruption);
}

TEST_F(DiskManagerTest, PageSizeMismatchOnReopenIsRejected) {
    {
        auto dm_or = DiskManager::Open(path_, 4096, true);
        ASSERT_TRUE(dm_or.ok()) << dm_or.status().ToString();

        auto dm = std::move(dm_or.value());
        ASSERT_TRUE(dm->Shutdown().ok());
    }

    auto dm_or = DiskManager::Open(path_, 8192, false);
    ASSERT_FALSE(dm_or.ok());
    EXPECT_EQ(dm_or.status().code(), Status::Code::kInvalidArgument);
}

} // namespace
} // namespace engine