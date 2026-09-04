#pragma once

#include "loreforge/core/identifier.h"
#include "loreforge/storage/storage_error.h"

#include <QDateTime>
#include <QList>
#include <QString>
#include <QUuid>

#include <optional>

namespace loreforge::storage {

class ProjectDatabase;

enum class LLMRunStatus {
    Queued,
    Running,
    Succeeded,
    Failed,
    Cancelled,
};

struct LLMRunRecord final {
    core::LLMRunId id;
    core::ProjectId projectId;
    QUuid requestId;
    QString provider;
    QString model;
    LLMRunStatus status = LLMRunStatus::Queued;
    int attemptCount = 0;
    int promptTokens = 0;
    int completionTokens = 0;
    int totalTokens = 0;
    QDateTime startedAt;
    std::optional<QDateTime> completedAt;
    std::optional<qint64> latencyMs;
    QString errorCode;
    QString errorMessage;

    friend bool operator==(const LLMRunRecord&, const LLMRunRecord&) = default;
};

class LLMRunRepository final {
  public:
    explicit LLMRunRepository(ProjectDatabase& database);

    [[nodiscard]] StorageStatus save(const LLMRunRecord& run);
    [[nodiscard]] StorageResult<LLMRunRecord> find(const core::LLMRunId& runId) const;
    [[nodiscard]] StorageResult<QList<LLMRunRecord>>
    listForProject(const core::ProjectId& projectId) const;

  private:
    ProjectDatabase& database_;
};

} // namespace loreforge::storage
