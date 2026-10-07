#include "markdown_source_fixture.h"

#include "loreforge/document/document_validation.h"
#include "loreforge/parser/markdown_source_parser.h"

#include <QTemporaryDir>
#include <QtTest>

using namespace loreforge;

class MarkdownSourceParserTest final : public QObject {
    Q_OBJECT

  private slots:
    void respectsSummaryOrderAndSourceOffsets();
    void retainsChapterIdsAcrossInsertionsAndEdits();
    void rejectsMalformedInput_data();
    void rejectsMalformedInput();
    void rejectsMissingFilesAndInvalidUtf8();
    void acceptsExplicitMetadata();
};

void MarkdownSourceParserTest::respectsSummaryOrderAndSourceOffsets() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QVERIFY(test::createMarkdownFixture(directory.path()));
    auto parsed = parser::MarkdownSourceParser::parseDirectory(directory.filePath("SUMMARY.md"));
    QVERIFY(std::holds_alternative<parser::MarkdownSourceImport>(parsed));
    const auto& imported = std::get<parser::MarkdownSourceImport>(parsed);
    QCOMPARE(imported.volumeCount, 2);
    QCOMPARE(imported.excludedPaths, QStringList({"about.md", "illustrations.md"}));
    const auto& doc = imported.document;
    QVERIFY(document::validateDocument(doc).isValid());
    QCOMPARE(doc.metadata.title, QStringLiteral("Synthetic Novel"));
    QCOMPARE(doc.metadata.sourceFormat, QStringLiteral("markdown-source"));
    QCOMPARE(doc.chapters.size(), 3);
    QCOMPARE(doc.chapters.first().title, QStringLiteral("第二卷 / 后写的章节"));
    QCOMPARE(doc.chapters.at(1).blocks.first().sourceSpan.sourceId, QStringLiteral("02/002.md"));
    QCOMPARE(doc.chapters.at(2).blocks.first().sourceSpan.sourceId, QStringLiteral("01/001.md"));
    const auto& blocks = doc.chapters.first().blocks;
    QCOMPARE(blocks.size(), 5);
    QCOMPARE(blocks.at(2).type, document::BlockType::Heading);
    QCOMPARE(blocks.at(3).type, document::BlockType::SceneBreak);
    const auto bytes = test::markdownFixtureChapter();
    qint64 end = 0;
    for (const auto& block : blocks) {
        const auto& span = block.sourceSpan;
        QVERIFY(span.startByte >= end);
        QVERIFY(span.endByte <= bytes.size());
        QCOMPARE(span.sourceId, QStringLiteral("02/010.md"));
        const auto raw =
            QString::fromUtf8(bytes.mid(span.startByte, span.endByte - span.startByte));
        if (block.type == document::BlockType::Paragraph) {
            QCOMPARE(raw, block.text);
        } else if (block.type == document::BlockType::Heading) {
            QVERIFY(raw.contains(block.text));
        }
        end = span.endByte;
    }
    QVERIFY(blocks.at(1).text.contains(QStringLiteral("\r\n")));
    auto again = parser::MarkdownSourceParser::parseDirectory(directory.path());
    QCOMPARE(std::get<parser::MarkdownSourceImport>(again).document.metadata.sourceHash,
             doc.metadata.sourceHash);
}

void MarkdownSourceParserTest::retainsChapterIdsAcrossInsertionsAndEdits() {
    QTemporaryDir directory;
    QVERIFY(test::createMarkdownFixture(directory.path()));
    const auto original = std::get<parser::MarkdownSourceImport>(
        parser::MarkdownSourceParser::parseDirectory(directory.path()));
    auto summary = test::markdownFixtureSummary();
    const auto marker = QStringLiteral("  - [后写的章节]").toUtf8();
    summary.replace(marker, QByteArray("  - [Inserted](02/000.md)\n") + marker);
    QVERIFY(test::writeMarkdownFixtureFile(directory.path(), "SUMMARY.md", summary));
    QVERIFY(test::writeMarkdownFixtureFile(directory.path(), "02/000.md",
                                           QByteArray("# New\n\nNew paragraph.\n")));
    const auto inserted = std::get<parser::MarkdownSourceImport>(
        parser::MarkdownSourceParser::parseDirectory(directory.path()));
    QCOMPARE(inserted.document.chapters.size(), 4);
    QCOMPARE(inserted.document.chapters.at(1).id, original.document.chapters.first().id);
    QVERIFY(inserted.document.metadata.sourceHash != original.document.metadata.sourceHash);
    QVERIFY(test::writeMarkdownFixtureFile(directory.path(), "02/010.md",
                                           QByteArray("# Revised\n\nChanged paragraph.\n")));
    const auto edited = std::get<parser::MarkdownSourceImport>(
        parser::MarkdownSourceParser::parseDirectory(directory.path()));
    QCOMPARE(edited.document.chapters.at(1).id, inserted.document.chapters.at(1).id);
    QVERIFY(edited.document.metadata.sourceHash != inserted.document.metadata.sourceHash);
    QVERIFY(test::writeMarkdownFixtureFile(directory.path(), "about.md",
                                           QByteArray("Changed excluded page.\n")));
    const auto supplementary = std::get<parser::MarkdownSourceImport>(
        parser::MarkdownSourceParser::parseDirectory(directory.path()));
    QCOMPARE(supplementary.document.metadata.sourceHash, edited.document.metadata.sourceHash);
}

void MarkdownSourceParserTest::rejectsMalformedInput_data() {
    QTest::addColumn<QByteArray>("summary");
    QTest::addColumn<int>("code");
    using Code = parser::MarkdownSourceErrorCode;
    QTest::newRow("traversal") << QByteArray("- [Outside](../escape.md)\n")
                               << int(Code::UnsafePath);
    QTest::newRow("absolute") << QByteArray("- [Outside](C:/escape.md)\n") << int(Code::UnsafePath);
    QTest::newRow("url") << QByteArray("- [Outside](https://example.com/a.md)\n")
                         << int(Code::UnsafePath);
    QTest::newRow("encoded") << QByteArray("- [Outside](%2e%2e/escape.md)\n")
                             << int(Code::UnsafePath);
    QTest::newRow("duplicate")
        << QByteArray("- [Volume](02/index.md)\n  - [A](02/002.md)\n  - [B](02/002.md)\n")
        << int(Code::DuplicatePath);
    QTest::newRow("orphan") << QByteArray("  - [A](02/002.md)\n") << int(Code::InvalidSummary);
    QTest::newRow("deep")
        << QByteArray("- [Volume](02/index.md)\n  - [A](02/002.md)\n    - [B](01/001.md)\n")
        << int(Code::InvalidSummary);
    QTest::newRow("not-volume-index")
        << QByteArray("- [Volume](about.md)\n  - [A](02/002.md)\n") << int(Code::InvalidSummary);
    QTest::newRow("malformed") << QByteArray("- not a link\n") << int(Code::InvalidSummary);
    QTest::newRow("empty") << QByteArray("# Contents\n") << int(Code::InvalidDocument);
}

void MarkdownSourceParserTest::rejectsMalformedInput() {
    QFETCH(QByteArray, summary);
    QFETCH(int, code);
    QTemporaryDir directory;
    QVERIFY(test::createMarkdownFixture(directory.path()));
    QVERIFY(test::writeMarkdownFixtureFile(directory.path(), "SUMMARY.md", summary));
    const auto result = parser::MarkdownSourceParser::parseDirectory(directory.path());
    QVERIFY(std::holds_alternative<parser::MarkdownSourceError>(result));
    QCOMPARE(int(std::get<parser::MarkdownSourceError>(result).code), code);
}

void MarkdownSourceParserTest::rejectsMissingFilesAndInvalidUtf8() {
    QTemporaryDir directory;
    auto result = parser::MarkdownSourceParser::parseDirectory(directory.path());
    QVERIFY(std::holds_alternative<parser::MarkdownSourceError>(result));
    QVERIFY(test::createMarkdownFixture(directory.path()));
    QVERIFY(QFile::remove(directory.filePath("02/002.md")));
    result = parser::MarkdownSourceParser::parseDirectory(directory.path());
    QCOMPARE(std::get<parser::MarkdownSourceError>(result).code,
             parser::MarkdownSourceErrorCode::UnsafePath);
    QVERIFY(test::writeMarkdownFixtureFile(directory.path(), "02/002.md", QByteArray("\xff", 1)));
    result = parser::MarkdownSourceParser::parseDirectory(directory.path());
    QCOMPARE(std::get<parser::MarkdownSourceError>(result).code,
             parser::MarkdownSourceErrorCode::InvalidUtf8);
    for (const auto& malformed :
         {QByteArray("\n \n"), QByteArray("---\nunclosed\n"), QByteArray("```cpp\nx\n```\n")}) {
        QVERIFY(test::writeMarkdownFixtureFile(directory.path(), "02/002.md", malformed));
        result = parser::MarkdownSourceParser::parseDirectory(directory.path());
        QVERIFY(std::holds_alternative<parser::MarkdownSourceError>(result));
    }
}

void MarkdownSourceParserTest::acceptsExplicitMetadata() {
    QTemporaryDir directory;
    QVERIFY(test::createMarkdownFixture(directory.path()));
    const parser::MarkdownSourceOptions options{
        QStringLiteral("User title"), {QStringLiteral("Human author")}, QStringLiteral("en")};
    const auto result = parser::MarkdownSourceParser::parseDirectory(directory.path(), options);
    QVERIFY(std::holds_alternative<parser::MarkdownSourceImport>(result));
    const auto& metadata = std::get<parser::MarkdownSourceImport>(result).document.metadata;
    QCOMPARE(metadata.title, options.title);
    QCOMPARE(metadata.authors, options.authors);
    QCOMPARE(metadata.language, options.language);
}

QTEST_GUILESS_MAIN(MarkdownSourceParserTest)
#include "markdown_source_parser_test.moc"
