#pragma once

#include <QString>

#include <optional>
#include <variant>

namespace loreforge::storage {

enum class StorageErrorCode {
    InvalidArgument,
    InvalidPath,
    FileAlreadyExists,
    FileNotFound,
    DriverUnavailable,
    CannotOpen,
    CorruptDatabase,
    UnsupportedSchema,
    MigrationFailed,
    TransactionFailed,
    Conflict,
    NotFound,
    InvalidDocument,
    CorruptData,
    SqlError,
};

struct StorageError final {
    StorageErrorCode code;
    QString message;
    QString technicalDetails;
    bool recoverable = true;

    friend bool operator==(const StorageError&, const StorageError&) = default;
};

using StorageStatus = std::optional<StorageError>;

template <typename T> using StorageResult = std::variant<T, StorageError>;

} // namespace loreforge::storage
