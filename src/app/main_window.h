#pragma once

#include "project_workspace.h"

#include <QMainWindow>
#include <QStringView>

#include <optional>

class QLabel;
class QTreeWidget;
class QTreeWidgetItem;
class QTextBrowser;

namespace loreforge::app {

class MainWindow final : public QMainWindow {
    Q_OBJECT

  public:
    explicit MainWindow(QWidget* parent = nullptr);

    [[nodiscard]] bool openProjectFile(QStringView filePath);

  private slots:
    void chooseProjectFile();
    void selectProjectItem(QTreeWidgetItem* current, QTreeWidgetItem* previous);
    void displayChapter(QTreeWidgetItem* current, QTreeWidgetItem* previous);

  private:
    void setWorkspace(StoredWorkspace workspace);
    void clearBookView();
    void showBook(qsizetype projectIndex, qsizetype bookIndex);
    void showLoadError(QString message);
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
    std::optional<StoredWorkspace> workspace_;
    qsizetype selectedProjectIndex_ = -1;
    qsizetype selectedBookIndex_ = -1;
};

} // namespace loreforge::app
