#include "main_window.h"
#include "character_highlight_preview.h"
#include "dialogue_segmentation.h"
#include "startup_profile.h"
#include "stream_monitor_widget.h"

#include "context_inspector.h"
#include "document_metrics.h"
#include "llm_workbench.h"
#include "loreforge/parser/markdown_source_parser.h"
#include "loreforge/storage/book_repository.h"
#include "loreforge/storage/project_database.h"
#include "loreforge/storage/repair_queue_repository.h"
#include "novel_analysis_workbench.h"
#include "repair_queue_widget.h"

#include <QAction>
#include <QCloseEvent>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QScopedValueRollback>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QTextCursor>
#include <QThread>
#include <QTimer>
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
constexpr int kChapterDetailsRole = Qt::UserRole + 3;

// ResizeToContents reacts synchronously to every dataChanged signal. Freeze geometry as
// well as painting while filling hundreds of rows; disabling painting alone is insufficient.
class ChapterTreeUpdate final {
  public:
    explicit ChapterTreeUpdate(QTreeWidget* tree) : tree_(tree), updates_(tree->updatesEnabled()) {
        tree_->setUpdatesEnabled(false);
        for (int column = 0; column < tree_->columnCount(); ++column) {
            modes_.append(tree_->header()->sectionResizeMode(column));
            if (modes_.last() == QHeaderView::ResizeToContents)
                tree_->header()->setSectionResizeMode(column, QHeaderView::Interactive);
        }
    }
    ~ChapterTreeUpdate() {
        for (int column = 0; column < modes_.size(); ++column)
            tree_->header()->setSectionResizeMode(column, modes_[column]);
        tree_->setUpdatesEnabled(updates_);
    }

  private:
    QTreeWidget* tree_;
    bool updates_;
    QList<QHeaderView::ResizeMode> modes_;
};

QIcon chapterStatusIcon(const QColor& color) {
    QPixmap pixmap(16, 16);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(color.darker(120));
    painter.setBrush(color);
    painter.drawEllipse(QRectF(3, 3, 10, 10));
    return QIcon(pixmap);
}

QString navigationSettingsPath() {
    const auto overridePath = qEnvironmentVariable("LOREFORGE_NAVIGATION_SETTINGS");
    return overridePath.isEmpty()
               ? QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) +
                     QStringLiteral("/workspace-navigation.ini")
               : overridePath;
}
QString navigationHash(const QString& value) {
    return QString::fromLatin1(
        QCryptographicHash::hash(value.toUtf8(), QCryptographicHash::Sha256).toHex());
}
QString navigationGroup(const QString& filePath) {
    auto path = QFileInfo(filePath).canonicalFilePath();
    if (path.isEmpty())
        path = QFileInfo(filePath).absoluteFilePath();
#ifdef Q_OS_WIN
    path = path.toCaseFolded();
#endif
    return QStringLiteral("workspaces/") + navigationHash(path);
}
QString navigationChapterKey(const QString& projectId, const QString& bookId) {
    return QStringLiteral("chapters/") + navigationHash(projectId + QLatin1Char('/') + bookId);
}

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
    taskIndicator_ = new TaskIndicator(this);
    statusBar()->addWidget(taskIndicator_);
    statusMessage_ = new QLabel(this);
    statusMessage_->setObjectName(QStringLiteral("workspaceStatusMessage"));
    statusMessage_->setTextFormat(Qt::PlainText);
    statusMessage_->setMinimumWidth(0);
    statusMessage_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    statusBar()->addWidget(statusMessage_, 1);
    statusMessageTimer_ = new QTimer(this);
    statusMessageTimer_->setSingleShot(true);
    connect(statusMessageTimer_, &QTimer::timeout, this, [this] {
        statusMessage_->clear();
        statusMessage_->setToolTip({});
    });

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

    auto* pages = new QTabWidget(central);
    pages->setObjectName(QStringLiteral("workspacePages"));
    auto* readingPage = new QWidget(pages);
    auto* readingLayout = new QVBoxLayout(readingPage);
    readingLayout->setContentsMargins(0, 0, 0, 0);
    auto* splitter = new QSplitter(Qt::Horizontal, readingPage);
    splitter->setObjectName(QStringLiteral("workspaceSplitter"));
    auto* projectColumn = new QSplitter(Qt::Vertical, splitter);
    projectColumn->setObjectName(QStringLiteral("projectColumnSplitter"));
    projectColumn->setChildrenCollapsible(false);
    projectExplorer_ = new QTreeWidget;
    projectExplorer_->setObjectName(QStringLiteral("projectExplorer"));
    projectExplorer_->setHeaderLabels({tr("Project / Book"), tr("Kind")});
    projectExplorer_->header()->setStretchLastSection(false);
    projectExplorer_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    projectExplorer_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    projectExplorer_->setMinimumWidth(260);
    auto* explorerPanel = panel(tr("Project Explorer"), projectExplorer_, projectColumn);
    explorerPanel->setObjectName(QStringLiteral("projectExplorerPanel"));
    projectColumn->addWidget(explorerPanel);

    auto* metadataScroll = new QScrollArea(projectColumn);
    metadataScroll->setObjectName(QStringLiteral("metadataScrollArea"));
    metadataScroll->setWidgetResizable(true);
    metadataScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    metadataScroll->setFrameShape(QFrame::NoFrame);
    projectColumn->addWidget(metadataScroll);
    projectColumn->setStretchFactor(0, 1);
    projectColumn->setStretchFactor(1, 3);
    projectColumn->setSizes({180, 540});
    splitter->addWidget(projectColumn);

    chapterTree_ = new QTreeWidget;
    chapterTree_->setObjectName(QStringLiteral("chapterTree"));
    chapterTree_->setUniformRowHeights(true);
    chapterTree_->setHeaderLabels(
        {tr("Chapter"), tr("字数"), tr("汉字"), tr("Blocks"), tr("分析"), tr("错字")});
    chapterTree_->headerItem()->setToolTip(
        4, tr("独立对白分割的最新流程：绿=通过，灰=未分析，黄=未完成/"
              "待确认，红=失败，蓝=运行中。点击查看角色详情。"));
    chapterTree_->headerItem()->setToolTip(
        5,
        tr("独立对白分析附带的模型校对建议；未发现不代表保证无错字。点击查看原文、建议与原因。"));
    const auto countPolicy =
        tr("只统计正文，不含标题、标点、空白或符号。字数按字母和数字逐字符计；汉字仅计汉字。");
    chapterTree_->headerItem()->setToolTip(1, countPolicy);
    chapterTree_->headerItem()->setToolTip(2, countPolicy);
    chapterTree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    chapterTree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    chapterTree_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    chapterTree_->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    chapterTree_->header()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    chapterTree_->header()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    chapterTree_->setMinimumWidth(410);
    splitter->addWidget(panel(tr("Chapter Tree"), chapterTree_, splitter));

    auto* details = new QWidget(metadataScroll);
    details->setObjectName(QStringLiteral("metadataPanel"));
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
    detailsLayout->addStretch();
    for (auto* label :
         {documentSummary_, bookMetadata_, sourceInfo_, chapterMetadata_, chapterStatus_}) {
        label->setWordWrap(true);
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        label->setMinimumWidth(0);
    }
    metadataScroll->setWidget(details);

    reader_ = new QTextBrowser;
    reader_->setObjectName(QStringLiteral("chapterReader"));
    reader_->setHtml(
        tr("<h2>LoreForge</h2><p>Use File &gt; Import Markdown Source to read a maintained "
           "novel directory containing SUMMARY.md, or Open Project to inspect stored data.</p>"));
    auto* readerPanel = panel(tr("Reader"), reader_, splitter);
    readerPanel->setObjectName(QStringLiteral("readerPanel"));
    splitter->addWidget(readerPanel);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 0);
    splitter->setStretchFactor(2, 1);
    readingLayout->addWidget(splitter);
    pages->addTab(readingPage, tr("阅读工作台"));
    auto* llmScroll = new QScrollArea(pages);
    llmScroll->setWidgetResizable(true);
    llmScroll->setFrameShape(QFrame::NoFrame);
    llmWorkbench_ = new LlmWorkbench(llmScroll);
    llmScroll->setWidget(llmWorkbench_);
    pages->addTab(llmScroll, tr("LLM 工作台"));
    auto* analysisScroll = new QScrollArea(pages);
    analysisScroll->setWidgetResizable(true);
    analysisScroll->setFrameShape(QFrame::NoFrame);
    analysisWorkbench_ = new NovelAnalysisWorkbench(analysisScroll);
    analysisScroll->setWidget(analysisWorkbench_);
    pages->addTab(analysisScroll, tr("小说分析"));
    auto* highlightPreview = new CharacterHighlightPreview(pages);
    pages->addTab(highlightPreview, tr("分割预览"));
    connect(analysisWorkbench_, &NovelAnalysisWorkbench::highlightImportedTextChanged,
            highlightPreview, &CharacterHighlightPreview::showImportedText);
    connect(analysisWorkbench_, &NovelAnalysisWorkbench::highlightSourceChanged, highlightPreview,
            &CharacterHighlightPreview::showAnalysis);
    analysisController_ = new ChapterAnalysisController(this);
    analysisWorkbench_->setConnectionValidator([this, pages] {
        const auto result = llmWorkbench_->analysisConnection();
        if (const auto* error = std::get_if<QString>(&result)) {
            const auto message = tr("请先配置当前工程的 API：%1").arg(*error);
            pages->setCurrentIndex(1);
            showStatusMessage(message);
            return message;
        }
        const auto& connection = std::get<AnalysisConnection>(result);
        analysisWorkbench_->setConnectionSummary(
            tr("模型：%1\n接口：%2\n思考：%3")
                .arg(connection.options.defaultModel, connection.options.endpoint.toString(),
                     !connection.options.enableThinking.has_value()
                         ? tr("跟随服务端")
                         : (*connection.options.enableThinking ? tr("开启") : tr("关闭"))));
        return QString{};
    });
    connect(llmWorkbench_, &LlmWorkbench::taskStatus, taskIndicator_, &TaskIndicator::report);
    connect(analysisController_, &ChapterAnalysisController::taskStatus, this,
            [this](TaskState state, const QString& text) {
                taskIndicator_->report(analysisTaskId_, state, text);
                if (analysisController_->busy())
                    refreshChapterIndicators();
            });
    connect(analysisWorkbench_, &NovelAnalysisWorkbench::chapterPreviewRequested, this,
            &MainWindow::prepareChapterPreview);
    connect(analysisWorkbench_, &NovelAnalysisWorkbench::chapterSelected, this, [this](int index) {
        if (index >= 0 && index < chapterTree_->topLevelItemCount()) {
            chapterTree_->setCurrentItem(chapterTree_->topLevelItem(index));
        }
    });
    connect(analysisWorkbench_, &NovelAnalysisWorkbench::analysisRequested, this,
            &MainWindow::startChapterAnalysis);
    connect(
        analysisWorkbench_, &NovelAnalysisWorkbench::continuationRequested, this,
        [this](QString pipeline, bool legacy) { runChapterAnalysis(std::move(pipeline), legacy); });
    connect(analysisWorkbench_, &NovelAnalysisWorkbench::cancellationRequested, this, [this] {
        stopAutoAnalysis();
        analysisController_->cancel();
    });
    connect(analysisWorkbench_, &NovelAnalysisWorkbench::autoNextChanged, this,
            [this](bool enabled) {
                if (!enabled)
                    ++autoNextGeneration_;
            });
    connect(analysisWorkbench_, &NovelAnalysisWorkbench::metricsChanged, llmWorkbench_,
            &LlmWorkbench::showAnalysisMetrics);
    connect(llmWorkbench_, &LlmWorkbench::configurationChanged, analysisWorkbench_,
            &NovelAnalysisWorkbench::invalidatePreview);
    connect(llmWorkbench_, &LlmWorkbench::configurationChanged, this,
            &MainWindow::stopAutoAnalysis);
    connect(analysisWorkbench_, &NovelAnalysisWorkbench::budgetsChanged, this,
            &MainWindow::stopAutoAnalysis);
    connect(llmWorkbench_, &LlmWorkbench::analysisBudgetsLoaded, analysisWorkbench_,
            &NovelAnalysisWorkbench::restoreBudgets);
    connect(analysisWorkbench_, &NovelAnalysisWorkbench::budgetsChanged, llmWorkbench_,
            &LlmWorkbench::setAnalysisBudgets);
    connect(analysisController_, &ChapterAnalysisController::busyChanged, llmWorkbench_,
            &LlmWorkbench::setAnalysisBusy);
    connect(analysisController_, &ChapterAnalysisController::busyChanged, analysisWorkbench_,
            &NovelAnalysisWorkbench::setBusy);
    connect(analysisController_, &ChapterAnalysisController::roleChanged, analysisWorkbench_,
            &NovelAnalysisWorkbench::updateRole);
    connect(analysisController_, &ChapterAnalysisController::roleChanged, llmWorkbench_,
            [this](int role, const QString& status, const QString& usage, qint64 latency) {
                llmWorkbench_->showAnalysisMetrics(
                    tr("当前小说任务：角色 %1 · %2\n%3 · 响应耗时 %4 ms\n"
                       "SSE 接收状态见底部；精确生成速度未提供，任务结束后刷新项目/本章累计。")
                        .arg(role + 1)
                        .arg(status, usage)
                        .arg(latency));
            });
    connect(analysisController_, &ChapterAnalysisController::roleChanged, this,
            [this](int role, const QString& status, const QString&, qint64) {
                if (role == 0 && status.startsWith(QStringLiteral("输出校验通过")))
                    reloadAnalysisHistory(
                        true); // Refresh only this chapter after a persisted result.
            });
    connect(analysisController_, &ChapterAnalysisController::finished, this,
            [this](QString status) {
                // Cancellation can finish after the selection has changed. Refresh the old row too.
                const auto* book = selectedBook();
                const auto* item = chapterTree_->currentItem();
                if (!book || !item ||
                    book->id.toString() !=
                        activeAnalysis_.value(QStringLiteral("book_id")).toString() ||
                    item->data(0, kChapterIndexRole).toLongLong() !=
                        activeAnalysis_.value(QStringLiteral("chapter_index")).toInteger(-1))
                    clearAnalysisHistoryCache();
                reloadAnalysisHistory(true);
                analysisWorkbench_->showError(std::move(status));
                scheduleNextChapter();
            });
    centralLayout->addWidget(pages, 1);
    auto* streamMonitor = new StreamMonitorWidget(central);
    centralLayout->addWidget(streamMonitor);
    connect(analysisController_, &ChapterAnalysisController::streamStarted, streamMonitor,
            &StreamMonitorWidget::beginRequest);
    connect(analysisController_, &ChapterAnalysisController::streamProgress, streamMonitor,
            &StreamMonitorWidget::updateRequest);
    connect(analysisController_, &ChapterAnalysisController::streamFinished, streamMonitor,
            &StreamMonitorWidget::finishRequest);
    connect(llmWorkbench_, &LlmWorkbench::streamStarted, streamMonitor,
            &StreamMonitorWidget::beginRequest);
    connect(llmWorkbench_, &LlmWorkbench::streamProgress, streamMonitor,
            &StreamMonitorWidget::updateRequest);
    connect(llmWorkbench_, &LlmWorkbench::streamFinished, streamMonitor,
            &StreamMonitorWidget::finishRequest);
    setCentralWidget(central);

    contextInspector_ = new ContextInspectorWidget(this);
    auto* contextDock = new QDockWidget(tr("Context Inspector"), this);
    contextDock->setObjectName(QStringLiteral("contextInspectorDock"));
    contextDock->setWidget(contextInspector_);
    contextDock->setMinimumWidth(360);
    addDockWidget(Qt::RightDockWidgetArea, contextDock);
    viewMenu->addAction(contextDock->toggleViewAction());
    auto* breathing = viewMenu->addAction(tr("任务灯呼吸动画"));
    breathing->setObjectName(QStringLiteral("taskBreathingAction"));
    breathing->setCheckable(true);
    breathing->setChecked(true);
    connect(breathing, &QAction::toggled, taskIndicator_, &TaskIndicator::setAnimationEnabled);

    repairQueue_ = new RepairQueueWidget(this);
    auto* repairDock = new QDockWidget(tr("Repair Queue"), this);
    repairDock->setObjectName(QStringLiteral("repairQueueDock"));
    repairDock->setWidget(repairQueue_);
    repairDock->setMinimumWidth(440);
    addDockWidget(Qt::BottomDockWidgetArea, repairDock);
    viewMenu->addAction(repairDock->toggleViewAction());
    connect(pages, &QTabWidget::currentChanged, this,
            [contextDock, repairDock, contextVisible = true, repairVisible = true,
             wasReading = true](int index) mutable {
                if (index != 0) {
                    if (wasReading) {
                        contextVisible = !contextDock->isHidden();
                        repairVisible = !repairDock->isHidden();
                    }
                    contextDock->hide();
                    repairDock->hide();
                } else {
                    contextDock->setVisible(contextVisible);
                    repairDock->setVisible(repairVisible);
                }
                wasReading = index == 0;
                contextDock->toggleViewAction()->setEnabled(index == 0);
                repairDock->toggleViewAction()->setEnabled(index == 0);
            });
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
    connect(chapterTree_, &QTreeWidget::itemClicked, this, &MainWindow::showChapterDetails);
    showStatusMessage(tr("Ready"));
}

MainWindow::~MainWindow() {
    closing_ = true;
    cancelProjectOpen();
    clearAnalysisHistoryCache();
    stopAutoAnalysis();
    analysisController_->cancel();
    for (auto* loader : std::as_const(backgroundLoaders_)) {
        disconnect(loader, nullptr, this, nullptr);
        loader->requestInterruption();
        loader->wait();
        delete loader;
    }
}

void MainWindow::closeEvent(QCloseEvent* event) {
    closing_ = true;
    cancelProjectOpen();
    clearAnalysisHistoryCache();
    stopAutoAnalysis();
    analysisController_->cancel();
    QMainWindow::closeEvent(event);
}

void MainWindow::showStatusMessage(const QString& message, int timeoutMs) {
    statusMessageTimer_->stop();
    statusMessage_->setText(message);
    statusMessage_->setToolTip(message);
    if (timeoutMs > 0) {
        statusMessageTimer_->start(timeoutMs);
    }
}

void MainWindow::inspectContext(const context::ContextInspectorData& context) {
    contextInspector_->inspect(context);
}

bool MainWindow::importMarkdownSource(QStringView sourcePath, QStringView projectPath) {
    if (!llmWorkbench_->flushConfiguration()) {
        showStatusMessage(tr("当前工程 API 配置保存失败，已保留当前工程与修改，未导入。"));
        return false;
    }
    const auto taskId = QUuid::createUuid();
    taskIndicator_->report(taskId, TaskState::Local, tr("正在导入小说"));
    auto parsed = parser::MarkdownSourceParser::parseDirectory(sourcePath);
    const auto fail = [this, taskId](const QString& message) {
        taskIndicator_->report(taskId, TaskState::Failed, tr("导入失败：%1").arg(message));
        // A failed import leaves the currently opened project usable.
        showStatusMessage(tr("Import failed: %1").arg(message));
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
    showStatusMessage(
        tr("Imported %1 volumes, %2 chapters; %3 supplementary entries excluded. Source unchanged.")
            .arg(imported.volumeCount)
            .arg(imported.document.chapters.size())
            .arg(imported.excludedPaths.size()));
    taskIndicator_->report(taskId, TaskState::Success, tr("小说导入完成"));
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
        QMessageBox::warning(this, tr("Markdown import failed"), statusMessage_->text());
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
    auto opened = storage::ProjectDatabase::openForSession(openedDatabasePath_);
    if (!id || std::holds_alternative<storage::StorageError>(opened)) {
        showStatusMessage(tr("Repair decision could not be saved."), 5000);
        reloadRepairQueue();
        return;
    }
    auto database = std::get<std::unique_ptr<storage::ProjectDatabase>>(std::move(opened));
    storage::RepairQueueRepository repairs(*database);
    const auto status = repairs.approve(*id, QDateTime::currentDateTimeUtc());
    if (status.has_value()) {
        showStatusMessage(status->message, 5000);
        database.reset();
        reloadRepairQueue();
        return;
    }
    showStatusMessage(tr("Candidate approved."), 3000);
}

void MainWindow::rejectRepairCandidate(QString candidateId) {
    if (openedDatabasePath_.isEmpty()) {
        return;
    }
    const auto id = core::ProofreadingCandidateId::fromString(candidateId);
    auto opened = storage::ProjectDatabase::openForSession(openedDatabasePath_);
    if (!id || std::holds_alternative<storage::StorageError>(opened)) {
        showStatusMessage(tr("Repair decision could not be saved."), 5000);
        reloadRepairQueue();
        return;
    }
    auto database = std::get<std::unique_ptr<storage::ProjectDatabase>>(std::move(opened));
    storage::RepairQueueRepository repairs(*database);
    const auto status = repairs.reject(*id, QDateTime::currentDateTimeUtc());
    if (status.has_value()) {
        showStatusMessage(status->message, 5000);
        database.reset();
        reloadRepairQueue();
        return;
    }
    showStatusMessage(tr("Candidate rejected."), 3000);
}

void MainWindow::editRepairSuggestion(QString candidateId, QString suggestion) {
    if (openedDatabasePath_.isEmpty()) {
        return;
    }
    const auto id = core::ProofreadingCandidateId::fromString(candidateId);
    auto opened = storage::ProjectDatabase::openForSession(openedDatabasePath_);
    if (!id || std::holds_alternative<storage::StorageError>(opened)) {
        showStatusMessage(tr("Edited suggestion could not be saved."), 5000);
        reloadRepairQueue();
        return;
    }
    auto database = std::get<std::unique_ptr<storage::ProjectDatabase>>(std::move(opened));
    storage::RepairQueueRepository repairs(*database);
    const auto status =
        repairs.editSuggestion(*id, std::move(suggestion), QDateTime::currentDateTimeUtc());
    if (status.has_value()) {
        showStatusMessage(status->message, 5000);
        database.reset();
        reloadRepairQueue();
        return;
    }
    showStatusMessage(tr("Suggestion updated; approval is required again."), 3000);
}

void MainWindow::protectRepairTerm(QString candidateId, QString canonicalSpelling) {
    if (openedDatabasePath_.isEmpty()) {
        return;
    }
    const auto id = core::ProofreadingCandidateId::fromString(candidateId);
    auto opened = storage::ProjectDatabase::openForSession(openedDatabasePath_);
    if (!id || std::holds_alternative<storage::StorageError>(opened)) {
        showStatusMessage(tr("Protected term could not be saved."), 5000);
        reloadRepairQueue();
        return;
    }
    auto database = std::get<std::unique_ptr<storage::ProjectDatabase>>(std::move(opened));
    storage::RepairQueueRepository repairs(*database);
    const auto status =
        repairs.ignoreAndProtect(*id, tr("Protected from Repair Queue: %1").arg(canonicalSpelling),
                                 QDateTime::currentDateTimeUtc());
    if (status.has_value()) {
        showStatusMessage(status->message, 5000);
        database.reset();
        reloadRepairQueue();
        return;
    }
    showStatusMessage(tr("Candidate rejected and term protected."), 3000);
}

void MainWindow::reloadRepairQueue() {
    if (openedDatabasePath_.isEmpty() || !workspace_.has_value()) {
        return;
    }
    auto opened = storage::ProjectDatabase::openForSession(openedDatabasePath_);
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

void MainWindow::trackBackgroundLoader(QThread* loader) {
    backgroundLoaders_.append(loader);
    connect(loader, &QThread::finished, this,
            [this, loader] { backgroundLoaders_.removeAll(loader); });
}

void MainWindow::updateLoadingControls() {
    if (analysisWorkbench_)
        analysisWorkbench_->setHistoryLoading(projectLoader_ || historyLoader_);
    if (projectExplorer_)
        projectExplorer_->setEnabled(!projectLoader_);
    if (chapterTree_)
        chapterTree_->setEnabled(!projectLoader_);
}

void MainWindow::cancelProjectOpen() {
    ++projectLoadGeneration_;
    if (projectLoader_) {
        projectLoader_->requestInterruption();
        projectLoader_ = nullptr;
        taskIndicator_->report(projectLoadTaskId_, TaskState::Cancelled, tr("已取消打开工程"));
    }
    updateLoadingControls();
}

bool MainWindow::openProjectFileAsync(QStringView filePath) {
    if (!llmWorkbench_->flushConfiguration()) {
        showStatusMessage(tr("当前工程 API 配置保存失败，未切换工程。"));
        return false;
    }
    closing_ = false;
    cancelProjectOpen();
    stopAutoAnalysis();
    analysisController_->cancel();
    clearAnalysisHistoryCache();
    const auto path = QFileInfo(filePath.trimmed().toString()).absoluteFilePath();
    if (filePath.trimmed().isEmpty()) {
        showLoadError(tr("A project file path is required."));
        return false;
    }
    struct OpenResult {
        std::optional<StoredWorkspace> workspace;
        QList<proofreading::RepairQueueItem> repairs;
        std::optional<storage::StorageError> error;
    };
    const auto result = std::make_shared<OpenResult>();
    const auto generation = projectLoadGeneration_;
    auto* loader = QThread::create([path, result] {
        // SQLite connection and every repository stay on this worker thread.
        auto opened = storage::ProjectDatabase::open(path); // Full audit once per project open.
        if (const auto* error = std::get_if<storage::StorageError>(&opened)) {
            result->error = *error;
            return;
        }
        if (QThread::currentThread()->isInterruptionRequested())
            return;
        auto database = std::get<std::unique_ptr<storage::ProjectDatabase>>(std::move(opened));
        auto workspace = ProjectWorkspaceLoader::load(*database);
        if (const auto* error = std::get_if<storage::StorageError>(&workspace)) {
            result->error = *error;
            return;
        }
        result->workspace = std::get<StoredWorkspace>(std::move(workspace));
        storage::RepairQueueRepository repairs(*database);
        for (const auto& project : result->workspace->projects) {
            if (QThread::currentThread()->isInterruptionRequested())
                return;
            auto listed = repairs.list(project.metadata.id);
            if (const auto* error = std::get_if<storage::StorageError>(&listed)) {
                result->error = *error;
                result->workspace.reset();
                return;
            }
            result->repairs.append(
                std::get<QList<proofreading::RepairQueueItem>>(std::move(listed)));
        }
    });
    projectLoader_ = loader;
    projectLoadTaskId_ = QUuid::createUuid();
    const auto taskId = projectLoadTaskId_;
    trackBackgroundLoader(loader);
    connect(loader, &QThread::finished, this, [this, loader, generation, result, path, taskId] {
        if (closing_ || generation != projectLoadGeneration_ || projectLoader_ != loader)
            return;
        projectLoader_ = nullptr;
        updateLoadingControls();
        if (result->error || !result->workspace) {
            const auto message = result->error ? result->error->message : tr("工程加载已取消。");
            showLoadError(message);
            taskIndicator_->report(taskId, TaskState::Failed, tr("打开工程失败：%1").arg(message));
            return;
        }
        openedDatabasePath_ = path;
        setWorkspace(std::move(*result->workspace));
        repairQueue_->setItems(std::move(result->repairs));
        taskIndicator_->report(taskId, TaskState::Success, tr("工程已打开，可开始阅读"));
        showStatusMessage(historyLoader_ ? tr("工程已打开，分析状态正在后台加载，可先阅读。")
                                         : tr("工程已打开。"),
                          5000);
    });
    connect(loader, &QThread::finished, loader, &QObject::deleteLater);
    updateLoadingControls();
    taskIndicator_->report(taskId, TaskState::Local, tr("正在后台校验并打开工程，可继续操作窗口"));
    showStatusMessage(tr("正在后台打开 %1…").arg(QFileInfo(path).fileName()));
    loader->start();
    return true;
}

bool MainWindow::openProjectFile(QStringView filePath) {
    closing_ = false;
    cancelProjectOpen();
    if (!llmWorkbench_->flushConfiguration()) {
        showStatusMessage(tr("当前工程 API 配置保存失败，已保留当前工程与修改，未切换。"));
        return false;
    }
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
    showStatusMessage(tr("Opened stored project data from %1").arg(absolutePath), 5000);
    return true;
}

void MainWindow::chooseProjectFile() {
    const auto filePath = QFileDialog::getOpenFileName(
        this, tr("Open LoreForge Project"), {},
        tr("LoreForge projects (*.loreforge *.sqlite *.sqlite3 *.db);;All files (*)"));
    if (filePath.isEmpty()) {
        return;
    }
    if (!openProjectFileAsync(filePath)) {
        QMessageBox::critical(this, tr("Open failed"), workspaceStatus_->text());
    }
}

void MainWindow::setWorkspace(StoredWorkspace workspace) {
    StartupProfile profile("apply-workspace");
    cancelProjectOpen();
    stopAutoAnalysis();
    QSettings navigation(navigationSettingsPath(), QSettings::IniFormat);
    navigation.beginGroup(navigationGroup(workspace.databasePath));
    const auto lastProject = navigation.value(QStringLiteral("last_project")).toString();
    const auto lastBook = navigation.value(QStringLiteral("last_book")).toString();
    analysisController_->cancel();
    llmWorkbench_->setProject({});
    workspace_ = std::move(workspace);
    selectedProjectIndex_ = -1;
    selectedBookIndex_ = -1;
    projectExplorer_->clear();
    clearBookView();

    qsizetype bookCount = 0;
    QTreeWidgetItem* rememberedBook = nullptr;
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
            if (project.metadata.id.toString() == lastProject && book.id.toString() == lastBook)
                rememberedBook = bookItem;
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
    projectExplorer_->setCurrentItem(rememberedBook                   ? rememberedBook
                                     : firstProject->childCount() > 0 ? firstProject->child(0)
                                                                      : firstProject);
}

void MainWindow::selectProjectItem(QTreeWidgetItem* current, QTreeWidgetItem* previous) {
    if (!llmWorkbench_->flushConfiguration()) {
        const QSignalBlocker blocker(projectExplorer_);
        projectExplorer_->setCurrentItem(previous);
        showStatusMessage(tr("当前 API 配置保存失败，已保留修改，未切换工程。"));
        return;
    }
    if (current == nullptr || !workspace_.has_value()) {
        clearBookView();
        return;
    }
    const auto projectIndex = current->data(0, kProjectIndexRole).toLongLong();
    if (projectIndex >= 0 && projectIndex < workspace_->projects.size()) {
        analysisController_->cancel();
        llmWorkbench_->setProject(openedDatabasePath_,
                                  workspace_->projects[projectIndex].metadata.id);
    }
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
    StartupProfile profile("populate-book");
    if (!workspace_.has_value() || projectIndex < 0 ||
        projectIndex >= workspace_->projects.size() || bookIndex < 0 ||
        bookIndex >= workspace_->projects.at(projectIndex).books.size()) {
        clearBookView();
        return;
    }
    clearAnalysisHistoryCache();
    ChapterTreeUpdate treeUpdate(chapterTree_);
    selectedProjectIndex_ = projectIndex;
    selectedBookIndex_ = bookIndex;
    const auto& book = workspace_->projects.at(projectIndex).books.at(bookIndex);
    QSettings navigation(navigationSettingsPath(), QSettings::IniFormat);
    navigation.beginGroup(navigationGroup(workspace_->databasePath));
    const auto rememberedChapter =
        navigation
            .value(navigationChapterKey(workspace_->projects[projectIndex].metadata.id.toString(),
                                        book.id.toString()))
            .toString();
    chapterTree_->clear();
    QList<QTreeWidgetItem*> chapterItems;
    chapterItems.reserve(book.chapters.size());
    for (const auto& chapter : book.chapters) {
        const auto metrics = DocumentMetrics::forChapter(chapter);
        auto* item = new QTreeWidgetItem({chapter.title, QString::number(metrics.characterCount),
                                          QString::number(metrics.hanCharacterCount),
                                          QString::number(metrics.blockCount)});
        item->setData(0, kChapterIndexRole, chapter.index);
        item->setToolTip(0, tr("Chapter ID: %1").arg(chapter.id.toString()));
        chapterItems.append(item);
    }
    chapterTree_->addTopLevelItems(chapterItems);

    const auto totals = DocumentMetrics::forDocument(book);
    documentSummary_->setText(tr("%1 — %2 chapters · 总字数：%3 · 总汉字：%4")
                                  .arg(book.metadata.title)
                                  .arg(book.chapters.size())
                                  .arg(totals.characterCount)
                                  .arg(totals.hanCharacterCount));
    bookMetadata_->setText(
        tr("Book ID: %1\nAuthors: %2\nLanguage: %3")
            .arg(book.id.toString(), authorsText(book.metadata.authors), book.metadata.language));
    sourceInfo_->setText(tr("Format: %1\nLocation: %2\nSHA-256: %3")
                             .arg(book.metadata.sourceFormat.toUpper(), book.metadata.sourceLocator,
                                  book.metadata.sourceHash.toHex()));
    if (!book.chapters.isEmpty()) {
        qsizetype rememberedIndex = 0;
        for (qsizetype index = 0; index < book.chapters.size(); ++index) {
            if (book.chapters[index].id.toString() == rememberedChapter) {
                rememberedIndex = index;
                break;
            }
        }
        chapterTree_->setCurrentItem(chapterTree_->topLevelItem(rememberedIndex));
    } else {
        chapterMetadata_->clear();
        chapterStatus_->setText(tr("○ Stored book contains no chapters"));
        reader_->clear();
    }
}

void MainWindow::displayChapter(QTreeWidgetItem* current, QTreeWidgetItem* previous) {
    StartupProfile profile("display-chapter");
    if (!advancingAutomatically_)
        stopAutoAnalysis();
    analysisController_->cancel();
    static_cast<void>(previous);
    const auto* book = selectedBook();
    if (current == nullptr || book == nullptr) {
        analysisWorkbench_->setSelectedBook(nullptr, -1);
        chapterMetadata_->clear();
        chapterStatus_->setText(tr("○ No chapter selected"));
        reader_->clear();
        return;
    }
    const auto chapterIndex = current->data(0, kChapterIndexRole).toLongLong();
    if (chapterIndex < 0 || chapterIndex >= book->chapters.size()) {
        analysisWorkbench_->setSelectedBook(nullptr, -1);
        chapterMetadata_->clear();
        chapterStatus_->setText(tr("○ No chapter selected"));
        reader_->clear();
        return;
    }
    const auto& chapter = book->chapters.at(chapterIndex);
    bool navigationSaved = true;
    if (workspace_ && !workspace_->databasePath.isEmpty()) {
        QSettings navigation(navigationSettingsPath(), QSettings::IniFormat);
        navigation.beginGroup(navigationGroup(workspace_->databasePath));
        const auto projectId = workspace_->projects[selectedProjectIndex_].metadata.id.toString();
        navigation.setValue(QStringLiteral("last_project"), projectId);
        navigation.setValue(QStringLiteral("last_book"), book->id.toString());
        navigation.setValue(navigationChapterKey(projectId, book->id.toString()),
                            chapter.id.toString());
        navigation.sync();
        navigationSaved = navigation.status() == QSettings::NoError;
    }
    analysisWorkbench_->setSelectedBook(book, chapterIndex);
    reloadAnalysisHistory();
    const auto metrics = DocumentMetrics::forChapter(chapter);
    chapterMetadata_->setText(tr("Chapter %1 of %2\nChapter ID: %3\nBlocks: %4\n字数：%5\n汉字：%6")
                                  .arg(chapter.index + 1)
                                  .arg(book->chapters.size())
                                  .arg(chapter.id.toString())
                                  .arg(metrics.blockCount)
                                  .arg(metrics.characterCount)
                                  .arg(metrics.hanCharacterCount));
    chapterStatus_->setText(tr("● Stored chapter · %1 source spans").arg(metrics.sourceSpanCount));
    chapterStatus_->setStyleSheet(QStringLiteral("font-weight:600;color:#167345;"));
    reader_->setHtml(chapterHtml(chapter));
    showStatusMessage(tr("Chapter %1 of %2 — 字数：%3 · 汉字：%4")
                          .arg(chapter.index + 1)
                          .arg(book->chapters.size())
                          .arg(metrics.characterCount)
                          .arg(metrics.hanCharacterCount));
    chapterStatus_->setToolTip(navigationSaved ? QString{}
                                               : tr("章节位置未能保存，请检查本机配置目录权限。"));
    if (!navigationSaved)
        showStatusMessage(chapterStatus_->toolTip(), 5000);
}

void MainWindow::clearBookView() {
    stopAutoAnalysis();
    analysisController_->cancel();
    clearAnalysisHistoryCache();
    analysisWorkbench_->setSelectedBook(nullptr, -1);
    llmWorkbench_->showAnalysisMetrics(tr("小说任务统计：尚未选择项目。"));
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

void MainWindow::prepareChapterPreview(int maximumTokens, int reservedTokens) {
    const auto taskId = QUuid::createUuid();
    taskIndicator_->report(taskId, TaskState::Local, tr("正在准备章节预览"));
    const auto* book = selectedBook();
    const auto* item = chapterTree_->currentItem();
    if (!book || !item || openedDatabasePath_.isEmpty()) {
        analysisWorkbench_->showError(tr("准备失败；未发送。请先打开项目并选择章节。"));
        taskIndicator_->report(taskId, TaskState::Failed, tr("预览失败：请先选择章节"));
        return;
    }
    auto built = ChapterPreviewBuilder::build(
        workspace_->projects.at(selectedProjectIndex_).metadata.id, *book,
        item->data(0, kChapterIndexRole).toLongLong(), maximumTokens, reservedTokens,
        analysisWorkbench_->dialogueOnly());
    if (const auto* error = std::get_if<storage::StorageError>(&built)) {
        analysisWorkbench_->invalidatePreview();
        analysisWorkbench_->showError(tr("准备失败；未发送。%1").arg(error->message));
        taskIndicator_->report(taskId, TaskState::Failed, tr("预览失败：%1").arg(error->message));
        return;
    }
    auto opened = storage::ProjectDatabase::openForSession(openedDatabasePath_);
    if (const auto* error = std::get_if<storage::StorageError>(&opened)) {
        analysisWorkbench_->invalidatePreview();
        analysisWorkbench_->showError(tr("准备失败；未发送。%1").arg(error->message));
        taskIndicator_->report(taskId, TaskState::Failed, tr("预览失败：%1").arg(error->message));
        return;
    }
    auto database = std::get<std::unique_ptr<storage::ProjectDatabase>>(std::move(opened));
    const auto stored =
        ChapterPreviewBuilder::store(*database, std::get<ChapterPreview>(std::move(built)));
    if (const auto* error = std::get_if<storage::StorageError>(&stored)) {
        analysisWorkbench_->invalidatePreview();
        analysisWorkbench_->showError(tr("准备失败；未发送。%1").arg(error->message));
        taskIndicator_->report(taskId, TaskState::Failed, tr("预览失败：%1").arg(error->message));
        return;
    }
    analysisWorkbench_->showChapterPreview(std::get<ChapterPreview>(stored));
    taskIndicator_->report(taskId, TaskState::Success, tr("章节预览准备完成 · 尚未发送"));
}

void MainWindow::startChapterAnalysis() {
    runChapterAnalysis({}, false);
}

void MainWindow::runChapterAnalysis(QString resumePipeline, bool allowLegacyEndpoint) {
    if (projectLoader_ || historyLoader_) {
        analysisWorkbench_->showError(tr("工程/历史仍在后台加载；未发送小说。"));
        return;
    }
    ++autoNextGeneration_;
    const auto* book = selectedBook();
    const auto* item = chapterTree_->currentItem();
    if (!book || !item || !analysisWorkbench_->preview() || analysisController_->busy()) {
        return;
    }
    const auto preview = *analysisWorkbench_->preview();
    analysisTaskId_ = QUuid::createUuid();
    taskIndicator_->report(analysisTaskId_, TaskState::Local, tr("正在检查分析请求"));
    if (!llmWorkbench_->flushConfiguration()) {
        stopAutoAnalysis();
        analysisWorkbench_->showError(tr("工程 API 配置保存失败，未发送。"));
        taskIndicator_->report(analysisTaskId_, TaskState::Failed, tr("API 配置保存失败，未发送"));
        return;
    }
    const auto connection = llmWorkbench_->analysisConnection();
    if (const auto* error = std::get_if<QString>(&connection)) {
        stopAutoAnalysis();
        analysisWorkbench_->showError(*error);
        taskIndicator_->report(analysisTaskId_, TaskState::Failed, *error);
        return;
    }
    const auto checked = ChapterPreviewBuilder::build(
        preview.snapshot.projectId, *book, item->data(0, kChapterIndexRole).toLongLong(),
        preview.snapshot.content.value(QStringLiteral("maximum_tokens")).toInt(),
        preview.snapshot.content.value(QStringLiteral("reserved_output_tokens")).toInt(),
        preview.snapshot.content.value(QStringLiteral("task_kind")).toString() ==
            QStringLiteral("dialogue"));
    if (!std::holds_alternative<ChapterPreview>(checked) ||
        std::get<ChapterPreview>(checked).snapshot.id != preview.snapshot.id) {
        analysisWorkbench_->invalidatePreview();
        stopAutoAnalysis();
        analysisWorkbench_->showError(tr("原文或章节已变化；未发送，请重新准备。"));
        taskIndicator_->report(analysisTaskId_, TaskState::Failed,
                               tr("原文已变化；未发送，请重新准备"));
        return;
    }
    const auto chapterIndex = item->data(0, kChapterIndexRole).toLongLong();
    activeAnalysis_ =
        QJsonObject{{QStringLiteral("database_path"), openedDatabasePath_},
                    {QStringLiteral("book_id"), book->id.toString()},
                    {QStringLiteral("chapter_id"), book->chapters[chapterIndex].id.toString()},
                    {QStringLiteral("chapter_index"), chapterIndex},
                    {QStringLiteral("dialogue_only"),
                     preview.snapshot.content.value(QStringLiteral("task_kind")).toString() ==
                         QStringLiteral("dialogue")},
                    {QStringLiteral("maximum_tokens"),
                     preview.snapshot.content.value(QStringLiteral("maximum_tokens"))},
                    {QStringLiteral("reserved_output_tokens"),
                     preview.snapshot.content.value(QStringLiteral("reserved_output_tokens"))}};
    analysisController_->start(
        openedDatabasePath_, *book, item->data(0, kChapterIndexRole).toLongLong(), preview,
        std::get<AnalysisConnection>(connection), std::move(resumePipeline), allowLegacyEndpoint);
}

void MainWindow::stopAutoAnalysis() {
    ++autoNextGeneration_;
    analysisWorkbench_->stopAutoNext();
}

void MainWindow::scheduleNextChapter() {
    if (!analysisWorkbench_->autoNextEnabled())
        return;
    if (!analysisController_->canAdvanceChapter()) {
        stopAutoAnalysis();
        showStatusMessage(tr("自动续章已停止：任务取消或本地/安全检查未通过；请查看本章记录。"));
        return;
    }
    const auto context = activeAnalysis_;
    const auto generation = ++autoNextGeneration_;
    QTimer::singleShot(0, this, [this, context, generation] {
        if (generation != autoNextGeneration_ || !analysisWorkbench_->autoNextEnabled() ||
            analysisController_->busy())
            return;
        const auto* book = selectedBook();
        const auto* item = chapterTree_->currentItem();
        const auto index = context.value(QStringLiteral("chapter_index")).toInteger(-1);
        if (!book || !item ||
            openedDatabasePath_ != context.value(QStringLiteral("database_path")).toString() ||
            book->id.toString() != context.value(QStringLiteral("book_id")).toString() ||
            index < 0 || index >= book->chapters.size() ||
            item->data(0, kChapterIndexRole).toLongLong() != index ||
            book->chapters[index].id.toString() !=
                context.value(QStringLiteral("chapter_id")).toString()) {
            stopAutoAnalysis();
            return;
        }
        if (index + 1 >= book->chapters.size()) {
            stopAutoAnalysis();
            analysisWorkbench_->showError(
                tr("已到末章，自动分析结束；失败和待确认结果保留在各章任务历史中。"));
            return;
        }
        {
            QScopedValueRollback<bool> transition(advancingAutomatically_, true);
            chapterTree_->setCurrentItem(chapterTree_->topLevelItem(index + 1));
        }
        if (!analysisWorkbench_->prepareAutomaticChapter(
                context.value(QStringLiteral("dialogue_only")).toBool(),
                context.value(QStringLiteral("maximum_tokens")).toInt(),
                context.value(QStringLiteral("reserved_output_tokens")).toInt())) {
            stopAutoAnalysis();
            return;
        }
        runChapterAnalysis({}, false);
    });
}

void MainWindow::clearAnalysisHistoryCache() {
    ++historyLoadGeneration_;
    if (historyLoader_) {
        historyLoader_->requestInterruption();
        historyLoader_ = nullptr;
    }
    readyHistory_.reset();
    analysisHistoryIndexed_ = false;
    analysisRunChapters_.clear();
    analysisChapterHistory_.clear();
    analysisHistoryLru_.clear();
    analysisChapterUsage_.clear();
    analysisProjectUsage_ = {};
    updateLoadingControls();
}

void MainWindow::startHistoryLoad() {
    if (historyLoader_ || closing_ || !workspace_ || selectedProjectIndex_ < 0)
        return;
    struct Result {
        storage::StorageResult<QList<AnalysisHistoryEntry>> history = QList<AnalysisHistoryEntry>{};
        QHash<QString, QString> index;
    };
    const auto result = std::make_shared<Result>();
    const auto generation = historyLoadGeneration_;
    const auto path = openedDatabasePath_;
    const auto projectId = workspace_->projects[selectedProjectIndex_].metadata.id;
    auto* loader = QThread::create([result, path, projectId] {
        result->history = ChapterAnalysisController::history(path, projectId, {}, &result->index);
    });
    historyLoader_ = loader;
    trackBackgroundLoader(loader);
    connect(loader, &QThread::finished, this, [this, loader, generation, result] {
        if (closing_ || generation != historyLoadGeneration_ || historyLoader_ != loader)
            return;
        historyLoader_ = nullptr;
        analysisRunChapters_ = std::move(result->index);
        readyHistory_ = std::move(result->history);
        updateLoadingControls();
        reloadAnalysisHistory(); // Apply to the currently selected chapter, not the starting one.
    });
    connect(loader, &QThread::finished, loader, &QObject::deleteLater);
    ChapterTreeUpdate treeUpdate(chapterTree_);
    for (int row = 0; row < chapterTree_->topLevelItemCount(); ++row) {
        auto* item = chapterTree_->topLevelItem(row);
        item->setText(4, tr("加载中"));
        item->setIcon(4, QIcon{});
        item->setText(5, tr("加载中"));
        item->setToolTip(4, tr("正在后台读取历史，尚不能判断是否已分析。"));
        item->setToolTip(5, tr("正在后台读取历史，尚不能判断校对状态。"));
        item->setData(4, kChapterDetailsRole, QJsonObject{});
    }
    updateLoadingControls();
    loader->start();
}

void MainWindow::reloadAnalysisHistory(bool refresh) {
    StartupProfile profile("apply-history");
    if (closing_)
        return;
    const auto* book = selectedBook();
    const auto* item = chapterTree_->currentItem();
    if (!book || !item || openedDatabasePath_.isEmpty()) {
        return;
    }
    const auto index = item->data(0, kChapterIndexRole).toLongLong();
    if (index < 0 || index >= book->chapters.size()) {
        return;
    }
    const auto chapterId = book->chapters[index].id.toString();
    if (historyLoader_) {
        updateLoadingControls();
        return;
    }
    if (analysisHistoryIndexed_ && !refresh && analysisChapterHistory_.contains(chapterId)) {
        analysisHistoryLru_.removeAll(chapterId);
        analysisHistoryLru_.append(chapterId);
        analysisWorkbench_->showHistory(analysisChapterHistory_.value(chapterId), chapterId,
                                        analysisProjectUsage_);
        return;
    }
    const bool initialLoad = !analysisHistoryIndexed_;
    // Tiny projects complete synchronously without a loading flash. Larger histories never
    // deserialize novel snapshots or provider artifacts on the GUI thread at initial open.
    if (initialLoad && !readyHistory_) {
        auto opened = storage::ProjectDatabase::openForSession(openedDatabasePath_);
        if (auto* database = std::get_if<std::unique_ptr<storage::ProjectDatabase>>(&opened)) {
            storage::LLMRunRepository runs(**database);
            const auto listed =
                runs.listForProject(workspace_->projects[selectedProjectIndex_].metadata.id);
            if (const auto* records = std::get_if<QList<storage::LLMRunRecord>>(&listed);
                records && records->size() >= 24) {
                startHistoryLoad();
                return;
            }
        }
    }
    const auto history =
        readyHistory_
            ? std::move(*readyHistory_)
            : ChapterAnalysisController::history(
                  openedDatabasePath_, workspace_->projects.at(selectedProjectIndex_).metadata.id,
                  initialLoad ? QStringView{} : QStringView(chapterId), &analysisRunChapters_);
    readyHistory_.reset();
    if (const auto* error = std::get_if<storage::StorageError>(&history)) {
        ChapterTreeUpdate treeUpdate(chapterTree_);
        clearAnalysisHistoryCache();
        analysisWorkbench_->showError(tr("历史读取失败：%1").arg(error->message));
        for (int row = 0; row < chapterTree_->topLevelItemCount(); ++row) {
            auto* chapter = chapterTree_->topLevelItem(row);
            chapter->setText(4, tr("读取失败"));
            chapter->setIcon(4, chapterStatusIcon(QColor(QStringLiteral("#c53535"))));
            chapter->setText(5, tr("未知"));
            chapter->setData(4, kChapterDetailsRole, QJsonObject{});
        }
        return;
    }
    const auto& loaded = std::get<QList<AnalysisHistoryEntry>>(history);
    QList<AnalysisHistoryEntry> currentHistory;
    QHash<QString, QPair<qint64, int>> usage;
    for (const auto& entry : loaded) {
        const auto id = entry.context.content.value(QStringLiteral("chapter_id")).toString();
        if (entry.outcome.value(QStringLiteral("usage_known")).toBool())
            usage[id].first += entry.run.totalTokens;
        else
            ++usage[id].second;
        if (id == chapterId)
            currentHistory.append(entry);
    }
    if (initialLoad) {
        analysisChapterUsage_ = usage;
        analysisProjectUsage_ = {};
        for (const auto& value : usage) {
            analysisProjectUsage_.first += value.first;
            analysisProjectUsage_.second += value.second;
        }
    } else {
        const auto previous = analysisChapterUsage_.value(chapterId);
        const auto current = usage.value(chapterId);
        analysisProjectUsage_.first += current.first - previous.first;
        analysisProjectUsage_.second += current.second - previous.second;
        analysisChapterUsage_[chapterId] = current;
    }
    analysisChapterHistory_[chapterId] = currentHistory;
    analysisHistoryLru_.removeAll(chapterId);
    analysisHistoryLru_.append(chapterId);
    // Bound full histories: keep four recently visited chapters, not the entire novel's requests.
    while (analysisHistoryLru_.size() > 4)
        analysisChapterHistory_.remove(analysisHistoryLru_.takeFirst());
    analysisHistoryIndexed_ = true;

    // Populate the whole tree only once; newly saved results update just their chapter.
    QHash<QString, QList<AnalysisHistoryEntry>> chapters;
    for (const auto& entry : loaded) {
        if (entry.context.content.value(QStringLiteral("book_id")).toString() ==
                book->id.toString() &&
            entry.context.content.value(QStringLiteral("task_kind")).toString() ==
                QStringLiteral("dialogue"))
            chapters[entry.context.content.value(QStringLiteral("chapter_id")).toString()].append(
                entry);
    }
    ChapterTreeUpdate treeUpdate(chapterTree_);
    for (int row = 0; row < chapterTree_->topLevelItemCount(); ++row) {
        auto* chapter = chapterTree_->topLevelItem(row);
        const auto chapterIndex = chapter->data(0, kChapterIndexRole).toInt();
        if (chapterIndex < 0 || chapterIndex >= book->chapters.size())
            continue;
        if (!initialLoad && chapterIndex != index)
            continue;
        const auto entries = chapters.value(book->chapters[chapterIndex].id.toString());
        QString state = tr("未分析"), color = QStringLiteral("#92979e");
        QStringList details;
        QJsonObject payload;
        if (!entries.isEmpty()) {
            const auto pipeline =
                entries.last().context.content.value(QStringLiteral("pipeline_id")).toString();
            const auto roles = latestPipelineRoles(entries, pipeline);
            const auto prefix = completedRolePrefix(roles);
            bool failed = false;
            const QStringList names{tr("分析员"), tr("复核员"), tr("审查员")};
            for (int role = 0; role < 3; ++role) {
                const auto* entry = roles[role];
                QString status = tr("尚未运行");
                if (entry) {
                    const bool valid = entry->run.status == storage::LLMRunStatus::Succeeded &&
                                       entry->artifacts.validation.isValid() &&
                                       entry->artifacts.parsedResponse.has_value();
                    failed |= entry->run.status == storage::LLMRunStatus::Failed ||
                              (entry->run.status == storage::LLMRunStatus::Succeeded && !valid);
                    status = valid ? tr("输出校验通过")
                             : entry->run.status == storage::LLMRunStatus::Failed ? tr("失败")
                             : entry->run.status == storage::LLMRunStatus::Cancelled
                                 ? tr("已取消")
                                 : tr("未完成/可能中断");
                    if (valid && role > 0)
                        status += QStringLiteral(" · ") + entry->artifacts.parsedResponse->object()
                                                              .value(QStringLiteral("verdict"))
                                                              .toString();
                    if (!entry->run.errorMessage.isEmpty())
                        status += QStringLiteral("\n") + entry->run.errorMessage;
                }
                details.append(names[role] + QStringLiteral("：") + status);
            }
            state = failed ? tr("失败") : prefix == 3 ? tr("待确认") : tr("未完成");
            color = failed ? QStringLiteral("#c53535") : QStringLiteral("#b98312");
            if (prefix >= 1) {
                auto normalized =
                    roles[0]->outcome.value(QStringLiteral("normalized_output")).toObject();
                if (normalized.isEmpty())
                    normalized = resolveDialogue(roles[0]->artifacts.parsedResponse->object(),
                                                 roles[0]->context.content)
                                     .output;
                payload = normalized;
                payload.remove(QStringLiteral("segments"));
                payload.remove(QStringLiteral("characters"));
                payload.insert(QStringLiteral("source"),
                               roles[0]->context.content.value(QStringLiteral("source")));
                if (prefix == 3 &&
                    roles[1]->artifacts.parsedResponse->object()
                            .value(QStringLiteral("verdict"))
                            .toString() == QStringLiteral("pass") &&
                    roles[2]->artifacts.parsedResponse->object()
                            .value(QStringLiteral("verdict"))
                            .toString() == QStringLiteral("pass") &&
                    normalized.value(QStringLiteral("extraction_issues")).toArray().isEmpty() &&
                    !normalized.isEmpty()) {
                    state = tr("已分析");
                    color = QStringLiteral("#26834b");
                }
            }
        }
        payload.insert(QStringLiteral("analysis_details"), details.join(QLatin1Char('\n')));
        chapter->setData(4, kChapterDetailsRole, payload);
        chapter->setText(4, state);
        chapter->setIcon(4, chapterStatusIcon(QColor(color)));
        chapter->setToolTip(4, tr("独立对白分割 · 最新流程（历史快照）\n%1\n%2")
                                   .arg(state, details.join(QLatin1Char('\n'))));
        const auto proofState = payload.value(QStringLiteral("proofreading_state")).toString();
        const auto count = payload.value(QStringLiteral("proofreading")).toArray().size();
        chapter->setText(5, count > 0                                 ? tr("疑似 %1 处").arg(count)
                            : proofState == QStringLiteral("checked") ? tr("未发现")
                            : proofState == QStringLiteral("invalid") ? tr("需核对")
                                                                      : tr("待检查"));
        chapter->setToolTip(
            5, tr("%1。模型建议，不保证检出所有错字，不自动修改原文。点击查看快照及详情。%2")
                   .arg(chapter->text(5), proofState == QStringLiteral("invalid")
                                              ? tr("\n存在无法定位的校对条目，请人工核对。")
                                              : QString{}));
    }
    refreshChapterIndicators();
    analysisWorkbench_->showHistory(currentHistory, chapterId, analysisProjectUsage_);
}

void MainWindow::refreshChapterIndicators() {
    if (!analysisController_->busy() ||
        !activeAnalysis_.value(QStringLiteral("dialogue_only")).toBool())
        return;
    const auto* book = selectedBook();
    if (!book ||
        activeAnalysis_.value(QStringLiteral("database_path")).toString() != openedDatabasePath_ ||
        activeAnalysis_.value(QStringLiteral("book_id")).toString() != book->id.toString())
        return;
    const auto index = activeAnalysis_.value(QStringLiteral("chapter_index")).toInt(-1);
    if (index >= 0 && index < chapterTree_->topLevelItemCount()) {
        auto* item = chapterTree_->topLevelItem(index);
        item->setText(4, tr("运行中"));
        item->setIcon(4, chapterStatusIcon(QColor(QStringLiteral("#287bca"))));
        item->setToolTip(4, tr("正在运行独立对白分割；结束后刷新为最终状态。"));
    }
}

void MainWindow::showChapterDetails(QTreeWidgetItem* item, int column) {
    if (!item || (column != 4 && column != 5))
        return;
    if (historyLoader_) {
        showStatusMessage(tr("历史仍在后台加载，请稍后查看详情。"), 5000);
        return;
    }
    const auto payload = item->data(4, kChapterDetailsRole).toJsonObject();
    if (column == 4) {
        QMessageBox::information(this, tr("章节分析状态"),
                                 item->text(0) + QStringLiteral("\n") + item->toolTip(4));
        return;
    }
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("chapterProofreadingDialog"));
    dialog.setWindowTitle(tr("校对问题 — %1").arg(item->text(0)));
    dialog.resize(850, 600);
    auto* layout = new QVBoxLayout(&dialog);
    auto* note = new QLabel(item->toolTip(5), &dialog);
    note->setWordWrap(true);
    layout->addWidget(note);
    auto* table = new QTreeWidget(&dialog);
    table->setObjectName(QStringLiteral("chapterProofreadingIssues"));
    table->setHeaderLabels({tr("原文"), tr("建议"), tr("原因")});
    table->header()->setSectionResizeMode(QHeaderView::Stretch);
    layout->addWidget(table);
    auto* source = new QPlainTextEdit(&dialog);
    source->setObjectName(QStringLiteral("chapterProofreadingSource"));
    source->setReadOnly(true);
    const auto snapshot = payload.value(QStringLiteral("source")).toObject();
    const auto bytes = snapshot.value(QStringLiteral("utf8")).toString().toUtf8();
    const auto base = snapshot.value(QStringLiteral("start_byte")).toInteger();
    source->setPlainText(QString::fromUtf8(bytes));
    source->setPlaceholderText(tr("此章尚无有效的校对快照，请运行新版独立对白分割。"));
    layout->addWidget(
        new QLabel(tr("只读原文快照（不是当前文件；选中问题定位，不自动修改）"), &dialog));
    layout->addWidget(source, 1);
    for (const auto& value : payload.value(QStringLiteral("proofreading")).toArray()) {
        const auto issue = value.toObject();
        auto* row =
            new QTreeWidgetItem(table, {issue.value(QStringLiteral("original")).toString(),
                                        issue.value(QStringLiteral("suggestion")).toString(),
                                        issue.value(QStringLiteral("reason")).toString()});
        row->setData(0, Qt::UserRole, issue);
    }
    connect(table, &QTreeWidget::currentItemChanged, source,
            [source, bytes, base](QTreeWidgetItem* row, QTreeWidgetItem*) {
                if (!row)
                    return;
                const auto issue = row->data(0, Qt::UserRole).toJsonObject();
                const auto start = issue.value(QStringLiteral("source_start")).toInteger(-1) - base;
                const auto end = issue.value(QStringLiteral("source_end")).toInteger(-1) - base;
                if (start < 0 || end <= start || end > bytes.size())
                    return;
                const auto normalized = [](QByteArrayView value) {
                    return QString::fromUtf8(value)
                        .replace(QStringLiteral("\r\n"), QStringLiteral("\n"))
                        .replace(QChar('\r'), QChar('\n'));
                };
                QTextCursor cursor(source->document());
                cursor.setPosition(normalized(QByteArrayView(bytes).first(start)).size());
                cursor.setPosition(normalized(QByteArrayView(bytes).first(end)).size(),
                                   QTextCursor::KeepAnchor);
                source->setTextCursor(cursor);
                source->ensureCursorVisible();
            });
    if (table->topLevelItemCount() > 0)
        table->setCurrentItem(table->topLevelItem(0));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
}

void MainWindow::showLoadError(QString message) {
    llmWorkbench_->setProject({});
    openedDatabasePath_.clear();
    workspace_.reset();
    repairQueue_->setItems({});
    projectExplorer_->clear();
    clearBookView();
    workspaceStatus_->setText(tr("Open failed: %1").arg(std::move(message)));
    workspaceStatus_->setStyleSheet(QStringLiteral("font-weight:600;color:#a12622;"));
    workspaceStatus_->setToolTip({});
    showStatusMessage(tr("Open failed"));
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
