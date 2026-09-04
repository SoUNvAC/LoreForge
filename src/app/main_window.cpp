#include "main_window.h"

#include "document_metrics.h"
#include "loreforge/storage/project_database.h"

#include <QAction>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTextBrowser>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidget>

#include <memory>
#include <utility>
#include <variant>

namespace loreforge::app {
namespace {

constexpr int kProjectIndexRole = Qt::UserRole;
constexpr int kBookIndexRole = Qt::UserRole + 1;
constexpr int kChapterIndexRole = Qt::UserRole + 2;

QGroupBox* panel(QString title, QWidget* content, QWidget* parent) {
    auto* group = new QGroupBox(std::move(title), parent);
    auto* layout = new QVBoxLayout(group);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->addWidget(content);
    return group;
}

QString authorsText(const QStringList& authors) {
    return authors.isEmpty() ? MainWindow::tr("Unknown") : authors.join(QStringLiteral(", "));
}

QString chapterHtml(const document::Chapter& chapter) {
    QString html =
        QStringLiteral("<style>body{font-family:serif;line-height:1.55;margin:20px;}"
                       "h2{margin-top:0;}p{white-space:pre-wrap;}hr{margin:1.5em 25%;}</style>");
    for (const auto& block : chapter.blocks) {
        const auto escaped = block.text.toHtmlEscaped();
        switch (block.type) {
        case document::BlockType::Heading:
            html += QStringLiteral("<h2>%1</h2>").arg(escaped);
            break;
        case document::BlockType::Paragraph:
        case document::BlockType::Unknown:
            html += QStringLiteral("<p>%1</p>").arg(escaped);
            break;
        case document::BlockType::SceneBreak:
            html += escaped.isEmpty()
                        ? QStringLiteral("<hr>")
                        : QStringLiteral("<p style=\"text-align:center\">%1</p>").arg(escaped);
            break;
        }
    }
    return html;
}

} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(tr("LoreForge"));
    resize(1280, 760);

    auto* openAction = new QAction(tr("Open Project..."), this);
    openAction->setObjectName(QStringLiteral("openProjectAction"));
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, &MainWindow::chooseProjectFile);
    menuBar()->addMenu(tr("&File"))->addAction(openAction);

    auto* central = new QWidget(this);
    auto* centralLayout = new QVBoxLayout(central);
    workspaceStatus_ = new QLabel(tr("○ No stored project is open"), central);
    workspaceStatus_->setObjectName(QStringLiteral("workspaceStatus"));
    workspaceStatus_->setStyleSheet(QStringLiteral("font-weight:600;color:#666;"));
    centralLayout->addWidget(workspaceStatus_);

    auto* splitter = new QSplitter(Qt::Horizontal, central);
    projectExplorer_ = new QTreeWidget;
    projectExplorer_->setObjectName(QStringLiteral("projectExplorer"));
    projectExplorer_->setHeaderLabels({tr("Project / Book"), tr("Kind")});
    projectExplorer_->header()->setStretchLastSection(false);
    projectExplorer_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    projectExplorer_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    projectExplorer_->setMinimumWidth(260);
    splitter->addWidget(panel(tr("Project Explorer"), projectExplorer_, splitter));

    chapterTree_ = new QTreeWidget;
    chapterTree_->setObjectName(QStringLiteral("chapterTree"));
    chapterTree_->setHeaderLabels({tr("Chapter"), tr("Words"), tr("Blocks")});
    chapterTree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    chapterTree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    chapterTree_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    chapterTree_->setMinimumWidth(310);
    splitter->addWidget(panel(tr("Chapter Tree"), chapterTree_, splitter));

    auto* details = new QWidget(splitter);
    auto* detailsLayout = new QVBoxLayout(details);
    detailsLayout->setContentsMargins(0, 0, 0, 0);
    documentSummary_ = new QLabel(tr("Open a stored LoreForge project to begin."), details);
    documentSummary_->setObjectName(QStringLiteral("documentSummary"));
    documentSummary_->setWordWrap(true);
    documentSummary_->setStyleSheet(QStringLiteral("font-size:16px;font-weight:600;"));
    detailsLayout->addWidget(documentSummary_);

    bookMetadata_ = new QLabel(details);
    bookMetadata_->setObjectName(QStringLiteral("bookMetadata"));
    bookMetadata_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    bookMetadata_->setWordWrap(true);
    detailsLayout->addWidget(panel(tr("Book Metadata"), bookMetadata_, details));

    sourceInfo_ = new QLabel(details);
    sourceInfo_->setObjectName(QStringLiteral("sourceInfo"));
    sourceInfo_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    sourceInfo_->setWordWrap(true);
    detailsLayout->addWidget(panel(tr("Source Information"), sourceInfo_, details));

    chapterMetadata_ = new QLabel(details);
    chapterMetadata_->setObjectName(QStringLiteral("chapterMetadata"));
    chapterMetadata_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    chapterMetadata_->setWordWrap(true);
    detailsLayout->addWidget(panel(tr("Chapter Metadata"), chapterMetadata_, details));

    chapterStatus_ = new QLabel(tr("○ No chapter selected"), details);
    chapterStatus_->setObjectName(QStringLiteral("chapterStatus"));
    chapterStatus_->setStyleSheet(QStringLiteral("font-weight:600;color:#666;"));
    detailsLayout->addWidget(chapterStatus_);

    reader_ = new QTextBrowser(details);
    reader_->setObjectName(QStringLiteral("chapterReader"));
    reader_->setHtml(
        tr("<h2>LoreForge</h2><p>Use File &gt; Open Project to inspect stored data.</p>"));
    detailsLayout->addWidget(panel(tr("Reader"), reader_, details), 1);
    splitter->addWidget(details);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 0);
    splitter->setStretchFactor(2, 1);
    centralLayout->addWidget(splitter, 1);
    setCentralWidget(central);

    connect(projectExplorer_, &QTreeWidget::currentItemChanged, this,
            &MainWindow::selectProjectItem);
    connect(chapterTree_, &QTreeWidget::currentItemChanged, this, &MainWindow::displayChapter);
    statusBar()->showMessage(tr("Ready"));
}

bool MainWindow::openProjectFile(QStringView filePath) {
    const auto requestedPath = filePath.trimmed();
    if (requestedPath.isEmpty()) {
        showLoadError(tr("A project file path is required."));
        return false;
    }
    const auto absolutePath = QFileInfo(requestedPath.toString()).absoluteFilePath();
    auto databaseResult = storage::ProjectDatabase::open(absolutePath);
    if (std::holds_alternative<storage::StorageError>(databaseResult)) {
        showLoadError(std::get<storage::StorageError>(databaseResult).message);
        return false;
    }
    auto database = std::get<std::unique_ptr<storage::ProjectDatabase>>(std::move(databaseResult));
    auto workspaceResult = ProjectWorkspaceLoader::load(*database);
    database.reset();
    if (std::holds_alternative<storage::StorageError>(workspaceResult)) {
        showLoadError(std::get<storage::StorageError>(workspaceResult).message);
        return false;
    }
    setWorkspace(std::get<StoredWorkspace>(std::move(workspaceResult)));
    statusBar()->showMessage(tr("Opened stored project data from %1").arg(absolutePath), 5000);
    return true;
}

void MainWindow::chooseProjectFile() {
    const auto filePath = QFileDialog::getOpenFileName(
        this, tr("Open LoreForge Project"), {},
        tr("LoreForge projects (*.loreforge *.sqlite *.sqlite3 *.db);;All files (*)"));
    if (filePath.isEmpty()) {
        return;
    }
    if (!openProjectFile(filePath)) {
        QMessageBox::critical(this, tr("Open failed"), workspaceStatus_->text());
    }
}

void MainWindow::setWorkspace(StoredWorkspace workspace) {
    workspace_ = std::move(workspace);
    selectedProjectIndex_ = -1;
    selectedBookIndex_ = -1;
    projectExplorer_->clear();
    clearBookView();

    qsizetype bookCount = 0;
    for (qsizetype projectIndex = 0; projectIndex < workspace_->projects.size(); ++projectIndex) {
        const auto& project = workspace_->projects.at(projectIndex);
        auto* projectItem =
            new QTreeWidgetItem(projectExplorer_, {project.metadata.name, tr("Project")});
        projectItem->setData(0, kProjectIndexRole, projectIndex);
        projectItem->setData(0, kBookIndexRole, -1);
        projectItem->setToolTip(
            0, tr("Project ID: %1\nCreated: %2")
                   .arg(project.metadata.id.toString(),
                        project.metadata.createdAt.toLocalTime().toString(Qt::ISODate)));
        for (qsizetype bookIndex = 0; bookIndex < project.books.size(); ++bookIndex) {
            const auto& book = project.books.at(bookIndex);
            auto* bookItem =
                new QTreeWidgetItem(projectItem, {book.metadata.title, tr("Stored book")});
            bookItem->setData(0, kProjectIndexRole, projectIndex);
            bookItem->setData(0, kBookIndexRole, bookIndex);
            bookItem->setToolTip(0, tr("Book ID: %1").arg(book.id.toString()));
            ++bookCount;
        }
        projectItem->setExpanded(true);
    }

    if (workspace_->projects.isEmpty()) {
        workspaceStatus_->setText(tr("○ Stored workspace contains no projects"));
        workspaceStatus_->setStyleSheet(QStringLiteral("font-weight:600;color:#8a6500;"));
        workspaceStatus_->setToolTip(workspace_->databasePath);
        return;
    }
    workspaceStatus_->setText(tr("● Stored workspace loaded · %1 projects · %2 books")
                                  .arg(workspace_->projects.size())
                                  .arg(bookCount));
    workspaceStatus_->setStyleSheet(QStringLiteral("font-weight:600;color:#167345;"));
    workspaceStatus_->setToolTip(workspace_->databasePath);
    auto* firstProject = projectExplorer_->topLevelItem(0);
    projectExplorer_->setCurrentItem(firstProject->childCount() > 0 ? firstProject->child(0)
                                                                    : firstProject);
}

void MainWindow::selectProjectItem(QTreeWidgetItem* current, QTreeWidgetItem* previous) {
    static_cast<void>(previous);
    if (current == nullptr || !workspace_.has_value()) {
        clearBookView();
        return;
    }
    const auto projectIndex = current->data(0, kProjectIndexRole).toLongLong();
    const auto bookIndex = current->data(0, kBookIndexRole).toLongLong();
    if (bookIndex < 0) {
        if (current->childCount() > 0) {
            projectExplorer_->setCurrentItem(current->child(0));
        } else {
            clearBookView();
            documentSummary_->setText(tr("This stored project contains no books."));
        }
        return;
    }
    showBook(projectIndex, bookIndex);
}

void MainWindow::showBook(qsizetype projectIndex, qsizetype bookIndex) {
    if (!workspace_.has_value() || projectIndex < 0 ||
        projectIndex >= workspace_->projects.size() || bookIndex < 0 ||
        bookIndex >= workspace_->projects.at(projectIndex).books.size()) {
        clearBookView();
        return;
    }
    selectedProjectIndex_ = projectIndex;
    selectedBookIndex_ = bookIndex;
    const auto& book = workspace_->projects.at(projectIndex).books.at(bookIndex);
    chapterTree_->clear();
    for (const auto& chapter : book.chapters) {
        const auto metrics = DocumentMetrics::forChapter(chapter);
        auto* item =
            new QTreeWidgetItem(chapterTree_, {chapter.title, QString::number(metrics.wordCount),
                                               QString::number(metrics.blockCount)});
        item->setData(0, kChapterIndexRole, chapter.index);
        item->setToolTip(0, tr("Chapter ID: %1").arg(chapter.id.toString()));
    }

    documentSummary_->setText(tr("%1 — %2 chapters, %3 words")
                                  .arg(book.metadata.title)
                                  .arg(book.chapters.size())
                                  .arg(DocumentMetrics::wordCount(book)));
    bookMetadata_->setText(
        tr("Book ID: %1\nAuthors: %2\nLanguage: %3")
            .arg(book.id.toString(), authorsText(book.metadata.authors), book.metadata.language));
    sourceInfo_->setText(tr("Format: %1\nLocation: %2\nSHA-256: %3")
                             .arg(book.metadata.sourceFormat.toUpper(), book.metadata.sourceLocator,
                                  book.metadata.sourceHash.toHex()));
    if (!book.chapters.isEmpty()) {
        chapterTree_->setCurrentItem(chapterTree_->topLevelItem(0));
    } else {
        chapterMetadata_->clear();
        chapterStatus_->setText(tr("○ Stored book contains no chapters"));
        reader_->clear();
    }
}

void MainWindow::displayChapter(QTreeWidgetItem* current, QTreeWidgetItem* previous) {
    static_cast<void>(previous);
    const auto* book = selectedBook();
    if (current == nullptr || book == nullptr) {
        chapterMetadata_->clear();
        chapterStatus_->setText(tr("○ No chapter selected"));
        reader_->clear();
        return;
    }
    const auto chapterIndex = current->data(0, kChapterIndexRole).toLongLong();
    if (chapterIndex < 0 || chapterIndex >= book->chapters.size()) {
        chapterMetadata_->clear();
        chapterStatus_->setText(tr("○ No chapter selected"));
        reader_->clear();
        return;
    }
    const auto& chapter = book->chapters.at(chapterIndex);
    const auto metrics = DocumentMetrics::forChapter(chapter);
    chapterMetadata_->setText(tr("Chapter %1 of %2\nChapter ID: %3\nBlocks: %4\nWords: %5")
                                  .arg(chapter.index + 1)
                                  .arg(book->chapters.size())
                                  .arg(chapter.id.toString())
                                  .arg(metrics.blockCount)
                                  .arg(metrics.wordCount));
    chapterStatus_->setText(tr("● Stored chapter · %1 source spans").arg(metrics.sourceSpanCount));
    chapterStatus_->setStyleSheet(QStringLiteral("font-weight:600;color:#167345;"));
    reader_->setHtml(chapterHtml(chapter));
    statusBar()->showMessage(tr("Chapter %1 of %2 — %3 words")
                                 .arg(chapter.index + 1)
                                 .arg(book->chapters.size())
                                 .arg(metrics.wordCount));
}

void MainWindow::clearBookView() {
    selectedProjectIndex_ = -1;
    selectedBookIndex_ = -1;
    chapterTree_->clear();
    documentSummary_->setText(tr("Select a stored book in Project Explorer."));
    bookMetadata_->clear();
    sourceInfo_->clear();
    chapterMetadata_->clear();
    chapterStatus_->setText(tr("○ No chapter selected"));
    chapterStatus_->setStyleSheet(QStringLiteral("font-weight:600;color:#666;"));
    reader_->clear();
}

void MainWindow::showLoadError(QString message) {
    workspace_.reset();
    projectExplorer_->clear();
    clearBookView();
    workspaceStatus_->setText(tr("Open failed: %1").arg(std::move(message)));
    workspaceStatus_->setStyleSheet(QStringLiteral("font-weight:600;color:#a12622;"));
    workspaceStatus_->setToolTip({});
    statusBar()->showMessage(tr("Open failed"));
}

const document::Document* MainWindow::selectedBook() const {
    if (!workspace_.has_value() || selectedProjectIndex_ < 0 ||
        selectedProjectIndex_ >= workspace_->projects.size() || selectedBookIndex_ < 0 ||
        selectedBookIndex_ >= workspace_->projects.at(selectedProjectIndex_).books.size()) {
        return nullptr;
    }
    return &workspace_->projects.at(selectedProjectIndex_).books.at(selectedBookIndex_);
}

} // namespace loreforge::app
