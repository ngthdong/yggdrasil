#pragma once

#include <string>

#include "engine/status.h"

namespace engine {

// Copies src to dst via raw POSIX read/write in 1 MiB chunks. dst is
// created (or truncated) if it already exists; src is left untouched. On
// any failure, a partially-written dst is removed.
Status CopyFile(const std::string& src, const std::string& dst);

} // namespace engine
