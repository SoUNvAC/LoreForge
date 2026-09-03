#include "document_fixture.h"

#include "loreforge/core/content_hash.h"
#include "loreforge/document/document_json_codec.h"

#include <QDir>
#include <QFile>
#include <QtTest>

class DocumentFixtureTest final : public QObject {
    Q_OBJECT

  private slots:
    void sourceFixtureHasFrozenHash();
    void goldenJsonDecodesToExpectedDocument();
};

void DocumentFixtureTest::sourceFixtureHasFrozenHash() {
    QFile source(QDir(QStringLiteral(LOREFORGE_TEST_FIXTURES_DIR))
                     .filePath(QStringLiteral("handcrafted.txt")));
    QVERIFY2(source.open(QIODevice::ReadOnly), qPrintable(source.errorString()));
    const auto bytes = source.readAll();

    QCOMPARE(bytes, loreforge::test::handcraftedSource());
    QCOMPARE(loreforge::core::ContentHash::sha256(QByteArrayView(bytes)).toHex(),
             QStringLiteral("752edf1bd9d7de62eb35651e65c0fa636fe57a893f404e0eba6b80a843933e62"));
}

void DocumentFixtureTest::goldenJsonDecodesToExpectedDocument() {
    QFile fixture(QDir(QStringLiteral(LOREFORGE_TEST_FIXTURES_DIR))
                      .filePath(QStringLiteral("document_v1.json")));
    QVERIFY2(fixture.open(QIODevice::ReadOnly), qPrintable(fixture.errorString()));

    const auto decoded = loreforge::document::DocumentJsonCodec::decode(fixture.readAll());
    QVERIFY(std::holds_alternative<loreforge::document::Document>(decoded));
    QCOMPARE(std::get<loreforge::document::Document>(decoded),
             loreforge::test::handcraftedDocument());
}

QTEST_APPLESS_MAIN(DocumentFixtureTest)

#include "document_fixture_test.moc"
