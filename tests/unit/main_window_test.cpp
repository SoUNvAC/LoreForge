#include "document_fixture.h"
#include "document_metrics.h"
#include "main_window.h"
#include "markdown_source_fixture.h"
#include "repair_queue_widget.h"

#include "loreforge/storage/book_repository.h"
#include "loreforge/storage/project_database.h"
#include "loreforge/storage/project_repository.h"
#include "loreforge/storage/repair_queue_repository.h"

#include <QAction>
#include <QDockWidget>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStatusBar>
#include <QTableWidget>
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
    void rendersInspectableContext();
    void presentsAndControlsTheRepairQueue();
    void loadsAndPersistsRepairQueueFromStoredProject();
    void importsMarkdownSourceAndReopensProject();
    void countsOnlyParagraphCharacters();
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

loreforge::proofreading::ProofreadingCandidate storedRepairCandidate() {
    const auto document = loreforge::test::handcraftedDocument();
    const auto& block = document.chapters.first().blocks.at(1);
    return {
        loreforge::core::ProofreadingCandidateId::fromStableKey(u"ui:stored-repair"_s),
        document.chapters.first().id,
        {block.sourceSpan.sourceId, block.sourceSpan.startByte, block.sourceSpan.startByte + 5},
        u"Hello"_s,
        u"Hallo"_s,
        loreforge::proofreading::CandidateCategory::Typo,
        0.91,
        u"Stored review evidence."_s,
        loreforge::proofreading::SemanticImpact::TextOnly,
        loreforge::proofreading::CandidateOrigin::Deterministic,
        u"deterministic-proofreader-v1"_s,
        document.metadata.sourceHash,
    };
}

} // namespace

void MainWindowTest::importsMarkdownSourceAndReopensProject() {
    QTemporaryDir source;
    QTemporaryDir destination;
    QVERIFY(source.isValid());
    QVERIFY(destination.isValid());
    QVERIFY(loreforge::test::createMarkdownFixture(source.path()));
    const auto path = destination.filePath(QStringLiteral("novel.loreforge"));
    loreforge::app::MainWindow window;
    QVERIFY(window.findChild<QAction*>(QStringLiteral("importMarkdownSourceAction")));
    QVERIFY2(window.importMarkdownSource(source.path(), path),
             qPrintable(window.statusBar()->currentMessage()));
    auto* tree = window.findChild<QTreeWidget*>(QStringLiteral("chapterTree"));
    QCOMPARE(tree->topLevelItemCount(), 3);
    QCOMPARE(tree->topLevelItem(0)->text(0), QStringLiteral("第二卷 / 后写的章节"));
    auto* reader = window.findChild<QTextBrowser*>(QStringLiteral("chapterReader"));
    QVERIFY(reader->toPlainText().contains(QStringLiteral("她打开了一扇门。")));
    QVERIFY(!reader->toPlainText().contains(QStringLiteral("title: 页面元数据")));
    QFile original(source.filePath(QStringLiteral("02/010.md")));
    QVERIFY(original.open(QIODevice::ReadOnly));
    QCOMPARE(original.readAll(), loreforge::test::markdownFixtureChapter());
    QVERIFY(!window.importMarkdownSource(source.path(), path));
    QCOMPARE(tree->topLevelItemCount(), 3);
    QVERIFY(!window.importMarkdownSource(source.path(),
                                         source.filePath(QStringLiteral("unsafe.loreforge"))));
    QVERIFY(!QFileInfo::exists(source.filePath(QStringLiteral("unsafe.loreforge"))));
    QVERIFY(window.openProjectFile(path));
    QCOMPARE(tree->topLevelItemCount(), 3);
    QVERIFY(loreforge::test::writeMarkdownFixtureFile(source.path(), QStringLiteral("02/002.md"),
                                                      QByteArray("\xff", 1)));
    const auto failedPath = destination.filePath(QStringLiteral("failed.loreforge"));
    QVERIFY(!window.importMarkdownSource(source.path(), failedPath));
    QVERIFY(!QFileInfo::exists(failedPath));
    QCOMPARE(tree->topLevelItemCount(), 3);
}

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
    QCOMPARE(chapterTree->headerItem()->text(1), QStringLiteral("字数"));
    QCOMPARE(chapterTree->headerItem()->text(2), QStringLiteral("汉字"));
    QCOMPARE(chapterTree->topLevelItem(0)->text(1), QStringLiteral("10"));
    QCOMPARE(chapterTree->topLevelItem(0)->text(2), QStringLiteral("0"));
    QCOMPARE(chapterTree->topLevelItem(0)->text(3), QStringLiteral("3"));
    QCOMPARE(chapterTree->topLevelItem(1)->text(0), QStringLiteral("Chapter Two"));
    QCOMPARE(chapterTree->topLevelItem(1)->text(1), QStringLiteral("7"));
    QCOMPARE(chapterTree->topLevelItem(1)->text(2), QStringLiteral("0"));
    QCOMPARE(chapterTree->topLevelItem(1)->text(3), QStringLiteral("2"));

    QVERIFY(workspaceStatus->text().contains(QStringLiteral("1 projects · 1 books")));
    QCOMPARE(workspaceStatus->toolTip(), databasePath);
    QVERIFY(summary->text().contains(QStringLiteral("总字数：17 · 总汉字：0")));
    QVERIFY(bookMetadata->text().contains(QStringLiteral("LoreForge Tests")));
    QVERIFY(bookMetadata->text().contains(QStringLiteral("Language: en")));
    QVERIFY(sourceInfo->text().contains(QStringLiteral("Format: TXT")));
    QVERIFY(sourceInfo->text().contains(QStringLiteral("fixtures/handcrafted.txt")));
    QVERIFY(sourceInfo->text().contains(
        loreforge::test::handcraftedDocument().metadata.sourceHash.toHex()));
    QVERIFY(chapterMetadata->text().contains(QStringLiteral("Chapter 1 of 2")));
    QVERIFY(chapterMetadata->text().contains(QStringLiteral("Blocks: 3")));
    QVERIFY(chapterMetadata->text().contains(QStringLiteral("字数：10\n汉字：0")));
    QVERIFY(chapterStatus->text().contains(QStringLiteral("3 source spans")));
    QVERIFY(reader->toPlainText().contains(QStringLiteral("Hello, world.")));

    chapterTree->setCurrentItem(chapterTree->topLevelItem(1));
    QVERIFY(reader->toPlainText().contains(QStringLiteral("Goodbye.")));
    QVERIFY(chapterMetadata->text().contains(QStringLiteral("Chapter 2 of 2")));
    QVERIFY(chapterMetadata->text().contains(QStringLiteral("字数：7\n汉字：0")));
}

void MainWindowTest::countsOnlyParagraphCharacters() {
    auto doc = loreforge::test::handcraftedDocument();
    doc.chapters[0].title = QStringLiteral("标题不应计入");
    doc.chapters[0].blocks[0].text = QStringLiteral("标题中文 ABC 123");
    doc.chapters[0].blocks[1].text = QStringLiteral("你好，world！2026");
    doc.chapters[0].blocks[2].text = QStringLiteral("场景分隔不计入");
    doc.chapters[1].blocks[1].text = QStringLiteral("再见。");
    const auto chapter = loreforge::app::DocumentMetrics::forChapter(doc.chapters.first());
    QCOMPARE(chapter.characterCount, 11);
    QCOMPARE(chapter.hanCharacterCount, 2);
    const auto totals = loreforge::app::DocumentMetrics::forDocument(doc);
    QCOMPARE(totals.characterCount, 13);
    QCOMPARE(totals.hanCharacterCount, 4);
    QCOMPARE(totals.blockCount, 5);
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

void MainWindowTest::rendersInspectableContext() {
    loreforge::app::MainWindow window;
    const QJsonObject schema{
        {u"type"_s, u"object"_s},
        {u"properties"_s, QJsonObject{{u"answer"_s, QJsonObject{{u"type"_s, u"string"_s}}}}},
    };
    const loreforge::context::ContextInspectorData context{
        u"Use evidence."_s,
        u"A city at night."_s,
        {u"Starwatch"_s},
        {u"Mara | alias: Captain"_s},
        {u"Chapter 1: Mara arrived."_s},
        {u"Who opened the gate?"_s},
        u"Mara reached the city."_s,
        u"She hears a sound."_s,
        u"Return one fact."_s,
        schema,
        {1'000, 200},
        321,
        1,
        2,
        3,
        u"[CURRENT CHAPTER]\nShe hears a sound."_s,
        u"SYSTEM:\nUse evidence.\n\nUSER:\n[CURRENT CHAPTER]\nShe hears a sound."_s,
    };

    window.inspectContext(context);

    auto* dock = window.findChild<QDockWidget*>(u"contextInspectorDock"_s);
    auto* sections = window.findChild<QTextBrowser*>(u"contextInspectorSections"_s);
    auto* rawPrompt = window.findChild<QPlainTextEdit*>(u"contextRawPrompt"_s);
    QVERIFY(dock != nullptr);
    QVERIFY(sections != nullptr);
    QVERIFY(rawPrompt != nullptr);
    const auto rendered = sections->toPlainText();
    QVERIFY(rendered.contains(u"System Rules"_s));
    QVERIFY(rendered.contains(u"Character Memory"_s));
    QVERIFY(rendered.contains(u"Mara"_s));
    QVERIFY(rendered.contains(u"Current Chapter"_s));
    QVERIFY(rendered.contains(u"Estimated: 321"_s));
    QVERIFY(rendered.contains(u"1 characters, 2 events, 3 open threads"_s));
    QCOMPARE(rawPrompt->toPlainText(), context.rawFinalPrompt);
}

void MainWindowTest::presentsAndControlsTheRepairQueue() {
    const auto text = u"Mara walk home."_s;
    const auto hash = loreforge::core::ContentHash::sha256(text);
    const loreforge::proofreading::ProofreadingCandidate candidate{
        loreforge::core::ProofreadingCandidateId::fromStableKey(u"ui:repair:candidate"_s),
        loreforge::core::ChapterId::fromStableKey(u"ui:repair:chapter"_s),
        {u"chapter.txt"_s, 5, 9},
        u"walk"_s,
        u"walks"_s,
        loreforge::proofreading::CandidateCategory::Typo,
        0.88,
        u"The subject requires a singular verb."_s,
        loreforge::proofreading::SemanticImpact::ActionChange,
        loreforge::proofreading::CandidateOrigin::Semantic,
        u"semantic-proofreader-v1"_s,
        hash,
    };
    const auto reviewed = loreforge::proofreading::RepairQueueWorkflow::review(
        loreforge::core::ProjectId::fromStableKey(u"ui:repair:project"_s), candidate, text,
        QDateTime::fromString(u"2026-10-04T08:00:00.000Z"_s, Qt::ISODateWithMs));
    QVERIFY(std::holds_alternative<loreforge::proofreading::RepairQueueItem>(reviewed));

    loreforge::app::MainWindow window;
    window.inspectRepairQueue({std::get<loreforge::proofreading::RepairQueueItem>(reviewed)});
    auto* dock = window.findChild<QDockWidget*>(u"repairQueueDock"_s);
    auto* queue = window.findChild<loreforge::app::RepairQueueWidget*>();
    auto* table = window.findChild<QTableWidget*>(u"repairQueueTable"_s);
    auto* evidence = window.findChild<QLabel*>(u"repairEvidence"_s);
    auto* context = window.findChild<QPlainTextEdit*>(u"repairSourceContext"_s);
    auto* suggestion = window.findChild<QLineEdit*>(u"repairSuggestion"_s);
    auto* edit = window.findChild<QPushButton*>(u"editRepairSuggestion"_s);
    auto* approve = window.findChild<QPushButton*>(u"approveRepairCandidate"_s);
    QVERIFY(dock != nullptr);
    QVERIFY(queue != nullptr);
    QVERIFY(table != nullptr);
    QVERIFY(evidence != nullptr);
    QVERIFY(context != nullptr);
    QVERIFY(suggestion != nullptr);
    QVERIFY(edit != nullptr);
    QVERIFY(approve != nullptr);
    QCOMPARE(table->rowCount(), 1);
    QCOMPARE(table->item(0, 0)->text(), u"TYPO"_s);
    QCOMPARE(table->item(0, 1)->text(), u"88%"_s);
    QCOMPARE(table->item(0, 3)->text(), u"REVIEW_REQUIRED"_s);
    QVERIFY(evidence->text().contains(u"singular verb"_s));
    QCOMPARE(context->toPlainText(), text);

    QSignalSpy edited(queue, &loreforge::app::RepairQueueWidget::suggestionEdited);
    suggestion->setText(u"walked"_s);
    edit->click();
    QCOMPARE(edited.count(), 1);
    QCOMPARE(table->item(0, 2)->text(), u"walked"_s);
    QSignalSpy approved(queue, &loreforge::app::RepairQueueWidget::candidateApproved);
    approve->click();
    QCOMPARE(approved.count(), 1);
    QCOMPARE(table->item(0, 3)->text(), u"APPROVED"_s);
    QVERIFY(!approve->isEnabled());
}

void MainWindowTest::loadsAndPersistsRepairQueueFromStoredProject() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto databasePath = directory.filePath(u"repair-ui.loreforge"_s);
    auto database = createStoredFixture(databasePath);
    QVERIFY(database != nullptr);
    loreforge::storage::RepairQueueRepository repairs(*database);
    const auto candidate = storedRepairCandidate();
    QVERIFY(!repairs
                 .enqueue(projectRecord().id, candidate, u"Hello, world."_s,
                          QDateTime::fromString(u"2026-10-04T08:00:00.000Z"_s, Qt::ISODateWithMs))
                 .has_value());
    database.reset();

    loreforge::app::MainWindow window;
    QVERIFY(window.openProjectFile(databasePath));
    auto* table = window.findChild<QTableWidget*>(u"repairQueueTable"_s);
    auto* approve = window.findChild<QPushButton*>(u"approveRepairCandidate"_s);
    QVERIFY(table != nullptr);
    QVERIFY(approve != nullptr);
    QCOMPARE(table->rowCount(), 1);
    QCOMPARE(table->item(0, 3)->text(), u"REVIEW_REQUIRED"_s);
    approve->click();

    auto reopened = loreforge::storage::ProjectDatabase::open(databasePath);
    QVERIFY(std::holds_alternative<std::unique_ptr<loreforge::storage::ProjectDatabase>>(reopened));
    database = std::get<std::unique_ptr<loreforge::storage::ProjectDatabase>>(std::move(reopened));
    loreforge::storage::RepairQueueRepository persisted(*database);
    const auto loaded = persisted.find(candidate.id);
    QVERIFY(std::holds_alternative<loreforge::proofreading::RepairQueueItem>(loaded));
    QCOMPARE(std::get<loreforge::proofreading::RepairQueueItem>(loaded).status,
             loreforge::proofreading::CandidateStatus::Approved);
}

QTEST_MAIN(MainWindowTest)

#include "main_window_test.moc"
