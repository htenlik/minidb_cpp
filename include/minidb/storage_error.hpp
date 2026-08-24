#pragma once

#include <stdexcept>
#include <string>

namespace minidb {

enum class StorageErrorKind {
    NoFrameAvailable,
    CorruptPage,
    InvalidPage,
    PinnedPageRelease,
};

class StorageError : public std::runtime_error {
public:
    StorageError(StorageErrorKind kind, std::string message)
        : std::runtime_error(std::move(message)), kind_(kind) {}

    [[nodiscard]] StorageErrorKind kind() const noexcept { return kind_; }

private:
    StorageErrorKind kind_;
};

} // namespace minidb
