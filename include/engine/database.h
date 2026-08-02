#pragma once

#include <memory>

#include "engine/options.h"
#include "engine/status.h"

namespace engine {

class Database {
  public:
    explicit Database(Options options);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    Status Open();
    Status Close();

    bool is_open() const { return is_open_; }
    const Options& options() const { return options_; }

  private:
    Options options_;
    bool is_open_ = false;
};

} // namespace engine
