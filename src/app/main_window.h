#pragma once

#include "chapter_analysis_controller.h"
#include "project_workspace.h"
#include <QUuid>

#include "loreforge/context/context_types.h"
#include "loreforge/proofreading/repair_queue.h"

#include <QJsonObject>
#include <QMainWindow>
#include <QStringView>

#include <optional>

class QLabel;
class QCloseEvent;
class QTimer;
class QThread;
class QTreeWidget;
class QTreeWidgetItem;
class QTextBrowser;

namespace loreforge::app {

class ContextInspectorWidget;
class RepairQueueWidget;
class LlmWorkbench;
class NovelAnalysisWorkbench;
class ChapterAnalysisController;
class TaskIndicator;

class MainWindow final : public QMainWindow {
    Q_OBJECT

  public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    [[nodiscard]] bool openProjectFile(QStringView filePath);
    [[nodiscard]] bool openProjectFileAsync(QStringView filePath);
    [[nodiscard]] bool projectOpening() const noexcept {
        return projectLoader_ != nullptr;
    }
    [[nodiscard]] bool analysisHistoryLoading() const noexcept {
        return historyLoader_ != nullptr;
    }
    [[nodiscard]] bool importMarkdownSource(QStringView sourcePath, QStringView projectPath);
    void inspectContext(const context::ContextInspectorData& context);
    void inspectRepairQueue(QList<proofreading::RepairQueueItem> items);

  protected:
    void closeEvent(QCloseEvent* event) override;

  private slots:
    void chooseProjectFile();
    void chooseMarkdownSource();
    void selectProjectItem(QTreeWidgetItem* current, QTreeWidgetItem* previous);
    void displayChapter(QTreeWidgetItem* current, QTreeWidgetItem* previous);
    void approveRepairCandidate(QString candidateId);
    void rejectRepairCandidate(QString candidateId);
    void editRepairSuggestion(QString candidateId, QString suggestion);
    void protectRepairTerm(QString candidateId, QString canonicalSpelling);
    void prepareChapterPreview(int maximumTokens, int reservedTokens);
    void startChapterAnalysis();

  private:
    void setWorkspace(StoredWorkspace workspace);
    void clearBookView();
    void showBook(qsizetype projectIndex, qsizetype bookIndex);
    void showLoadError(QString message);
    void reloadRepairQueue();
    void reloadAnalysisHistory(bool refresh = false);
    void clearAnalysisHistoryCache();
    void trackBackgroundLoader(QThread* loader);
    void cancelProjectOpen();
    void updateLoadingControls();
    void startHistoryLoad();
    void refreshChapterIndicators();
    void showChapterDetails(QTreeWidgetItem* item, int column);
    void runChapterAnalysis(QString resumePipeline, bool allowLegacyEndpoint);
    void stopAutoAnalysis();
    void scheduleNextChapter();
    void showStatusMessage(const QString& message, int timeoutMs = 0);
    [[nodiscard]] const document::Document* selectedBook() const;

    QTreeWidget* projectExplorer_ = nullptr;
    QTreeWidget* chapterTree_ = nullptr;
    QTextBrowser* reader_ = nullptr;
    QLabel* workspaceStatus_ = nullptr;
    QLabel* documentSummary_ = nullptr;
    QLabel* bookMetadata_ = nullptr;
    QLabel* sourceInfo_ = nullptr;
    QLabel* chapterMetadata_ = nullptr;
    QLabel* chapterStatus_ = nullptr;
    ContextInspectorWidget* contextInspector_ = nullptr;
    RepairQueueWidget* repairQueue_ = nullptr;
    LlmWorkbench* llmWorkbench_ = nullptr;
    NovelAnalysisWorkbench* analysisWorkbench_ = nullptr;
    ChapterAnalysisController* analysisController_ = nullptr;
    TaskIndicator* taskIndicator_ = nullptr;
    QLabel* statusMessage_ = nullptr;
    QTimer* statusMessageTimer_ = nullptr;
    QUuid analysisTaskId_;
    std::optional<StoredWorkspace> workspace_;
    qsizetype selectedProjectIndex_ = -1;
    qsizetype selectedBookIndex_ = -1;
    QString openedDatabasePath_;
    QJsonObject activeAnalysis_;
    quint64 autoNextGeneration_ = 0;
    bool advancingAutomatically_ = false;
    bool analysisHistoryIndexed_ = false;
    QHash<QString, QString> analysisRunChapters_;
    QHash<QString, QList<AnalysisHistoryEntry>> analysisChapterHistory_;
    QStringList analysisHistoryLru_;
    QHash<QString, QPair<qint64, int>> analysisChapterUsage_;
    QPair<qint64, int> analysisProjectUsage_{};
    QThread* projectLoader_ = nullptr;
    bool closing_ = false;
    QThread* historyLoader_ = nullptr;
    QList<QThread*> backgroundLoaders_;
    quint64 projectLoadGeneration_ = 0;
    quint64 historyLoadGeneration_ = 0;
    QUuid projectLoadTaskId_;
    std::optional<storage::StorageResult<QList<AnalysisHistoryEntry>>> readyHistory_;
};

} // namespace loreforge::app
