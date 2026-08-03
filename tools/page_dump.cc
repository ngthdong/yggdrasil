// page_dump: hex-dumps a page from an engine database file, decoding the
// superblock header if page_id == 0. Usage: page_dump <file> <page_id>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

#include "engine/disk_manager.h"
#include "engine/superblock.h"

namespace {
void HexDump(const char* buf, size_t len) {
  for (size_t i = 0; i < len; i += 16) {
    std::printf("%06zx  ", i);
    for (size_t j = 0; j < 16; ++j) {
      if (i + j < len) std::printf("%02x ", static_cast<unsigned char>(buf[i + j]));
      else std::printf("   ");
    }
    std::printf(" ");
    for (size_t j = 0; j < 16 && i + j < len; ++j) {
      char c = buf[i + j];
      std::printf("%c", (c >= 32 && c < 127) ? c : '.');
    }
    std::printf("\n");
  }
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: page_dump <file> <page_id>\n";
    return 1;
  }
  std::string path = argv[1];
  int page_id = std::atoi(argv[2]);

  auto dm_or = engine::DiskManager::Open(path, 4096, /*create_if_missing=*/false);
  if (!dm_or.ok()) {
    std::cerr << "failed to open " << path << ": " << dm_or.status().ToString() << "\n";
    return 1;
  }
  auto dm = std::move(dm_or.value());

  std::vector<char> buf(dm->page_size());
  auto s = dm->ReadPage(page_id, buf.data());
  if (!s.ok()) {
    std::cerr << "ReadPage(" << page_id << ") failed: " << s.ToString() << "\n";
    return 1;
  }

  std::printf("page %d of %u, page_size=%u\n", page_id, dm->GetNumPages(), dm->page_size());

  if (page_id == 0) {
    auto sb_or = engine::Superblock::DeserializeFrom(buf.data(), dm->page_size());
    if (sb_or.ok()) {
      const auto& sb = sb_or.value();
      std::printf("-- superblock --\n");
      std::printf("  format_version:        %u\n", sb.format_version);
      std::printf("  page_size:             %u\n", sb.page_size);
      std::printf("  page_count:            %u\n", sb.page_count);
      std::printf("  free_list_head_page_id:%d\n", sb.free_list_head_page_id);
      std::printf("  root_page_id:          %d\n", sb.root_page_id);
    } else {
      std::printf("-- superblock decode FAILED: %s --\n", sb_or.status().ToString().c_str());
    }
  }

  std::printf("-- hex dump (first 128 bytes) --\n");
  HexDump(buf.data(), std::min<size_t>(128, buf.size()));
  return 0;
}