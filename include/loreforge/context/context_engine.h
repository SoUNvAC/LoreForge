#pragma once

#include "loreforge/context/context_builder.h"
#include "loreforge/storage/storage_error.h"

#include <variant>

namespace loreforge::storage {
class InferenceRepository;
}

namespace loreforge::context {

enum class ContextOperationErrorCode {
    BuildFailed,
    StorageFailed,
    InvalidStoredSnapshot,
    InvalidRequestTemplate,
};

struct ContextOperationError final {
    ContextOperationErrorCode code;
    QString message;
    QString details;

    friend bool operator==(const ContextOperationError&, const ContextOperationError&) = default;
};

struct StoredContextRequest final {
    inference::ContextSnapshot snapshot;
    llm::LLMRequest request;

    friend bool operator==(const StoredContextRequest&, const StoredContextRequest&) = default;
};

using ContextPreparationResult = std::variant<BuiltContext, ContextOperationError>;
using StoredContextRequestResult = std::variant<StoredContextRequest, ContextOperationError>;

class ContextEngine final {
  public:
    explicit ContextEngine(storage::InferenceRepository& repository);

    [[nodiscard]] ContextPreparationResult
    prepareAndStore(ContextBuildInput input, ContextBudget budget, QDateTime createdAt);
    [[nodiscard]] StoredContextRequestResult
    requestFromStoredSnapshot(const core::ContextSnapshotId& snapshotId,
                              llm::LLMRequest requestTemplate) const;

  private:
    storage::InferenceRepository& repository_;
};

} // namespace loreforge::context
