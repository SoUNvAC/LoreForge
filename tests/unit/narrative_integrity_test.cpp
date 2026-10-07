#include "loreforge/narrative/integrity_checker.h"

#include <QtTest>

#include <algorithm>
#include <limits>

using namespace Qt::StringLiterals;
using namespace loreforge;
using namespace loreforge::narrative;

namespace {
const auto characterId = core::CharacterMemoryId::fromStableKey(u"integrity:aran"_s);
const auto otherId = core::CharacterMemoryId::fromStableKey(u"integrity:lin"_s);

IntegrityAnchor anchor(const IntegrityInput& input, qsizetype chapter = 0) {
    const auto& source = input.chapters[chapter];
    return {source.id,
            {ClaimBasis::Evidence, {{source.span, QString::fromUtf8(source.utf8)}}, 0.95}};
}

IntegrityInput fixture() {
    IntegrityInput input;
    input.projectId = core::ProjectId::fromStableKey(u"integrity-project"_s);
    input.clockId = u"story-minutes"_s;
    const QStringList paragraphs{u"阿岚出生，后来被称为岚。他死在城中。秘密将在十分钟后告诉他。"_s,
                                 u"回忆中阿岚开口说话。从城中到城门至少需要五分钟。"_s,
                                 u"阿岚复活，随后说话。先前的声音来自录音。"_s};
    qint64 offset = 0;
    for (qsizetype i = 0; i < paragraphs.size(); ++i) {
        const auto bytes = paragraphs[i].toUtf8();
        input.chapters.append({core::ChapterId::fromStableKey(u"integrity-chapter-%1"_s.arg(i)),
                               {u"novel.txt"_s, offset, offset + bytes.size()},
                               bytes,
                               core::ContentHash::sha256(QByteArrayView(bytes))});
        offset += bytes.size();
    }
    input.characters = {{characterId,
                         u"阿岚"_s,
                         {0, 0},
                         anchor(input),
                         {{u"岚"_s, {5, 0}, StoryMoment{20, 0}, anchor(input)}}}};
    input.information = {{u"secret"_s, u"城门钥匙的位置"_s}};
    input.revelations = {{u"secret"_s, characterId, {10, 0}, anchor(input)}};
    input.locations = {{u"city"_s, u"城中"_s}, {u"gate"_s, u"城门"_s}};
    input.travelConstraints = {{u"city"_s, u"gate"_s, 5, anchor(input, 1)}};
    return input;
}

IntegrityEvent makeEvent(const IntegrityInput& input, QString id, IntegrityEventKind kind,
                         qint64 tick, qsizetype chapter = 0, qint64 order = 0) {
    return {std::move(id), kind, characterId, {tick, order}, anchor(input, chapter), {}, {}, {}};
}

qsizetype count(const IntegrityReport& report, IntegrityRule rule) {
    return std::count_if(report.diagnostics.cbegin(), report.diagnostics.cend(),
                         [rule](const auto& item) { return item.rule == rule; });
}

void verifyRejected(const IntegrityInput& input) {
    const auto report = NarrativeIntegrityChecker::check(input);
    QVERIFY(!report.isValid());
    QVERIFY(!report.errors.isEmpty());
    QVERIFY(report.diagnostics.isEmpty());
    QVERIFY(report.explained.isEmpty());
    QVERIFY(!report.inputHash.isValid());
}
} // namespace

class NarrativeIntegrityTest final : public QObject {
    Q_OBJECT
  private slots:
    void respectsExplicitExistenceAndStoryChronology();
    void checksDeathSpeechAndGroundedRevival();
    void keepsExplanationsScopedAndAuditable();
    void checksCharacterSpecificKnowledge();
    void checksDirectedTravelAndUnknownBounds();
    void validatesNamesWithoutMergingIdentities();
    void retainsInferenceAndConfidence();
    void rejectsStaleAndMalformedEvidence();
    void rejectsInvalidTypedFactsAtomically();
    void isDeterministicAndReadOnly();
    void checksSameTickOrderingAndLargeTimes();
};

void NarrativeIntegrityTest::respectsExplicitExistenceAndStoryChronology() {
    auto input = fixture();
    input.characters[0].existsFrom = {5, 0};
    input.events = {makeEvent(input, u"before-birth"_s, IntegrityEventKind::Appearance, 4),
                    makeEvent(input, u"at-birth"_s, IntegrityEventKind::Appearance, 5),
                    makeEvent(input, u"later"_s, IntegrityEventKind::Speech, 6)};
    const auto result = NarrativeIntegrityChecker::check(input);
    QVERIFY(result.isValid());
    QCOMPARE(count(result, IntegrityRule::AppearanceBeforeExistence), 1);
    QCOMPARE(result.diagnostics[0].eventId, u"before-birth"_s);
    // A late chapter can narrate earlier story time: no automatic death contradiction.
    input.characters[0].existsFrom = {0, 0};
    input.events = {makeEvent(input, u"death"_s, IntegrityEventKind::Death, 20, 0),
                    makeEvent(input, u"flashback"_s, IntegrityEventKind::Speech, 10, 1)};
    QVERIFY(NarrativeIntegrityChecker::check(input).diagnostics.isEmpty());
}

void NarrativeIntegrityTest::checksDeathSpeechAndGroundedRevival() {
    auto input = fixture();
    input.events = {makeEvent(input, u"death"_s, IntegrityEventKind::Death, 1),
                    makeEvent(input, u"speech"_s, IntegrityEventKind::Speech, 2, 1),
                    makeEvent(input, u"revival"_s, IntegrityEventKind::Revival, 3, 2),
                    makeEvent(input, u"alive-speech"_s, IntegrityEventKind::Speech, 4, 2)};
    auto report = NarrativeIntegrityChecker::check(input);
    QVERIFY(report.isValid());
    QCOMPARE(count(report, IntegrityRule::SpeechAfterDeath), 1);
    QCOMPARE(report.diagnostics.first().evidence.size(), 2);
    QCOMPARE(report.diagnostics.first().chapterId, input.chapters[1].id);
    input.events[2].anchor.support = {ClaimBasis::Inference, {}, 0.5};
    report = NarrativeIntegrityChecker::check(input);
    QVERIFY(report.isValid());
    QCOMPARE(count(report, IntegrityRule::SpeechAfterDeath), 2);
}

void NarrativeIntegrityTest::keepsExplanationsScopedAndAuditable() {
    auto input = fixture();
    input.events = {makeEvent(input, u"death"_s, IntegrityEventKind::Death, 1),
                    makeEvent(input, u"recording"_s, IntegrityEventKind::Speech, 2, 1),
                    makeEvent(input, u"unexplained"_s, IntegrityEventKind::Speech, 3, 2)};
    input.explanations = {{u"recording"_s, IntegrityRule::SpeechAfterDeath,
                           u"声音来自先前录音，不是复活"_s, anchor(input, 2)}};
    auto report = NarrativeIntegrityChecker::check(input);
    QVERIFY(report.isValid());
    QCOMPARE(count(report, IntegrityRule::SpeechAfterDeath), 1);
    QCOMPARE(report.diagnostics.first().eventId, u"unexplained"_s);
    QCOMPARE(report.explained.size(), 1);
    QCOMPARE(report.explained.first().diagnostic.eventId, u"recording"_s);
    QCOMPARE(report.explained.first().reason, input.explanations.first().reason);
    input.explanations[0].anchor.support = {ClaimBasis::Inference, {}, 0.9};
    verifyRejected(input);
    input.explanations[0].anchor = anchor(input, 2);
    input.explanations.append(input.explanations.first());
    verifyRejected(input);
}

void NarrativeIntegrityTest::checksCharacterSpecificKnowledge() {
    auto input = fixture();
    input.characters.append({otherId, u"林"_s, {0, 0}, anchor(input), {}});
    input.events = {makeEvent(input, u"early"_s, IntegrityEventKind::Knowledge, 9),
                    makeEvent(input, u"on-time"_s, IntegrityEventKind::Knowledge, 10),
                    makeEvent(input, u"other-character"_s, IntegrityEventKind::Knowledge, 11)};
    for (auto& item : input.events) {
        item.informationId = u"secret"_s;
    }
    input.events[2].characterId = otherId;
    const auto report = NarrativeIntegrityChecker::check(input);
    QVERIFY(report.isValid());
    QCOMPARE(count(report, IntegrityRule::KnowledgeBeforeRevelation), 1);
    QCOMPARE(count(report, IntegrityRule::UnknownKnowledgeAccess), 1);
    // Omitting access does not prove a contradiction, even if another person knows it.
    QCOMPARE(report.diagnostics.last().severity, IntegritySeverity::Notice);
    input.events[1].informationId = u"missing-fact"_s;
    QCOMPARE(count(NarrativeIntegrityChecker::check(input), IntegrityRule::UnresolvedInformation),
             1);
}

void NarrativeIntegrityTest::checksDirectedTravelAndUnknownBounds() {
    auto input = fixture();
    input.events = {makeEvent(input, u"city"_s, IntegrityEventKind::LocationPresence, 1),
                    makeEvent(input, u"gate-too-soon"_s, IntegrityEventKind::LocationPresence, 3),
                    makeEvent(input, u"return"_s, IntegrityEventKind::LocationPresence, 4),
                    makeEvent(input, u"gate-on-time"_s, IntegrityEventKind::LocationPresence, 9)};
    input.events[0].locationId = u"city"_s;
    input.events[1].locationId = u"gate"_s;
    input.events[2].locationId = u"city"_s;
    input.events[3].locationId = u"gate"_s;
    auto report = NarrativeIntegrityChecker::check(input);
    QVERIFY(report.isValid());
    QCOMPARE(count(report, IntegrityRule::ImpossibleLocationTransition), 1);
    QCOMPARE(count(report, IntegrityRule::UnknownTravelConstraint), 1);
    input.events[1].locationId = u"unknown"_s;
    report = NarrativeIntegrityChecker::check(input);
    QCOMPARE(count(report, IntegrityRule::UnresolvedLocation), 1);
    QCOMPARE(count(report, IntegrityRule::ImpossibleLocationTransition), 0);
    QCOMPARE(count(report, IntegrityRule::UnknownTravelConstraint), 0);
}

void NarrativeIntegrityTest::validatesNamesWithoutMergingIdentities() {
    auto input = fixture();
    input.events = {makeEvent(input, u"early-alias"_s, IntegrityEventKind::Appearance, 4),
                    makeEvent(input, u"valid-alias"_s, IntegrityEventKind::Speech, 5),
                    makeEvent(input, u"expired-alias"_s, IntegrityEventKind::Speech, 20),
                    makeEvent(input, u"typo"_s, IntegrityEventKind::Speech, 21)};
    for (auto& item : input.events) {
        item.usedName = u"岚"_s;
    }
    input.events[3].usedName = u"阿蓝"_s;
    auto report = NarrativeIntegrityChecker::check(input);
    QVERIFY(report.isValid());
    QCOMPARE(count(report, IntegrityRule::InvalidNameVariant), 3);
    input.characters.append({otherId, u"岚"_s, {0, 0}, anchor(input), {}});
    report = NarrativeIntegrityChecker::check(input);
    QCOMPARE(count(report, IntegrityRule::AmbiguousNameVariant), 1);
    input.events[3].characterId = core::CharacterMemoryId::fromStableKey(u"unknown"_s);
    report = NarrativeIntegrityChecker::check(input);
    QCOMPARE(count(report, IntegrityRule::UnresolvedCharacter), 1);
    // NFC/case folding is allowed, fuzzy typo matching and global alias merging are not.
    input.characters[0].canonicalName = u"Élan"_s;
    input.events = {makeEvent(input, u"unicode"_s, IntegrityEventKind::Appearance, 1)};
    input.events[0].usedName = u"E\u0301LAN"_s;
    QVERIFY(NarrativeIntegrityChecker::check(input).diagnostics.isEmpty());
}

void NarrativeIntegrityTest::retainsInferenceAndConfidence() {
    auto input = fixture();
    input.characters[0].existsFrom = {10, 0};
    input.characters[0].anchor.support = {ClaimBasis::Inference, {}, 0.4};
    input.events = {makeEvent(input, u"inferred"_s, IntegrityEventKind::Speech, 5)};
    const auto report = NarrativeIntegrityChecker::check(input);
    QVERIFY(report.isValid());
    QCOMPARE(report.diagnostics.size(), 1);
    QCOMPARE(report.diagnostics.first().basis, ClaimBasis::Inference);
    QCOMPARE(report.diagnostics.first().confidence, 0.4);
    QCOMPARE(report.diagnostics.first().severity, IntegritySeverity::Warning);
}

void NarrativeIntegrityTest::rejectsStaleAndMalformedEvidence() {
    const auto original = fixture();
    auto input = original;
    input.chapters[0].utf8.append('x');
    verifyRejected(input);
    input = original;
    input.chapters[0].utf8.truncate(1);
    verifyRejected(input);
    input = original;
    input.characters[0].anchor.support.evidence[0].text = u"编造的引文"_s;
    verifyRejected(input);
    input = original;
    auto& evidence = input.characters[0].anchor.support.evidence[0];
    evidence.sourceSpan.startByte += 1;
    evidence.text = QString::fromUtf8(input.chapters[0].utf8.sliced(1));
    verifyRejected(input);
    input = original;
    input.characters[0].anchor.support.evidence.append(
        input.characters[0].anchor.support.evidence.first());
    verifyRejected(input);
    input = original;
    input.characters[0].anchor.chapterId = {};
    verifyRejected(input);
    input = original;
    input.characters[0].anchor.support.confidence = std::numeric_limits<double>::quiet_NaN();
    verifyRejected(input);
    input = original;
    input.chapters[0].utf8 = QByteArray(1, static_cast<char>(0xff));
    input.chapters[0].span.endByte = 1;
    input.chapters[0].hash = core::ContentHash::sha256(QByteArrayView(input.chapters[0].utf8));
    verifyRejected(input);
}

void NarrativeIntegrityTest::rejectsInvalidTypedFactsAtomically() {
    const auto original = fixture();
    auto input = original;
    input.events = {makeEvent(input, u"speech"_s, IntegrityEventKind::Speech, 1)};
    input.events[0].informationId = u"secret"_s;
    verifyRejected(input);
    input.events[0].informationId.reset();
    input.events.append(input.events[0]);
    verifyRejected(input);
    input.events[1].id = u"ambiguous-order"_s;
    verifyRejected(input);
    input = original;
    input.revelations.append(input.revelations.first());
    verifyRejected(input);
    input = original;
    input.travelConstraints[0].minimumTicks = -1;
    verifyRejected(input);
    input = original;
    input.characters[0].variants[0].validUntil = input.characters[0].variants[0].validFrom;
    verifyRejected(input);
    input = original;
    input.explanations = {
        {u"missing-event"_s, IntegrityRule::SpeechAfterDeath, u"reason"_s, anchor(input)}};
    verifyRejected(input);
    input = original;
    input.characters.append(input.characters.first());
    verifyRejected(input);
    input = original;
    input.clockId.clear();
    verifyRejected(input);
    input = original;
    input.chapters[1].span = input.chapters[0].span;
    input.chapters[1].utf8 = input.chapters[0].utf8;
    input.chapters[1].hash = input.chapters[0].hash;
    verifyRejected(input);
}

void NarrativeIntegrityTest::isDeterministicAndReadOnly() {
    auto input = fixture();
    input.events = {makeEvent(input, u"death"_s, IntegrityEventKind::Death, 5),
                    makeEvent(input, u"speech"_s, IntegrityEventKind::Speech, 6, 2),
                    makeEvent(input, u"knowledge"_s, IntegrityEventKind::Knowledge, 7, 1)};
    input.events[2].informationId = u"secret"_s;
    const auto originalBytes = input.chapters[0].utf8;
    const auto report = NarrativeIntegrityChecker::check(input);
    QVERIFY(report.isValid());
    auto reordered = input;
    std::reverse(reordered.events.begin(), reordered.events.end());
    std::reverse(reordered.chapters.begin(), reordered.chapters.end());
    std::reverse(reordered.locations.begin(), reordered.locations.end());
    const auto same = NarrativeIntegrityChecker::check(reordered);
    QCOMPARE(same.inputHash, report.inputHash);
    QCOMPARE(same.diagnostics, report.diagnostics);
    QCOMPARE(input.chapters[0].utf8, originalBytes);
    input.revelations[0].availableFrom = {6, 0};
    const auto changed = NarrativeIntegrityChecker::check(input);
    QVERIFY(changed.inputHash != report.inputHash);
    QCOMPARE(count(changed, IntegrityRule::KnowledgeBeforeRevelation), 0);
}

void NarrativeIntegrityTest::checksSameTickOrderingAndLargeTimes() {
    auto input = fixture();
    input.events = {makeEvent(input, u"before"_s, IntegrityEventKind::Speech, 5, 0, 0),
                    makeEvent(input, u"death"_s, IntegrityEventKind::Death, 5, 0, 1),
                    makeEvent(input, u"after"_s, IntegrityEventKind::Speech, 5, 0, 2)};
    const auto report = NarrativeIntegrityChecker::check(input);
    QCOMPARE(count(report, IntegrityRule::SpeechAfterDeath), 1);
    QCOMPARE(report.diagnostics.first().eventId, u"after"_s);
    input.events = {makeEvent(input, u"city"_s, IntegrityEventKind::LocationPresence, 0),
                    makeEvent(input, u"gate"_s, IntegrityEventKind::LocationPresence,
                              std::numeric_limits<qint64>::max())};
    input.events[0].locationId = u"city"_s;
    input.events[1].locationId = u"gate"_s;
    const auto large = NarrativeIntegrityChecker::check(input);
    QVERIFY(large.isValid());
    QVERIFY(large.diagnostics.isEmpty());
}

QTEST_GUILESS_MAIN(NarrativeIntegrityTest)
#include "narrative_integrity_test.moc"
