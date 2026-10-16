#include "novel_analysis_workbench.h"
#include "chapter_evidence.h"

#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextCursor>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace loreforge::app {
namespace {
QString verdictText(const QString& value) {
    if (value == QStringLiteral("pass"))
        return NovelAnalysisWorkbench::tr("通过");
    if (value == QStringLiteral("redo"))
        return NovelAnalysisWorkbench::tr("建议重做");
    if (value == QStringLiteral("human"))
        return NovelAnalysisWorkbench::tr("需人工判断");
    return NovelAnalysisWorkbench::tr("未知");
}
QJsonObject evidenceOutput(const AnalysisHistoryEntry& entry, QStringList* errors = nullptr) {
    if (!entry.artifacts.parsedResponse)
        return {};
    const auto output = entry.artifacts.parsedResponse->object();
    if (entry.artifacts.outputSchemaVersion == 1)
        return output; // Historical byte-range contracts remain read-only and renderable.
    if (entry.artifacts.outputSchemaVersion != 2 &&
        entry.artifacts.outputSchemaVersion != chapterWireSchemaVersion)
        return {};
    const auto resolved = resolveChapterEvidence(
        output, entry.context.content,
        entry.context.content.value(QStringLiteral("role_index")).toInt() != 0);
    if (errors)
        *errors = resolved.errors;
    return resolved.output;
}
void attachEvidence(QTreeWidgetItem* item, const QByteArray& bytes, qint64 base, qint64 start,
                    qint64 end) {
    item->setData(0, Qt::UserRole, QString::fromUtf8(bytes.mid(start - base, end - start)));
    const auto position = [&](qint64 offset) {
        auto prefix = QString::fromUtf8(bytes.first(offset - base));
        prefix.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        prefix.replace(QLatin1Char('\r'), QLatin1Char('\n'));
        return prefix.size();
    };
    item->setData(0, Qt::UserRole + 1, position(start));
    item->setData(0, Qt::UserRole + 2, position(end));
}
} // namespace

NovelAnalysisWorkbench::NovelAnalysisWorkbench(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("novelAnalysisWorkbench"));
    auto* layout = new QVBoxLayout(this);
    auto* note =
        new QLabel(tr("人物 · 剧情 · 地点/设定事实 · 伏笔/线索\n"
                      "分析 → 复核 → LLM 审查，共用一个模型，串行运行；不会自动改写原文。"),
                   this);
    note->setWordWrap(true);
    layout->addWidget(note);
    chapters_ = new QComboBox(this);
    chapters_->setObjectName(QStringLiteral("analysisChapterSelector"));
    layout->addWidget(chapters_);
    auto* budgetGroup = new QGroupBox(tr("分析参数（随工程保存，按需修改）"), this);
    budgetGroup->setObjectName(QStringLiteral("analysisBudgetOptions"));
    budgetGroup->setCheckable(true);
    budgetGroup->setChecked(false);
    auto* budgetBody = new QWidget(budgetGroup);
    auto* budgetLayout = new QVBoxLayout(budgetGroup);
    budgetLayout->addWidget(budgetBody);
    auto* budgets = new QFormLayout(budgetBody);
    contextLimit_ = new QSpinBox(budgetBody);
    contextLimit_->setObjectName(QStringLiteral("llmChapterContextLimit"));
    contextLimit_->setRange(1024, 1048576);
    contextLimit_->setValue(32768);
    outputBudget_ = new QSpinBox(budgetBody);
    outputBudget_->setObjectName(QStringLiteral("llmChapterOutputBudget"));
    outputBudget_->setRange(256, 65536);
    outputBudget_->setValue(4096);
    budgets->addRow(tr("上下文总上限 token（按服务实际配置填写）"), contextLimit_);
    budgets->addRow(tr("每角色预留输出 token"), outputBudget_);
    budgetBody->hide();
    connect(budgetGroup, &QGroupBox::toggled, budgetBody, &QWidget::setVisible);
    layout->addWidget(budgetGroup);
    auto* buttons = new QHBoxLayout;
    prepare_ = new QPushButton(tr("仅预览原文与请求（可选）"), this);
    prepare_->setObjectName(QStringLiteral("llmPrepareChapterPreview"));
    analyze_ = new QPushButton(tr("从头重新分析"), this);
    analyze_->setObjectName(QStringLiteral("llmAnalyzeChapter"));
    resume_ = new QPushButton(tr("继续复核与审查"), this);
    resume_->setObjectName(QStringLiteral("analysisContinueTask"));
    cancel_ = new QPushButton(tr("取消任务"), this);
    cancel_->setObjectName(QStringLiteral("analysisCancelTask"));
    buttons->addWidget(analyze_);
    buttons->addWidget(resume_);
    buttons->addWidget(prepare_);
    buttons->addWidget(cancel_);
    layout->addLayout(buttons);
    previewStatus_ = new QLabel(tr("请先导入并选择 Markdown 章节。准备预览不会发送正文。"), this);
    previewStatus_->setObjectName(QStringLiteral("llmChapterPreviewStatus"));
    previewStatus_->setWordWrap(true);
    previewStatus_->setTextFormat(Qt::PlainText);
    layout->addWidget(previewStatus_);
    auto* previewGroup = new QGroupBox(tr("原文与请求预览（可选）"), this);
    previewGroup->setObjectName(QStringLiteral("analysisPreviewOptions"));
    previewGroup->setCheckable(true);
    previewGroup->setChecked(false);
    auto* previewLayout = new QVBoxLayout(previewGroup);
    auto* previews = new QTabWidget(previewGroup);
    previewLayout->addWidget(previews);
    previews->hide();
    connect(previewGroup, &QGroupBox::toggled, previews, &QWidget::setVisible);
    sourcePreview_ = new QPlainTextEdit(previews);
    sourcePreview_->setObjectName(QStringLiteral("llmChapterSourcePreview"));
    sourcePreview_->setReadOnly(true);
    sourcePreview_->setMinimumHeight(180);
    messages_ = new QPlainTextEdit(previews);
    messages_->setObjectName(QStringLiteral("llmChapterMessagesPreview"));
    messages_->setReadOnly(true);
    previews->addTab(sourcePreview_, tr("原文预览"));
    previews->addTab(messages_, tr("完整消息 JSON"));
    layout->addWidget(previewGroup);
    taskStatus_ = new QLabel(tr("尚无任务，不自动发送或恢复任务。"), this);
    taskStatus_->setObjectName(QStringLiteral("analysisTaskStatus"));
    taskStatus_->setWordWrap(true);
    taskStatus_->setTextFormat(Qt::PlainText);
    layout->addWidget(taskStatus_);
    roles_ = new QTableWidget(3, 4, this);
    roles_->setObjectName(QStringLiteral("llmRoleStatus"));
    roles_->setHorizontalHeaderLabels({tr("角色"), tr("状态"), tr("Token"), tr("耗时 ms")});
    roles_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    roles_->verticalHeader()->hide();
    roles_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    roles_->setMinimumHeight(125);
    const QStringList names{tr("分析员"), tr("复核员（同模型交叉检查）"), tr("审查员（LLM 角色）")};
    for (int row = 0; row < 3; ++row) {
        roles_->setItem(row, 0, new QTableWidgetItem(names[row]));
        for (int col = 1; col < 4; ++col) {
            roles_->setItem(row, col, new QTableWidgetItem(QStringLiteral("—")));
        }
    }
    layout->addWidget(roles_);
    totals_ = new QLabel(this);
    totals_->setObjectName(QStringLiteral("analysisTokenTotals"));
    totals_->setWordWrap(true);
    layout->addWidget(totals_);
    auto* results = new QTabWidget(this);
    results->setObjectName(QStringLiteral("analysisResultTabs"));
    for (const auto& title : QStringList{tr("人物"), tr("剧情"), tr("地点 / 设定事实"),
                                         tr("伏笔 / 线索"), tr("复核 / 审查")}) {
        auto* tree = new QTreeWidget(results);
        tree->setHeaderLabels({tr("结论 / 原文证据"), tr("依据 / 置信度")});
        tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        tree->setMinimumHeight(200);
        results_.append(tree);
        results->addTab(tree, title);
        connect(tree, &QTreeWidget::currentItemChanged, this,
                [this, previewGroup](QTreeWidgetItem* item) {
                    if (!item || !item->data(0, Qt::UserRole).isValid()) {
                        return;
                    }
                    const auto quote = item->data(0, Qt::UserRole).toString();
                    previewGroup->setChecked(true);
                    evidence_->setPlainText(quote);
                    auto cursor = sourcePreview_->textCursor();
                    cursor.setPosition(item->data(0, Qt::UserRole + 1).toInt());
                    cursor.setPosition(item->data(0, Qt::UserRole + 2).toInt(),
                                       QTextCursor::KeepAnchor);
                    sourcePreview_->setTextCursor(cursor);
                    sourcePreview_->ensureCursorVisible();
                });
    }
    layout->addWidget(results);
    evidence_ = new QPlainTextEdit(this);
    evidence_->setObjectName(QStringLiteral("analysisEvidence"));
    evidence_->setReadOnly(true);
    evidence_->setMaximumHeight(120);
    evidence_->setPlaceholderText(tr("点击证据条目，查看从快照截取的原文并定位。"));
    layout->addWidget(evidence_);
    historyList_ = new QListWidget(this);
    historyList_->setObjectName(QStringLiteral("analysisHistory"));
    historyList_->setMaximumHeight(160);
    layout->addWidget(new QLabel(tr("当前章任务历史（含失败和中断，不自动续跑）"), this));
    layout->addWidget(historyList_);
    diagnostics_ = new QPlainTextEdit(this);
    diagnostics_->setObjectName(QStringLiteral("analysisDiagnostics"));
    diagnostics_->setReadOnly(true);
    diagnostics_->setMinimumHeight(180);
    layout->addWidget(diagnostics_);
    connect(chapters_, &QComboBox::currentIndexChanged, this,
            &NovelAnalysisWorkbench::chapterSelected);
    connect(prepare_, &QPushButton::clicked, this, [this, previewGroup] {
        previewGroup->setChecked(true);
        emit chapterPreviewRequested(contextLimit_->value(), outputBudget_->value());
    });
    connect(contextLimit_, &QSpinBox::valueChanged, this,
            &NovelAnalysisWorkbench::invalidatePreview);
    connect(outputBudget_, &QSpinBox::valueChanged, this,
            &NovelAnalysisWorkbench::invalidatePreview);
    const auto budgetsChanged = [this] {
        emit this->budgetsChanged(contextLimit_->value(), outputBudget_->value());
    };
    connect(contextLimit_, &QSpinBox::valueChanged, this, budgetsChanged);
    connect(outputBudget_, &QSpinBox::valueChanged, this, budgetsChanged);
    connect(cancel_, &QPushButton::clicked, this, &NovelAnalysisWorkbench::cancellationRequested);
    connect(historyList_, &QListWidget::currentRowChanged, this,
            &NovelAnalysisWorkbench::showHistoryEntry);
    connect(analyze_, &QPushButton::clicked, this, [this] { confirmAndStart(false); });
    connect(resume_, &QPushButton::clicked, this, [this] { confirmAndStart(true); });
    updateButtons();
}

void NovelAnalysisWorkbench::confirmAndStart(bool continuing) {
    if (busy_ || !available_) {
        return;
    }
    const auto pipeline = resumePipeline_;
    const auto prefix = resumePrefix_;
    if (continuing && (pipeline.isEmpty() || prefix < 1 || prefix > 2))
        return;
    if (connectionValidator_) {
        const auto error = connectionValidator_();
        if (!error.isEmpty()) {
            showError(error);
            return;
        }
    }
    // Preparation is local and synchronous; every start rechecks current source.
    emit chapterPreviewRequested(contextLimit_->value(), outputBudget_->value());
    if (!preview_)
        return;
    const auto id = preview_->snapshot.id;
    QMessageBox dialog(
        QMessageBox::Question, tr("确认发送本章"),
        tr("章节：%1\n%2\n\n").arg(chapters_->currentText(), connectionSummary_) +
            (continuing
                 ? tr("复用：%1\n即将运行：%2（最多 %3 "
                      "次新请求）。\n继续前会重新核验原文、配置及前序结果。\n")
                       .arg(prefix == 1 ? tr("分析员结果") : tr("分析员与复核员结果"),
                            prefix == 1 ? tr("复核员 → 审查员") : tr("审查员"))
                       .arg(3 - prefix)
                 : tr("从分析员重新开始，最多串行运行分析、复核、审查三次，不复用历史结果。\n")) +
            (continuing && legacyEndpoint_
                 ? tr("注意：旧任务未记录接口地址，无法核对旧地址。本次明确使用上方当前接口。\n")
                 : QString{}) +
            tr("将当前章原文发送到以上 API。\n"
               "后续角色会收到本章原文和前一步输出；不包含其他章节，不自动重试、不自动改写。\n"
               "旧请求可能仍在远端运行；本次会产生新的用量。\n"
               "请求和结果将保存到项目，不覆盖历史。确认继续？"),
        QMessageBox::NoButton, this);
    dialog.setObjectName(QStringLiteral("analysisSendConfirmation"));
    dialog.setTextFormat(Qt::PlainText);
    auto* confirm = dialog.addButton(tr("确认"), QMessageBox::AcceptRole);
    confirm->setObjectName(QStringLiteral("analysisConfirmSend"));
    auto* cancel = dialog.addButton(tr("取消"), QMessageBox::RejectRole);
    dialog.setDefaultButton(cancel);
    dialog.setEscapeButton(cancel);
    dialog.exec();
    if (dialog.clickedButton() == confirm && preview_ && preview_->snapshot.id == id && !busy_) {
        if (continuing) {
            if (resumePipeline_ == pipeline && resumePrefix_ == prefix)
                emit continuationRequested(pipeline, legacyEndpoint_);
        } else
            emit analysisRequested();
    }
}

void NovelAnalysisWorkbench::updateButtons() {
    prepare_->setEnabled(available_ && !busy_);
    analyze_->setEnabled(available_ && !busy_);
    resume_->setVisible(resumePrefix_ == 1 || resumePrefix_ == 2);
    resume_->setEnabled(available_ && !busy_ && (resumePrefix_ == 1 || resumePrefix_ == 2));
    resume_->setText(resumePrefix_ == 2 ? tr("继续审查") : tr("继续复核与审查"));
    cancel_->setEnabled(busy_);
    chapters_->setEnabled(!busy_);
    contextLimit_->setEnabled(!busy_);
    outputBudget_->setEnabled(!busy_);
}
void NovelAnalysisWorkbench::setSelectedBook(const document::Document* book, qsizetype index) {
    resumePipeline_.clear();
    resumePrefix_ = 0;
    const QSignalBlocker blocker(chapters_);
    chapters_->clear();
    chapterId_.clear();
    bookId_ = book ? book->id.toString() : QString{};
    QStringList importedText;
    available_ = false;
    if (book) {
        for (const auto& chapter : book->chapters) {
            chapters_->addItem(chapter.title);
        }
        chapters_->setCurrentIndex(static_cast<int>(index));
        if (index >= 0 && index < book->chapters.size()) {
            chapterId_ = book->chapters[index].id.toString();
            available_ = book->metadata.sourceFormat == QStringLiteral("markdown-source");
            for (const auto& block : book->chapters[index].blocks)
                importedText.append(block.text);
        }
    }
    invalidatePreview();
    historyList_->clear();
    history_.clear();
    diagnostics_->clear();
    evidence_->clear();
    for (auto* tree : results_) {
        tree->clear();
    }
    for (int role = 0; role < 3; ++role) {
        updateRole(role, tr("尚无任务"), QStringLiteral("—"), 0);
    }
    taskStatus_->setText(tr("当前章已更新；不会自动发送。"));
    emit highlightImportedTextChanged(bookId_, importedText.join(QStringLiteral("\n\n")));
}
void NovelAnalysisWorkbench::invalidatePreview() {
    preview_.reset();
    sourcePreview_->clear();
    messages_->clear();
    previewStatus_->setText(tr("选章后可直接开始分析，程序会自动准备并检查原文；也可先预览。"));
    updateButtons();
}

void NovelAnalysisWorkbench::restoreBudgets(int maximum, int reserved) {
    const QSignalBlocker first(contextLimit_);
    const QSignalBlocker second(outputBudget_);
    contextLimit_->setValue(maximum);
    outputBudget_->setValue(reserved);
    invalidatePreview();
}

void NovelAnalysisWorkbench::setConnectionValidator(std::function<QString()> validator) {
    connectionValidator_ = std::move(validator);
}

void NovelAnalysisWorkbench::setConnectionSummary(QString summary) {
    connectionSummary_ = std::move(summary);
}
void NovelAnalysisWorkbench::showChapterPreview(const ChapterPreview& preview) {
    preview_ = preview;
    displayedSource_ = preview.snapshot.content.value(QStringLiteral("source")).toObject();
    sourcePreview_->setPlainText(displayedSource_.value(QStringLiteral("utf8")).toString());
    emit highlightSourceChanged(bookId_, displayedSource_, {},
                                tr("原文快照预览；尚未应用人物分析结果。"));
    messages_->setPlainText(preview.messagesJson);
    previewStatus_->setText(
        tr("已保存到 .loreforge 项目；尚未发送。\n快照 ID：%1\nSHA-256：%2\n"
           "提示词 v%3 · 输出契约 v%4 · 估算输入 %5 token（非精确计数）。后续角色另行检查预算。")
            .arg(preview.snapshot.id.toString(), preview.snapshot.contentHash.toHex())
            .arg(preview.prompt.version)
            .arg(preview.schema.version)
            .arg(preview.estimatedTokens));
    updateButtons();
}
void NovelAnalysisWorkbench::showError(QString message) {
    taskStatus_->setText(message);
    if (!preview_ && message.startsWith(tr("准备失败"))) {
        previewStatus_->setText(message);
    }
}
void NovelAnalysisWorkbench::setBusy(bool busy) {
    busy_ = busy;
    if (busy) {
        emit highlightSourceChanged(bookId_, displayedSource_, {},
                                    tr("正在运行新任务；尚无本轮正文分割，不显示旧结果染色。"));
        taskStatus_->setText(tr("单章任务运行中；可取消，不自动重试。"));
        for (auto* tree : results_) {
            tree->clear();
        }
        evidence_->clear();
        diagnostics_->clear();
        for (int role = 0; role < 3; ++role) {
            updateRole(role, tr("等待"), QStringLiteral("—"), 0);
        }
    }
    updateButtons();
}
void NovelAnalysisWorkbench::updateRole(int role, QString status, QString usage, qint64 latency) {
    if (role < 0 || role >= 3) {
        return;
    }
    roles_->item(role, 1)->setText(std::move(status));
    roles_->item(role, 2)->setText(std::move(usage));
    roles_->item(role, 3)->setText(latency > 0 ? QString::number(latency) : QStringLiteral("—"));
}
const std::optional<ChapterPreview>& NovelAnalysisWorkbench::preview() const noexcept {
    return preview_;
}

void NovelAnalysisWorkbench::showHistory(QList<AnalysisHistoryEntry> history, QString chapterId) {
    resumePipeline_.clear();
    resumePrefix_ = 0;
    updateButtons();
    history_.clear();
    historyList_->clear();
    qint64 projectTokens = 0, chapterTokens = 0;
    int projectUnknown = 0, chapterUnknown = 0;
    for (const auto& entry : history) {
        const auto current =
            entry.context.content.value(QStringLiteral("chapter_id")).toString() == chapterId;
        if (entry.outcome.value(QStringLiteral("usage_known")).toBool()) {
            projectTokens += entry.run.totalTokens;
            if (current) {
                chapterTokens += entry.run.totalTokens;
            }
        } else {
            ++projectUnknown;
            if (current) {
                ++chapterUnknown;
            }
        }
        if (!current) {
            continue;
        }
        history_.append(entry);
        const auto role = entry.context.content.value(QStringLiteral("role_index")).toInt();
        const auto status = entry.run.status == storage::LLMRunStatus::Succeeded
                                ? tr("输出校验通过")
                                : (entry.run.status == storage::LLMRunStatus::Failed
                                       ? tr("失败")
                                       : (entry.run.status == storage::LLMRunStatus::Cancelled
                                              ? tr("取消")
                                              : tr("未完成/可能中断")));
        historyList_->addItem(
            tr("%1 · %2 · %3 · %4")
                .arg(entry.run.startedAt.toLocalTime().toString(Qt::ISODate))
                .arg(role == 0 ? tr("分析") : (role == 1 ? tr("复核") : tr("审查")), status,
                     entry.run.model));
    }
    const auto summary =
        tr("小说任务（持久化，含失败/重复运行，不含连接测试）：\n"
           "项目已知 %1 token · %2 次用量未知；本章已知 %3 token · %4 次用量未知。\n"
           "精确生成速度：未提供；SSE 接收状态见底部，各角色显示真实响应耗时。")
            .arg(projectTokens)
            .arg(projectUnknown)
            .arg(chapterTokens)
            .arg(chapterUnknown);
    totals_->setText(summary);
    emit metricsChanged(summary);
    if (!history_.isEmpty()) {
        historyList_->setCurrentRow(static_cast<int>(history_.size() - 1));
    }
}

void NovelAnalysisWorkbench::showHistoryEntry(int index) {
    if (index < 0 || index >= history_.size()) {
        return;
    }
    const auto& entry = history_[index];
    displayedSource_ = entry.context.content.value(QStringLiteral("source")).toObject();
    // History is always rendered against its immutable source, not the current file.
    sourcePreview_->setPlainText(displayedSource_.value(QStringLiteral("utf8")).toString());
    messages_->setPlainText(QString::fromUtf8(
        QJsonDocument(entry.context.content.value(QStringLiteral("messages")).toArray()).toJson()));
    previewStatus_->setText(
        tr("正在查看历史快照：%1；不是实时原文。点击开始会自动重读并核验当前原文。")
            .arg(entry.context.id.toString()));
    preview_.reset();
    updateButtons();
    diagnostics_->setPlainText(
        entry.run.id.toString() + QLatin1Char('\n') + entry.run.errorMessage + QLatin1Char('\n') +
        QString::fromUtf8(QJsonDocument(entry.outcome).toJson()) +
        (entry.artifacts.parsedResponse
             ? QLatin1Char('\n') + QString::fromUtf8(entry.artifacts.parsedResponse->toJson())
             : QString{}));
    const auto pipeline = entry.context.content.value(QStringLiteral("pipeline_id")).toString();
    const auto roles = latestPipelineRoles(history_, pipeline);
    const auto* analyzer = roles[0];
    const auto* checker = roles[1];
    const auto* reviewer = roles[2];
    resumePipeline_ = pipeline;
    resumePrefix_ = completedRolePrefix(roles);
    for (int role = 0; role < resumePrefix_; ++role) {
        if (roles[role]->artifacts.outputSchemaVersion != chapterWireSchemaVersion ||
            roles[role]->artifacts.promptVersion != chapterPromptVersion) {
            resumePrefix_ = 0;
            previewStatus_->setText(
                tr("旧版证据契约：可查看历史；请从头重新分析，以使用段落编号证据。"));
            break;
        }
    }
    legacyEndpoint_ = false;
    for (int role = 0; role < resumePrefix_; ++role) {
        legacyEndpoint_ = legacyEndpoint_ ||
                          !roles[role]->context.content.contains(QStringLiteral("api_endpoint"));
    }
    updateButtons();
    for (auto* tree : results_) {
        tree->clear();
    }
    if (analyzer && analyzer->run.status == storage::LLMRunStatus::Succeeded &&
        analyzer->artifacts.parsedResponse) {
        renderAnalysis(evidenceOutput(*analyzer), displayedSource_);
    }
    const auto good = [](const AnalysisHistoryEntry* value) {
        return value && value->run.status == storage::LLMRunStatus::Succeeded &&
               value->artifacts.parsedResponse && value->artifacts.validation.isValid();
    };
    if (good(analyzer) && good(checker) && good(reviewer)) {
        taskStatus_->setText(reviewConclusion(checker->artifacts.parsedResponse->object(),
                                              reviewer->artifacts.parsedResponse->object()));
    } else {
        taskStatus_->setText(tr("历史任务未完成或失败；已校验分析仅为候选，尚未最终通过。"));
    }
    if (good(analyzer)) {
        QStringList segmentationErrors;
        const auto output = evidenceOutput(*analyzer, &segmentationErrors);
        const auto finalPass = good(checker) && good(reviewer) &&
                               checker->artifacts.parsedResponse->object()
                                       .value(QStringLiteral("verdict"))
                                       .toString() == QStringLiteral("pass") &&
                               reviewer->artifacts.parsedResponse->object()
                                       .value(QStringLiteral("verdict"))
                                       .toString() == QStringLiteral("pass");
        emit highlightSourceChanged(
            analyzer->context.content.value(QStringLiteral("book_id")).toString(bookId_),
            analyzer->context.content.value(QStringLiteral("source")).toObject(), output,
            !segmentationErrors.isEmpty()
                ? tr("所选历史不满足正文分割校验：%1\n请从头重新分析；不会将其作为有效分割显示。")
                      .arg(segmentationErrors.join(QLatin1Char('\n')))
            : finalPass
                ? tr("三角色流程通过（同模型意见）；显示正文分割与说话人归属，请核对模型判断。")
                : tr("候选预览：后续角色未完成、存在分歧或建议重做；不是最终确认结果。"));
    } else {
        emit highlightSourceChanged(bookId_, displayedSource_, {},
                                    tr("所选流程没有有效的分析员结果，未染色。"));
    }
    const auto bytes = displayedSource_.value(QStringLiteral("utf8")).toString().toUtf8();
    const auto base = displayedSource_.value(QStringLiteral("start_byte")).toInteger();
    for (const auto* review : {checker, reviewer}) {
        if (!good(review) || !review->artifacts.parsedResponse) {
            continue;
        }
        const auto role = review->context.content.value(QStringLiteral("role_index")).toInt();
        const auto output = evidenceOutput(*review);
        auto* parent = new QTreeWidgetItem(
            results_.last(),
            {tr("%1：%2").arg(role == 1 ? tr("复核员") : tr("LLM 审查员"),
                              verdictText(output.value(QStringLiteral("verdict")).toString())),
             tr("同模型角色意见")});
        for (const auto& value : output.value(QStringLiteral("issues")).toArray()) {
            const auto issue = value.toObject();
            const auto start = issue.value(QStringLiteral("source_start")).toInteger(-1);
            const auto end = issue.value(QStringLiteral("source_end")).toInteger(-1);
            if (start < base || end <= start || end - base > bytes.size()) {
                continue;
            }
            auto* item =
                new QTreeWidgetItem(parent, {issue.value(QStringLiteral("reason")).toString(),
                                             tr("字节 [%1, %2)").arg(start).arg(end)});
            attachEvidence(item, bytes, base, start, end);
        }
        parent->setExpanded(true);
    }
    for (int role = 0; role < 3; ++role)
        updateRole(role, tr("尚未运行"), QStringLiteral("—"), 0);
    for (const auto* roleEntry : {analyzer, checker, reviewer}) {
        if (!roleEntry) {
            continue;
        }
        updateRole(
            roleEntry->context.content.value(QStringLiteral("role_index")).toInt(),
            good(roleEntry)
                ? (roleEntry->context.content.value(QStringLiteral("role_index")).toInt() == 0
                       ? tr("输出校验通过")
                       : tr("输出校验通过；内容意见：%1")
                             .arg(verdictText(roleEntry->artifacts.parsedResponse->object()
                                                  .value(QStringLiteral("verdict"))
                                                  .toString())))
                : tr("失败/取消/未完成"),
            roleEntry->outcome.value(QStringLiteral("usage_known")).toBool()
                ? QString::number(roleEntry->run.totalTokens)
                : tr("用量未知"),
            roleEntry->run.latencyMs.value_or(0));
    }
}

void NovelAnalysisWorkbench::renderAnalysis(const QJsonObject& output, const QJsonObject& source) {
    const auto bytes = source.value(QStringLiteral("utf8")).toString().toUtf8();
    const auto base = source.value(QStringLiteral("start_byte")).toInteger();
    const auto add = [&](QTreeWidget* tree, const QJsonObject& claim, QString label) {
        const auto basis =
            claim.value(QStringLiteral("inferred")).toBool() ? tr("推断") : tr("原文证据");
        auto* item = new QTreeWidgetItem(
            tree, {label, basis + QStringLiteral(" / ") +
                              QString::number(claim.value(QStringLiteral("confidence")).toDouble(),
                                              'f', 2)});
        for (const auto& value : claim.value(QStringLiteral("evidence")).toArray()) {
            const auto span = value.toObject();
            const auto start = span.value(QStringLiteral("source_start")).toInteger(-1);
            const auto end = span.value(QStringLiteral("source_end")).toInteger(-1);
            if (start < base || end <= start || end - base > bytes.size()) {
                continue;
            }
            const auto quote = QString::fromUtf8(bytes.mid(start - base, end - start));
            auto* evidence =
                new QTreeWidgetItem(item, {tr("字节 [%1, %2)：%3").arg(start).arg(end).arg(quote),
                                           QStringLiteral("UTF-8")});
            attachEvidence(evidence, bytes, base, start, end);
        }
        item->setExpanded(true);
    };
    for (const auto& value : output.value(QStringLiteral("characters")).toArray()) {
        const auto claim = value.toObject();
        QStringList aliases;
        for (const auto& alias : claim.value(QStringLiteral("aliases")).toArray()) {
            aliases.append(alias.toObject().value(QStringLiteral("name")).toString());
        }
        add(results_[0], claim,
            claim.value(QStringLiteral("name")).toString() +
                (aliases.isEmpty() ? QString{}
                                   : tr("（别名：%1）").arg(aliases.join(QStringLiteral("、")))));
    }
    add(results_[1], output.value(QStringLiteral("summary")).toObject(),
        tr("摘要：%1")
            .arg(output.value(QStringLiteral("summary"))
                     .toObject()
                     .value(QStringLiteral("text"))
                     .toString()));
    for (const auto& value : output.value(QStringLiteral("events")).toArray()) {
        add(results_[1], value.toObject(),
            value.toObject().value(QStringLiteral("description")).toString());
    }
    for (const auto& value : output.value(QStringLiteral("locations")).toArray()) {
        add(results_[2], value.toObject(),
            value.toObject().value(QStringLiteral("name")).toString());
    }
    for (const auto& value : output.value(QStringLiteral("important_facts")).toArray()) {
        add(results_[2], value.toObject(),
            value.toObject().value(QStringLiteral("text")).toString());
    }
    for (const auto& value : output.value(QStringLiteral("open_threads")).toArray()) {
        add(results_[3], value.toObject(),
            value.toObject().value(QStringLiteral("text")).toString());
    }
}
} // namespace loreforge::app
