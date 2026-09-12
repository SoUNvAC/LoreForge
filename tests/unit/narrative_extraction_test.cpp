#include "loreforge/narrative/dialogue_extractor.h"
#include "loreforge/narrative/extraction_evaluator.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

using namespace Qt::StringLiterals;

class NarrativeExtractionTest final : public QObject {
    Q_OBJECT

  private slots:
    void extractsGoldenChaptersWithoutRewritingSource();
    void rejectsMissingAndOverlappingCoverage();
    void rejectsRewrittenTextAndInvalidAttribution();
    void rejectsBoundariesInsideUtf8Characters();
    void reportsGoldenEvaluationMetrics();
};

namespace {

struct GoldenSample final {
    loreforge::core::ChapterId chapterId;
    loreforge::core::SourceSpan chapterSpan;
    QByteArray source;
    QJsonObject output;
};

QList<GoldenSample> goldenSamples() {
    QFile fixture(QDir(QStringLiteral(LOREFORGE_TEST_FIXTURES_DIR))
                      .filePath(QStringLiteral("dialogue_samples.json")));
    if (!fixture.open(QIODevice::ReadOnly)) {
        return {};
    }
    const auto document = QJsonDocument::fromJson(fixture.readAll());
    QList<GoldenSample> samples;
    for (const auto& value : document.object().value(u"samples"_s).toArray()) {
        const auto sample = value.toObject();
        const auto source = sample.value(u"source"_s).toString().toUtf8();
        const auto chapterId =
            loreforge::core::ChapterId::fromStableKey(sample.value(u"chapter_key"_s).toString());
        const auto sourceId = sample.value(u"source_id"_s).toString();
        const auto chapterStart = sample.value(u"chapter_start"_s).toInteger();
        samples.append({
            chapterId,
            {sourceId, chapterStart, chapterStart + source.size()},
            source,
            QJsonObject{{u"chapter_id"_s, chapterId.toString()},
                        {u"segments"_s, sample.value(u"segments"_s).toArray()}},
        });
    }
    return samples;
}

GoldenSample goldenSample() {
    const auto samples = goldenSamples();
    return samples.isEmpty() ? GoldenSample{} : samples.first();
}

loreforge::narrative::ChapterSegmentation extractGolden() {
    const auto sample = goldenSample();
    const auto result = loreforge::narrative::DialogueExtractor::extract(
        sample.chapterId, sample.chapterSpan, sample.source, sample.output);
    return result.extraction.value_or(loreforge::narrative::ChapterSegmentation{});
}

bool containsError(const loreforge::narrative::DialogueExtractionResult& result,
                   loreforge::narrative::ExtractionErrorCode code) {
    for (const auto& error : result.errors) {
        if (error.code == code) {
            return true;
        }
    }
    return false;
}

} // namespace

void NarrativeExtractionTest::extractsGoldenChaptersWithoutRewritingSource() {
    const auto samples = goldenSamples();
    QCOMPARE(samples.size(), 2);
    QList<loreforge::narrative::ChapterSegmentation> extractions;
    for (const auto& sample : samples) {
        QVERIFY(sample.chapterId.isValid());
        const auto result = loreforge::narrative::DialogueExtractor::extract(
            sample.chapterId, sample.chapterSpan, sample.source, sample.output);
        QVERIFY2(result.isValid(),
                 qPrintable(result.errors.isEmpty() ? QString{} : result.errors.first().message));
        QCOMPARE(result.extraction->reconstructedSource(), sample.source);
        extractions.append(*result.extraction);
    }

    const auto& extraction = extractions.first();
    QCOMPARE(samples.first().source.size(), 78);
    QCOMPARE(extraction.segments.size(), 7);
    const QList<loreforge::narrative::SegmentType> expectedTypes{
        loreforge::narrative::SegmentType::Narration, loreforge::narrative::SegmentType::Dialogue,
        loreforge::narrative::SegmentType::Narration, loreforge::narrative::SegmentType::Dialogue,
        loreforge::narrative::SegmentType::Narration, loreforge::narrative::SegmentType::Dialogue,
        loreforge::narrative::SegmentType::Narration,
    };
    for (qsizetype index = 0; index < expectedTypes.size(); ++index) {
        QCOMPARE(extraction.segments.at(index).type, expectedTypes.at(index));
    }
    QCOMPARE(extraction.segments.at(1).type, loreforge::narrative::SegmentType::Dialogue);
    QCOMPARE(extraction.segments.at(1).text, uR"("Are you there?")"_s);
    QCOMPARE(extraction.segments.at(1).speaker, std::optional<QString>{u"Mara"_s});
    QCOMPARE(extraction.segments.at(3).speaker, std::optional<QString>{u"Ivo"_s});
    QVERIFY(!extraction.segments.at(5).speaker.has_value());
    QVERIFY(!extraction.segments.first().speaker.has_value());
    QCOMPARE(extraction.segments.at(5).confidence, 0.72);
    QCOMPARE(extraction.segments.at(5).sourceSpan.startByte, 1093);
    QCOMPARE(extraction.segments.at(5).sourceSpan.endByte, 1108);

    const auto& unicodeExtraction = extractions.at(1);
    QCOMPARE(samples.at(1).source.size(), 29);
    QCOMPARE(unicodeExtraction.segments.at(1).text, u"“走吧。”"_s);
    QCOMPARE(unicodeExtraction.segments.at(1).type, loreforge::narrative::SegmentType::Dialogue);
    QVERIFY(!unicodeExtraction.segments.at(1).speaker.has_value());

    auto omittedSpeakerOutput = samples.first().output;
    auto omittedSpeakerSegments = omittedSpeakerOutput.value(u"segments"_s).toArray();
    auto unknownDialogue = omittedSpeakerSegments.at(5).toObject();
    unknownDialogue.remove(u"speaker"_s);
    omittedSpeakerSegments.replace(5, unknownDialogue);
    omittedSpeakerOutput.insert(u"segments"_s, omittedSpeakerSegments);
    const auto omittedSpeaker = loreforge::narrative::DialogueExtractor::extract(
        samples.first().chapterId, samples.first().chapterSpan, samples.first().source,
        omittedSpeakerOutput);
    QVERIFY(omittedSpeaker.isValid());
    QVERIFY(!omittedSpeaker.extraction->segments.at(5).speaker.has_value());
}

void NarrativeExtractionTest::rejectsMissingAndOverlappingCoverage() {
    const auto sample = goldenSample();
    auto gapOutput = sample.output;
    auto gapSegments = gapOutput.value(u"segments"_s).toArray();
    auto second = gapSegments.at(1).toObject();
    second.insert(u"source_start"_s, second.value(u"source_start"_s).toInteger() + 1);
    gapSegments.replace(1, second);
    gapOutput.insert(u"segments"_s, gapSegments);
    const auto gap = loreforge::narrative::DialogueExtractor::extract(
        sample.chapterId, sample.chapterSpan, sample.source, gapOutput);
    QVERIFY(!gap.isValid());
    QVERIFY(containsError(gap, loreforge::narrative::ExtractionErrorCode::CoverageGap));

    auto overlapOutput = sample.output;
    auto overlapSegments = overlapOutput.value(u"segments"_s).toArray();
    second = overlapSegments.at(1).toObject();
    second.insert(u"source_start"_s, second.value(u"source_start"_s).toInteger() - 1);
    overlapSegments.replace(1, second);
    overlapOutput.insert(u"segments"_s, overlapSegments);
    const auto overlap = loreforge::narrative::DialogueExtractor::extract(
        sample.chapterId, sample.chapterSpan, sample.source, overlapOutput);
    QVERIFY(!overlap.isValid());
    QVERIFY(containsError(overlap, loreforge::narrative::ExtractionErrorCode::CoverageOverlap));
}

void NarrativeExtractionTest::rejectsRewrittenTextAndInvalidAttribution() {
    const auto sample = goldenSample();
    auto rewrittenOutput = sample.output;
    auto rewrittenSegments = rewrittenOutput.value(u"segments"_s).toArray();
    auto dialogue = rewrittenSegments.at(1).toObject();
    dialogue.insert(u"text"_s, u"A rewritten line"_s);
    rewrittenSegments.replace(1, dialogue);
    rewrittenOutput.insert(u"segments"_s, rewrittenSegments);
    const auto rewritten = loreforge::narrative::DialogueExtractor::extract(
        sample.chapterId, sample.chapterSpan, sample.source, rewrittenOutput);
    QVERIFY(!rewritten.isValid());
    QVERIFY(containsError(rewritten, loreforge::narrative::ExtractionErrorCode::SchemaViolation));

    auto speakerOutput = sample.output;
    auto speakerSegments = speakerOutput.value(u"segments"_s).toArray();
    auto narration = speakerSegments.first().toObject();
    narration.insert(u"speaker"_s, u"Mara"_s);
    speakerSegments.replace(0, narration);
    speakerOutput.insert(u"segments"_s, speakerSegments);
    const auto invalidSpeaker = loreforge::narrative::DialogueExtractor::extract(
        sample.chapterId, sample.chapterSpan, sample.source, speakerOutput);
    QVERIFY(!invalidSpeaker.isValid());
    QVERIFY(
        containsError(invalidSpeaker, loreforge::narrative::ExtractionErrorCode::InvalidSpeaker));

    auto confidenceOutput = sample.output;
    auto confidenceSegments = confidenceOutput.value(u"segments"_s).toArray();
    narration = confidenceSegments.first().toObject();
    narration.insert(u"confidence"_s, 1.1);
    confidenceSegments.replace(0, narration);
    confidenceOutput.insert(u"segments"_s, confidenceSegments);
    const auto invalidConfidence = loreforge::narrative::DialogueExtractor::extract(
        sample.chapterId, sample.chapterSpan, sample.source, confidenceOutput);
    QVERIFY(!invalidConfidence.isValid());
    QVERIFY(containsError(invalidConfidence,
                          loreforge::narrative::ExtractionErrorCode::InvalidConfidence));

    auto wrongChapterOutput = sample.output;
    wrongChapterOutput.insert(
        u"chapter_id"_s, loreforge::core::ChapterId::fromStableKey(u"wrong-chapter"_s).toString());
    const auto wrongChapter = loreforge::narrative::DialogueExtractor::extract(
        sample.chapterId, sample.chapterSpan, sample.source, wrongChapterOutput);
    QVERIFY(!wrongChapter.isValid());
    QVERIFY(
        containsError(wrongChapter, loreforge::narrative::ExtractionErrorCode::ChapterMismatch));
}

void NarrativeExtractionTest::rejectsBoundariesInsideUtf8Characters() {
    const auto chapterId = loreforge::core::ChapterId::fromStableKey(u"utf8-boundary"_s);
    const auto source = QStringLiteral("你说").toUtf8();
    const loreforge::core::SourceSpan span{u"fixtures/dialogue/utf8.txt"_s, 0, source.size()};
    const QJsonObject output{
        {u"chapter_id"_s, chapterId.toString()},
        {u"segments"_s,
         QJsonArray{
             QJsonObject{{u"type"_s, u"narration"_s},
                         {u"source_start"_s, 0},
                         {u"source_end"_s, 1},
                         {u"confidence"_s, 1.0}},
             QJsonObject{{u"type"_s, u"narration"_s},
                         {u"source_start"_s, 1},
                         {u"source_end"_s, source.size()},
                         {u"confidence"_s, 1.0}},
         }},
    };
    const auto result =
        loreforge::narrative::DialogueExtractor::extract(chapterId, span, source, output);
    QVERIFY(!result.isValid());
    QVERIFY(containsError(result, loreforge::narrative::ExtractionErrorCode::InvalidUtf8));
}

void NarrativeExtractionTest::reportsGoldenEvaluationMetrics() {
    const auto expected = extractGolden();
    QVERIFY(expected.chapterId.isValid());
    const auto perfect = loreforge::narrative::ExtractionEvaluator::compare(expected, expected);
    QCOMPARE(perfect.segmentCoverage, 1.0);
    QCOMPARE(perfect.boundaryCorrectness, 1.0);
    QCOMPARE(perfect.speakerCorrectness, 1.0);
    QCOMPARE(perfect.unknownSpeakerHandling, 1.0);

    auto incomplete = expected;
    incomplete.segments.removeLast();
    incomplete.segments.removeLast();
    const auto incompleteMetrics =
        loreforge::narrative::ExtractionEvaluator::compare(expected, incomplete);
    QVERIFY(incompleteMetrics.segmentCoverage < 1.0);
    QVERIFY(incompleteMetrics.boundaryCorrectness < 1.0);
    QCOMPARE(incompleteMetrics.unknownSpeakerHandling, 0.0);

    auto wrongSpeaker = expected;
    wrongSpeaker.segments[1].speaker = u"Ivo"_s;
    const auto speakerMetrics =
        loreforge::narrative::ExtractionEvaluator::compare(expected, wrongSpeaker);
    QCOMPARE(speakerMetrics.speakerCorrectness, 2.0 / 3.0);
    QCOMPARE(speakerMetrics.unknownSpeakerHandling, 1.0);

    auto wrongChapter = expected;
    wrongChapter.chapterId = loreforge::core::ChapterId::fromStableKey(u"another-chapter"_s);
    QCOMPARE(loreforge::narrative::ExtractionEvaluator::compare(expected, wrongChapter),
             loreforge::narrative::ExtractionMetrics{});
}

QTEST_GUILESS_MAIN(NarrativeExtractionTest)

#include "narrative_extraction_test.moc"
