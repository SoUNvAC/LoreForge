#include "loreforge/regression/model_regression.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;
using namespace loreforge;
using namespace loreforge::regression;

namespace {
core::ContentHash hash(const QString& text) {
    return core::ContentHash::sha256(QStringView(text));
}
QByteArray corpusJson() {
    QFile file(
        u"%1/regression/golden-v1.json"_s.arg(QString::fromUtf8(LOREFORGE_TEST_FIXTURES_DIR)));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}
GoldenCorpus corpus() {
    return std::get<GoldenCorpus>(ModelRegressionSuite::loadCorpus(corpusJson()));
}
RunManifest manifest(const GoldenCorpus& golden, const QString& run) {
    return {run,
            {u"fixture-only"_s, u"synthetic-test-model"_s, run},
            golden.hash,
            hash(u"prompt-bundle-v1"_s),
            hash(u"schema-bundle-v1"_s),
            hash(u"configuration-v1"_s),
            QDateTime::fromString(u"2026-10-07T08:00:00Z"_s, Qt::ISODate)};
}
QList<CaseObservation> observations(const GoldenCorpus& golden, const QString& run) {
    QList<CaseObservation> result;
    for (const auto& item : golden.cases) {
        result.append({item.id, core::LLMRunId::fromStableKey(run + u":"_s + item.id),
                       item.sourceHash, hash(u"fixed-context:"_s + item.id),
                       hash(u"synthetic-response:"_s + item.id), item.segmentation,
                       item.entities.required, item.events.required, item.context.required,
                       item.edits, llm::TokenUsage{100, 50, 150}, 1000});
    }
    return result;
}
RunReport report(const GoldenCorpus& golden, const QString& run,
                 const QList<CaseObservation>& captured) {
    return std::get<RunReport>(
        ModelRegressionSuite::evaluate(golden, manifest(golden, run), captured));
}
bool issue(const ComparisonReport& comparison, const QString& metric, const QString& caseId = {}) {
    return std::any_of(comparison.issues.cbegin(), comparison.issues.cend(), [&](const auto& item) {
        return item.metric == metric && (caseId.isEmpty() || item.caseId == caseId);
    });
}
RegressionReview approve(const ComparisonReport& comparison) {
    return {comparison.hash, u"test human reviewer"_s,
            u"Synthetic gate test only; not a real model approval"_s,
            QDateTime::fromString(u"2026-10-07T09:00:00Z"_s, Qt::ISODate), true};
}
} // namespace

class ModelRegressionTest final : public QObject {
    Q_OBJECT
  private slots:
    void loadsRepresentativeVersionedCorpus();
    void scoresPerfectCapturesAndExportsReceipts();
    void catchesQualityRegressionsPerCase();
    void catchesHallucinationAndUnsafeProofreading();
    void rejectsIncompleteAndStaleCaptures();
    void preservesMissingMeasurementsAndCostBudgets();
    void requiresIndependentComparableRuns();
    void requiresHashBoundHumanReview();
    void rejectsMalformedGoldensAndPolicies();
    void isDeterministicAndAllowsOptionalTruth();
    void roundTripsDurableReportsAndRejectsTampering();
};

void ModelRegressionTest::loadsRepresentativeVersionedCorpus() {
    const auto golden = corpus();
    QCOMPARE(golden.cases.size(), 6);
    QVERIFY(golden.hash.isValid());
    QSet<QString> tags;
    for (const auto& item : golden.cases) {
        for (const auto& tag : item.tags) {
            tags.insert(tag);
        }
        QCOMPARE(item.segmentation.reconstructedSource(), item.sourceUtf8);
    }
    for (const auto& tag : {u"chinese"_s, u"english"_s, u"unknown-speaker"_s, u"protected-terms"_s,
                            u"flashback"_s, u"knowledge-access"_s, u"proofreading"_s}) {
        QVERIFY(tags.contains(tag));
    }
}

void ModelRegressionTest::scoresPerfectCapturesAndExportsReceipts() {
    const auto golden = corpus();
    const auto baseline = report(golden, u"baseline"_s, observations(golden, u"baseline"_s));
    const auto candidate = report(golden, u"candidate"_s, observations(golden, u"candidate"_s));
    for (const auto metric : metrics()) {
        QVERIFY(baseline.aggregate.value(metric));
        const auto expected = metric == Metric::HallucinationRate ? 0.0
                              : metric == Metric::TokenUsage      ? 150.0
                              : metric == Metric::Latency         ? 1000.0
                                                                  : 1.0;
        QCOMPARE(*baseline.aggregate.value(metric), expected);
    }
    const auto comparison = std::get<ComparisonReport>(
        ModelRegressionSuite::compare(baseline, candidate, ModelRegressionSuite::defaultPolicy()));
    QVERIFY(comparison.issues.isEmpty());
    QVERIFY(comparison.hash.isValid());
    const auto exported =
        QJsonDocument::fromJson(ModelRegressionSuite::encodeComparison(comparison)).object();
    QCOMPARE(exported.value(u"comparison_hash"_s).toString(), comparison.hash.toHex());
    QVERIFY(exported.value(u"baseline"_s).isObject());
    QCOMPARE(QJsonDocument::fromJson(ModelRegressionSuite::encodeRun(candidate))
                 .object()
                 .value(u"report_hash"_s)
                 .toString(),
             candidate.hash.toHex());
}

void ModelRegressionTest::catchesQualityRegressionsPerCase() {
    const auto golden = corpus();
    const auto baseline = report(golden, u"baseline"_s, observations(golden, u"baseline"_s));
    auto captured = observations(golden, u"candidate"_s);
    captured[0].segmentation.segments[1].speaker = u"Ivo"_s;
    captured[0].events.clear();
    captured[0].entities.removeLast();
    auto comparison = std::get<ComparisonReport>(ModelRegressionSuite::compare(
        baseline, report(golden, u"candidate"_s, captured), ModelRegressionSuite::defaultPolicy()));
    QVERIFY(issue(comparison, u"speaker_accuracy"_s, golden.cases[0].id));
    QVERIFY(issue(comparison, u"event_f1"_s, golden.cases[0].id));
    QVERIFY(issue(comparison, u"entity_f1"_s, golden.cases[0].id));
    captured = observations(golden, u"candidate"_s);
    captured[1].segmentation.segments[1].speaker = u"invented speaker"_s;
    comparison = std::get<ComparisonReport>(ModelRegressionSuite::compare(
        baseline, report(golden, u"candidate"_s, captured), ModelRegressionSuite::defaultPolicy()));
    QVERIFY(issue(comparison, u"speaker_accuracy"_s, golden.cases[1].id));
    captured = observations(golden, u"candidate"_s);
    captured[0].segmentation.segments[1].type = narrative::SegmentType::Narration;
    captured[0].segmentation.segments[1].speaker.reset();
    comparison = std::get<ComparisonReport>(ModelRegressionSuite::compare(
        baseline, report(golden, u"candidate"_s, captured), ModelRegressionSuite::defaultPolicy()));
    QVERIFY(issue(comparison, u"dialogue_accuracy"_s, golden.cases[0].id));
}

void ModelRegressionTest::catchesHallucinationAndUnsafeProofreading() {
    const auto golden = corpus();
    const auto baseline = report(golden, u"baseline"_s, observations(golden, u"baseline"_s));
    auto captured = observations(golden, u"candidate"_s);
    captured[4].contextAssertions = {u"unexplained.revival"_s};
    captured[0].entities.append(u"invented entity"_s);
    const auto& source = golden.cases[2];
    const auto start = source.sourceUtf8.indexOf(QString(u"星阙"_s).toUtf8());
    captured[2].edits.append(
        {{source.segmentation.sourceSpan.sourceId, start, start + 6}, u"星阙"_s, u"星门"_s});
    auto comparison = std::get<ComparisonReport>(ModelRegressionSuite::compare(
        baseline, report(golden, u"candidate"_s, captured), ModelRegressionSuite::defaultPolicy()));
    QVERIFY(issue(comparison, u"hallucination_rate"_s));
    QVERIFY(issue(comparison, u"context_consistency"_s, golden.cases[4].id));
    QVERIFY(issue(comparison, u"proofreading_precision"_s, golden.cases[2].id));
    captured = observations(golden, u"candidate"_s);
    for (auto& item : captured) {
        item.edits.clear();
    }
    comparison = std::get<ComparisonReport>(ModelRegressionSuite::compare(
        baseline, report(golden, u"candidate"_s, captured), ModelRegressionSuite::defaultPolicy()));
    QVERIFY(issue(comparison, u"proofreading_recall"_s));
    QVERIFY(issue(comparison, u"proofreading_precision"_s));
}

void ModelRegressionTest::rejectsIncompleteAndStaleCaptures() {
    const auto golden = corpus();
    const auto good = observations(golden, u"candidate"_s);
    auto captured = good;
    captured.removeLast();
    QVERIFY(std::holds_alternative<RegressionError>(
        ModelRegressionSuite::evaluate(golden, manifest(golden, u"candidate"_s), captured)));
    captured = good;
    captured[1] = captured[0];
    QVERIFY(std::holds_alternative<RegressionError>(
        ModelRegressionSuite::evaluate(golden, manifest(golden, u"candidate"_s), captured)));
    captured = good;
    captured[0].sourceHash = hash(u"stale"_s);
    QVERIFY(std::holds_alternative<RegressionError>(
        ModelRegressionSuite::evaluate(golden, manifest(golden, u"candidate"_s), captured)));
    captured = good;
    captured[0].segmentation.segments[0].text = u"invented text"_s;
    QVERIFY(std::holds_alternative<RegressionError>(
        ModelRegressionSuite::evaluate(golden, manifest(golden, u"candidate"_s), captured)));
    captured = good;
    captured[0].entities.append(captured[0].entities.first());
    QVERIFY(std::holds_alternative<RegressionError>(
        ModelRegressionSuite::evaluate(golden, manifest(golden, u"candidate"_s), captured)));
    captured = good;
    captured[0].usage->totalTokens = 1;
    QVERIFY(std::holds_alternative<RegressionError>(
        ModelRegressionSuite::evaluate(golden, manifest(golden, u"candidate"_s), captured)));
    auto changed = golden;
    changed.cases[0].events.required.clear();
    QVERIFY(std::holds_alternative<RegressionError>(
        ModelRegressionSuite::evaluate(changed, manifest(changed, u"candidate"_s), good)));
}

void ModelRegressionTest::preservesMissingMeasurementsAndCostBudgets() {
    const auto golden = corpus();
    const auto baseline = report(golden, u"baseline"_s, observations(golden, u"baseline"_s));
    auto captured = observations(golden, u"candidate"_s);
    captured[0].usage.reset();
    captured[0].latencyMs.reset();
    const auto missing = report(golden, u"candidate"_s, captured);
    QVERIFY(!missing.aggregate.value(Metric::TokenUsage));
    QVERIFY(!missing.aggregate.value(Metric::Latency));
    auto comparison = std::get<ComparisonReport>(
        ModelRegressionSuite::compare(baseline, missing, ModelRegressionSuite::defaultPolicy()));
    QVERIFY(issue(comparison, u"token_usage"_s));
    QVERIFY(issue(comparison, u"latency_ms"_s));
    captured = observations(golden, u"candidate"_s);
    captured[0].usage = llm::TokenUsage{100, 100, 200};
    captured[0].latencyMs = 1600;
    comparison = std::get<ComparisonReport>(ModelRegressionSuite::compare(
        baseline, report(golden, u"candidate"_s, captured), ModelRegressionSuite::defaultPolicy()));
    QVERIFY(issue(comparison, u"token_usage"_s, golden.cases[0].id));
    QVERIFY(issue(comparison, u"latency_ms"_s, golden.cases[0].id));
    QVERIFY(std::holds_alternative<RegressionError>(
        ModelRegressionSuite::authorize(comparison, approve(comparison), baseline.hash)));
}

void ModelRegressionTest::requiresIndependentComparableRuns() {
    const auto golden = corpus();
    const auto baseline = report(golden, u"baseline"_s, observations(golden, u"baseline"_s));
    auto captured = observations(golden, u"candidate"_s);
    captured[0].captureId = core::LLMRunId::fromString(baseline.captureIds.first()).value();
    auto candidate = report(golden, u"candidate"_s, captured);
    QVERIFY(std::holds_alternative<RegressionError>(
        ModelRegressionSuite::compare(baseline, candidate, ModelRegressionSuite::defaultPolicy())));
    auto changedManifest = manifest(golden, u"candidate"_s);
    changedManifest.promptHash = hash(u"changed-prompt"_s);
    candidate = std::get<RunReport>(ModelRegressionSuite::evaluate(
        golden, changedManifest, observations(golden, u"candidate"_s)));
    QVERIFY(std::holds_alternative<RegressionError>(
        ModelRegressionSuite::compare(baseline, candidate, ModelRegressionSuite::defaultPolicy())));
    captured = observations(golden, u"candidate"_s);
    captured[0].contextHash = hash(u"different-input-context"_s);
    candidate = report(golden, u"candidate"_s, captured);
    QVERIFY(std::holds_alternative<RegressionError>(
        ModelRegressionSuite::compare(baseline, candidate, ModelRegressionSuite::defaultPolicy())));
    candidate = report(golden, u"candidate"_s, observations(golden, u"candidate"_s));
    candidate.cases[0].values[Metric::EntityF1] = 0.0;
    QVERIFY(std::holds_alternative<RegressionError>(
        ModelRegressionSuite::compare(baseline, candidate, ModelRegressionSuite::defaultPolicy())));
}

void ModelRegressionTest::requiresHashBoundHumanReview() {
    const auto golden = corpus();
    const auto baseline = report(golden, u"baseline"_s, observations(golden, u"baseline"_s));
    const auto candidate = report(golden, u"candidate"_s, observations(golden, u"candidate"_s));
    const auto comparison = std::get<ComparisonReport>(
        ModelRegressionSuite::compare(baseline, candidate, ModelRegressionSuite::defaultPolicy()));
    auto review = approve(comparison);
    QVERIFY(std::holds_alternative<PromotionAuthorization>(
        ModelRegressionSuite::authorize(comparison, review, baseline.hash)));
    review.approved = false;
    QVERIFY(std::holds_alternative<RegressionError>(
        ModelRegressionSuite::authorize(comparison, review, baseline.hash)));
    review = approve(comparison);
    review.comparisonHash = hash(u"other-comparison"_s);
    QVERIFY(std::holds_alternative<RegressionError>(
        ModelRegressionSuite::authorize(comparison, review, baseline.hash)));
    review = approve(comparison);
    QVERIFY(std::holds_alternative<RegressionError>(
        ModelRegressionSuite::authorize(comparison, review, hash(u"new-baseline"_s))));
    auto modified = comparison;
    modified.policy.maximumQualityRegression = 1.0;
    QVERIFY(std::holds_alternative<RegressionError>(
        ModelRegressionSuite::authorize(modified, review, baseline.hash)));
    review.reviewedAt = QDateTime::fromString(u"2026-10-06T00:00:00Z"_s, Qt::ISODate);
    QVERIFY(std::holds_alternative<RegressionError>(
        ModelRegressionSuite::authorize(comparison, review, baseline.hash)));
}

void ModelRegressionTest::rejectsMalformedGoldensAndPolicies() {
    QVERIFY(std::holds_alternative<RegressionError>(ModelRegressionSuite::loadCorpus("not-json")));
    auto object = QJsonDocument::fromJson(corpusJson()).object();
    auto cases = object.value(u"cases"_s).toArray();
    auto first = cases[0].toObject();
    first.insert(u"source"_s, u"different source"_s);
    cases[0] = first;
    object.insert(u"cases"_s, cases);
    QVERIFY(std::holds_alternative<RegressionError>(
        ModelRegressionSuite::loadCorpus(QJsonDocument(object).toJson())));
    const auto golden = corpus();
    const auto baseline = report(golden, u"baseline"_s, observations(golden, u"baseline"_s));
    const auto candidate = report(golden, u"candidate"_s, observations(golden, u"candidate"_s));
    auto policy = ModelRegressionSuite::defaultPolicy();
    policy.absoluteLimits.remove(Metric::ProofreadingRecall);
    QVERIFY(std::holds_alternative<RegressionError>(
        ModelRegressionSuite::compare(baseline, candidate, policy)));
    policy = ModelRegressionSuite::defaultPolicy();
    policy.maximumTokenRatio = std::nan("");
    QVERIFY(std::holds_alternative<RegressionError>(
        ModelRegressionSuite::compare(baseline, candidate, policy)));
}

void ModelRegressionTest::isDeterministicAndAllowsOptionalTruth() {
    const auto golden = corpus();
    auto captured = observations(golden, u"candidate"_s);
    const auto first = report(golden, u"candidate"_s, captured);
    std::reverse(captured.begin(), captured.end());
    const auto same = report(golden, u"candidate"_s, captured);
    QCOMPARE(first.hash, same.hash);
    QCOMPARE(ModelRegressionSuite::encodeRun(first), ModelRegressionSuite::encodeRun(same));
    captured = observations(golden, u"candidate"_s);
    captured[4].entities.append(u"门"_s);
    const auto candidate = report(golden, u"candidate"_s, captured);
    const auto baseline = report(golden, u"baseline"_s, observations(golden, u"baseline"_s));
    const auto comparison = std::get<ComparisonReport>(
        ModelRegressionSuite::compare(baseline, candidate, ModelRegressionSuite::defaultPolicy()));
    QVERIFY(comparison.issues.isEmpty());
}

void ModelRegressionTest::roundTripsDurableReportsAndRejectsTampering() {
    const auto golden = corpus();
    const auto baseline = report(golden, u"baseline"_s, observations(golden, u"baseline"_s));
    const auto candidate = report(golden, u"candidate"_s, observations(golden, u"candidate"_s));
    const auto serialized = ModelRegressionSuite::encodeRun(baseline);
    const auto restored = ModelRegressionSuite::loadRun(serialized);
    QVERIFY(std::holds_alternative<RunReport>(restored));
    QCOMPARE(std::get<RunReport>(restored).hash, baseline.hash);
    QCOMPARE(ModelRegressionSuite::encodeRun(std::get<RunReport>(restored)), serialized);
    const auto comparison = ModelRegressionSuite::compare(std::get<RunReport>(restored), candidate,
                                                          ModelRegressionSuite::defaultPolicy());
    QVERIFY(std::holds_alternative<ComparisonReport>(comparison));
    QVERIFY(std::get<ComparisonReport>(comparison).issues.isEmpty());
    auto object = QJsonDocument::fromJson(serialized).object();
    auto aggregate = object.value(u"aggregate"_s).toObject();
    aggregate.insert(u"entity_f1"_s, 0.0);
    object.insert(u"aggregate"_s, aggregate);
    QVERIFY(std::holds_alternative<RegressionError>(
        ModelRegressionSuite::loadRun(QJsonDocument(object).toJson())));
    object = QJsonDocument::fromJson(serialized).object();
    auto cases = object.value(u"cases"_s).toArray();
    auto first = cases[0].toObject();
    auto values = first.value(u"metrics"_s).toObject();
    values.insert(u"token_usage"_s, 1.5);
    first.insert(u"metrics"_s, values);
    cases[0] = first;
    object.insert(u"cases"_s, cases);
    QVERIFY(std::holds_alternative<RegressionError>(
        ModelRegressionSuite::loadRun(QJsonDocument(object).toJson())));
    QVERIFY(std::holds_alternative<RegressionError>(ModelRegressionSuite::loadRun("[]")));
}

QTEST_GUILESS_MAIN(ModelRegressionTest)
#include "model_regression_test.moc"
