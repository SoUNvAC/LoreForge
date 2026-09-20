#include "loreforge/narrative/chapter_analyzer.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

using namespace Qt::StringLiterals;

class ChapterAnalysisTest final : public QObject {
    Q_OBJECT

  private slots:
    void analyzesGoldenChapterWithSourceEvidence();
    void rejectsUngroundedDirectClaims();
    void rejectsModelSuppliedEvidenceText();
    void rejectsInvalidEvidenceBoundaries();
    void rejectsDuplicateNamesAndWrongChapter();
    void rejectsDuplicateEvidenceAndInvalidConfidence();
};

namespace {

struct GoldenAnalysisSample final {
    loreforge::core::ChapterId chapterId;
    loreforge::core::SourceSpan chapterSpan;
    QByteArray source;
    QJsonObject output;
};

GoldenAnalysisSample goldenSample() {
    QFile fixture(QDir(QStringLiteral(LOREFORGE_TEST_FIXTURES_DIR))
                      .filePath(QStringLiteral("chapter_analysis_samples.json")));
    if (!fixture.open(QIODevice::ReadOnly)) {
        return {};
    }
    const auto document = QJsonDocument::fromJson(fixture.readAll());
    const auto sample = document.object().value(u"samples"_s).toArray().first().toObject();
    const auto chapterId =
        loreforge::core::ChapterId::fromStableKey(sample.value(u"chapter_key"_s).toString());
    const auto source = sample.value(u"source"_s).toString().toUtf8();
    const auto chapterStart = sample.value(u"chapter_start"_s).toInteger();
    auto output = sample.value(u"output"_s).toObject();
    output.insert(u"chapter_id"_s, chapterId.toString());
    return {
        chapterId,
        {sample.value(u"source_id"_s).toString(), chapterStart, chapterStart + source.size()},
        source,
        output,
    };
}

bool containsError(const loreforge::narrative::ChapterAnalysisResult& result,
                   loreforge::narrative::ChapterAnalysisErrorCode code) {
    for (const auto& error : result.errors) {
        if (error.code == code) {
            return true;
        }
    }
    return false;
}

QJsonObject minimalOutput(const loreforge::core::ChapterId& chapterId, qint64 evidenceStart,
                          qint64 evidenceEnd) {
    return {
        {u"chapter_id"_s, chapterId.toString()},
        {u"characters"_s, QJsonArray{}},
        {u"locations"_s, QJsonArray{}},
        {u"events"_s, QJsonArray{}},
        {u"summary"_s,
         QJsonObject{
             {u"text"_s, u"A minimal summary."_s},
             {u"inferred"_s, false},
             {u"confidence"_s, 1.0},
             {u"evidence"_s, QJsonArray{QJsonObject{{u"source_start"_s, evidenceStart},
                                                    {u"source_end"_s, evidenceEnd}}}},
         }},
        {u"important_facts"_s, QJsonArray{}},
        {u"open_threads"_s, QJsonArray{}},
    };
}

} // namespace

void ChapterAnalysisTest::analyzesGoldenChapterWithSourceEvidence() {
    const auto sample = goldenSample();
    QVERIFY(sample.chapterId.isValid());
    QCOMPARE(sample.source.size(), 162);
    const auto result = loreforge::narrative::ChapterAnalyzer::analyze(
        sample.chapterId, sample.chapterSpan, sample.source, sample.output);
    QVERIFY2(result.isValid(),
             qPrintable(result.errors.isEmpty() ? QString{} : result.errors.first().message));

    const auto& analysis = *result.analysis;
    QCOMPARE(analysis.characters.size(), 2);
    QCOMPARE(analysis.characters.first().name, u"Mara"_s);
    QCOMPARE(analysis.characters.first().support.basis, loreforge::narrative::ClaimBasis::Evidence);
    QCOMPARE(analysis.characters.first().support.evidence.first().text, u"Mara"_s);
    QCOMPARE(analysis.characters.first().aliases.first().name, u"She"_s);
    QCOMPARE(analysis.characters.first().aliases.first().support.basis,
             loreforge::narrative::ClaimBasis::Inference);
    QCOMPARE(analysis.locations.at(1).name, u"Starwatch"_s);
    QCOMPARE(analysis.events.first().location, std::optional<QString>{u"Starwatch"_s});
    QCOMPARE(analysis.events.first().support.evidence.first().text,
             u"A red light began blinking above the sealed door."_s);
    QCOMPARE(analysis.summary.support.evidence.size(), 2);
    QCOMPARE(analysis.importantFacts.first().support.evidence.first().text, u"sealed door"_s);
    QCOMPARE(analysis.openThreads.size(), 2);
    QVERIFY(analysis.openThreads.first().support.isGrounded());
    QCOMPARE(analysis.openThreads.at(1).support.basis, loreforge::narrative::ClaimBasis::Inference);
    QVERIFY(analysis.openThreads.at(1).support.evidence.isEmpty());
    QVERIFY(analysis.openThreads.at(1).support.isGrounded());
}

void ChapterAnalysisTest::rejectsUngroundedDirectClaims() {
    const auto sample = goldenSample();
    auto output = sample.output;
    auto facts = output.value(u"important_facts"_s).toArray();
    auto fact = facts.first().toObject();
    fact.insert(u"evidence"_s, QJsonArray{});
    facts.replace(0, fact);
    output.insert(u"important_facts"_s, facts);
    const auto result = loreforge::narrative::ChapterAnalyzer::analyze(
        sample.chapterId, sample.chapterSpan, sample.source, output);
    QVERIFY(!result.isValid());
    QVERIFY(containsError(result, loreforge::narrative::ChapterAnalysisErrorCode::UngroundedClaim));
}

void ChapterAnalysisTest::rejectsModelSuppliedEvidenceText() {
    const auto sample = goldenSample();
    auto output = sample.output;
    auto characters = output.value(u"characters"_s).toArray();
    auto character = characters.first().toObject();
    auto evidence = character.value(u"evidence"_s).toArray();
    auto span = evidence.first().toObject();
    span.insert(u"text"_s, u"A fabricated quote"_s);
    evidence.replace(0, span);
    character.insert(u"evidence"_s, evidence);
    characters.replace(0, character);
    output.insert(u"characters"_s, characters);
    const auto result = loreforge::narrative::ChapterAnalyzer::analyze(
        sample.chapterId, sample.chapterSpan, sample.source, output);
    QVERIFY(!result.isValid());
    QVERIFY(containsError(result, loreforge::narrative::ChapterAnalysisErrorCode::SchemaViolation));
}

void ChapterAnalysisTest::rejectsInvalidEvidenceBoundaries() {
    const auto chapterId = loreforge::core::ChapterId::fromStableKey(u"analysis-utf8"_s);
    const auto source = QStringLiteral("你").toUtf8();
    const loreforge::core::SourceSpan span{u"fixtures/analysis/utf8.txt"_s, 100, 103};
    const auto output = minimalOutput(chapterId, 100, 101);
    const auto result =
        loreforge::narrative::ChapterAnalyzer::analyze(chapterId, span, source, output);
    QVERIFY(!result.isValid());
    QVERIFY(containsError(result, loreforge::narrative::ChapterAnalysisErrorCode::InvalidEvidence));

    const auto sample = goldenSample();
    auto outsideOutput = sample.output;
    auto summary = outsideOutput.value(u"summary"_s).toObject();
    summary.insert(u"evidence"_s,
                   QJsonArray{QJsonObject{{u"source_start"_s, 3999}, {u"source_end"_s, 4004}}});
    outsideOutput.insert(u"summary"_s, summary);
    const auto outside = loreforge::narrative::ChapterAnalyzer::analyze(
        sample.chapterId, sample.chapterSpan, sample.source, outsideOutput);
    QVERIFY(!outside.isValid());
    QVERIFY(
        containsError(outside, loreforge::narrative::ChapterAnalysisErrorCode::InvalidEvidence));
}

void ChapterAnalysisTest::rejectsDuplicateNamesAndWrongChapter() {
    const auto sample = goldenSample();
    auto duplicateOutput = sample.output;
    auto characters = duplicateOutput.value(u"characters"_s).toArray();
    auto duplicate = characters.first().toObject();
    duplicate.insert(u"name"_s, u"mara"_s);
    duplicate.insert(u"aliases"_s, QJsonArray{});
    characters.append(duplicate);
    duplicateOutput.insert(u"characters"_s, characters);
    const auto duplicateResult = loreforge::narrative::ChapterAnalyzer::analyze(
        sample.chapterId, sample.chapterSpan, sample.source, duplicateOutput);
    QVERIFY(!duplicateResult.isValid());
    QVERIFY(containsError(duplicateResult,
                          loreforge::narrative::ChapterAnalysisErrorCode::DuplicateName));

    auto wrongChapterOutput = sample.output;
    wrongChapterOutput.insert(
        u"chapter_id"_s,
        loreforge::core::ChapterId::fromStableKey(u"another-analysis-chapter"_s).toString());
    const auto wrongChapter = loreforge::narrative::ChapterAnalyzer::analyze(
        sample.chapterId, sample.chapterSpan, sample.source, wrongChapterOutput);
    QVERIFY(!wrongChapter.isValid());
    QVERIFY(containsError(wrongChapter,
                          loreforge::narrative::ChapterAnalysisErrorCode::ChapterMismatch));
}

void ChapterAnalysisTest::rejectsDuplicateEvidenceAndInvalidConfidence() {
    const auto sample = goldenSample();
    auto duplicateOutput = sample.output;
    auto summary = duplicateOutput.value(u"summary"_s).toObject();
    auto evidence = summary.value(u"evidence"_s).toArray();
    evidence.append(evidence.first());
    summary.insert(u"evidence"_s, evidence);
    duplicateOutput.insert(u"summary"_s, summary);
    const auto duplicateResult = loreforge::narrative::ChapterAnalyzer::analyze(
        sample.chapterId, sample.chapterSpan, sample.source, duplicateOutput);
    QVERIFY(!duplicateResult.isValid());
    QVERIFY(containsError(duplicateResult,
                          loreforge::narrative::ChapterAnalysisErrorCode::DuplicateEvidence));

    auto confidenceOutput = sample.output;
    auto facts = confidenceOutput.value(u"important_facts"_s).toArray();
    auto fact = facts.first().toObject();
    fact.insert(u"confidence"_s, 1.01);
    facts.replace(0, fact);
    confidenceOutput.insert(u"important_facts"_s, facts);
    const auto confidenceResult = loreforge::narrative::ChapterAnalyzer::analyze(
        sample.chapterId, sample.chapterSpan, sample.source, confidenceOutput);
    QVERIFY(!confidenceResult.isValid());
    QVERIFY(containsError(confidenceResult,
                          loreforge::narrative::ChapterAnalysisErrorCode::InvalidConfidence));
}

QTEST_GUILESS_MAIN(ChapterAnalysisTest)

#include "chapter_analysis_test.moc"
