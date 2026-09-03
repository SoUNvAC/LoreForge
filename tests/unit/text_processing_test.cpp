#include "loreforge/text/chapter_heading_detector.h"
#include "loreforge/text/paragraph_detector.h"
#include "loreforge/text/text_normalizer.h"
#include "loreforge/text/word_counter.h"

#include <QtTest>

using namespace Qt::StringLiterals;

class TextProcessingTest final : public QObject {
    Q_OBJECT

  private slots:
    void normalizesBomLineEndingsAndUnicode();
    void detectsParagraphBoundariesAndSceneBreaks();
    void detectsEnglishAndChineseChapterHeadings();
    void rejectsOrdinaryLinesAsHeadings();
    void countsLatinWordsAndCjkIdeographs();
};

void TextProcessingTest::normalizesBomLineEndingsAndUnicode() {
    const QString source = QString(QChar::ByteOrderMark) + u"Cafe\u0301\r\nNext\rLast"_s;
    QCOMPARE(loreforge::text::TextNormalizer::normalize(source), u"Caf\u00e9\nNext\nLast"_s);
}

void TextProcessingTest::detectsParagraphBoundariesAndSceneBreaks() {
    QVERIFY(loreforge::text::ParagraphDetector::isBlank(u" \t"));
    QVERIFY(loreforge::text::ParagraphDetector::isSceneBreak(u"* * *"));
    QVERIFY(loreforge::text::ParagraphDetector::isSceneBreak(u"---"));
    QVERIFY(!loreforge::text::ParagraphDetector::isSceneBreak(u"ordinary text"));
    QCOMPARE(
        loreforge::text::ParagraphDetector::joinWrappedLines({u" First line "_s, u"second line"_s}),
        u"First line second line"_s);
}

void TextProcessingTest::detectsEnglishAndChineseChapterHeadings() {
    const auto english = loreforge::text::ChapterHeadingDetector::detect(u"Chapter One");
    const auto roman = loreforge::text::ChapterHeadingDetector::detect(u"CHAPTER IV — Home");
    const auto chinese = loreforge::text::ChapterHeadingDetector::detect(u"第十二章：归来");
    const auto chineseSpaced = loreforge::text::ChapterHeadingDetector::detect(u"第一章 初见");
    const auto prologue = loreforge::text::ChapterHeadingDetector::detect(u"Prologue");
    QVERIFY(english.has_value());
    QVERIFY(roman.has_value());
    QVERIFY(chinese.has_value());
    QVERIFY(chineseSpaced.has_value());
    QVERIFY(prologue.has_value());
    QCOMPARE(*english, u"Chapter One"_s);
    QCOMPARE(*roman, u"CHAPTER IV — Home"_s);
    QCOMPARE(*chinese, u"第十二章：归来"_s);
    QCOMPARE(*chineseSpaced, u"第一章 初见"_s);
    QCOMPARE(*prologue, u"Prologue"_s);
}

void TextProcessingTest::rejectsOrdinaryLinesAsHeadings() {
    QVERIFY(!loreforge::text::ChapterHeadingDetector::detect(u"Chapter house on the hill"));
    QVERIFY(!loreforge::text::ChapterHeadingDetector::detect(u"The first chapter begins"));
    QVERIFY(!loreforge::text::ChapterHeadingDetector::detect(u"第十二天"));
}

void TextProcessingTest::countsLatinWordsAndCjkIdeographs() {
    QCOMPARE(loreforge::text::WordCounter::count(u"Hello, world! It's 2026."), 4);
    QCOMPARE(loreforge::text::WordCounter::count(u"你好，世界"), 4);
    QCOMPARE(loreforge::text::WordCounter::count(u"e\u0301lan vital"), 2);
    QCOMPARE(loreforge::text::WordCounter::count(QStringView{}), 0);
}

QTEST_APPLESS_MAIN(TextProcessingTest)

#include "text_processing_test.moc"
