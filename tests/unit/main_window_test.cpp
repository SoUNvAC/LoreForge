#include "document_fixture.h"
#include "main_window.h"

#include "loreforge/storage/book_repository.h"
#include "loreforge/storage/project_database.h"
#include "loreforge/storage/project_repository.h"

#include <QAction>
#include <QLabel>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QTreeWidget>
#include <QtTest>

#include <memory>
#include <variant>

using namespace Qt::StringLiterals;

class MainWindowTest final : public QObject {
    Q_OBJECT

  private slots:
    void displaysStoredWorkspaceWithoutReorderingDomainData();
    void reportsProjectOpenFailuresInTheInterface();
};

namespace {

loreforge::storage::ProjectRecord projectRecord() {
    return {
        loreforge::core::ProjectId::fromStableKey(u"fixtures/ui-project.loreforge"_s),
        u"Stored Fixture Project"_s,
        QDateTime::fromString(u"2026-09-04T00:00:00.000Z"_s, Qt::ISODateWithMs),
    };
}

std::unique_ptr<loreforge::storage::ProjectDatabase> createStoredFixture(const QString& path) {
    auto result = loreforge::storage::ProjectDatabase::create(path);
    if (!std::holds_alternative<std::unique_ptr<loreforge::storage::ProjectDatabase>>(result)) {
        return {};
    }
    auto database =
        std::get<std::unique_ptr<loreforge::storage::ProjectDatabase>>(std::move(result));
    loreforge::storage::ProjectRepository projects(*database);
    if (projects.create(projectRecord()).has_value()) {
        return {};
    }
    loreforge::storage::BookRepository books(*database);
    if (books.saveDocument(projectRecord().id, loreforge::test::handcraftedDocument())
            .has_value()) {
        return {};
    }
    return database;
}

} // namespace

void MainWindowTest::displaysStoredWorkspaceWithoutReorderingDomainData() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto databasePath = directory.filePath(QStringLiteral("ui-project.loreforge"));
    auto database = createStoredFixture(databasePath);
    QVERIFY(database != nullptr);
    database.reset();

    loreforge::app::MainWindow window;
    QVERIFY(window.openProjectFile(databasePath));

    auto* openAction = window.findChild<QAction*>(QStringLiteral("openProjectAction"));
    auto* projectExplorer = window.findChild<QTreeWidget*>(QStringLiteral("projectExplorer"));
    auto* chapterTree = window.findChild<QTreeWidget*>(QStringLiteral("chapterTree"));
    auto* reader = window.findChild<QTextBrowser*>(QStringLiteral("chapterReader"));
    auto* workspaceStatus = window.findChild<QLabel*>(QStringLiteral("workspaceStatus"));
    auto* summary = window.findChild<QLabel*>(QStringLiteral("documentSummary"));
    auto* bookMetadata = window.findChild<QLabel*>(QStringLiteral("bookMetadata"));
    auto* sourceInfo = window.findChild<QLabel*>(QStringLiteral("sourceInfo"));
    auto* chapterMetadata = window.findChild<QLabel*>(QStringLiteral("chapterMetadata"));
    auto* chapterStatus = window.findChild<QLabel*>(QStringLiteral("chapterStatus"));
    QVERIFY(openAction != nullptr);
    QVERIFY(projectExplorer != nullptr);
    QVERIFY(chapterTree != nullptr);
    QVERIFY(reader != nullptr);
    QVERIFY(workspaceStatus != nullptr);
    QVERIFY(summary != nullptr);
    QVERIFY(bookMetadata != nullptr);
    QVERIFY(sourceInfo != nullptr);
    QVERIFY(chapterMetadata != nullptr);
    QVERIFY(chapterStatus != nullptr);
    QCOMPARE(openAction->shortcut(), QKeySequence::Open);

    QCOMPARE(projectExplorer->topLevelItemCount(), 1);
    const auto* projectItem = projectExplorer->topLevelItem(0);
    QCOMPARE(projectItem->text(0), QStringLiteral("Stored Fixture Project"));
    QCOMPARE(projectItem->text(1), QStringLiteral("Project"));
    QCOMPARE(projectItem->childCount(), 1);
    QVERIFY(projectItem->toolTip(0).contains(projectRecord().id.toString()));
    const auto* bookItem = projectItem->child(0);
    QCOMPARE(bookItem->text(0), QStringLiteral("Handcrafted Fixture"));
    QCOMPARE(bookItem->text(1), QStringLiteral("Stored book"));

    QCOMPARE(chapterTree->topLevelItemCount(), 2);
    QCOMPARE(chapterTree->topLevelItem(0)->text(0), QStringLiteral("Chapter One"));
    QCOMPARE(chapterTree->topLevelItem(0)->text(1), QStringLiteral("2"));
    QCOMPARE(chapterTree->topLevelItem(0)->text(2), QStringLiteral("3"));
    QCOMPARE(chapterTree->topLevelItem(1)->text(0), QStringLiteral("Chapter Two"));
    QCOMPARE(chapterTree->topLevelItem(1)->text(1), QStringLiteral("1"));
    QCOMPARE(chapterTree->topLevelItem(1)->text(2), QStringLiteral("2"));

    QVERIFY(workspaceStatus->text().contains(QStringLiteral("1 projects · 1 books")));
    QCOMPARE(workspaceStatus->toolTip(), databasePath);
    QVERIFY(summary->text().contains(QStringLiteral("2 chapters, 3 words")));
    QVERIFY(bookMetadata->text().contains(QStringLiteral("LoreForge Tests")));
    QVERIFY(bookMetadata->text().contains(QStringLiteral("Language: en")));
    QVERIFY(sourceInfo->text().contains(QStringLiteral("Format: TXT")));
    QVERIFY(sourceInfo->text().contains(QStringLiteral("fixtures/handcrafted.txt")));
    QVERIFY(sourceInfo->text().contains(
        loreforge::test::handcraftedDocument().metadata.sourceHash.toHex()));
    QVERIFY(chapterMetadata->text().contains(QStringLiteral("Chapter 1 of 2")));
    QVERIFY(chapterMetadata->text().contains(QStringLiteral("Blocks: 3")));
    QVERIFY(chapterMetadata->text().contains(QStringLiteral("Words: 2")));
    QVERIFY(chapterStatus->text().contains(QStringLiteral("3 source spans")));
    QVERIFY(reader->toPlainText().contains(QStringLiteral("Hello, world.")));

    chapterTree->setCurrentItem(chapterTree->topLevelItem(1));
    QVERIFY(reader->toPlainText().contains(QStringLiteral("Goodbye.")));
    QVERIFY(chapterMetadata->text().contains(QStringLiteral("Chapter 2 of 2")));
    QVERIFY(chapterMetadata->text().contains(QStringLiteral("Words: 1")));
}

void MainWindowTest::reportsProjectOpenFailuresInTheInterface() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    loreforge::app::MainWindow window;
    QVERIFY(!window.openProjectFile(directory.filePath(QStringLiteral("missing.loreforge"))));

    auto* projectExplorer = window.findChild<QTreeWidget*>(QStringLiteral("projectExplorer"));
    auto* workspaceStatus = window.findChild<QLabel*>(QStringLiteral("workspaceStatus"));
    QVERIFY(projectExplorer != nullptr);
    QVERIFY(workspaceStatus != nullptr);
    QCOMPARE(projectExplorer->topLevelItemCount(), 0);
    QVERIFY(workspaceStatus->text().startsWith(QStringLiteral("Open failed:")));
}

QTEST_MAIN(MainWindowTest)

#include "main_window_test.moc"
