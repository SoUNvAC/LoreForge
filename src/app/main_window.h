#pragma once

#include "loreforge/document/document.h"

#include <QMainWindow>

#include <optional>

class QLabel;
class QListWidget;
class QTextBrowser;

namespace loreforge::app {

class MainWindow final : public QMainWindow {
    Q_OBJECT

  public:
    explicit MainWindow(QWidget* parent = nullptr);

    void setDocument(const document::Document& document);

  private slots:
    void importText();
    void displayChapter(int row);

  private:
    [[nodiscard]] qsizetype chapterWordCount(const document::Chapter& chapter) const;

    QListWidget* chapterList_ = nullptr;
    QTextBrowser* reader_ = nullptr;
    QLabel* documentSummary_ = nullptr;
    std::optional<document::Document> document_;
};

} // namespace loreforge::app
