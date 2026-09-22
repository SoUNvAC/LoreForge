#include "loreforge/context/context_builder.h"
#include "loreforge/context/relevant_memory_retriever.h"
#include "loreforge/text/token_estimator.h"

#include <QJsonArray>
#include <QtTest>

#include <algorithm>

using namespace Qt::StringLiterals;

class ContextEngineTest final : public QObject {
    Q_OBJECT

  private slots:
    void buildsDeterministicInspectableContext();
    void enforcesBudgetAndReportsOmissions();
    void rejectsInvalidInputs();
    void rejectsMalformedStoredContext();
    void ranksCjkReferencesAsRelevant();
    void estimatesMixedLanguageTokensDeterministically();
};

namespace {

loreforge::narrative::ClaimSupport direct(QString sourceId, qint64 start, QString text) {
    return {loreforge::narrative::ClaimBasis::Evidence,
            {{{std::move(sourceId), start, start + text.toUtf8().size()}, std::move(text)}},
            0.97};
}

loreforge::narrative::StoryStateSnapshot storyState(const loreforge::core::ProjectId& projectId,
                                                    bool withMemory) {
    const auto chapterId = loreforge::core::ChapterId::fromStableKey(u"context:chapter:0"_s);
    const auto sourceId = u"context/chapter-0.txt"_s;
    const auto source = u"Mara and Ivo wait."_s;
    const loreforge::narrative::ClaimSupport inferred{
        loreforge::narrative::ClaimBasis::Inference, {}, 0.75};
    loreforge::narrative::ChapterAnalysis analysis;
    analysis.chapterId = chapterId;
    analysis.sourceSpan = {sourceId, 0, source.toUtf8().size()};
    if (withMemory) {
        analysis.characters = {
            {u"Mara"_s, {{u"Captain"_s, inferred}}, direct(sourceId, 0, u"Mara"_s)},
            {u"Ivo"_s, {}, direct(sourceId, 9, u"Ivo"_s)},
        };
        analysis.events = {{u"Mara and Ivo wait."_s,
                            {u"Mara"_s, u"Ivo"_s},
                            std::nullopt,
                            direct(sourceId, 0, source)}};
        analysis.openThreads = {{u"What are they waiting for?"_s, inferred}};
    }
    analysis.summary = {u"Two people wait."_s, inferred};
    const auto result = loreforge::narrative::StoryStateRebuilder::rebuild(
        projectId, {{projectId, 0, std::move(analysis)}});
    return *result.snapshot;
}

QJsonObject outputSchema() {
    return {
        {u"$schema"_s, u"https://json-schema.org/draft/2020-12/schema"_s},
        {u"type"_s, u"object"_s},
        {u"required"_s, QJsonArray{u"answer"_s}},
        {u"properties"_s, QJsonObject{{u"answer"_s, QJsonObject{{u"type"_s, u"string"_s}}}}},
        {u"additionalProperties"_s, false},
    };
}

loreforge::context::ContextBuildInput input(const loreforge::core::ProjectId& projectId,
                                            bool withMemory = true) {
    return {projectId,
            storyState(projectId, withMemory),
            u"Use only supplied evidence."_s,
            u"A test novel."_s,
            {u"Starwatch"_s},
            u"Mara and Ivo were waiting."_s,
            u"Mara hears a sound behind the gate."_s,
            u"Return the next relevant fact."_s,
            outputSchema()};
}

bool containsError(const loreforge::context::ContextBuildResult& result,
                   loreforge::context::ContextBuildErrorCode code) {
    return std::any_of(result.errors.cbegin(), result.errors.cend(),
                       [code](const auto& error) { return error.code == code; });
}

} // namespace

void ContextEngineTest::buildsDeterministicInspectableContext() {
    const auto projectId = loreforge::core::ProjectId::fromStableKey(u"context-project"_s);
    const loreforge::context::ContextBudget budget{1'200, 200};
    const auto first = loreforge::context::ContextBuilder::build(
        input(projectId), budget,
        QDateTime::fromString(u"2026-09-22T01:00:00.000Z"_s, Qt::ISODateWithMs));
    QVERIFY2(first.isValid(),
             qPrintable(first.errors.isEmpty() ? QString{} : first.errors.first().message));
    const auto second = loreforge::context::ContextBuilder::build(
        input(projectId), budget,
        QDateTime::fromString(u"2026-09-22T02:00:00.000Z"_s, Qt::ISODateWithMs));
    QVERIFY(second.isValid());
    QCOMPARE(first.context->snapshot.id, second.context->snapshot.id);
    QCOMPARE(first.context->snapshot.contentHash, second.context->snapshot.contentHash);
    QCOMPARE(first.context->snapshot.content, second.context->snapshot.content);

    const auto& inspector = first.context->inspector;
    QVERIFY(inspector.estimatedTokens <= budget.promptTokenLimit());
    QVERIFY(inspector.characterMemory.join(QLatin1Char('\n')).contains(u"Mara"_s));
    QVERIFY(inspector.eventMemory.join(QLatin1Char('\n')).contains(u"wait"_s));
    QVERIFY(inspector.rawFinalPrompt.contains(u"SYSTEM:"_s));
    QVERIFY(inspector.rawFinalPrompt.contains(u"[CURRENT CHAPTER]"_s));
    QCOMPARE(first.context->messages.size(), 2);
    QCOMPARE(first.context->messages.first().role, loreforge::llm::LLMRole::System);

    const auto restored = loreforge::context::inspectorFromSnapshot(first.context->snapshot);
    QVERIFY(restored.has_value());
    QCOMPARE(*restored, inspector);
}

void ContextEngineTest::enforcesBudgetAndReportsOmissions() {
    const auto projectId = loreforge::core::ProjectId::fromStableKey(u"context-budget"_s);
    const auto createdAt = QDateTime::fromString(u"2026-09-22T01:00:00.000Z"_s, Qt::ISODateWithMs);
    const auto base =
        loreforge::context::ContextBuilder::build(input(projectId, false), {2'000, 200}, createdAt);
    QVERIFY(base.isValid());
    const auto mandatoryTokens = base.context->inspector.estimatedTokens;

    const loreforge::context::ContextBudget tight{mandatoryTokens + 201, 200};
    const auto bounded =
        loreforge::context::ContextBuilder::build(input(projectId), tight, createdAt);
    QVERIFY(bounded.isValid());
    QCOMPARE(bounded.context->inspector.estimatedTokens <= tight.promptTokenLimit(), true);
    QVERIFY(bounded.context->inspector.omittedCharacters > 0);
    QVERIFY(bounded.context->inspector.omittedEvents > 0);
    QVERIFY(bounded.context->inspector.omittedOpenThreads > 0);

    const loreforge::context::ContextBudget impossible{mandatoryTokens + 199, 200};
    const auto rejected =
        loreforge::context::ContextBuilder::build(input(projectId, false), impossible, createdAt);
    QVERIFY(!rejected.isValid());
    QVERIFY(containsError(
        rejected, loreforge::context::ContextBuildErrorCode::MandatoryContentExceedsBudget));
}

void ContextEngineTest::rejectsInvalidInputs() {
    const auto projectId = loreforge::core::ProjectId::fromStableKey(u"context-invalid"_s);
    auto wrongProject = input(projectId);
    wrongProject.projectId = loreforge::core::ProjectId::fromStableKey(u"another-project"_s);
    const auto mismatch = loreforge::context::ContextBuilder::build(
        wrongProject, {1'000, 200},
        QDateTime::fromString(u"2026-09-22T01:00:00.000Z"_s, Qt::ISODateWithMs));
    QVERIFY(!mismatch.isValid());
    QVERIFY(containsError(mismatch, loreforge::context::ContextBuildErrorCode::StoryStateMismatch));
}

void ContextEngineTest::rejectsMalformedStoredContext() {
    const auto projectId = loreforge::core::ProjectId::fromStableKey(u"context-corrupt"_s);
    const auto built = loreforge::context::ContextBuilder::build(
        input(projectId), {1'200, 200},
        QDateTime::fromString(u"2026-09-22T01:00:00.000Z"_s, Qt::ISODateWithMs));
    QVERIFY(built.isValid());
    auto snapshot = built.context->snapshot;
    auto budget = snapshot.content.value(u"budget"_s).toObject();
    budget.insert(u"prompt_token_limit"_s, 1);
    snapshot.content.insert(u"budget"_s, budget);
    snapshot.contentHash =
        loreforge::core::ContentHash::sha256(loreforge::inference::canonicalJson(snapshot.content));
    snapshot.id = loreforge::core::ContextSnapshotId::fromStableKey(
        projectId.toString() + QLatin1Char(':') + snapshot.contentHash.toHex());
    QVERIFY(!loreforge::context::inspectorFromSnapshot(snapshot).has_value());
}

void ContextEngineTest::ranksCjkReferencesAsRelevant() {
    auto state = storyState(loreforge::core::ProjectId::fromStableKey(u"context-cjk"_s), true);
    state.characters[0].aliases.append(u"队长"_s);
    const auto ranked = loreforge::context::RelevantMemoryRetriever::rank(
        state, u"队长听见门后有响声。"_s, u"继续这一幕。"_s);
    QVERIFY(!ranked.isEmpty());
    QCOMPARE(ranked.first().kind, loreforge::context::MemoryKind::Character);
    QCOMPARE(ranked.first().stableId, state.characters.first().id.toString());
}

void ContextEngineTest::estimatesMixedLanguageTokensDeterministically() {
    QCOMPARE(loreforge::text::TokenEstimator::estimate(u"hello world"_s), 4);
    QCOMPARE(loreforge::text::TokenEstimator::estimate(u"你好世界"_s), 4);
    QCOMPARE(loreforge::text::TokenEstimator::estimate(u"hello，你好"_s), 5);
}

QTEST_GUILESS_MAIN(ContextEngineTest)

#include "context_engine_test.moc"
