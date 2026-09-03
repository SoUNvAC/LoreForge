#include "document_fixture.h"

#include "loreforge/document/document_hash.h"
#include "loreforge/document/document_validation.h"

#include <QtTest>

class DocumentModelTest final : public QObject {
    Q_OBJECT

  private slots:
    void handcraftedDocumentIsValid();
    void hashIsStableAndSensitiveToContent();
    void rejectsInvalidChapterOrdering();
    void rejectsDuplicateChapterIds();
    void rejectsOverlappingSourceSpans();
    void rejectsSourceSpansOverlappingAcrossChapters();
    void allowsAnEmptyChapter();
};

void DocumentModelTest::handcraftedDocumentIsValid() {
    const auto document = loreforge::test::handcraftedDocument();
    QVERIFY(loreforge::document::validateDocument(document).isValid());
}

void DocumentModelTest::hashIsStableAndSensitiveToContent() {
    auto document = loreforge::test::handcraftedDocument();
    const auto original = loreforge::document::computeContentHash(document);

    QCOMPARE(original, loreforge::document::computeContentHash(document));
    QCOMPARE(original.toHex(),
             QStringLiteral("9f1202426fa961233d640381c5d915fce56cc4258c1c7fc6ccf8ebfe3077f0c7"));
    document.chapters[0].blocks[1].text = QStringLiteral("Hello, changed world.");
    QVERIFY(original != loreforge::document::computeContentHash(document));
}

void DocumentModelTest::rejectsInvalidChapterOrdering() {
    auto document = loreforge::test::handcraftedDocument();
    document.chapters[1].index = 3;

    const auto result = loreforge::document::validateDocument(document);
    QVERIFY(!result.isValid());
    QCOMPARE(result.errors.last().code,
             loreforge::document::DocumentValidationCode::InvalidChapterOrder);
}

void DocumentModelTest::rejectsDuplicateChapterIds() {
    auto document = loreforge::test::handcraftedDocument();
    document.chapters[1].id = document.chapters[0].id;

    const auto result = loreforge::document::validateDocument(document);
    QVERIFY(!result.isValid());
    QCOMPARE(result.errors.last().code,
             loreforge::document::DocumentValidationCode::DuplicateChapterId);
}

void DocumentModelTest::rejectsOverlappingSourceSpans() {
    auto document = loreforge::test::handcraftedDocument();
    document.chapters[0].blocks[1].sourceSpan.startByte = 10;

    const auto result = loreforge::document::validateDocument(document);
    QVERIFY(!result.isValid());
    QCOMPARE(result.errors.last().code,
             loreforge::document::DocumentValidationCode::OverlappingSourceSpan);
}

void DocumentModelTest::rejectsSourceSpansOverlappingAcrossChapters() {
    auto document = loreforge::test::handcraftedDocument();
    document.chapters[1].blocks[0].sourceSpan = {QStringLiteral("fixtures/handcrafted.txt"), 24,
                                                 41};

    const auto result = loreforge::document::validateDocument(document);
    QVERIFY(!result.isValid());
    QCOMPARE(result.errors.last().code,
             loreforge::document::DocumentValidationCode::OverlappingSourceSpan);
}

void DocumentModelTest::allowsAnEmptyChapter() {
    auto document = loreforge::test::handcraftedDocument();
    document.chapters[1].blocks.clear();

    QVERIFY(loreforge::document::validateDocument(document).isValid());
}

QTEST_APPLESS_MAIN(DocumentModelTest)

#include "document_model_test.moc"
