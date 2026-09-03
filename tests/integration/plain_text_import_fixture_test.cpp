#include "document_fixture.h"

#include "loreforge/document/document_json_codec.h"
#include "loreforge/parser/plain_text_parser.h"

#include <QDir>
#include <QtTest>

#include <variant>

class PlainTextImportFixtureTest final : public QObject {
    Q_OBJECT

  private slots:
    void importsFixtureIntoGoldenDocumentReproducibly();
};

void PlainTextImportFixtureTest::importsFixtureIntoGoldenDocumentReproducibly() {
    const auto fixturePath = QDir(QStringLiteral(LOREFORGE_TEST_FIXTURES_DIR))
                                 .filePath(QStringLiteral("handcrafted.txt"));
    const loreforge::parser::PlainTextImportOptions options{
        QStringLiteral("fixtures/handcrafted.txt"),
        QStringLiteral("fixtures/handcrafted.txt"),
        QStringLiteral("Handcrafted Fixture"),
        {QStringLiteral("LoreForge Tests")},
        QStringLiteral("en"),
    };

    const auto first = loreforge::parser::PlainTextParser::parseFile(fixturePath, options);
    const auto second = loreforge::parser::PlainTextParser::parseFile(fixturePath, options);
    QVERIFY(std::holds_alternative<loreforge::document::Document>(first));
    QVERIFY(std::holds_alternative<loreforge::document::Document>(second));

    const auto& firstDocument = std::get<loreforge::document::Document>(first);
    const auto& secondDocument = std::get<loreforge::document::Document>(second);
    QCOMPARE(firstDocument, loreforge::test::handcraftedDocument());
    QCOMPARE(secondDocument, firstDocument);

    const auto firstJson = loreforge::document::DocumentJsonCodec::encode(firstDocument);
    const auto secondJson = loreforge::document::DocumentJsonCodec::encode(secondDocument);
    QVERIFY(std::holds_alternative<QByteArray>(firstJson));
    QCOMPARE(std::get<QByteArray>(firstJson), std::get<QByteArray>(secondJson));
}

QTEST_APPLESS_MAIN(PlainTextImportFixtureTest)

#include "plain_text_import_fixture_test.moc"
