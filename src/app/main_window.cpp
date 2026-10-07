#include "main_window.h"

#include "context_inspector.h"
#include "document_metrics.h"
#include "loreforge/parser/markdown_source_parser.h"
#include "loreforge/storage/book_repository.h"
#include "loreforge/storage/project_database.h"
#include "loreforge/storage/repair_queue_repository.h"
#include "repair_queue_widget.h"

#include <QAction>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTemporaryDir>
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
    auto* fileMenu = menuBar()->addMenu(tr("&File"));
    auto* importAction = new QAction(tr("Import Markdown Source..."), this);
    importAction->setObjectName(QStringLiteral("importMarkdownSourceAction"));
    connect(importAction, &QAction::triggered, this, &MainWindow::chooseMarkdownSource);
    fileMenu->addAction(importAction);
    fileMenu->addAction(openAction);
    auto* viewMenu = menuBar()->addMenu(tr("&View"));

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
    documentSummary_ =
        new QLabel(tr("Import a Markdown source directory or open a stored project."), details);
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
        tr("<h2>LoreForge</h2><p>Use File &gt; Import Markdown Source to read a maintained "
           "novel directory containing SUMMARY.md, or Open Project to inspect stored data.</p>"));
    detailsLayout->addWidget(panel(tr("Reader"), reader_, details), 1);
    splitter->addWidget(details);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 0);
    splitter->setStretchFactor(2, 1);
    centralLayout->addWidget(splitter, 1);
    setCentralWidget(central);

    contextInspector_ = new ContextInspectorWidget(this);
    auto* contextDock = new QDockWidget(tr("Context Inspector"), this);
    contextDock->setObjectName(QStringLiteral("contextInspectorDock"));
    contextDock->setWidget(contextInspector_);
    contextDock->setMinimumWidth(360);
    addDockWidget(Qt::RightDockWidgetArea, contextDock);
    viewMenu->addAction(contextDock->toggleViewAction());

    repairQueue_ = new RepairQueueWidget(this);
    auto* repairDock = new QDockWidget(tr("Repair Queue"), this);
    repairDock->setObjectName(QStringLiteral("repairQueueDock"));
    repairDock->setWidget(repairQueue_);
    repairDock->setMinimumWidth(440);
    addDockWidget(Qt::BottomDockWidgetArea, repairDock);
    viewMenu->addAction(repairDock->toggleViewAction());
    connect(repairQueue_, &RepairQueueWidget::candidateApproved, this,
            &MainWindow::approveRepairCandidate);
    connect(repairQueue_, &RepairQueueWidget::candidateRejected, this,
            &MainWindow::rejectRepairCandidate);
    connect(repairQueue_, &RepairQueueWidget::suggestionEdited, this,
            &MainWindow::editRepairSuggestion);
    connect(repairQueue_, &RepairQueueWidget::termProtectionRequested, this,
            &MainWindow::protectRepairTerm);

    connect(projectExplorer_, &QTreeWidget::currentItemChanged, this,
            &MainWindow::selectProjectItem);
    connect(chapterTree_, &QTreeWidget::currentItemChanged, this, &MainWindow::displayChapter);
    statusBar()->showMessage(tr("Ready"));
}

void MainWindow::inspectContext(const context::ContextInspectorData& context) {
    contextInspector_->inspect(context);
}

bool MainWindow::importMarkdownSource(QStringView sourcePath, QStringView projectPath) {
    auto parsed = parser::MarkdownSourceParser::parseDirectory(sourcePath);
    const auto fail = [this](const QString& message) {
        // A failed import leaves the currently opened project usable.
        statusBar()->showMessage(tr("Import failed: %1").arg(message));
        return false;
    };
    if (const auto* error = std::get_if<parser::MarkdownSourceError>(&parsed)) {
        return fail(error->message);
    }
    const auto& imported = std::get<parser::MarkdownSourceImport>(parsed);
    QFileInfo target(projectPath.toString());
    const auto parent = QFileInfo(target.absolutePath()).canonicalFilePath();
    if (projectPath.isEmpty() || target.exists() || parent.isEmpty()) {
        return fail(tr("Choose a new project file in an existing directory; existing files are "
                       "never overwritten."));
    }
    const auto relative = QDir(imported.document.metadata.sourceLocator).relativeFilePath(parent);
    if (!QDir::isAbsolutePath(relative) && relative != QStringLiteral("..") &&
        !relative.startsWith(QStringLiteral("../"))) {
        return fail(tr("Save the project outside the read-only novel source directory."));
    }
    // Stage in the destination filesystem; publish only after every repository write succeeds.
    QTemporaryDir staging(QDir(parent).filePath(QStringLiteral(".loreforge-import-XXXXXX")));
    if (!staging.isValid()) {
        return fail(tr("Could not create an import staging directory."));
    }
    const auto stagedPath = staging.filePath(QStringLiteral("project.loreforge"));
    auto created = storage::ProjectDatabase::create(stagedPath);
    if (const auto* error = std::get_if<storage::StorageError>(&created)) {
        return fail(error->message);
    }
    auto database = std::get<std::unique_ptr<storage::ProjectDatabase>>(std::move(created));
    storage::ProjectRepository projects(*database);
    storage::BookRepository books(*database);
    const auto projectId = core::ProjectId::fromStableKey(target.absoluteFilePath());
    if (const auto error = projects.create(
            {projectId, imported.document.metadata.title, QDateTime::currentDateTimeUtc()})) {
        return fail(error->message);
    }
    if (const auto error = books.saveDocument(projectId, imported.document)) {
        return fail(error->message);
    }
    auto loaded = ProjectWorkspaceLoader::load(*database);
    if (const auto* error = std::get_if<storage::StorageError>(&loaded)) {
        return fail(error->message);
    }
    auto workspace = std::get<StoredWorkspace>(std::move(loaded));
    database.reset();
    if (!QFile::rename(stagedPath, target.absoluteFilePath())) {
        return fail(tr("Could not publish the imported project; no existing file was replaced."));
    }
    openedDatabasePath_ = target.absoluteFilePath();
    workspace.databasePath = openedDatabasePath_;
    repairQueue_->setItems({});
    setWorkspace(std::move(workspace));
    statusBar()->showMessage(
        tr("Imported %1 volumes, %2 chapters; %3 supplementary entries excluded. Source unchanged.")
            .arg(imported.volumeCount)
            .arg(imported.document.chapters.size())
            .arg(imported.excludedPaths.size()));
    return true;
}

void MainWindow::chooseMarkdownSource() {
    const auto source =
        QFileDialog::getExistingDirectory(this, tr("Select Markdown novel source (SUMMARY.md)"));
    if (source.isEmpty()) {
        return;
    }
    const auto destination = QFileDialog::getSaveFileName(
        this, tr("Save imported project outside the source directory"),
        QStringLiteral("novel.loreforge"), tr("LoreForge projects (*.loreforge)"));
    if (destination.isEmpty()) {
        return;
    }
    if (!importMarkdownSource(source, destination)) {
        QMessageBox::warning(this, tr("Markdown import failed"), statusBar()->currentMessage());
    }
}

void MainWindow::inspectRepairQueue(QList<proofreading::RepairQueueItem> items) {
    repairQueue_->setItems(std::move(items));
}

void MainWindow::approveRepairCandidate(QString candidateId) {
    if (openedDatabasePath_.isEmpty()) {
        return;
    }
    const auto id = core::ProofreadingCandidateId::fromString(candidateId);
    auto opened = storage::ProjectDatabase::open(openedDatabasePath_);
    if (!id || std::holds_alternative<storage::StorageError>(opened)) {
        statusBar()->showMessage(tr("Repair decision could not be saved."), 5000);
        reloadRepairQueue();
        return;
    }
    auto database = std::get<std::unique_ptr<storage::ProjectDatabase>>(std::move(opened));
    storage::RepairQueueRepository repairs(*database);
    const auto status = repairs.approve(*id, QDateTime::currentDateTimeUtc());
    if (status.has_value()) {
        statusBar()->showMessage(status->message, 5000);
        database.reset();
        reloadRepairQueue();
        return;
    }
    statusBar()->showMessage(tr("Candidate approved."), 3000);
}

void MainWindow::rejectRepairCandidate(QString candidateId) {
    if (openedDatabasePath_.isEmpty()) {
        return;
    }
    const auto id = core::ProofreadingCandidateId::fromString(candidateId);
    auto opened = storage::ProjectDatabase::open(openedDatabasePath_);
    if (!id || std::holds_alternative<storage::StorageError>(opened)) {
        statusBar()->showMessage(tr("Repair decision could not be saved."), 5000);
        reloadRepairQueue();
        return;
    }
    auto database = std::get<std::unique_ptr<storage::ProjectDatabase>>(std::move(opened));
    storage::RepairQueueRepository repairs(*database);
    const auto status = repairs.reject(*id, QDateTime::currentDateTimeUtc());
    if (status.has_value()) {
        statusBar()->showMessage(status->message, 5000);
        database.reset();
        reloadRepairQueue();
        return;
    }
    statusBar()->showMessage(tr("Candidate rejected."), 3000);
}

void MainWindow::editRepairSuggestion(QString candidateId, QString suggestion) {
    if (openedDatabasePath_.isEmpty()) {
        return;
    }
    const auto id = core::ProofreadingCandidateId::fromString(candidateId);
    auto opened = storage::ProjectDatabase::open(openedDatabasePath_);
    if (!id || std::holds_alternative<storage::StorageError>(opened)) {
        statusBar()->showMessage(tr("Edited suggestion could not be saved."), 5000);
        reloadRepairQueue();
        return;
    }
    auto database = std::get<std::unique_ptr<storage::ProjectDatabase>>(std::move(opened));
    storage::RepairQueueRepository repairs(*database);
    const auto status =
        repairs.editSuggestion(*id, std::move(suggestion), QDateTime::currentDateTimeUtc());
    if (status.has_value()) {
        statusBar()->showMessage(status->message, 5000);
        database.reset();
        reloadRepairQueue();
        return;
    }
    statusBar()->showMessage(tr("Suggestion updated; approval is required again."), 3000);
}

void MainWindow::protectRepairTerm(QString candidateId, QString canonicalSpelling) {
    if (openedDatabasePath_.isEmpty()) {
        return;
    }
    const auto id = core::ProofreadingCandidateId::fromString(candidateId);
    auto opened = storage::ProjectDatabase::open(openedDatabasePath_);
    if (!id || std::holds_alternative<storage::StorageError>(opened)) {
        statusBar()->showMessage(tr("Protected term could not be saved."), 5000);
        reloadRepairQueue();
        return;
    }
    auto database = std::get<std::unique_ptr<storage::ProjectDatabase>>(std::move(opened));
    storage::RepairQueueRepository repairs(*database);
    const auto status =
        repairs.ignoreAndProtect(*id, tr("Protected from Repair Queue: %1").arg(canonicalSpelling),
                                 QDateTime::currentDateTimeUtc());
    if (status.has_value()) {
        statusBar()->showMessage(status->message, 5000);
        database.reset();
        reloadRepairQueue();
        return;
    }
    statusBar()->showMessage(tr("Candidate rejected and term protected."), 3000);
}

void MainWindow::reloadRepairQueue() {
    if (openedDatabasePath_.isEmpty() || !workspace_.has_value()) {
        return;
    }
    auto opened = storage::ProjectDatabase::open(openedDatabasePath_);
    if (std::holds_alternative<storage::StorageError>(opened)) {
        return;
    }
    auto database = std::get<std::unique_ptr<storage::ProjectDatabase>>(std::move(opened));
    storage::RepairQueueRepository repairs(*database);
    QList<proofreading::RepairQueueItem> items;
    for (const auto& project : workspace_->projects) {
        const auto listed = repairs.list(project.metadata.id);
        if (std::holds_alternative<storage::StorageError>(listed)) {
            return;
        }
        items.append(std::get<QList<proofreading::RepairQueueItem>>(listed));
    }
    repairQueue_->setItems(std::move(items));
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
    if (std::holds_alternative<storage::StorageError>(workspaceResult)) {
        showLoadError(std::get<storage::StorageError>(workspaceResult).message);
        return false;
    }
    auto workspace = std::get<StoredWorkspace>(std::move(workspaceResult));
    QList<proofreading::RepairQueueItem> repairItems;
    storage::RepairQueueRepository repairs(*database);
    for (const auto& project : workspace.projects) {
        const auto listed = repairs.list(project.metadata.id);
        if (std::holds_alternative<storage::StorageError>(listed)) {
            showLoadError(std::get<storage::StorageError>(listed).message);
            return false;
        }
        repairItems.append(std::get<QList<proofreading::RepairQueueItem>>(listed));
    }
    database.reset();
    openedDatabasePath_ = absolutePath;
    setWorkspace(std::move(workspace));
    repairQueue_->setItems(std::move(repairItems));
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
    openedDatabasePath_.clear();
    workspace_.reset();
    repairQueue_->setItems({});
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
