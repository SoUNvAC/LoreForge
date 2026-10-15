#pragma once

#include "analysis_connection.h"
#include "chapter_preview.h"
#include "loreforge/storage/llm_run_repository.h"
#include "loreforge/storage/project_database.h"
#include "task_indicator.h"

#include <QObject>
#include <QTimer>
#include <array>

namespace loreforge::app {

struct AnalysisHistoryEntry final {
    storage::LLMRunRecord run;
    inference::ContextSnapshot context;
    inference::LLMRunArtifacts artifacts;
    QJsonObject outcome;
};
using PipelineRoles = std::array<const AnalysisHistoryEntry*, 3>;
[[nodiscard]] PipelineRoles latestPipelineRoles(const QList<AnalysisHistoryEntry>& history,
                                                const QString& pipeline);
[[nodiscard]] int completedRolePrefix(const PipelineRoles& roles);
[[nodiscard]] QString reviewConclusion(const QJsonObject& checker, const QJsonObject& reviewer);

class ChapterAnalysisController final : public QObject {
    Q_OBJECT
  public:
    explicit ChapterAnalysisController(QObject* parent = nullptr);
    ~ChapterAnalysisController() override;
    [[nodiscard]] bool busy() const noexcept;
    void start(QString databasePath, document::Document book, qsizetype chapterIndex,
               ChapterPreview preview, AnalysisConnection connection, QString resumePipeline = {},
               bool allowLegacyEndpoint = false);
    void cancel();
    [[nodiscard]] static storage::StorageResult<QList<AnalysisHistoryEntry>>
    history(QStringView databasePath, const core::ProjectId& projectId);

  signals:
    void busyChanged(bool busy);
    void roleChanged(int role, QString status, QString usage, qint64 latencyMs);
    void finished(QString status);
    void taskStatus(TaskState state, QString text);
    void streamStarted(QUuid requestId, QString label);
    void streamProgress(QUuid requestId, QString text, bool generating);
    void streamFinished(QUuid requestId, QString status);

  private:
    void nextRole();
    void complete(llm::LLMResult result);
    void stop(QString message, TaskState state = TaskState::Failed);
    [[nodiscard]] bool sourceUnchanged() const;
    QString loadContinuation(const QString& pipeline, bool allowLegacyEndpoint);
    std::unique_ptr<storage::ProjectDatabase> database_;
    std::unique_ptr<llm::QwenClient> client_;
    document::Document book_;
    qsizetype chapterIndex_ = -1;
    ChapterPreview source_;
    ChapterPreview rolePreview_;
    AnalysisConnection connection_;
    QList<QJsonObject> outputs_;
    storage::LLMRunRecord run_;
    QString pipelineId_;
    QString executionId_;
    QStringList reusedRunIds_;
    int attemptSequence_ = 0;
    QUuid requestId_;
    int role_ = 0;
    bool busy_ = false;
    bool cancelled_ = false;
    bool persisted_ = false;
    bool closing_ = false;
    QTimer streamSilence_;
};

} // namespace loreforge::app
