#include "chapter_preview.h"
#include "markdown_source_fixture.h"

#include "loreforge/parser/markdown_source_parser.h"
#include "loreforge/storage/inference_repository.h"
#include "loreforge/storage/project_database.h"
#include "loreforge/storage/project_repository.h"

#include <QJsonArray>
#include <QTemporaryDir>
#include <QtTest>

namespace {
loreforge::document::Document imported(const QString& path) {
    auto result = loreforge::parser::MarkdownSourceParser::parseDirectory(path);
    if (!std::holds_alternative<loreforge::parser::MarkdownSourceImport>(result)) {
        return {};
    }
    return std::get<loreforge::parser::MarkdownSourceImport>(std::move(result)).document;
}
const auto projectId = loreforge::core::ProjectId::fromStableKey(QStringLiteral("preview-test"));
using Preview = loreforge::app::ChapterPreview;
using Builder = loreforge::app::ChapterPreviewBuilder;
using Error = loreforge::storage::StorageError;
using Database = loreforge::storage::ProjectDatabase;
} // namespace

class ChapterPreviewTest final : public QObject {
    Q_OBJECT
  private slots:
    void capturesExactCurrentChapterWithProvenance();
    void rejectsChangedMissingAndUnsafeSources();
    void refusesOverBudgetWithoutTruncating();
    void persistsImmutableVersionsAndReusesSnapshots();
    void rollsBackFailedStorage();
};

void ChapterPreviewTest::capturesExactCurrentChapterWithProvenance() {
    QTemporaryDir source;
    QVERIFY(loreforge::test::createMarkdownFixture(source.path()));
    const auto book = imported(source.path());
    const auto result = Builder::build(projectId, book, 0, 32768, 4096);
    QVERIFY(std::holds_alternative<Preview>(result));
    const auto& preview = std::get<Preview>(result);
    const auto content = preview.snapshot.content;
    const auto span = content.value(QStringLiteral("source")).toObject();
    const auto start = span.value(QStringLiteral("start_byte")).toInteger();
    const auto end = span.value(QStringLiteral("end_byte")).toInteger();
    QCOMPARE(span.value(QStringLiteral("utf8")).toString().toUtf8(),
             loreforge::test::markdownFixtureChapter().mid(start, end - start));
    QCOMPARE(span.value(QStringLiteral("source_id")).toString(), QStringLiteral("02/010.md"));
    QCOMPARE(content.value(QStringLiteral("blocks")).toArray().size(),
             book.chapters[0].blocks.size());
    QCOMPARE(content.value(QStringLiteral("chapter_id")).toString(),
             book.chapters[0].id.toString());
    QCOMPARE(content.value(QStringLiteral("source_file_sha256")).toString(),
             loreforge::core::ContentHash::sha256(
                 QByteArrayView(loreforge::test::markdownFixtureChapter()))
                 .toHex());
    QVERIFY(!content.value(QStringLiteral("sent")).toBool());
    const auto messages = QJsonDocument::fromJson(preview.messagesJson.toUtf8()).array();
    QCOMPARE(messages, content.value(QStringLiteral("messages")).toArray());
    QCOMPARE(messages.size(), 2);
    const auto user =
        QJsonDocument::fromJson(
            messages[1].toObject().value(QStringLiteral("content")).toString().toUtf8())
            .object();
    QCOMPARE(user.value(QStringLiteral("source")).toObject(), span);
    QCOMPARE(user.value(QStringLiteral("output_schema")).toObject(), preview.schema.schema);
    QVERIFY(!preview.messagesJson.contains(QStringLiteral("页面元数据")));
    QVERIFY(!preview.messagesJson.contains(QStringLiteral("Second file.")));
    QVERIFY(!preview.messagesJson.contains(source.path()));
    QVERIFY(preview.estimatedTokens > 0);
    auto repeated = Builder::build(projectId, book, 0, 32768, 4096);
    QCOMPARE(std::get<Preview>(repeated).snapshot.id, preview.snapshot.id);
    auto changedBudget = Builder::build(projectId, book, 0, 40000, 4096);
    QVERIFY(std::get<Preview>(changedBudget).snapshot.id != preview.snapshot.id);
}

void ChapterPreviewTest::rejectsChangedMissingAndUnsafeSources() {
    QTemporaryDir source;
    QVERIFY(loreforge::test::createMarkdownFixture(source.path()));
    const auto book = imported(source.path());
    QVERIFY(loreforge::test::writeMarkdownFixtureFile(
        source.path(), QStringLiteral("02/010.md"), QByteArray("# Changed\n\nChanged content.\n")));
    auto result = Builder::build(projectId, book, 0, 32768, 4096);
    QVERIFY(std::holds_alternative<Error>(result));
    QVERIFY(std::get<Error>(result).message.contains(QStringLiteral("不一致")));
    QVERIFY(loreforge::test::writeMarkdownFixtureFile(source.path(), QStringLiteral("02/010.md"),
                                                      QByteArray("\xff", 1)));
    QVERIFY(std::holds_alternative<Error>(Builder::build(projectId, book, 0, 32768, 4096)));
    auto unsafe = book;
    unsafe.chapters[0].blocks[0].sourceSpan.sourceId = QStringLiteral("../outside.md");
    QVERIFY(std::holds_alternative<Error>(Builder::build(projectId, unsafe, 0, 32768, 4096)));
    unsafe.metadata.sourceLocator = source.filePath(QStringLiteral("missing"));
    QVERIFY(std::holds_alternative<Error>(Builder::build(projectId, unsafe, 0, 32768, 4096)));
    unsafe = book;
    unsafe.metadata.sourceFormat = QStringLiteral("txt");
    QVERIFY(std::holds_alternative<Error>(Builder::build(projectId, unsafe, 0, 32768, 4096)));
}

void ChapterPreviewTest::refusesOverBudgetWithoutTruncating() {
    QTemporaryDir source;
    QVERIFY(loreforge::test::createMarkdownFixture(source.path()));
    const auto book = imported(source.path());
    const auto result = Builder::build(projectId, book, 0, 32768, 4096);
    QVERIFY(std::holds_alternative<Preview>(result));
    const auto estimated = static_cast<int>(std::get<Preview>(result).estimatedTokens);
    QVERIFY(std::holds_alternative<Preview>(
        Builder::build(projectId, book, 0, estimated + 4096, 4096)));
    const auto over = Builder::build(projectId, book, 0, estimated + 4095, 4096);
    QVERIFY(std::holds_alternative<Error>(over));
    QVERIFY(std::get<Error>(over).message.contains(QStringLiteral("未截断")));
    QVERIFY(std::holds_alternative<Error>(Builder::build(projectId, book, 0, 4096, 4096)));
    QVERIFY(std::holds_alternative<Error>(Builder::build(projectId, book, -1, 32768, 4096)));
}

void ChapterPreviewTest::persistsImmutableVersionsAndReusesSnapshots() {
    QTemporaryDir source;
    QTemporaryDir destination;
    QVERIFY(loreforge::test::createMarkdownFixture(source.path()));
    const auto book = imported(source.path());
    auto result = Builder::build(projectId, book, 0, 32768, 4096);
    QVERIFY(std::holds_alternative<Preview>(result));
    auto preview = std::get<Preview>(result);
    const auto path = destination.filePath(QStringLiteral("preview.loreforge"));
    auto created = Database::create(path);
    QVERIFY(std::holds_alternative<std::unique_ptr<Database>>(created));
    auto database = std::get<std::unique_ptr<Database>>(std::move(created));
    loreforge::storage::ProjectRepository projects(*database);
    QVERIFY(
        !projects.create({projectId, QStringLiteral("Preview"), QDateTime::currentDateTimeUtc()}));
    auto saved = Builder::store(*database, preview);
    QVERIFY2(std::holds_alternative<Preview>(saved),
             std::holds_alternative<Error>(saved)
                 ? qPrintable(std::get<Error>(saved).message + QStringLiteral(" ") +
                              std::get<Error>(saved).technicalDetails)
                 : "");
    auto repeated = Builder::build(projectId, book, 0, 32768, 4096);
    auto savedAgain = Builder::store(*database, std::get<Preview>(repeated));
    QVERIFY(std::holds_alternative<Preview>(savedAgain));
    QCOMPARE(std::get<Preview>(savedAgain).snapshot, std::get<Preview>(saved).snapshot);
    auto conflict = preview;
    conflict.prompt.text += QStringLiteral(" different");
    conflict.prompt.contentHash =
        loreforge::core::ContentHash::sha256(QStringView(conflict.prompt.text));
    QVERIFY(std::holds_alternative<Error>(Builder::store(*database, conflict)));
    database.reset();
    auto reopened = Database::open(path);
    QVERIFY(std::holds_alternative<std::unique_ptr<Database>>(reopened));
    database = std::get<std::unique_ptr<Database>>(std::move(reopened));
    loreforge::storage::InferenceRepository repository(*database);
    const auto stored = repository.findContextSnapshot(preview.snapshot.id);
    QVERIFY(std::holds_alternative<loreforge::inference::ContextSnapshot>(stored));
    QCOMPARE(std::get<loreforge::inference::ContextSnapshot>(stored), preview.snapshot);
    QVERIFY(std::holds_alternative<loreforge::inference::PromptVersion>(
        repository.findPromptVersion(preview.prompt.prompt.id, 1)));
    QVERIFY(std::holds_alternative<loreforge::inference::OutputSchema>(
        repository.findOutputSchema(preview.schema.id, 1)));
}

void ChapterPreviewTest::rollsBackFailedStorage() {
    QTemporaryDir source;
    QTemporaryDir destination;
    QVERIFY(loreforge::test::createMarkdownFixture(source.path()));
    auto built = Builder::build(projectId, imported(source.path()), 0, 32768, 4096);
    QVERIFY(std::holds_alternative<Preview>(built));
    const auto preview = std::get<Preview>(built);
    auto created = Database::create(destination.filePath(QStringLiteral("empty.loreforge")));
    QVERIFY(std::holds_alternative<std::unique_ptr<Database>>(created));
    auto database = std::get<std::unique_ptr<Database>>(std::move(created));
    QVERIFY(std::holds_alternative<Error>(Builder::store(*database, preview)));
    loreforge::storage::InferenceRepository repository(*database);
    QVERIFY(
        std::holds_alternative<Error>(repository.findPromptVersion(preview.prompt.prompt.id, 1)));
    QVERIFY(std::holds_alternative<Error>(repository.findOutputSchema(preview.schema.id, 1)));
    QVERIFY(std::holds_alternative<Error>(repository.findContextSnapshot(preview.snapshot.id)));
}

QTEST_GUILESS_MAIN(ChapterPreviewTest)
#include "chapter_preview_test.moc"
