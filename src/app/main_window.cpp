#include "main_window.h"

#include "loreforge/parser/plain_text_parser.h"
#include "loreforge/text/word_counter.h"

#include <QAction>
#include <QFileDialog>
#include <QLabel>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTextBrowser>
#include <QVBoxLayout>
#include <QWidget>

#include <variant>

namespace loreforge::app {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(tr("LoreForge"));
    resize(960, 640);

    auto* importAction = new QAction(tr("Import Text..."), this);
    importAction->setShortcut(QKeySequence::Open);
    connect(importAction, &QAction::triggered, this, &MainWindow::importText);
    menuBar()->addMenu(tr("&File"))->addAction(importAction);

    auto* central = new QWidget(this);
    auto* centralLayout = new QVBoxLayout(central);
    documentSummary_ = new QLabel(tr("Import a UTF-8 text file to begin."), central);
    documentSummary_->setObjectName(QStringLiteral("documentSummary"));
    centralLayout->addWidget(documentSummary_);

    auto* splitter = new QSplitter(Qt::Horizontal, central);
    chapterList_ = new QListWidget(splitter);
    chapterList_->setObjectName(QStringLiteral("chapterList"));
    chapterList_->setMinimumWidth(220);
    reader_ = new QTextBrowser(splitter);
    reader_->setObjectName(QStringLiteral("chapterReader"));
    reader_->setHtml(tr("<h2>LoreForge</h2><p>Use File &gt; Import Text to open a novel.</p>"));
    splitter->addWidget(chapterList_);
    splitter->addWidget(reader_);
    splitter->setStretchFactor(1, 1);
    centralLayout->addWidget(splitter, 1);
    setCentralWidget(central);

    connect(chapterList_, &QListWidget::currentRowChanged, this, &MainWindow::displayChapter);
    statusBar()->showMessage(tr("Ready"));
}

void MainWindow::setDocument(const document::Document& document) {
    document_ = document;
    chapterList_->clear();

    qsizetype totalWords = 0;
    for (const auto& chapter : document.chapters) {
        chapterList_->addItem(chapter.title);
        totalWords += chapterWordCount(chapter);
    }

    documentSummary_->setText(tr("%1 — %2 chapters, %3 words")
                                  .arg(document.metadata.title)
                                  .arg(document.chapters.size())
                                  .arg(totalWords));
    if (!document.chapters.isEmpty()) {
        chapterList_->setCurrentRow(0);
    }
}

void MainWindow::importText() {
    const auto filePath = QFileDialog::getOpenFileName(this, tr("Import UTF-8 Text"), {},
                                                       tr("Text files (*.txt);;All files (*)"));
    if (filePath.isEmpty()) {
        return;
    }

    const auto result = parser::PlainTextParser::parseFile(filePath);
    if (std::holds_alternative<parser::PlainTextParseError>(result)) {
        const auto& parseError = std::get<parser::PlainTextParseError>(result);
        QMessageBox::critical(this, tr("Import failed"), parseError.message);
        statusBar()->showMessage(tr("Import failed"));
        return;
    }

    setDocument(std::get<document::Document>(result));
    statusBar()->showMessage(tr("Imported %1").arg(filePath), 5000);
}

void MainWindow::displayChapter(int row) {
    if (!document_.has_value() || row < 0 || row >= document_->chapters.size()) {
        reader_->clear();
        return;
    }

    const auto& chapter = document_->chapters.at(row);
    QString html;
    for (const auto& block : chapter.blocks) {
        auto escaped = block.text.toHtmlEscaped();
        switch (block.type) {
        case document::BlockType::Heading:
            html += QStringLiteral("<h2>%1</h2>").arg(escaped);
            break;
        case document::BlockType::Paragraph:
            html += QStringLiteral("<p>%1</p>").arg(escaped);
            break;
        case document::BlockType::SceneBreak:
            html += QStringLiteral("<p style=\"text-align:center\">%1</p>").arg(escaped);
            break;
        case document::BlockType::Unknown:
            html += QStringLiteral("<p>%1</p>").arg(escaped);
            break;
        }
    }
    reader_->setHtml(html);
    statusBar()->showMessage(tr("Chapter %1 of %2 — %3 words")
                                 .arg(row + 1)
                                 .arg(document_->chapters.size())
                                 .arg(chapterWordCount(chapter)));
}

qsizetype MainWindow::chapterWordCount(const document::Chapter& chapter) const {
    qsizetype words = 0;
    for (const auto& block : chapter.blocks) {
        if (block.type == document::BlockType::Paragraph) {
            words += text::WordCounter::count(block.text);
        }
    }
    return words;
}

} // namespace loreforge::app
