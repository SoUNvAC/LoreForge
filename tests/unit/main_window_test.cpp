#include "document_fixture.h"
#include "main_window.h"

#include <QLabel>
#include <QListWidget>
#include <QTextBrowser>
#include <QtTest>

class MainWindowTest final : public QObject {
    Q_OBJECT

  private slots:
    void displaysImportedDocumentWithoutReorderingIt();
};

void MainWindowTest::displaysImportedDocumentWithoutReorderingIt() {
    loreforge::app::MainWindow window;
    window.setDocument(loreforge::test::handcraftedDocument());

    auto* chapterList = window.findChild<QListWidget*>(QStringLiteral("chapterList"));
    auto* reader = window.findChild<QTextBrowser*>(QStringLiteral("chapterReader"));
    auto* summary = window.findChild<QLabel*>(QStringLiteral("documentSummary"));
    QVERIFY(chapterList != nullptr);
    QVERIFY(reader != nullptr);
    QVERIFY(summary != nullptr);
    QCOMPARE(chapterList->count(), 2);
    QCOMPARE(chapterList->item(0)->text(), QStringLiteral("Chapter One"));
    QCOMPARE(chapterList->item(1)->text(), QStringLiteral("Chapter Two"));
    QVERIFY(summary->text().contains(QStringLiteral("2 chapters, 3 words")));

    chapterList->setCurrentRow(1);
    QVERIFY(reader->toPlainText().contains(QStringLiteral("Goodbye.")));
}

QTEST_MAIN(MainWindowTest)

#include "main_window_test.moc"
