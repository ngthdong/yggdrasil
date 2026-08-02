#include "engine/database.h"

namespace engine {

Database::Database(Options options) : options_(std::move(options)) {}

Database::~Database() {
    if (is_open_) {
        Close();
    }
}

Status Database::Open() {
    if (is_open_) {
        return Status::InvalidArgument("Database is already open");
    }
    Status validate = options_.Validate();
    if (!validate.ok()) {
        return validate;
    }

    is_open_ = true;
    return Status::OK();
}

Status Database::Close() {
    if (!is_open_) {
        return Status::OK();
    }
    is_open_ = false;
    return Status::OK();
}

} // namespace engine
