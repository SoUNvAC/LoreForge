#pragma once

#include "chapter_analysis_controller.h"

#include <QWidget>

class QComboBox;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTreeWidget;

namespace loreforge::app {
class NovelAnalysisWorkbench final : public QWidget {
    Q_OBJECT
  public:
    explicit NovelAnalysisWorkbench(QWidget* parent = nullptr);
    void setSelectedBook(const document::Document* book, qsizetype chapterIndex);
    void showChapterPreview(const ChapterPreview& preview);
    void showError(QString message);
    void setBusy(bool busy);
    void updateRole(int role, QString status, QString usage, qint64 latencyMs);
    void showHistory(QList<AnalysisHistoryEntry> history, QString chapterId);
    [[nodiscard]] const std::optional<ChapterPreview>& preview() const noexcept;
    void invalidatePreview();

  signals:
    void chapterSelected(int index);
    void chapterPreviewRequested(int maximumTokens, int reservedTokens);
    void analysisRequested();
    void cancellationRequested();
    void metricsChanged(QString summary);

  private:
    void showHistoryEntry(int index);
    void renderAnalysis(const QJsonObject& output, const QJsonObject& source);
    void updateButtons();
    QComboBox* chapters_ = nullptr;
    QSpinBox* contextLimit_ = nullptr;
    QSpinBox* outputBudget_ = nullptr;
    QPushButton* prepare_ = nullptr;
    QPushButton* analyze_ = nullptr;
    QPushButton* cancel_ = nullptr;
    QLabel* previewStatus_ = nullptr;
    QLabel* taskStatus_ = nullptr;
    QLabel* totals_ = nullptr;
    QPlainTextEdit* sourcePreview_ = nullptr;
    QPlainTextEdit* messages_ = nullptr;
    QPlainTextEdit* diagnostics_ = nullptr;
    QPlainTextEdit* evidence_ = nullptr;
    QTableWidget* roles_ = nullptr;
    QListWidget* historyList_ = nullptr;
    QList<QTreeWidget*> results_;
    QList<AnalysisHistoryEntry> history_;
    QJsonObject displayedSource_;
    QString chapterId_;
    std::optional<ChapterPreview> preview_;
    bool available_ = false;
    bool busy_ = false;
};
} // namespace loreforge::app
