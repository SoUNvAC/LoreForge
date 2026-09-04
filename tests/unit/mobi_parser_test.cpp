#include "loreforge/core/content_hash.h"
#include "loreforge/document/document.h"
#include "loreforge/parser/mobi_parser.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>
#include <QtTest>

#include <variant>

using namespace Qt::StringLiterals;

class MobiParserTest final : public QObject {
    Q_OBJECT

  private slots:
    void importsUtf8MobiAcrossRecordBoundaryAgainstGoldenFixture();
    void importsPalmDocCompressedCp1252AgainstGoldenFixture();
    void expandsPalmDocBackReferences();
    void stripsDeclaredTextRecordTrailers();
    void rejectsEncryptedBooksVisibly();
    void rejectsHuffCdicCompressionVisibly();
    void rejectsKf8OnlyBooksVisibly();
    void rejectsCorruptRecordOffsets();
    void rejectsCorruptPalmDocBackReferences();
    void requiresSourceLocator();
};

namespace {

QString testRoot() {
    return QString::fromUtf8(LOREFORGE_TEST_SOURCE_DIR);
}

QByteArray fileBytes(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

QString fixturePath(QStringView name) {
    return testRoot() + QStringLiteral("/fixtures/mobi/") + name.toString() +
           QStringLiteral(".mobi");
}

loreforge::parser::MobiImportOptions options(QStringView fixture) {
    return {QStringLiteral("fixtures/") + fixture.toString() + QStringLiteral(".mobi"), {}, {}, {}};
}

const loreforge::document::Document&
documentFrom(const loreforge::parser::MobiParseResult& result) {
    return std::get<loreforge::document::Document>(result);
}

QJsonObject projection(const loreforge::document::Document& document) {
    QJsonArray chapters;
    for (const auto& chapter : document.chapters) {
        QJsonArray blocks;
        for (const auto& block : chapter.blocks) {
            blocks.append(QJsonObject{
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
        {QStringLiteral("format"), document.metadata.sourceFormat},
        {QStringLiteral("language"), document.metadata.language},
        {QStringLiteral("title"), document.metadata.title},
    };
}

void compareWithGolden(QStringView name, const loreforge::document::Document& document) {
    const auto path =
        testRoot() + QStringLiteral("/golden/mobi/") + name.toString() + QStringLiteral(".json");
    QJsonParseError parseError;
    const auto expected = QJsonDocument::fromJson(fileBytes(path), &parseError);
    QCOMPARE(parseError.error, QJsonParseError::NoError);
    QCOMPARE(QJsonDocument(projection(document)), expected);
}

void verifyProvenance(const loreforge::document::Document& document) {
    qint64 lastEnd = -1;
    for (const auto& chapter : document.chapters) {
        for (const auto& block : chapter.blocks) {
            QVERIFY(block.sourceSpan.isValid());
            QCOMPARE(block.sourceSpan.sourceId,
                     document.metadata.sourceLocator +
                         QStringLiteral("#text&layer=palmdoc-decoded-utf8"));
            QVERIFY(block.sourceSpan.startByte >= lastEnd);
            QVERIFY(block.sourceSpan.endByte > block.sourceSpan.startByte);
            lastEnd = block.sourceSpan.endByte;
        }
    }
}

void compareError(QStringView fixture, loreforge::parser::MobiParseErrorCode expected) {
    const auto source = fileBytes(fixturePath(fixture));
    const auto result = loreforge::parser::MobiParser::parse(source, options(fixture));
    QVERIFY(std::holds_alternative<loreforge::parser::MobiParseError>(result));
    QCOMPARE(std::get<loreforge::parser::MobiParseError>(result).code, expected);
    QVERIFY(!std::get<loreforge::parser::MobiParseError>(result).message.isEmpty());
}

} // namespace

void MobiParserTest::importsUtf8MobiAcrossRecordBoundaryAgainstGoldenFixture() {
    const auto source = fileBytes(fixturePath(u"utf8_uncompressed"));
    const auto result = loreforge::parser::MobiParser::parse(source, options(u"utf8_uncompressed"));
    QVERIFY2(std::holds_alternative<loreforge::document::Document>(result),
             std::get_if<loreforge::parser::MobiParseError>(&result)
                 ? qPrintable(std::get<loreforge::parser::MobiParseError>(result).message)
                 : "Unexpected parse result");
    const auto& document = documentFrom(result);
    QCOMPARE(document.metadata.sourceHash,
             loreforge::core::ContentHash::sha256(QByteArrayView(source)));
    compareWithGolden(u"utf8_uncompressed", document);
    verifyProvenance(document);

    const auto repeated =
        loreforge::parser::MobiParser::parse(source, options(u"utf8_uncompressed"));
    QVERIFY(std::holds_alternative<loreforge::document::Document>(repeated));
    QCOMPARE(document, documentFrom(repeated));
}

void MobiParserTest::importsPalmDocCompressedCp1252AgainstGoldenFixture() {
    const auto source = fileBytes(fixturePath(u"cp1252_palmdoc"));
    const auto result = loreforge::parser::MobiParser::parse(source, options(u"cp1252_palmdoc"));
    QVERIFY2(std::holds_alternative<loreforge::document::Document>(result),
             std::get_if<loreforge::parser::MobiParseError>(&result)
                 ? qPrintable(std::get<loreforge::parser::MobiParseError>(result).message)
                 : "Unexpected parse result");
    compareWithGolden(u"cp1252_palmdoc", documentFrom(result));
    verifyProvenance(documentFrom(result));
}

void MobiParserTest::expandsPalmDocBackReferences() {
    const auto source = fileBytes(fixturePath(u"palmdoc_backref"));
    const auto result = loreforge::parser::MobiParser::parse(source, options(u"palmdoc_backref"));
    QVERIFY(std::holds_alternative<loreforge::document::Document>(result));
    const auto& document = documentFrom(result);
    QCOMPARE(document.chapters.size(), 1);
    QCOMPARE(document.chapters.first().blocks.first().text, QStringLiteral("repeat repeat"));
}

void MobiParserTest::stripsDeclaredTextRecordTrailers() {
    const auto source = fileBytes(fixturePath(u"record_trailer"));
    const auto result = loreforge::parser::MobiParser::parse(source, options(u"record_trailer"));
    QVERIFY(std::holds_alternative<loreforge::document::Document>(result));
    QCOMPARE(documentFrom(result).chapters.first().blocks.first().text,
             QStringLiteral("Trailer removed."));
}

void MobiParserTest::rejectsEncryptedBooksVisibly() {
    compareError(u"encrypted", loreforge::parser::MobiParseErrorCode::EncryptedDocument);
}

void MobiParserTest::rejectsHuffCdicCompressionVisibly() {
    compareError(u"huff_cdic", loreforge::parser::MobiParseErrorCode::UnsupportedCompression);
}

void MobiParserTest::rejectsKf8OnlyBooksVisibly() {
    compareError(u"kf8_only", loreforge::parser::MobiParseErrorCode::UnsupportedFormat);
}

void MobiParserTest::rejectsCorruptRecordOffsets() {
    auto source = fileBytes(fixturePath(u"utf8_uncompressed"));
    QVERIFY(source.size() > 94);
    const auto firstOffset =
        qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(source.constData() + 78));
    qToBigEndian(firstOffset, reinterpret_cast<uchar*>(source.data() + 86));
    const auto result = loreforge::parser::MobiParser::parse(source, options(u"corrupt"));
    QVERIFY(std::holds_alternative<loreforge::parser::MobiParseError>(result));
    QCOMPARE(std::get<loreforge::parser::MobiParseError>(result).code,
             loreforge::parser::MobiParseErrorCode::InvalidContainer);
}

void MobiParserTest::rejectsCorruptPalmDocBackReferences() {
    compareError(u"corrupt_backref", loreforge::parser::MobiParseErrorCode::CorruptText);
}

void MobiParserTest::requiresSourceLocator() {
    const auto source = fileBytes(fixturePath(u"utf8_uncompressed"));
    const auto result = loreforge::parser::MobiParser::parse(source, {});
    QVERIFY(std::holds_alternative<loreforge::parser::MobiParseError>(result));
    QCOMPARE(std::get<loreforge::parser::MobiParseError>(result).code,
             loreforge::parser::MobiParseErrorCode::MissingSourceLocator);
}

QTEST_APPLESS_MAIN(MobiParserTest)

#include "mobi_parser_test.moc"
