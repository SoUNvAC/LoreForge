#pragma once

#include "analysis_connection.h"
#include "chapter_preview.h"
#include "loreforge/storage/llm_run_repository.h"
#include "loreforge/storage/project_database.h"

#include <QObject>

namespace loreforge::app {

struct AnalysisHistoryEntry final {
    storage::LLMRunRecord run;
    inference::ContextSnapshot context;
    inference::LLMRunArtifacts artifacts;
    QJsonObject outcome;
};

class ChapterAnalysisController final : public QObject {
    Q_OBJECT
  public:
    explicit ChapterAnalysisController(QObject* parent = nullptr);
    ~ChapterAnalysisController() override;
    [[nodiscard]] bool busy() const noexcept;
    void start(QString databasePath, document::Document book, qsizetype chapterIndex,
               ChapterPreview preview, AnalysisConnection connection);
    void cancel();
    [[nodiscard]] static storage::StorageResult<QList<AnalysisHistoryEntry>>
    history(QStringView databasePath, const core::ProjectId& projectId);

  signals:
    void busyChanged(bool busy);
    void roleChanged(int role, QString status, QString usage, qint64 latencyMs);
    void finished(QString status);

  private:
    void nextRole();
    void complete(llm::LLMResult result);
    void stop(QString message);
    [[nodiscard]] bool sourceUnchanged() const;
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
    QUuid requestId_;
    int role_ = 0;
    bool busy_ = false;
    bool cancelled_ = false;
    bool persisted_ = false;
    bool closing_ = false;
};

} // namespace loreforge::app
