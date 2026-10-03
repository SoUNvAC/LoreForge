#include "loreforge/inference/output_validator.h"
#include "loreforge/proofreading/deterministic_proofreader.h"
#include "loreforge/proofreading/protected_term_registry.h"
#include "loreforge/proofreading/repair_queue.h"
#include "loreforge/proofreading/semantic_proofreader.h"

#include <QJsonArray>
#include <QtTest>

#include <algorithm>

using namespace Qt::StringLiterals;

class ProofreadingEngineTest final : public QObject {
    Q_OBJECT

  private slots:
    void detectsDeterministicCandidatesWithoutMutation();
    void respectsProtectedTerms();
    void rejectsSourcesWithoutExactUtf8Provenance();
    void validatesSemanticCandidatesAgainstSource();
    void rejectsSemanticChangesToProtectedTermsAndBrokenBoundaries();
    void requiresHumanApprovalOrExplicitManualAuthorization();
};

namespace {

loreforge::proofreading::ProofreadingSource source(QString text, qint64 start = 100) {
    const auto bytes = text.toUtf8();
    return {
        loreforge::core::ChapterId::fromStableKey(u"proofreading:chapter:0"_s),
        {u"novel/chapter-0.txt"_s, start, start + bytes.size()},
        bytes,
        loreforge::core::ContentHash::sha256(bytes),
    };
}

loreforge::proofreading::ProofreadingCandidate
candidateFor(const loreforge::proofreading::ProofreadingSource& input) {
    return {
        loreforge::core::ProofreadingCandidateId::fromStableKey(u"repair:candidate:0"_s),
        input.chapterId,
        {input.sourceSpan.sourceId, input.sourceSpan.startByte, input.sourceSpan.startByte + 4},
        u"walk"_s,
        u"walks"_s,
        loreforge::proofreading::CandidateCategory::Typo,
        0.88,
        u"The verb form disagrees with the subject."_s,
        loreforge::proofreading::SemanticImpact::ActionChange,
        loreforge::proofreading::CandidateOrigin::Semantic,
        u"semantic-proofreader-v1"_s,
        input.sourceHash,
    };
}

loreforge::proofreading::ProofreadingPolicy policy() {
    return {
        {{u"LoreForge"_s, {u"Lore Forge"_s}, true}},
        {{u"Sacred  Word!!"_s, {}, u"Intentional ritual spelling."_s, std::nullopt},
         {u"Mara"_s, {}, u"Canonical character name."_s, std::nullopt}},
        {u"Mara"_s},
        2,
    };
}

bool hasCategory(const QList<loreforge::proofreading::ProofreadingCandidate>& candidates,
                 loreforge::proofreading::CandidateCategory category) {
    return std::any_of(candidates.cbegin(), candidates.cend(), [category](const auto& candidate) {
        return candidate.category == category;
    });
}

bool hasError(const loreforge::proofreading::ProofreadingResult& result,
              loreforge::proofreading::ProofreadingErrorCode code) {
    return std::any_of(result.errors.cbegin(), result.errors.cend(),
                       [code](const auto& error) { return error.code == code; });
}

QJsonObject semanticOutput(const loreforge::proofreading::ProofreadingSource& input, qint64 start,
                           qint64 end, QString suggestion) {
    return {
        {u"chapter_id"_s, input.chapterId.toString()},
        {u"candidates"_s, QJsonArray{QJsonObject{
                              {u"source_start"_s, start},
                              {u"source_end"_s, end},
                              {u"suggested_text"_s, std::move(suggestion)},
                              {u"category"_s, u"TYPO"_s},
                              {u"confidence"_s, 0.88},
                              {u"evidence"_s, u"The verb form disagrees with the subject."_s},
                              {u"semantic_impact"_s, u"ACTION_CHANGE"_s},
                          }}},
    };
}

} // namespace

void ProofreadingEngineTest::detectsDeterministicCandidatesWithoutMutation() {
    const auto input = source(u"Mara went  home!!\n"
                              "Very very quiet.\n"
                              "Lore Forge arrived.\n"
                              "Mara waited.\n"
                              "Mara waited.\n"
                              "Mera spoke to Lore Forge.\n"
                              "Sacred  Word!!"_s);
    const auto originalBytes = input.utf8;
    const auto first =
        loreforge::proofreading::DeterministicProofreader::analyze({input}, policy());
    QVERIFY2(first.isValid(),
             qPrintable(first.errors.isEmpty() ? QString{} : first.errors.first().message));
    const auto second =
        loreforge::proofreading::DeterministicProofreader::analyze({input}, policy());
    QVERIFY(second.isValid());
    QCOMPARE(first.report, second.report);
    QCOMPARE(input.utf8, originalBytes);

    const auto& candidates = first.report->candidates;
    QVERIFY(hasCategory(candidates, loreforge::proofreading::CandidateCategory::DuplicatedText));
    QVERIFY(hasCategory(candidates, loreforge::proofreading::CandidateCategory::Spacing));
    QVERIFY(hasCategory(candidates, loreforge::proofreading::CandidateCategory::Punctuation));
    QVERIFY(hasCategory(candidates,
                        loreforge::proofreading::CandidateCategory::TerminologyInconsistency));
    QVERIFY(hasCategory(candidates, loreforge::proofreading::CandidateCategory::NameInconsistency));
    QCOMPARE(first.report->nameFrequencies.value(u"Mara"_s), 3);
    QCOMPARE(first.report->sourceHash, input.sourceHash);
    const auto repeatedPunctuation =
        std::find_if(candidates.cbegin(), candidates.cend(), [](const auto& candidate) {
            return candidate.category == loreforge::proofreading::CandidateCategory::Punctuation &&
                   candidate.originalText == QStringLiteral("!!");
        });
    QVERIFY(repeatedPunctuation != candidates.cend());
    QCOMPARE(repeatedPunctuation->suggestedText, u"!"_s);

    for (const auto& candidate : candidates) {
        QVERIFY(candidate.id.isValid());
        QVERIFY(candidate.id.toString().startsWith(u"candidate_"_s));
        QCOMPARE(candidate.origin, loreforge::proofreading::CandidateOrigin::Deterministic);
        QCOMPARE(candidate.sourceHash, input.sourceHash);
        const auto relative = candidate.sourceSpan.startByte - input.sourceSpan.startByte;
        const auto length = candidate.sourceSpan.lengthBytes();
        QCOMPARE(QString::fromUtf8(input.utf8.sliced(relative, length)), candidate.originalText);
    }
}

void ProofreadingEngineTest::respectsProtectedTerms() {
    const auto input = source(u"Mara went  home!!\nSacred  Word!!"_s);
    loreforge::proofreading::ProtectedTermRegistry registry(policy().protectedTerms);
    const auto protectedSpans = registry.protectedSpans(input);
    QCOMPARE(protectedSpans.size(), 2);

    const auto result =
        loreforge::proofreading::DeterministicProofreader::analyze({input}, policy());
    QVERIFY(result.isValid());
    for (const auto& candidate : result.report->candidates) {
        QVERIFY(!registry.protects(input, candidate.sourceSpan));
    }
    QVERIFY(result.report->candidates.size() >= 2);
}

void ProofreadingEngineTest::rejectsSourcesWithoutExactUtf8Provenance() {
    auto invalid = source(u"Text"_s);
    ++invalid.sourceSpan.endByte;
    const auto result = loreforge::proofreading::DeterministicProofreader::analyze({invalid}, {});
    QVERIFY(!result.isValid());
    QVERIFY(hasError(result, loreforge::proofreading::ProofreadingErrorCode::InvalidSource));

    const auto first = source(u"First"_s, 0);
    auto other = source(u"Other"_s, 20);
    other.chapterId = loreforge::core::ChapterId::fromStableKey(u"proofreading:chapter:1"_s);
    const auto mixed =
        loreforge::proofreading::DeterministicProofreader::analyze({first, other}, {});
    QVERIFY(!mixed.isValid());
    QVERIFY(hasError(mixed, loreforge::proofreading::ProofreadingErrorCode::InvalidSource));
}

void ProofreadingEngineTest::validatesSemanticCandidatesAgainstSource() {
    const auto input = source(u"Mara walk home."_s, 50);
    const auto relativeStart = input.utf8.indexOf("walk");
    const auto output = semanticOutput(input, input.sourceSpan.startByte + relativeStart,
                                       input.sourceSpan.startByte + relativeStart + 4, u"walks"_s);
    QVERIFY(loreforge::inference::OutputValidator::validate(
                loreforge::proofreading::SemanticProofreader::outputSchema(), output)
                .isValid());
    const auto originalBytes = input.utf8;
    const auto result = loreforge::proofreading::SemanticProofreader::extract(input, output, {});
    QVERIFY2(result.isValid(),
             qPrintable(result.errors.isEmpty() ? QString{} : result.errors.first().message));
    QCOMPARE(result.report->candidates.size(), 1);
    const auto& candidate = result.report->candidates.first();
    QCOMPARE(candidate.originalText, u"walk"_s);
    QCOMPARE(candidate.suggestedText, u"walks"_s);
    QCOMPARE(candidate.origin, loreforge::proofreading::CandidateOrigin::Semantic);
    QCOMPARE(candidate.semanticImpact, loreforge::proofreading::SemanticImpact::ActionChange);
    QCOMPARE(result.report->sourceHash, input.sourceHash);
    QCOMPARE(input.utf8, originalBytes);
}

void ProofreadingEngineTest::rejectsSemanticChangesToProtectedTermsAndBrokenBoundaries() {
    const auto protectedSource = source(u"Mara waits."_s, 20);
    const auto protectedOutput = semanticOutput(protectedSource, 20, 24, u"Maria"_s);
    const auto protectedResult = loreforge::proofreading::SemanticProofreader::extract(
        protectedSource, protectedOutput, policy());
    QVERIFY(!protectedResult.isValid());
    QVERIFY(
        hasError(protectedResult, loreforge::proofreading::ProofreadingErrorCode::ProtectedTerm));

    const auto unicodeSource = source(u"玛拉"_s, 200);
    const auto brokenOutput = semanticOutput(unicodeSource, 201, 202, u"她"_s);
    const auto brokenResult =
        loreforge::proofreading::SemanticProofreader::extract(unicodeSource, brokenOutput, {});
    QVERIFY(!brokenResult.isValid());
    QVERIFY(
        hasError(brokenResult, loreforge::proofreading::ProofreadingErrorCode::InvalidCandidate));
}

void ProofreadingEngineTest::requiresHumanApprovalOrExplicitManualAuthorization() {
    const auto input = source(u"Mara walk home."_s, 50);
    const auto now = QDateTime::fromString(u"2026-10-04T08:00:00.000Z"_s, Qt::ISODateWithMs);
    const auto reviewed = loreforge::proofreading::RepairQueueWorkflow::review(
        loreforge::core::ProjectId::fromStableKey(u"repair:project"_s), candidateFor(input),
        u"Mara walk home."_s, now);
    QVERIFY(std::holds_alternative<loreforge::proofreading::RepairQueueItem>(reviewed));
    const auto pending = std::get<loreforge::proofreading::RepairQueueItem>(reviewed);
    QCOMPARE(pending.status, loreforge::proofreading::CandidateStatus::ReviewRequired);

    const auto denied = loreforge::proofreading::RepairGate::authorizeApprovedCandidate(pending);
    QVERIFY(std::holds_alternative<loreforge::proofreading::RepairQueueError>(denied));
    const auto edited = loreforge::proofreading::RepairQueueWorkflow::editSuggestion(
        pending, u"walked"_s, now.addSecs(1));
    QVERIFY(std::holds_alternative<loreforge::proofreading::RepairQueueItem>(edited));
    const auto approved = loreforge::proofreading::RepairQueueWorkflow::approve(
        std::get<loreforge::proofreading::RepairQueueItem>(edited), now.addSecs(2));
    QVERIFY(std::holds_alternative<loreforge::proofreading::RepairQueueItem>(approved));
    const auto authorization = loreforge::proofreading::RepairGate::authorizeApprovedCandidate(
        std::get<loreforge::proofreading::RepairQueueItem>(approved));
    QVERIFY(std::holds_alternative<loreforge::proofreading::PatchAuthorization>(authorization));
    const auto& patch = std::get<loreforge::proofreading::PatchAuthorization>(authorization);
    QCOMPARE(patch.origin, loreforge::proofreading::PatchOrigin::ApprovedCandidate);
    QCOMPARE(patch.replacementText, u"walked"_s);
    QVERIFY(patch.candidateId.has_value());

    const auto unaccountedManual = loreforge::proofreading::RepairGate::authorizeManualEdit(
        input.chapterId, candidateFor(input).sourceSpan, u"walk"_s, u"walked"_s, input.sourceHash,
        {});
    QVERIFY(std::holds_alternative<loreforge::proofreading::RepairQueueError>(unaccountedManual));
    const auto manual = loreforge::proofreading::RepairGate::authorizeManualEdit(
        input.chapterId, candidateFor(input).sourceSpan, u"walk"_s, u"walked"_s, input.sourceHash,
        u"Author requested tense change"_s);
    QVERIFY(std::holds_alternative<loreforge::proofreading::PatchAuthorization>(manual));
    QCOMPARE(std::get<loreforge::proofreading::PatchAuthorization>(manual).origin,
             loreforge::proofreading::PatchOrigin::ExplicitManualEdit);
}

QTEST_GUILESS_MAIN(ProofreadingEngineTest)

#include "proofreading_engine_test.moc"
