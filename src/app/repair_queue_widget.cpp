#include "repair_queue_widget.h"

#include <QDateTime>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include <variant>

namespace loreforge::app {

RepairQueueWidget::RepairQueueWidget(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    table_ = new QTableWidget(this);
    table_->setObjectName(QStringLiteral("repairQueueTable"));
    table_->setColumnCount(4);
    table_->setHorizontalHeaderLabels(
        {tr("Category"), tr("Confidence"), tr("Suggestion"), tr("Status")});
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(table_, 1);

    auto* form = new QFormLayout;
    evidence_ = new QLabel(this);
    evidence_->setObjectName(QStringLiteral("repairEvidence"));
    evidence_->setWordWrap(true);
    evidence_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(tr("Evidence"), evidence_);
    sourceContext_ = new QPlainTextEdit(this);
    sourceContext_->setObjectName(QStringLiteral("repairSourceContext"));
    sourceContext_->setReadOnly(true);
    sourceContext_->setMaximumBlockCount(200);
    sourceContext_->setMinimumHeight(100);
    form->addRow(tr("Source context"), sourceContext_);
    suggestion_ = new QLineEdit(this);
    suggestion_->setObjectName(QStringLiteral("repairSuggestion"));
    form->addRow(tr("Suggestion"), suggestion_);
    layout->addLayout(form);

    auto* actions = new QHBoxLayout;
    approve_ = new QPushButton(tr("Approve"), this);
    approve_->setObjectName(QStringLiteral("approveRepairCandidate"));
    reject_ = new QPushButton(tr("Reject"), this);
    reject_->setObjectName(QStringLiteral("rejectRepairCandidate"));
    edit_ = new QPushButton(tr("Save suggestion"), this);
    edit_->setObjectName(QStringLiteral("editRepairSuggestion"));
    protect_ = new QPushButton(tr("Ignore & protect term"), this);
    protect_->setObjectName(QStringLiteral("protectRepairTerm"));
    actions->addWidget(approve_);
    actions->addWidget(reject_);
    actions->addWidget(edit_);
    actions->addWidget(protect_);
    layout->addLayout(actions);
    decisionError_ = new QLabel(this);
    decisionError_->setObjectName(QStringLiteral("repairDecisionError"));
    decisionError_->setStyleSheet(QStringLiteral("color:#a12622;"));
    decisionError_->setWordWrap(true);
    layout->addWidget(decisionError_);

    connect(table_, &QTableWidget::currentCellChanged, this,
            [this](int currentRow, int, int, int) { selectRow(currentRow); });
    connect(approve_, &QPushButton::clicked, this, &RepairQueueWidget::approveSelected);
    connect(reject_, &QPushButton::clicked, this, &RepairQueueWidget::rejectSelected);
    connect(edit_, &QPushButton::clicked, this, &RepairQueueWidget::editSelectedSuggestion);
    connect(protect_, &QPushButton::clicked, this, &RepairQueueWidget::protectSelectedTerm);
    refreshDetails();
}

void RepairQueueWidget::setItems(QList<proofreading::RepairQueueItem> items) {
    items_ = std::move(items);
    table_->setRowCount(items_.size());
    for (int row = 0; row < items_.size(); ++row) {
        refreshRow(row);
    }
    if (items_.isEmpty()) {
        selectedRow_ = -1;
        refreshDetails();
    } else {
        table_->setCurrentCell(0, 0);
    }
}

const QList<proofreading::RepairQueueItem>& RepairQueueWidget::items() const noexcept {
    return items_;
}

void RepairQueueWidget::selectRow(int row) {
    selectedRow_ = row >= 0 && row < items_.size() ? row : -1;
    refreshDetails();
}

void RepairQueueWidget::approveSelected() {
    if (selectedRow_ < 0) {
        return;
    }
    const auto id = items_.at(selectedRow_).candidate.id.toString();
    const auto result = proofreading::RepairQueueWorkflow::approve(items_.at(selectedRow_),
                                                                   QDateTime::currentDateTimeUtc());
    applyResult(result);
    if (std::holds_alternative<proofreading::RepairQueueItem>(result)) {
        emit candidateApproved(id);
    }
}

void RepairQueueWidget::rejectSelected() {
    if (selectedRow_ < 0) {
        return;
    }
    const auto id = items_.at(selectedRow_).candidate.id.toString();
    const auto result = proofreading::RepairQueueWorkflow::reject(items_.at(selectedRow_),
                                                                  QDateTime::currentDateTimeUtc());
    applyResult(result);
    if (std::holds_alternative<proofreading::RepairQueueItem>(result)) {
        emit candidateRejected(id);
    }
}

void RepairQueueWidget::editSelectedSuggestion() {
    if (selectedRow_ < 0) {
        return;
    }
    const auto id = items_.at(selectedRow_).candidate.id.toString();
    const auto text = suggestion_->text();
    const auto result = proofreading::RepairQueueWorkflow::editSuggestion(
        items_.at(selectedRow_), text, QDateTime::currentDateTimeUtc());
    applyResult(result);
    if (std::holds_alternative<proofreading::RepairQueueItem>(result)) {
        emit suggestionEdited(id,
                              std::get<proofreading::RepairQueueItem>(result).currentSuggestion);
    }
}

void RepairQueueWidget::protectSelectedTerm() {
    if (selectedRow_ < 0) {
        return;
    }
    const auto id = items_.at(selectedRow_).candidate.id.toString();
    const auto term = items_.at(selectedRow_).candidate.originalText.trimmed();
    const auto result = proofreading::RepairQueueWorkflow::reject(items_.at(selectedRow_),
                                                                  QDateTime::currentDateTimeUtc());
    applyResult(result);
    if (std::holds_alternative<proofreading::RepairQueueItem>(result)) {
        emit termProtectionRequested(id, term);
    }
}

void RepairQueueWidget::refreshRow(int row) {
    const auto& item = items_.at(row);
    table_->setItem(
        row, 0, new QTableWidgetItem(proofreading::candidateCategoryName(item.candidate.category)));
    table_->setItem(
        row, 1,
        new QTableWidgetItem(QString::number(item.candidate.confidence * 100.0, 'f', 0) +
                             QLatin1Char('%')));
    table_->setItem(row, 2, new QTableWidgetItem(item.currentSuggestion));
    table_->setItem(row, 3, new QTableWidgetItem(proofreading::candidateStatusName(item.status)));
}

void RepairQueueWidget::refreshDetails() {
    const bool selected = selectedRow_ >= 0 && selectedRow_ < items_.size();
    if (!selected) {
        evidence_->clear();
        sourceContext_->clear();
        suggestion_->clear();
        decisionError_->clear();
        approve_->setEnabled(false);
        reject_->setEnabled(false);
        edit_->setEnabled(false);
        protect_->setEnabled(false);
        return;
    }
    const auto& item = items_.at(selectedRow_);
    evidence_->setText(item.candidate.evidence);
    sourceContext_->setPlainText(item.sourceContext);
    suggestion_->setText(item.currentSuggestion);
    decisionError_->clear();
    const bool pending = item.status == proofreading::CandidateStatus::Detected ||
                         item.status == proofreading::CandidateStatus::ReviewRequired;
    const bool editable = item.status != proofreading::CandidateStatus::Patched &&
                          item.status != proofreading::CandidateStatus::Submitted &&
                          item.status != proofreading::CandidateStatus::Merged &&
                          item.status != proofreading::CandidateStatus::Stale;
    approve_->setEnabled(pending);
    reject_->setEnabled(pending);
    protect_->setEnabled(pending);
    edit_->setEnabled(editable);
    suggestion_->setEnabled(editable);
}

void RepairQueueWidget::applyResult(const proofreading::RepairQueueResult& result) {
    if (std::holds_alternative<proofreading::RepairQueueError>(result)) {
        decisionError_->setText(std::get<proofreading::RepairQueueError>(result).message);
        return;
    }
    items_[selectedRow_] = std::get<proofreading::RepairQueueItem>(result);
    refreshRow(selectedRow_);
    refreshDetails();
}

} // namespace loreforge::app
