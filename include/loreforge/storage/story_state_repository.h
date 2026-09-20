#pragma once

#include "loreforge/core/identifier.h"
#include "loreforge/narrative/story_memory.h"
#include "loreforge/storage/storage_error.h"

#include <QList>

namespace loreforge::storage {

class ProjectDatabase;

class StoryStateRepository final {
  public:
    explicit StoryStateRepository(ProjectDatabase& database);

    [[nodiscard]] StorageStatus saveChapterRecord(const narrative::ChapterMemoryRecord& record);
    [[nodiscard]] StorageResult<QList<narrative::ChapterMemoryRecord>>
    loadChapterRecords(const core::ProjectId& projectId, qsizetype throughChapterSequence) const;
    [[nodiscard]] StorageResult<narrative::StoryStateSnapshot>
    rebuildAndSave(const core::ProjectId& projectId, qsizetype throughChapterSequence);
    [[nodiscard]] StorageResult<narrative::StoryStateSnapshot>
    loadSnapshot(const core::ProjectId& projectId, qsizetype throughChapterSequence) const;

  private:
    ProjectDatabase& database_;
};

} // namespace loreforge::storage
