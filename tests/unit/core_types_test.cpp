#include "loreforge/core/content_hash.h"
#include "loreforge/core/identifier.h"
#include "loreforge/core/source_span.h"

#include <QtTest>

using namespace Qt::StringLiterals;

class CoreTypesTest final : public QObject {
    Q_OBJECT

  private slots:
    void stableIdentifiersAreDeterministicAndTyped();
    void identifierParsingRejectsMalformedValues();
    void contentHashMatchesSha256();
    void sourceSpanUsesHalfOpenUtf8ByteRanges();
};

void CoreTypesTest::stableIdentifiersAreDeterministicAndTyped() {
    const auto first = loreforge::core::BookId::fromStableKey(u"fixture/book.txt"_s);
    const auto repeated = loreforge::core::BookId::fromStableKey(u"fixture/book.txt"_s);
    const auto other = loreforge::core::BookId::fromStableKey(u"fixture/other.txt"_s);

    QCOMPARE(first, repeated);
    QVERIFY(first != other);
    QVERIFY(first.toString().startsWith(u"book_"_s));
    QCOMPARE(first.toString().size(), 37);
    QCOMPARE(first.toString(), u"book_3a37d7fcbc792450008ee1fc6d723cba"_s);

    const auto chapter = loreforge::core::ChapterId::fromStableKey(u"fixture/book.txt"_s);
    QVERIFY(chapter.toString().startsWith(u"chapter_"_s));
}

void CoreTypesTest::identifierParsingRejectsMalformedValues() {
    const auto book = loreforge::core::BookId::fromStableKey(u"fixture/book.txt"_s);

    QCOMPARE(loreforge::core::BookId::fromString(book.toString()), book);
    QVERIFY(!loreforge::core::BookId::fromString(u"chapter_0123456789abcdef0123456789abcdef"));
    QVERIFY(!loreforge::core::BookId::fromString(u"book_ABCDEF0123456789ABCDEF0123456789"));
    QVERIFY(!loreforge::core::BookId::fromString(u"book_short"));
}

void CoreTypesTest::contentHashMatchesSha256() {
    const auto hash = loreforge::core::ContentHash::sha256(u"LoreForge");
    QCOMPARE(hash.toHex(), u"f1702348f93eadeee5b30668843a7aab2955d836ca1f74b5321a80aa953ad9dd"_s);
    QCOMPARE(loreforge::core::ContentHash::fromHex(hash.toHex()), hash);
    QVERIFY(!loreforge::core::ContentHash::fromHex(u"not-a-hash"));
}

void CoreTypesTest::sourceSpanUsesHalfOpenUtf8ByteRanges() {
    const loreforge::core::SourceSpan first{u"novel.txt"_s, 4, 10};
    const loreforge::core::SourceSpan touching{u"novel.txt"_s, 10, 14};
    const loreforge::core::SourceSpan overlapping{u"novel.txt"_s, 9, 12};
    const loreforge::core::SourceSpan otherSource{u"other.txt"_s, 9, 12};

    QVERIFY(first.isValid());
    QCOMPARE(first.lengthBytes(), 6);
    QVERIFY(first.contains(4));
    QVERIFY(first.contains(9));
    QVERIFY(!first.contains(10));
    QVERIFY(!first.overlaps(touching));
    QVERIFY(first.overlaps(overlapping));
    QVERIFY(!first.overlaps(otherSource));

    const loreforge::core::SourceSpan invalid{u"novel.txt"_s, 10, 4};
    QVERIFY(!invalid.isValid());
    QVERIFY(!invalid.contains(7));
}

QTEST_APPLESS_MAIN(CoreTypesTest)

#include "core_types_test.moc"
