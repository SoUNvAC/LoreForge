#include "loreforge/git/semantic_diff.h"

#include <QtTest>

using namespace Qt::StringLiterals;
using namespace loreforge;
using namespace loreforge::git;

namespace {

core::ChapterId chapterId(qsizetype sequence) {
    return core::ChapterId::fromStableKey(u"revision:chapter:%1"_s.arg(sequence));
}

SourceRevision revision(const QList<QByteArray>& chapters) {
    SourceRevision result;
    result.sourceId = u"novel.txt"_s;
    for (qsizetype sequence = 0; sequence < chapters.size(); ++sequence) {
        const auto start = result.utf8.size();
        result.utf8 += chapters[sequence];
        result.chapters.append({chapterId(sequence),
                                sequence,
                                {result.sourceId, start, result.utf8.size()},
                                std::nullopt});
    }
    result.sourceHash = core::ContentHash::sha256(QByteArrayView(result.utf8));
    return result;
}

proofreading::RepairQueueItem
approved(const SourceRevision& source, qsizetype sequence, const QString& original,
         const QString& replacement,
         proofreading::CandidateCategory category = proofreading::CandidateCategory::Typo,
         proofreading::SemanticImpact impact = proofreading::SemanticImpact::TextOnly) {
    const auto offset =
        source.utf8.indexOf(original.toUtf8(), source.chapters[sequence].sourceSpan.startByte);
    proofreading::ProofreadingCandidate candidate;
    candidate.id = core::ProofreadingCandidateId::fromStableKey(u"candidate:%1"_s.arg(offset));
    candidate.chapterId = source.chapters[sequence].id;
    candidate.sourceSpan = {source.sourceId, offset, offset + original.toUtf8().size()};
    candidate.originalText = original;
    candidate.suggestedText = replacement;
    candidate.category = category;
    candidate.confidence = 1.0;
    candidate.evidence = u"Human reviewed exact source repair"_s;
    candidate.semanticImpact = impact;
    candidate.sourceHash = source.sourceHash;
    candidate.detectorVersion = u"test-v1"_s;
    const auto now = QDateTime::currentDateTimeUtc();
    const auto queued =
        proofreading::RepairQueueWorkflow::review(core::ProjectId::fromStableKey(u"project"_s),
                                                  candidate, QString::fromUtf8(source.utf8), now);
    const auto resolved = proofreading::RepairQueueWorkflow::approve(
        std::get<proofreading::RepairQueueItem>(queued), now);
    return std::get<proofreading::RepairQueueItem>(resolved);
}

narrative::ChapterAnalysis analysis(const SourceRevision& source, const QString& name,
                                    const QString& eventText = {}) {
    const auto& chapter = source.chapters.first();
    narrative::ChapterAnalysis result;
    result.chapterId = chapter.id;
    result.sourceSpan = chapter.sourceSpan;
    const narrative::ClaimSupport summarySupport{
        narrative::ClaimBasis::Evidence,
        {{chapter.sourceSpan, QString::fromUtf8(source.utf8)}},
        1.0};
    result.summary = {u"A character acts."_s, summarySupport};
    if (!name.isEmpty()) {
        const auto start = source.utf8.indexOf(name.toUtf8());
        const narrative::ClaimSupport nameSupport{
            narrative::ClaimBasis::Evidence,
            {{{source.sourceId, start, start + name.toUtf8().size()}, name}},
            1.0};
        result.characters.append({name, {}, nameSupport});
    }
    if (!eventText.isEmpty()) {
        result.events.append({eventText, {name}, std::nullopt, summarySupport});
    }
    return result;
}

SemanticDiffReport compare(const SourceRevision& before, const SourceRevision& after,
                           const QList<proofreading::RepairQueueItem>& reviews = {}) {
    return std::get<SemanticDiffReport>(SemanticDiffer::compare(before, after, reviews));
}

} // namespace

class SemanticDiffTest final : public QObject {
    Q_OBJECT
  private slots:
    void noChangeHasNoInvalidation();
    void distinguishesReviewedTyposFromUnknownAndSemanticChanges();
    void separatesMultipleRepairsAndTracksShiftedProvenance();
    void detectsEntitiesAndStoryEvents();
    void handlesUtf8InsertionsDeletionsAndDisjointHunks();
    void rejectsStaleReviewsAndInvalidProjections();
    void handlesPartialReviewsAndStructuralChangesConservatively();
    void boundsLargeDiffWork();
};

void SemanticDiffTest::noChangeHasNoInvalidation() {
    const auto source = revision({QByteArray("One.\n"), QByteArray("Two.\n")});
    const auto report = compare(source, source);
    QCOMPARE(report.impact, RevisionImpact::NoChange);
    QVERIFY(report.changedSpans.isEmpty());
    QVERIFY(report.changedChapters.isEmpty());
    QVERIFY(report.invalidation.analysisChapters.isEmpty());
    QVERIFY(!report.invalidation.storyStateSnapshotsDirtyFrom);
}

void SemanticDiffTest::distinguishesReviewedTyposFromUnknownAndSemanticChanges() {
    const auto before = revision({QByteArray("Mara sees teh gate.\n")});
    const auto after = revision({QByteArray("Mara sees the gate.\n")});
    const auto review = approved(before, 0, u"teh"_s, u"the"_s);
    const auto report = compare(before, after, {review});
    QCOMPARE(report.impact, RevisionImpact::TypoOnly);
    QCOMPARE(report.changedChapters.size(), 1);
    QVERIFY(!report.changedChapters.first().storySemanticsMayChange);
    QVERIFY(!report.invalidation.storySemanticsMayChange);
    QCOMPARE(report.invalidation.storyStateSnapshotsDirtyFrom, std::optional<qsizetype>(0));
    QCOMPARE(report.changedSpans.first().candidateId, std::optional(review.candidate.id));
    QCOMPARE(compare(before, after).impact, RevisionImpact::Unknown);

    auto semanticReview = review;
    semanticReview.candidate.semanticImpact = proofreading::SemanticImpact::ActionChange;
    QCOMPARE(compare(before, after, {semanticReview}).impact, RevisionImpact::SemanticChange);
}

void SemanticDiffTest::separatesMultipleRepairsAndTracksShiftedProvenance() {
    const auto before = revision(
        {QByteArray("He walk.\n"), QByteArray("Unchanged middle.\n"), QByteArray("teh end.\n")});
    const auto after = revision(
        {QByteArray("He walks.\n"), QByteArray("Unchanged middle.\n"), QByteArray("the end.\n")});
    const auto report = compare(
        before, after,
        {approved(before, 0, u"walk"_s, u"walks"_s), approved(before, 2, u"teh"_s, u"the"_s)});
    QCOMPARE(report.impact, RevisionImpact::TypoOnly);
    QCOMPARE(report.changedSpans.size(), 2);
    QCOMPARE(report.changedSpans[1].after.startByte, report.changedSpans[1].before.startByte + 1);
    QCOMPARE(report.changedChapters.size(), 2);
    QVERIFY(report.invalidation.provenanceChapters.contains(chapterId(1)));
    QVERIFY(report.invalidation.analysisChapters.contains(chapterId(1)));
    QVERIFY(!report.invalidation.requiresChapterRemap);
    QVERIFY(!report.invalidation.storySemanticsMayChange);
}

void SemanticDiffTest::detectsEntitiesAndStoryEvents() {
    auto before = revision({QByteArray("Mara opens the gate.\n")});
    auto after = revision({QByteArray("Nara closes the gate.\n")});
    before.chapters[0].analysis = analysis(before, u"Mara"_s, u"Mara opens the gate"_s);
    after.chapters[0].analysis = analysis(after, u"Nara"_s, u"Nara closes the gate"_s);
    const auto report = compare(before, after);
    QCOMPARE(report.impact, RevisionImpact::SemanticChange);
    QVERIFY(report.changedChapters.first().affectedEntities.contains(u"Mara"_s));
    QVERIFY(report.changedChapters.first().affectedEntities.contains(u"Nara"_s));
    QVERIFY(report.invalidation.storySemanticsMayChange);
    QVERIFY(!report.changedChapters.first().entityImpactUncertain);

    const auto namedAfter = revision({QByteArray("Nara opens the gate.\n")});
    const auto nameReview = approved(before, 0, u"Mara"_s, u"Nara"_s,
                                     proofreading::CandidateCategory::NameInconsistency);
    QCOMPARE(compare(before, namedAfter, {nameReview}).impact, RevisionImpact::SemanticChange);
    const auto mislabeled = approved(before, 0, u"Mara"_s, u"Nara"_s);
    QCOMPARE(compare(before, namedAfter, {mislabeled}).impact, RevisionImpact::SemanticChange);
}

void SemanticDiffTest::handlesUtf8InsertionsDeletionsAndDisjointHunks() {
    const auto before =
        revision({u"她来了。\n"_s.toUtf8(), QByteArray("Middle.\n"), QByteArray("Last.\n")});
    const auto after =
        revision({u"他来了。\n"_s.toUtf8(), QByteArray("Middle.\n"), QByteArray("New last.\n")});
    const auto report = compare(before, after);
    QCOMPARE(report.changedSpans.size(), 2);
    QCOMPARE(report.changedSpans[0].removedUtf8, u"她"_s.toUtf8());
    QCOMPARE(report.changedSpans[0].addedUtf8, u"他"_s.toUtf8());
    QCOMPARE(report.changedChapters.size(), 2);
    QVERIFY(!report.invalidation.analysisChapters.contains(chapterId(1)));
    const auto empty = revision({QByteArray{}});
    const auto text = revision({QByteArray("New\n")});
    QCOMPARE(compare(empty, text).changedSpans.first().before.lengthBytes(), 0);
    QCOMPARE(compare(text, empty).changedSpans.first().after.lengthBytes(), 0);
}

void SemanticDiffTest::rejectsStaleReviewsAndInvalidProjections() {
    const auto before = revision({u"她走了。\n"_s.toUtf8()});
    const auto after = revision({u"他走了。\n"_s.toUtf8()});
    auto bad = before;
    bad.sourceHash = after.sourceHash;
    QVERIFY(std::holds_alternative<GitError>(SemanticDiffer::compare(bad, after)));
    bad = before;
    bad.chapters[0].sourceSpan.startByte = 1;
    QVERIFY(std::holds_alternative<GitError>(SemanticDiffer::compare(bad, after)));
    auto review = approved(before, 0, u"她"_s, u"他"_s);
    review.status = proofreading::CandidateStatus::Rejected;
    QVERIFY(std::holds_alternative<GitError>(SemanticDiffer::compare(before, after, {review})));
    review.status = proofreading::CandidateStatus::Approved;
    review.candidate.sourceHash = after.sourceHash;
    QVERIFY(std::holds_alternative<GitError>(SemanticDiffer::compare(before, after, {review})));
    bad = before;
    bad.utf8 = QByteArray("\xff");
    bad.sourceHash = core::ContentHash::sha256(QByteArrayView(bad.utf8));
    QVERIFY(std::holds_alternative<GitError>(SemanticDiffer::compare(bad, after)));
}

void SemanticDiffTest::handlesPartialReviewsAndStructuralChangesConservatively() {
    const auto before = revision({QByteArray("teh first.\n"), QByteArray("Last.\n")});
    const auto after = revision({QByteArray("the first.\n"), QByteArray("Deleted event.\n")});
    const auto report = compare(before, after, {approved(before, 0, u"teh"_s, u"the"_s)});
    QCOMPARE(report.impact, RevisionImpact::Unknown);
    QVERIFY(report.invalidation.storySemanticsMayChange);
    auto removed = revision({QByteArray("teh first.\n")});
    QCOMPARE(compare(before, removed).impact, RevisionImpact::SemanticChange);
    QVERIFY(compare(before, removed).invalidation.requiresChapterRemap);
    auto unmappedBefore = before;
    auto unmappedAfter = after;
    unmappedBefore.chapters.clear();
    unmappedAfter.chapters.clear();
    QVERIFY(compare(unmappedBefore, unmappedAfter).invalidation.requiresChapterRemap);
    QCOMPARE(compare(unmappedBefore, unmappedAfter).invalidation.storyStateSnapshotsDirtyFrom,
             std::optional<qsizetype>(0));
    auto remapped = before;
    remapped.chapters[0].sourceSpan.endByte -= 1;
    remapped.chapters[1].sourceSpan.startByte -= 1;
    const auto remappedReport = compare(before, remapped);
    QVERIFY(remappedReport.invalidation.requiresChapterRemap);
    QCOMPARE(remappedReport.impact, RevisionImpact::Unknown);
}

void SemanticDiffTest::boundsLargeDiffWork() {
    QByteArray before;
    for (int i = 0; i < 1100; ++i) {
        before += QByteArray::number(i) + "\n";
    }
    auto after = before;
    after.replace(0, 1, "x");
    const auto report = compare(revision({before}), revision({after}));
    QVERIFY(report.conservativeSpanFallback);
    QCOMPARE(report.impact, RevisionImpact::Unknown);
}

QTEST_GUILESS_MAIN(SemanticDiffTest)
#include "semantic_diff_test.moc"
