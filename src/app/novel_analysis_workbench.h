#pragma once

#include "chapter_analysis_controller.h"

#include <QWidget>
#include <functional>

class QComboBox;
class QCheckBox;
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
    void restoreBudgets(int maximumTokens, int reservedTokens);
    void setConnectionValidator(std::function<QString()> validator);
    void setConnectionSummary(QString summary);
    explicit NovelAnalysisWorkbench(QWidget* parent = nullptr);
    void setSelectedBook(const document::Document* book, qsizetype chapterIndex);
    void showChapterPreview(const ChapterPreview& preview);
    void showError(QString message);
    void setBusy(bool busy);
    void updateRole(int role, QString status, QString usage, qint64 latencyMs);
    void showHistory(QList<AnalysisHistoryEntry> history, QString chapterId);
    [[nodiscard]] const std::optional<ChapterPreview>& preview() const noexcept;
    void invalidatePreview();
    [[nodiscard]] bool autoNextEnabled() const;
    void stopAutoNext();
    [[nodiscard]] bool prepareAutomaticChapter(bool dialogueOnly, int maximumTokens,
                                               int reservedTokens);
    [[nodiscard]] bool dialogueOnly() const noexcept {
        return dialogueOnly_;
    }

  signals:
    void budgetsChanged(int maximumTokens, int reservedTokens);
    void chapterSelected(int index);
    void chapterPreviewRequested(int maximumTokens, int reservedTokens);
    void analysisRequested();
    void continuationRequested(QString pipelineId, bool allowLegacyEndpoint);
    void cancellationRequested();
    void autoNextChanged(bool enabled);
    void metricsChanged(QString summary);
    void highlightImportedTextChanged(QString bookId, QString text);
    void highlightSourceChanged(QString bookId, QJsonObject source, QJsonObject analysis,
                                QString status);

  private:
    void showHistoryEntry(int index);
    void renderAnalysis(const QJsonObject& output, const QJsonObject& source);
    void updateButtons();
    void confirmAndStart(bool continuing, bool dialogueOnly = false);
    bool dialogueOnly_ = false;
    QCheckBox* autoNext_ = nullptr;
    bool autoNextAuthorized_ = false;
    QPushButton* dialogue_ = nullptr;
    QComboBox* chapters_ = nullptr;
    QSpinBox* contextLimit_ = nullptr;
    QSpinBox* outputBudget_ = nullptr;
    QPushButton* prepare_ = nullptr;
    QPushButton* analyze_ = nullptr;
    QPushButton* resume_ = nullptr;
    QString resumePipeline_;
    int resumePrefix_ = 0;
    bool legacyEndpoint_ = false;
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
    QString bookId_;
    std::optional<ChapterPreview> preview_;
    bool available_ = false;
    bool busy_ = false;
    std::function<QString()> connectionValidator_;
    QString connectionSummary_;
};
} // namespace loreforge::app
