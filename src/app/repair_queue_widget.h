#pragma once

#include "loreforge/proofreading/repair_queue.h"

#include <QList>
#include <QWidget>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;

namespace loreforge::app {

class RepairQueueWidget final : public QWidget {
    Q_OBJECT

  public:
    explicit RepairQueueWidget(QWidget* parent = nullptr);

    void setItems(QList<proofreading::RepairQueueItem> items);
    [[nodiscard]] const QList<proofreading::RepairQueueItem>& items() const noexcept;

  signals:
    void candidateApproved(QString candidateId);
    void candidateRejected(QString candidateId);
    void suggestionEdited(QString candidateId, QString suggestion);
    void termProtectionRequested(QString candidateId, QString canonicalSpelling);

  private slots:
    void selectRow(int row);
    void approveSelected();
    void rejectSelected();
    void editSelectedSuggestion();
    void protectSelectedTerm();

  private:
    void refreshRow(int row);
    void refreshDetails();
    void applyResult(const proofreading::RepairQueueResult& result);

    QList<proofreading::RepairQueueItem> items_;
    int selectedRow_ = -1;
    QTableWidget* table_ = nullptr;
    QLabel* evidence_ = nullptr;
    QPlainTextEdit* sourceContext_ = nullptr;
    QLineEdit* suggestion_ = nullptr;
    QLabel* decisionError_ = nullptr;
    QPushButton* approve_ = nullptr;
    QPushButton* reject_ = nullptr;
    QPushButton* edit_ = nullptr;
    QPushButton* protect_ = nullptr;
};

} // namespace loreforge::app
