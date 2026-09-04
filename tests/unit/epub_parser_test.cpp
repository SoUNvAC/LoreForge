#include "loreforge/core/content_hash.h"
#include "loreforge/document/document.h"
#include "loreforge/parser/epub_parser.h"

#include <QBuffer>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtCore/private/qzipwriter_p.h>
#include <QtTest>

#include <variant>

using namespace Qt::StringLiterals;

class EpubParserTest final : public QObject {
    Q_OBJECT

  private slots:
    void importsStructuredEpubAgainstGoldenFixture();
    void importsWithoutNavigationAgainstGoldenFixture();
    void importsEpub2NcxAgainstGoldenFixture();
    void rejectsUnsafeArchivePaths();
    void rejectsMalformedXhtmlAtTheParserBoundary();
    void requiresAnExactMimetypeEntry();
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

QByteArray createArchive(const QList<std::pair<QString, QByteArray>>& files) {
    QByteArray archive;
    QBuffer buffer(&archive);
    if (!buffer.open(QIODevice::WriteOnly)) {
        return {};
    }
    QZipWriter writer(&buffer);
    writer.setCompressionPolicy(QZipWriter::NeverCompress);
    for (const auto& [path, bytes] : files) {
        writer.addFile(path, bytes);
        writer.setCompressionPolicy(QZipWriter::AutoCompress);
    }
    writer.close();
    return archive;
}

QByteArray packFixture(QStringView name) {
    const auto root = QDir(testRoot() + QStringLiteral("/fixtures/epub/") + name.toString());
    QList<std::pair<QString, QByteArray>> files;
    files.append({QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip")});

    QStringList paths;
    QDirIterator iterator(root.absolutePath(), QDir::Files, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const auto absolutePath = iterator.next();
        const auto relativePath =
            root.relativeFilePath(absolutePath).replace(QLatin1Char('\\'), QLatin1Char('/'));
        if (relativePath != QStringLiteral("mimetype")) {
            paths.append(relativePath);
        }
    }
    paths.sort(Qt::CaseSensitive);
    for (const auto& path : paths) {
        files.append({path, fileBytes(root.filePath(path))});
    }
    return createArchive(files);
}

const loreforge::document::Document&
documentFrom(const loreforge::parser::EpubParseResult& result) {
    return std::get<loreforge::document::Document>(result);
}

QJsonObject projection(const loreforge::document::Document& document) {
    QJsonArray chapters;
    for (const auto& chapter : document.chapters) {
        QJsonArray blocks;
        for (const auto& block : chapter.blocks) {
            const auto marker = block.sourceSpan.sourceId.lastIndexOf(QStringLiteral("!/"));
            blocks.append(QJsonObject{
                {QStringLiteral("source"), block.sourceSpan.sourceId.mid(marker + 2)},
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
    const auto goldenPath =
        testRoot() + QStringLiteral("/golden/epub/") + name.toString() + QStringLiteral(".json");
    QJsonParseError parseError;
    const auto expected = QJsonDocument::fromJson(fileBytes(goldenPath), &parseError);
    QCOMPARE(parseError.error, QJsonParseError::NoError);
    QCOMPARE(QJsonDocument(projection(document)), expected);
}

loreforge::parser::EpubImportOptions options(QStringView fixture) {
    return {QStringLiteral("fixtures/") + fixture.toString() + QStringLiteral(".epub"), {}, {}, {}};
}

void verifyProvenance(const loreforge::document::Document& document) {
    for (const auto& chapter : document.chapters) {
        for (const auto& block : chapter.blocks) {
            QVERIFY2(block.sourceSpan.isValid(), "Every EPUB block must retain a valid byte span.");
            QVERIFY(block.sourceSpan.sourceId.startsWith(document.metadata.sourceLocator +
                                                         QStringLiteral("!/")));
            QVERIFY(block.sourceSpan.endByte > block.sourceSpan.startByte);
        }
    }
}

} // namespace

void EpubParserTest::importsStructuredEpubAgainstGoldenFixture() {
    const auto source = packFixture(u"structured");
    const auto result = loreforge::parser::EpubParser::parse(source, options(u"structured"));
    QVERIFY(std::holds_alternative<loreforge::document::Document>(result));

    const auto& document = documentFrom(result);
    QCOMPARE(document.metadata.sourceHash,
             loreforge::core::ContentHash::sha256(QByteArrayView(source)));
    compareWithGolden(u"structured", document);
    verifyProvenance(document);

    const auto repeated = loreforge::parser::EpubParser::parse(source, options(u"structured"));
    QVERIFY(std::holds_alternative<loreforge::document::Document>(repeated));
    QCOMPARE(document, documentFrom(repeated));
}

void EpubParserTest::importsWithoutNavigationAgainstGoldenFixture() {
    const auto source = packFixture(u"no_toc");
    const auto result = loreforge::parser::EpubParser::parse(source, options(u"no_toc"));
    QVERIFY(std::holds_alternative<loreforge::document::Document>(result));
    compareWithGolden(u"no_toc", documentFrom(result));
    verifyProvenance(documentFrom(result));
}

void EpubParserTest::importsEpub2NcxAgainstGoldenFixture() {
    const auto source = packFixture(u"epub2");
    const auto result = loreforge::parser::EpubParser::parse(source, options(u"epub2"));
    QVERIFY(std::holds_alternative<loreforge::document::Document>(result));
    compareWithGolden(u"epub2", documentFrom(result));
    verifyProvenance(documentFrom(result));
}

void EpubParserTest::rejectsUnsafeArchivePaths() {
    auto source = createArchive({
        {QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip")},
        {QStringLiteral("EPUB/outside.xhtml"), QByteArrayLiteral("unsafe")},
    });
    QCOMPARE(source.replace("EPUB/outside.xhtml", "C:/a/outside.xhtml").count("C:/a/outside.xhtml"),
             2);
    const auto result = loreforge::parser::EpubParser::parse(source, options(u"unsafe"));
    QVERIFY(std::holds_alternative<loreforge::parser::EpubParseError>(result));
    QCOMPARE(std::get<loreforge::parser::EpubParseError>(result).code,
             loreforge::parser::EpubParseErrorCode::UnsafeArchive);
}

void EpubParserTest::rejectsMalformedXhtmlAtTheParserBoundary() {
    const auto source = createArchive({
        {QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip")},
        {QStringLiteral("META-INF/container.xml"),
         fileBytes(testRoot() + QStringLiteral("/fixtures/epub/no_toc/META-INF/container.xml"))},
        {QStringLiteral("book.opf"),
         fileBytes(testRoot() + QStringLiteral("/fixtures/epub/no_toc/book.opf"))},
        {QStringLiteral("one.xhtml"), QByteArrayLiteral("<html><body><p>broken</body></html>")},
        {QStringLiteral("two.xhtml"),
         fileBytes(testRoot() + QStringLiteral("/fixtures/epub/no_toc/two.xhtml"))},
    });
    const auto result = loreforge::parser::EpubParser::parse(source, options(u"malformed"));
    QVERIFY(std::holds_alternative<loreforge::parser::EpubParseError>(result));
    QCOMPARE(std::get<loreforge::parser::EpubParseError>(result).code,
             loreforge::parser::EpubParseErrorCode::InvalidXhtml);
}

void EpubParserTest::requiresAnExactMimetypeEntry() {
    const auto source = createArchive({
        {QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip\n")},
    });
    const auto result = loreforge::parser::EpubParser::parse(source, options(u"bad-mimetype"));
    QVERIFY(std::holds_alternative<loreforge::parser::EpubParseError>(result));
    QCOMPARE(std::get<loreforge::parser::EpubParseError>(result).code,
             loreforge::parser::EpubParseErrorCode::InvalidContainer);
}

QTEST_APPLESS_MAIN(EpubParserTest)

#include "epub_parser_test.moc"
