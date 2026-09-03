#include "document_fixture.h"

#include "loreforge/document/document_json_codec.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

class DocumentJsonCodecTest final : public QObject {
    Q_OBJECT

  private slots:
    void roundTripPreservesDocument();
    void rejectsTamperedContent();
    void rejectsUnsupportedSchema();
    void rejectsInvalidDocumentsBeforeEncoding();
};

void DocumentJsonCodecTest::roundTripPreservesDocument() {
    const auto original = loreforge::test::handcraftedDocument();
    const auto encoded = loreforge::document::DocumentJsonCodec::encode(original);
    QVERIFY(std::holds_alternative<QByteArray>(encoded));

    const auto decoded =
        loreforge::document::DocumentJsonCodec::decode(std::get<QByteArray>(encoded));
    QVERIFY(std::holds_alternative<loreforge::document::Document>(decoded));
    QCOMPARE(std::get<loreforge::document::Document>(decoded), original);
}

void DocumentJsonCodecTest::rejectsTamperedContent() {
    const auto encoded =
        loreforge::document::DocumentJsonCodec::encode(loreforge::test::handcraftedDocument());
    auto json = QJsonDocument::fromJson(std::get<QByteArray>(encoded));
    auto root = json.object();
    auto document = root.value(QStringLiteral("document")).toObject();
    auto chapters = document.value(QStringLiteral("chapters")).toArray();
    auto chapter = chapters[0].toObject();
    auto blocks = chapter.value(QStringLiteral("blocks")).toArray();
    auto block = blocks[1].toObject();
    block.insert(QStringLiteral("text"), QStringLiteral("Tampered"));
    blocks[1] = block;
    chapter.insert(QStringLiteral("blocks"), blocks);
    chapters[0] = chapter;
    document.insert(QStringLiteral("chapters"), chapters);
    root.insert(QStringLiteral("document"), document);

    const auto decoded = loreforge::document::DocumentJsonCodec::decode(
        QJsonDocument(root).toJson(QJsonDocument::Compact));
    QVERIFY(std::holds_alternative<loreforge::document::DocumentJsonError>(decoded));
    QCOMPARE(std::get<loreforge::document::DocumentJsonError>(decoded).code,
             loreforge::document::DocumentJsonErrorCode::ContentHashMismatch);
}

void DocumentJsonCodecTest::rejectsUnsupportedSchema() {
    const auto decoded = loreforge::document::DocumentJsonCodec::decode(
        QByteArrayLiteral(R"({"schema_version":99,"document":{}})"));

    QVERIFY(std::holds_alternative<loreforge::document::DocumentJsonError>(decoded));
    QCOMPARE(std::get<loreforge::document::DocumentJsonError>(decoded).code,
             loreforge::document::DocumentJsonErrorCode::UnsupportedSchema);
}

void DocumentJsonCodecTest::rejectsInvalidDocumentsBeforeEncoding() {
    auto document = loreforge::test::handcraftedDocument();
    document.chapters.clear();

    const auto encoded = loreforge::document::DocumentJsonCodec::encode(document);
    QVERIFY(std::holds_alternative<loreforge::document::DocumentJsonError>(encoded));
    QCOMPARE(std::get<loreforge::document::DocumentJsonError>(encoded).code,
             loreforge::document::DocumentJsonErrorCode::InvalidDocument);
}

QTEST_APPLESS_MAIN(DocumentJsonCodecTest)

#include "document_json_codec_test.moc"
