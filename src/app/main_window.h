#pragma once

#include "project_workspace.h"

#include "loreforge/context/context_types.h"
#include "loreforge/proofreading/repair_queue.h"

#include <QMainWindow>
#include <QStringView>

#include <optional>

class QLabel;
class QTreeWidget;
class QTreeWidgetItem;
class QTextBrowser;

namespace loreforge::app {

class ContextInspectorWidget;
class RepairQueueWidget;

class MainWindow final : public QMainWindow {
    Q_OBJECT

  public:
    explicit MainWindow(QWidget* parent = nullptr);

    [[nodiscard]] bool openProjectFile(QStringView filePath);
    void inspectContext(const context::ContextInspectorData& context);
    void inspectRepairQueue(QList<proofreading::RepairQueueItem> items);

  private slots:
    void chooseProjectFile();
    void selectProjectItem(QTreeWidgetItem* current, QTreeWidgetItem* previous);
    void displayChapter(QTreeWidgetItem* current, QTreeWidgetItem* previous);
    void approveRepairCandidate(QString candidateId);
    void rejectRepairCandidate(QString candidateId);
    void editRepairSuggestion(QString candidateId, QString suggestion);
    void protectRepairTerm(QString candidateId, QString canonicalSpelling);

  private:
    void setWorkspace(StoredWorkspace workspace);
    void clearBookView();
    void showBook(qsizetype projectIndex, qsizetype bookIndex);
    void showLoadError(QString message);
    void reloadRepairQueue();
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
    std::optional<StoredWorkspace> workspace_;
    qsizetype selectedProjectIndex_ = -1;
    qsizetype selectedBookIndex_ = -1;
    QString openedDatabasePath_;
};

} // namespace loreforge::app
