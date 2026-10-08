#pragma once

#include "loreforge/document/document.h"
#include "loreforge/inference/inference_types.h"
#include "loreforge/storage/storage_error.h"

namespace loreforge::storage {
class ProjectDatabase;
}

namespace loreforge::app {

struct ChapterPreview final {
    inference::ContextSnapshot snapshot;
    inference::PromptVersion prompt;
    inference::OutputSchema schema;
    QString messagesJson;
    qsizetype estimatedTokens = 0;
};

class ChapterPreviewBuilder final {
  public:
    [[nodiscard]] static storage::StorageResult<ChapterPreview>
    build(const core::ProjectId& projectId, const document::Document& book, qsizetype chapterIndex,
          int maximumTokens, int reservedTokens);
    [[nodiscard]] static storage::StorageResult<ChapterPreview>
    store(storage::ProjectDatabase& database, ChapterPreview preview);
};

} // namespace loreforge::app
