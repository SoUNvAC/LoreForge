#include "loreforge/core/content_hash.h"
#include "loreforge/document/document.h"
#include "loreforge/parser/pdf_parser.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

#include <variant>

using namespace Qt::StringLiterals;

class PdfParserTest final : public QObject {
    Q_OBJECT

  private slots:
    void importsSimplePdfAgainstGoldenFixture();
    void restoresTwoColumnReadingOrderAgainstGoldenFixture();
    void failsVisiblyForAmbiguousLayout();
    void appliesManualReadingOrderAndChapterCorrection();
    void skipsAnExplicitNonNarrativePage();
    void failsVisiblyWhenOcrIsRequired();
    void rejectsInvalidCorrectionsAndInvalidPdfBytes();
};

namespace {

QString testRoot() {
    return QString::fromUtf8(LOREFORGE_TEST_SOURCE_DIR);
}

QString fixturePath(QStringView name) {
    return testRoot() + QStringLiteral("/fixtures/pdf/") + name.toString() + QStringLiteral(".pdf");
}

QByteArray fileBytes(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

loreforge::parser::PdfImportOptions options(QStringView name) {
    return {QStringLiteral("fixtures/") + name.toString() + QStringLiteral(".pdf"),
            {},
            {},
            u"en"_s,
            {}};
}

const loreforge::document::Document& documentFrom(const loreforge::parser::PdfParseResult& result) {
    return std::get<loreforge::document::Document>(result);
}

QJsonObject projection(const loreforge::document::Document& document) {
    QJsonArray chapters;
    for (const auto& chapter : document.chapters) {
        QJsonArray blocks;
        for (const auto& block : chapter.blocks) {
            blocks.append(QJsonObject{
                {QStringLiteral("confidence"), block.extractionConfidence.value_or(-1.0)},
                {QStringLiteral("source"), block.sourceSpan.sourceId},
                {QStringLiteral("text"), block.text},
                {QStringLiteral("type"), loreforge::document::blockTypeToString(block.type)},
            });
        }
        chapters.append(QJsonObject{
            {QStringLiteral("blocks"), blocks},
            {QStringLiteral("title"), chapter.title},
        });
    }
    return {
        {QStringLiteral("authors"), QJsonArray::fromStringList(document.metadata.authors)},
        {QStringLiteral("chapters"), chapters},
        {QStringLiteral("language"), document.metadata.language},
        {QStringLiteral("title"), document.metadata.title},
    };
}

void compareWithGolden(QStringView name, const loreforge::document::Document& document) {
    QJsonParseError parseError;
    const auto expected =
        QJsonDocument::fromJson(fileBytes(testRoot() + QStringLiteral("/golden/pdf/") +
                                          name.toString() + QStringLiteral(".json")),
                                &parseError);
    QCOMPARE(parseError.error, QJsonParseError::NoError);
    QCOMPARE(QJsonDocument(projection(document)), expected);
}

} // namespace

void PdfParserTest::importsSimplePdfAgainstGoldenFixture() {
    const auto source = fileBytes(fixturePath(u"simple"));
    const auto result = loreforge::parser::PdfParser::parse(source, options(u"simple"));
    QVERIFY(std::holds_alternative<loreforge::document::Document>(result));
    const auto& document = documentFrom(result);
    QCOMPARE(document.metadata.sourceHash,
             loreforge::core::ContentHash::sha256(QByteArrayView(source)));
    compareWithGolden(u"simple", document);

    for (const auto& chapter : document.chapters) {
        for (const auto& block : chapter.blocks) {
            QVERIFY(block.sourceSpan.isValid());
            QVERIFY(block.sourceSpan.sourceId.contains(QStringLiteral("&layer=extracted-text")));
            QVERIFY(block.extractionConfidence.has_value());
        }
    }
}

void PdfParserTest::restoresTwoColumnReadingOrderAgainstGoldenFixture() {
    const auto result = loreforge::parser::PdfParser::parse(fileBytes(fixturePath(u"two_column")),
                                                            options(u"two_column"));
    QVERIFY(std::holds_alternative<loreforge::document::Document>(result));
    compareWithGolden(u"two_column", documentFrom(result));
}

void PdfParserTest::failsVisiblyForAmbiguousLayout() {
    const auto result = loreforge::parser::PdfParser::parse(fileBytes(fixturePath(u"ambiguous")),
                                                            options(u"ambiguous"));
    QVERIFY(std::holds_alternative<loreforge::parser::PdfParseError>(result));
    const auto& parseError = std::get<loreforge::parser::PdfParseError>(result);
    QCOMPARE(parseError.code, loreforge::parser::PdfParseErrorCode::AmbiguousReadingOrder);
    QCOMPARE(parseError.pageIndex, 0);
}

void PdfParserTest::appliesManualReadingOrderAndChapterCorrection() {
    auto importOptions = options(u"ambiguous");
    importOptions.pageCorrections.append(
        {0, loreforge::parser::PdfReadingOrder::SingleColumn, u"Curated Chapter"_s, false});
    const auto result =
        loreforge::parser::PdfParser::parse(fileBytes(fixturePath(u"ambiguous")), importOptions);
    QVERIFY(std::holds_alternative<loreforge::document::Document>(result));
    const auto& document = documentFrom(result);
    QCOMPARE(document.chapters.size(), 1);
    QCOMPARE(document.chapters.first().title, u"Curated Chapter"_s);
    QCOMPARE(document.chapters.first().blocks.first().text, u"Chapter 1"_s);
    QCOMPARE(document.chapters.first().blocks.first().extractionConfidence, 0.98);
}

void PdfParserTest::skipsAnExplicitNonNarrativePage() {
    auto importOptions = options(u"simple");
    importOptions.pageCorrections.append({1, loreforge::parser::PdfReadingOrder::Auto, {}, true});
    const auto result =
        loreforge::parser::PdfParser::parse(fileBytes(fixturePath(u"simple")), importOptions);
    QVERIFY(std::holds_alternative<loreforge::document::Document>(result));
    const auto& document = documentFrom(result);
    QCOMPARE(document.chapters.size(), 1);
    QCOMPARE(document.chapters.first().title, u"Chapter 1"_s);
}

void PdfParserTest::failsVisiblyWhenOcrIsRequired() {
    const auto result = loreforge::parser::PdfParser::parse(fileBytes(fixturePath(u"scanned")),
                                                            options(u"scanned"));
    QVERIFY(std::holds_alternative<loreforge::parser::PdfParseError>(result));
    const auto& parseError = std::get<loreforge::parser::PdfParseError>(result);
    QCOMPARE(parseError.code, loreforge::parser::PdfParseErrorCode::NoExtractableText);
    QCOMPARE(parseError.pageIndex, 0);
}

void PdfParserTest::rejectsInvalidCorrectionsAndInvalidPdfBytes() {
    auto importOptions = options(u"simple");
    importOptions.pageCorrections.append({8, loreforge::parser::PdfReadingOrder::Auto, {}, false});
    const auto invalidCorrection =
        loreforge::parser::PdfParser::parse(fileBytes(fixturePath(u"simple")), importOptions);
    QVERIFY(std::holds_alternative<loreforge::parser::PdfParseError>(invalidCorrection));
    QCOMPARE(std::get<loreforge::parser::PdfParseError>(invalidCorrection).code,
             loreforge::parser::PdfParseErrorCode::InvalidCorrection);

    importOptions.pageCorrections.clear();
    importOptions.pageCorrections.append(
        {0, loreforge::parser::PdfReadingOrder::SingleColumn, {}, true});
    const auto conflictingCorrection =
        loreforge::parser::PdfParser::parse(fileBytes(fixturePath(u"simple")), importOptions);
    QVERIFY(std::holds_alternative<loreforge::parser::PdfParseError>(conflictingCorrection));
    QCOMPARE(std::get<loreforge::parser::PdfParseError>(conflictingCorrection).code,
             loreforge::parser::PdfParseErrorCode::InvalidCorrection);

    const auto invalidPdf =
        loreforge::parser::PdfParser::parse(QByteArrayLiteral("not a pdf"), options(u"invalid"));
    QVERIFY(std::holds_alternative<loreforge::parser::PdfParseError>(invalidPdf));
    QCOMPARE(std::get<loreforge::parser::PdfParseError>(invalidPdf).code,
             loreforge::parser::PdfParseErrorCode::InvalidPdf);
}

QTEST_GUILESS_MAIN(PdfParserTest)

#include "pdf_parser_test.moc"
