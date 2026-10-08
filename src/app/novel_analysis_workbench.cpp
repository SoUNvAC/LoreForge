#include "novel_analysis_workbench.h"

#include <QComboBox>
#include <QFormLayout>
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
    auto* budgets = new QFormLayout;
    contextLimit_ = new QSpinBox(this);
    contextLimit_->setObjectName(QStringLiteral("llmChapterContextLimit"));
    contextLimit_->setRange(1024, 1048576);
    contextLimit_->setValue(32768);
    outputBudget_ = new QSpinBox(this);
    outputBudget_->setObjectName(QStringLiteral("llmChapterOutputBudget"));
    outputBudget_->setRange(256, 65536);
    outputBudget_->setValue(4096);
    budgets->addRow(tr("上下文总上限 token（按服务实际配置填写）"), contextLimit_);
    budgets->addRow(tr("每角色预留输出 token"), outputBudget_);
    layout->addLayout(budgets);
    auto* buttons = new QHBoxLayout;
    prepare_ = new QPushButton(tr("准备当前章快照与预览"), this);
    prepare_->setObjectName(QStringLiteral("llmPrepareChapterPreview"));
    analyze_ = new QPushButton(tr("开始单章分析"), this);
    analyze_->setObjectName(QStringLiteral("llmAnalyzeChapter"));
    cancel_ = new QPushButton(tr("取消任务"), this);
    cancel_->setObjectName(QStringLiteral("analysisCancelTask"));
    buttons->addWidget(prepare_);
    buttons->addWidget(analyze_);
    buttons->addWidget(cancel_);
    layout->addLayout(buttons);
    previewStatus_ = new QLabel(tr("请先导入并选择 Markdown 章节。准备预览不会发送正文。"), this);
    previewStatus_->setObjectName(QStringLiteral("llmChapterPreviewStatus"));
    previewStatus_->setWordWrap(true);
    previewStatus_->setTextFormat(Qt::PlainText);
    layout->addWidget(previewStatus_);
    auto* previews = new QTabWidget(this);
    sourcePreview_ = new QPlainTextEdit(previews);
    sourcePreview_->setObjectName(QStringLiteral("llmChapterSourcePreview"));
    sourcePreview_->setReadOnly(true);
    sourcePreview_->setMinimumHeight(180);
    messages_ = new QPlainTextEdit(previews);
    messages_->setObjectName(QStringLiteral("llmChapterMessagesPreview"));
    messages_->setReadOnly(true);
    previews->addTab(sourcePreview_, tr("原文预览"));
    previews->addTab(messages_, tr("完整消息 JSON"));
    layout->addWidget(previews);
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
        connect(tree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
            if (!item || !item->data(0, Qt::UserRole).isValid()) {
                return;
            }
            const auto quote = item->data(0, Qt::UserRole).toString();
            evidence_->setPlainText(quote);
            auto cursor = sourcePreview_->textCursor();
            cursor.setPosition(item->data(0, Qt::UserRole + 1).toInt());
            cursor.setPosition(item->data(0, Qt::UserRole + 2).toInt(), QTextCursor::KeepAnchor);
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
    connect(prepare_, &QPushButton::clicked, this, [this] {
        emit chapterPreviewRequested(contextLimit_->value(), outputBudget_->value());
    });
    connect(contextLimit_, &QSpinBox::valueChanged, this,
            &NovelAnalysisWorkbench::invalidatePreview);
    connect(outputBudget_, &QSpinBox::valueChanged, this,
            &NovelAnalysisWorkbench::invalidatePreview);
    connect(cancel_, &QPushButton::clicked, this, &NovelAnalysisWorkbench::cancellationRequested);
    connect(historyList_, &QListWidget::currentRowChanged, this,
            &NovelAnalysisWorkbench::showHistoryEntry);
    connect(analyze_, &QPushButton::clicked, this, [this] {
        if (!preview_ || busy_) {
            return;
        }
        const auto id = preview_->snapshot.id;
        QMessageBox dialog(
            QMessageBox::Question, tr("确认发送本章"),
            tr("将当前章原文发送到 LLM 页配置的 API，并最多串行运行分析、复核、审查三次。\n"
               "后续角色会收到本章原文和前一步输出；不包含其他章节，不自动重试、不自动改写。\n"
               "请求和结果将保存到项目，可能包含小说原文。确认继续？"),
            QMessageBox::NoButton, this);
        dialog.setObjectName(QStringLiteral("analysisSendConfirmation"));
        auto* confirm = dialog.addButton(tr("确认"), QMessageBox::AcceptRole);
        confirm->setObjectName(QStringLiteral("analysisConfirmSend"));
        auto* cancel = dialog.addButton(tr("取消"), QMessageBox::RejectRole);
        dialog.setDefaultButton(cancel);
        dialog.setEscapeButton(cancel);
        dialog.exec();
        if (dialog.clickedButton() == confirm && preview_ && preview_->snapshot.id == id &&
            !busy_) {
            emit analysisRequested();
        }
    });
    updateButtons();
}

void NovelAnalysisWorkbench::updateButtons() {
    prepare_->setEnabled(available_ && !busy_);
    analyze_->setEnabled(available_ && preview_.has_value() && !busy_);
    cancel_->setEnabled(busy_);
    chapters_->setEnabled(!busy_);
    contextLimit_->setEnabled(!busy_);
    outputBudget_->setEnabled(!busy_);
}
void NovelAnalysisWorkbench::setSelectedBook(const document::Document* book, qsizetype index) {
    const QSignalBlocker blocker(chapters_);
    chapters_->clear();
    chapterId_.clear();
    available_ = false;
    if (book) {
        for (const auto& chapter : book->chapters) {
            chapters_->addItem(chapter.title);
        }
        chapters_->setCurrentIndex(static_cast<int>(index));
        if (index >= 0 && index < book->chapters.size()) {
            chapterId_ = book->chapters[index].id.toString();
            available_ = book->metadata.sourceFormat == QStringLiteral("markdown-source");
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
}
void NovelAnalysisWorkbench::invalidatePreview() {
    preview_.reset();
    sourcePreview_->clear();
    messages_->clear();
    previewStatus_->setText(tr("请选择章节并准备快照；配置/预算/章节变更后须重新准备。"));
    updateButtons();
}
void NovelAnalysisWorkbench::showChapterPreview(const ChapterPreview& preview) {
    preview_ = preview;
    displayedSource_ = preview.snapshot.content.value(QStringLiteral("source")).toObject();
    sourcePreview_->setPlainText(displayedSource_.value(QStringLiteral("utf8")).toString());
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
                                ? tr("已校验")
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
           "生成速度：未提供（非流式）；各角色显示真实响应耗时。")
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
    previewStatus_->setText(tr("正在查看历史角色快照：%1；不是实时原文。开始新任务前请重新准备。")
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
    const AnalysisHistoryEntry* analyzer = nullptr;
    const AnalysisHistoryEntry* checker = nullptr;
    const AnalysisHistoryEntry* reviewer = nullptr;
    for (const auto& candidate : history_) {
        if (candidate.context.content.value(QStringLiteral("pipeline_id")).toString() != pipeline) {
            continue;
        }
        const auto role = candidate.context.content.value(QStringLiteral("role_index")).toInt();
        if (role == 0) {
            analyzer = &candidate;
        } else if (role == 1) {
            checker = &candidate;
        } else {
            reviewer = &candidate;
        }
    }
    for (auto* tree : results_) {
        tree->clear();
    }
    if (analyzer && analyzer->run.status == storage::LLMRunStatus::Succeeded &&
        analyzer->artifacts.parsedResponse) {
        renderAnalysis(analyzer->artifacts.parsedResponse->object(), displayedSource_);
    }
    const auto good = [](const AnalysisHistoryEntry* value) {
        return value && value->run.status == storage::LLMRunStatus::Succeeded;
    };
    if (good(analyzer) && good(checker) && good(reviewer)) {
        const auto checked = checker->outcome.value(QStringLiteral("verdict")).toString();
        const auto reviewed = reviewer->outcome.value(QStringLiteral("verdict")).toString();
        taskStatus_->setText(
            reviewed == QStringLiteral("redo")
                ? tr("需重做")
                : (checked == QStringLiteral("pass") && reviewed == QStringLiteral("pass")
                       ? tr("通过（同模型角色检查，非独立验证；没有修改原文）")
                       : tr("待人工确认")));
    } else {
        taskStatus_->setText(tr("历史任务未完成或失败；已校验分析仅为候选，尚未最终通过。"));
    }
    const auto bytes = displayedSource_.value(QStringLiteral("utf8")).toString().toUtf8();
    const auto base = displayedSource_.value(QStringLiteral("start_byte")).toInteger();
    for (const auto* review : {checker, reviewer}) {
        if (!good(review) || !review->artifacts.parsedResponse) {
            continue;
        }
        const auto role = review->context.content.value(QStringLiteral("role_index")).toInt();
        const auto output = review->artifacts.parsedResponse->object();
        auto* parent = new QTreeWidgetItem(
            results_.last(), {tr("%1：%2").arg(role == 1 ? tr("复核员") : tr("LLM 审查员"),
                                               output.value(QStringLiteral("verdict")).toString()),
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
    for (const auto* roleEntry : {analyzer, checker, reviewer}) {
        if (!roleEntry) {
            continue;
        }
        updateRole(
            roleEntry->context.content.value(QStringLiteral("role_index")).toInt(),
            good(roleEntry)
                ? (roleEntry->context.content.value(QStringLiteral("role_index")).toInt() == 0
                       ? tr("已校验")
                       : tr("已校验：%1")
                             .arg(roleEntry->outcome.value(QStringLiteral("verdict")).toString()))
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
