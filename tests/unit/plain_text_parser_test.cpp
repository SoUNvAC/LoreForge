#include "loreforge/core/content_hash.h"
#include "loreforge/parser/plain_text_parser.h"
#include "loreforge/text/word_counter.h"

#include <QtTest>

#include <variant>

using namespace Qt::StringLiterals;

class PlainTextParserTest final : public QObject {
    Q_OBJECT

  private slots:
    void preservesOriginalBytesForBomAndCrlfSource();
    void normalizesLfAndCrlfToTheSameText();
    void createsOneChapterWhenNoHeadingExists();
    void retainsConsecutiveHeadingsAsEmptyChapters();
    void importsALargeChapter();
    void rejectsInvalidUtf8();
    void rejectsWhitespaceOnlyInput();
};

namespace {

loreforge::parser::PlainTextImportOptions options() {
    return {
        u"fixtures/test.txt"_s,
        u"fixtures/test.txt"_s,
        u"Test Novel"_s,
        {u"Test Author"_s},
        u"en"_s,
    };
}

const loreforge::document::Document&
parsedDocument(const loreforge::parser::PlainTextParseResult& result) {
    return std::get<loreforge::document::Document>(result);
}

QStringList blockTexts(const loreforge::document::Document& document) {
    QStringList texts;
    for (const auto& chapter : document.chapters) {
        for (const auto& block : chapter.blocks) {
            texts.append(block.text);
        }
    }
    return texts;
}

} // namespace

void PlainTextParserTest::preservesOriginalBytesForBomAndCrlfSource() {
    const QByteArray source =
        QByteArray("\xEF\xBB\xBF", 3) + QByteArray("Chapter 1\r\nCaf\xC3\xA9\r\n", 18);
    const auto result = loreforge::parser::PlainTextParser::parse(source, options());
    QVERIFY(std::holds_alternative<loreforge::document::Document>(result));

    const auto& document = parsedDocument(result);
    QCOMPARE(document.metadata.sourceHash,
             loreforge::core::ContentHash::sha256(QByteArrayView(source)));
    QCOMPARE(document.chapters.size(), 1);
    QCOMPARE(document.chapters.first().blocks.size(), 2);
    QCOMPARE(document.chapters.first().blocks.at(0).sourceSpan.startByte, 3);
    QCOMPARE(document.chapters.first().blocks.at(0).sourceSpan.endByte, 12);
    QCOMPARE(document.chapters.first().blocks.at(1).text, u"Caf\u00e9"_s);
    QCOMPARE(document.chapters.first().blocks.at(1).sourceSpan.startByte, 14);
    QCOMPARE(document.chapters.first().blocks.at(1).sourceSpan.endByte, 19);
}

void PlainTextParserTest::normalizesLfAndCrlfToTheSameText() {
    const auto lf = loreforge::parser::PlainTextParser::parse(
        QByteArrayLiteral("Chapter 1\nFirst line\nsecond line\n\n***\n"), options());
    const auto crlf = loreforge::parser::PlainTextParser::parse(
        QByteArrayLiteral("Chapter 1\r\nFirst line\r\nsecond line\r\n\r\n***\r\n"), options());

    QVERIFY(std::holds_alternative<loreforge::document::Document>(lf));
    QVERIFY(std::holds_alternative<loreforge::document::Document>(crlf));
    QCOMPARE(blockTexts(parsedDocument(lf)), blockTexts(parsedDocument(crlf)));
    QCOMPARE(parsedDocument(lf).chapters.first().blocks.at(1).text, u"First line second line"_s);
}

void PlainTextParserTest::createsOneChapterWhenNoHeadingExists() {
    const auto result = loreforge::parser::PlainTextParser::parse(
        QByteArrayLiteral("First line\nsecond line\n"), options());
    QVERIFY(std::holds_alternative<loreforge::document::Document>(result));

    const auto& chapters = parsedDocument(result).chapters;
    QCOMPARE(chapters.size(), 1);
    QCOMPARE(chapters.first().title, u"Test Novel"_s);
    QCOMPARE(chapters.first().blocks.first().text, u"First line second line"_s);
}

void PlainTextParserTest::retainsConsecutiveHeadingsAsEmptyChapters() {
    const auto result = loreforge::parser::PlainTextParser::parse(
        QByteArrayLiteral("Chapter 1\nChapter 2\nBody\n"), options());
    QVERIFY(std::holds_alternative<loreforge::document::Document>(result));

    const auto& chapters = parsedDocument(result).chapters;
    QCOMPARE(chapters.size(), 2);
    QCOMPARE(chapters.at(0).blocks.size(), 1);
    QCOMPARE(chapters.at(1).blocks.size(), 2);
}

void PlainTextParserTest::importsALargeChapter() {
    QByteArray source = QByteArrayLiteral("Chapter 1\n");
    constexpr qsizetype wordCount = 50'000;
    for (qsizetype index = 0; index < wordCount; ++index) {
        source.append("word ");
    }

    const auto result = loreforge::parser::PlainTextParser::parse(source, options());
    QVERIFY(std::holds_alternative<loreforge::document::Document>(result));
    const auto& paragraph = parsedDocument(result).chapters.first().blocks.at(1);
    QCOMPARE(loreforge::text::WordCounter::count(paragraph.text), wordCount);
}

void PlainTextParserTest::rejectsInvalidUtf8() {
    QByteArray source = QByteArrayLiteral("Chapter 1\n");
    source.append(static_cast<char>(0xC3));
    source.append('(');
    const auto result = loreforge::parser::PlainTextParser::parse(source, options());
    QVERIFY(std::holds_alternative<loreforge::parser::PlainTextParseError>(result));
    QCOMPARE(std::get<loreforge::parser::PlainTextParseError>(result).code,
             loreforge::parser::PlainTextParseErrorCode::InvalidUtf8);

    source.chop(1);
    const auto truncatedResult = loreforge::parser::PlainTextParser::parse(source, options());
    QVERIFY(std::holds_alternative<loreforge::parser::PlainTextParseError>(truncatedResult));
    QCOMPARE(std::get<loreforge::parser::PlainTextParseError>(truncatedResult).code,
             loreforge::parser::PlainTextParseErrorCode::InvalidUtf8);
}

void PlainTextParserTest::rejectsWhitespaceOnlyInput() {
    const auto result =
        loreforge::parser::PlainTextParser::parse(QByteArrayLiteral(" \t\r\n\r\n"), options());
    QVERIFY(std::holds_alternative<loreforge::parser::PlainTextParseError>(result));
    QCOMPARE(std::get<loreforge::parser::PlainTextParseError>(result).code,
             loreforge::parser::PlainTextParseErrorCode::EmptyInput);
}

QTEST_APPLESS_MAIN(PlainTextParserTest)

#include "plain_text_parser_test.moc"
