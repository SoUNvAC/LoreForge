#include "regression_support.h"

#include <cmath>
#include <limits>

namespace loreforge::regression {
using namespace Qt::StringLiterals;
namespace {
bool cost(Metric metric) {
    return metric == Metric::TokenUsage || metric == Metric::Latency;
}
bool lowerIsBetter(Metric metric) {
    return cost(metric) || metric == Metric::HallucinationRate;
}
QJsonObject encodeMetrics(const QMap<Metric, std::optional<double>>& values) {
    QJsonObject result;
    for (const auto metric : metrics()) {
        const auto value = values.value(metric);
        result.insert(metricName(metric), value ? QJsonValue(*value) : QJsonValue());
    }
    return result;
}
QJsonObject identityJson(const ModelIdentity& identity) {
    return {{u"backend"_s, identity.backend},
            {u"model"_s, identity.model},
            {u"revision"_s, identity.revision}};
}
QJsonObject manifestJson(const RunManifest& manifest) {
    return {{u"run_id"_s, manifest.runId},
            {u"model"_s, identityJson(manifest.identity)},
            {u"corpus_hash"_s, manifest.corpusHash.toHex()},
            {u"prompt_hash"_s, manifest.promptHash.toHex()},
            {u"schema_hash"_s, manifest.schemaHash.toHex()},
            {u"configuration_hash"_s, manifest.configurationHash.toHex()},
            {u"captured_at"_s, manifest.capturedAt.toUTC().toString(Qt::ISODateWithMs)}};
}
bool validManifest(const RunManifest& manifest) {
    return detail::text(manifest.runId) && detail::text(manifest.identity.backend) &&
           detail::text(manifest.identity.model) && detail::text(manifest.identity.revision) &&
           manifest.corpusHash.isValid() && manifest.promptHash.isValid() &&
           manifest.schemaHash.isValid() && manifest.configurationHash.isValid() &&
           manifest.capturedAt.isValid();
}
QJsonObject runJson(const RunReport& report) {
    QJsonObject contexts;
    for (auto it = report.contextHashes.cbegin(); it != report.contextHashes.cend(); ++it) {
        contexts.insert(it.key(), it.value().toHex());
    }
    QJsonArray cases;
    auto sorted = report.cases;
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.caseId < b.caseId; });
    for (const auto& item : sorted) {
        cases.append(
            QJsonObject{{u"case_id"_s, item.caseId}, {u"metrics"_s, encodeMetrics(item.values)}});
    }
    return {{u"format"_s, ModelRegressionSuite::version()},
            {u"manifest"_s, manifestJson(report.manifest)},
            {u"cases"_s, cases},
            {u"aggregate"_s, encodeMetrics(report.aggregate)},
            {u"captures_hash"_s, report.capturesHash.toHex()},
            {u"capture_ids"_s, detail::strings(report.captureIds)},
            {u"context_hashes"_s, contexts}};
}
core::ContentHash runHash(const RunReport& report) {
    return detail::hash(QJsonDocument(runJson(report)).toJson(QJsonDocument::Compact));
}
QMap<Metric, std::optional<double>> aggregate(const QList<CaseMetrics>& cases) {
    auto sorted = cases;
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.caseId < b.caseId; });
    QMap<Metric, std::optional<double>> values;
    for (const auto metric : metrics()) {
        double sum = 0.0;
        qsizetype count = 0;
        for (const auto& item : sorted) {
            const auto value = item.values.value(metric);
            if (value) {
                sum += *value;
                ++count;
            }
        }
        values.insert(metric, count == 0 || (cost(metric) && count != cases.size())
                                  ? std::nullopt
                                  : std::optional(sum / static_cast<double>(count)));
    }
    return values;
}
bool validRun(const RunReport& report) {
    if (!validManifest(report.manifest) || report.cases.isEmpty() ||
        !report.capturesHash.isValid() || report.captureIds.size() != report.cases.size() ||
        !detail::validStrings(report.captureIds) ||
        report.contextHashes.size() != report.cases.size() || !report.hash.isValid() ||
        report.hash != runHash(report) || report.aggregate != aggregate(report.cases)) {
        return false;
    }
    QSet<QString> ids;
    for (const auto& item : report.cases) {
        if (!detail::text(item.caseId) || ids.contains(item.caseId) ||
            item.values.size() != metrics().size() ||
            !report.contextHashes.value(item.caseId).isValid()) {
            return false;
        }
        ids.insert(item.caseId);
        for (const auto metric : metrics()) {
            if (!item.values.contains(metric)) {
                return false;
            }
            const auto value = item.values.value(metric);
            if (value &&
                (!std::isfinite(*value) || *value < 0.0 || (!cost(metric) && *value > 1.0))) {
                return false;
            }
            if (value && cost(metric) &&
                (std::floor(*value) != *value ||
                 *value > (metric == Metric::TokenUsage
                               ? static_cast<double>(std::numeric_limits<int>::max())
                               : static_cast<double>(qint64{1} << 53)))) {
                return false;
            }
        }
    }
    return true;
}
std::optional<double> labelsF1(const GoldenLabels& golden, const QStringList& actual) {
    if (golden.required.isEmpty() && actual.isEmpty()) {
        return std::nullopt;
    }
    qsizetype correct = 0;
    qsizetype found = 0;
    for (const auto& label : actual) {
        if (golden.allowed.contains(label)) {
            ++correct;
        }
    }
    for (const auto& label : golden.required) {
        if (actual.contains(label)) {
            ++found;
        }
    }
    const double precision = actual.isEmpty() ? 1.0 : static_cast<double>(correct) / actual.size();
    const double recall =
        golden.required.isEmpty() ? 1.0 : static_cast<double>(found) / golden.required.size();
    return precision + recall == 0.0 ? 0.0 : 2.0 * precision * recall / (precision + recall);
}
CaseMetrics score(const GoldenCase& golden, const CaseObservation& actual) {
    CaseMetrics result{golden.id, {}};
    qint64 typeMatches = 0;
    qint64 dialogueBytes = 0;
    qint64 speakerMatches = 0;
    qsizetype rightIndex = 0;
    for (const auto& expected : golden.segmentation.segments) {
        if (expected.type == narrative::SegmentType::Dialogue) {
            dialogueBytes += expected.sourceSpan.lengthBytes();
        }
        while (rightIndex < actual.segmentation.segments.size() &&
               actual.segmentation.segments[rightIndex].sourceSpan.endByte <=
                   expected.sourceSpan.startByte) {
            ++rightIndex;
        }
        for (auto i = rightIndex; i < actual.segmentation.segments.size(); ++i) {
            const auto& observed = actual.segmentation.segments[i];
            if (observed.sourceSpan.startByte >= expected.sourceSpan.endByte) {
                break;
            }
            const auto length =
                std::min(observed.sourceSpan.endByte, expected.sourceSpan.endByte) -
                std::max(observed.sourceSpan.startByte, expected.sourceSpan.startByte);
            if (observed.type == expected.type) {
                typeMatches += length;
                if (expected.type == narrative::SegmentType::Dialogue &&
                    expected.speaker == observed.speaker) {
                    speakerMatches += length;
                }
            }
        }
    }
    result.values[Metric::DialogueAccuracy] =
        static_cast<double>(typeMatches) / golden.sourceUtf8.size();
    result.values[Metric::SpeakerAccuracy] =
        dialogueBytes == 0 ? std::nullopt
                           : std::optional(static_cast<double>(speakerMatches) / dialogueBytes);
    result.values[Metric::EntityF1] = labelsF1(golden.entities, actual.entities);
    result.values[Metric::EventF1] = labelsF1(golden.events, actual.events);
    result.values[Metric::ContextConsistency] = labelsF1(golden.context, actual.contextAssertions);
    qsizetype unsupported = 0;
    for (const auto& label : actual.entities) {
        if (!golden.entities.allowed.contains(label)) {
            ++unsupported;
        }
    }
    for (const auto& label : actual.events) {
        if (!golden.events.allowed.contains(label)) {
            ++unsupported;
        }
    }
    for (const auto& label : actual.contextAssertions) {
        if (!golden.context.allowed.contains(label)) {
            ++unsupported;
        }
    }
    const auto claims =
        actual.entities.size() + actual.events.size() + actual.contextAssertions.size();
    result.values[Metric::HallucinationRate] =
        claims == 0 ? 0.0 : static_cast<double>(unsupported) / claims;
    qsizetype correctEdits = 0;
    for (const auto& edit : actual.edits) {
        if (golden.edits.contains(edit)) {
            ++correctEdits;
        }
    }
    result.values[Metric::ProofreadingPrecision] =
        actual.edits.isEmpty()
            ? std::nullopt
            : std::optional(static_cast<double>(correctEdits) / actual.edits.size());
    result.values[Metric::ProofreadingRecall] =
        golden.edits.isEmpty()
            ? std::nullopt
            : std::optional(static_cast<double>(correctEdits) / golden.edits.size());
    result.values[Metric::TokenUsage] =
        actual.usage ? std::optional(static_cast<double>(actual.usage->totalTokens)) : std::nullopt;
    result.values[Metric::Latency] =
        actual.latencyMs ? std::optional(static_cast<double>(*actual.latencyMs)) : std::nullopt;
    return result;
}
QJsonObject comparisonJson(const ComparisonReport& report) {
    QJsonArray issues;
    for (const auto& issue : report.issues) {
        issues.append(QJsonArray{issue.caseId, issue.metric, issue.reason});
    }
    QJsonObject limits;
    for (auto it = report.policy.absoluteLimits.cbegin(); it != report.policy.absoluteLimits.cend();
         ++it) {
        limits.insert(metricName(it.key()), it.value());
    }
    return {{u"format"_s, u"loreforge-model-comparison-v1"_s},
            {u"baseline"_s, runJson(report.baseline)},
            {u"candidate"_s, runJson(report.candidate)},
            {u"policy"_s,
             QJsonObject{{u"absolute_limits"_s, limits},
                         {u"maximum_quality_regression"_s, report.policy.maximumQualityRegression},
                         {u"maximum_token_ratio"_s, report.policy.maximumTokenRatio},
                         {u"maximum_latency_ratio"_s, report.policy.maximumLatencyRatio}}},
            {u"issues"_s, issues}};
}
} // namespace

QString metricName(Metric metric) {
    switch (metric) {
    case Metric::DialogueAccuracy:
        return u"dialogue_accuracy"_s;
    case Metric::SpeakerAccuracy:
        return u"speaker_accuracy"_s;
    case Metric::EntityF1:
        return u"entity_f1"_s;
    case Metric::EventF1:
        return u"event_f1"_s;
    case Metric::HallucinationRate:
        return u"hallucination_rate"_s;
    case Metric::ContextConsistency:
        return u"context_consistency"_s;
    case Metric::ProofreadingPrecision:
        return u"proofreading_precision"_s;
    case Metric::ProofreadingRecall:
        return u"proofreading_recall"_s;
    case Metric::TokenUsage:
        return u"token_usage"_s;
    case Metric::Latency:
        return u"latency_ms"_s;
    }
    return {};
}
QList<Metric> metrics() {
    return {Metric::DialogueAccuracy,
            Metric::SpeakerAccuracy,
            Metric::EntityF1,
            Metric::EventF1,
            Metric::HallucinationRate,
            Metric::ContextConsistency,
            Metric::ProofreadingPrecision,
            Metric::ProofreadingRecall,
            Metric::TokenUsage,
            Metric::Latency};
}
QString ModelRegressionSuite::version() {
    return u"loreforge-model-regression-v1"_s;
}
RegressionPolicy ModelRegressionSuite::defaultPolicy() {
    return {{{Metric::DialogueAccuracy, 0.95},
             {Metric::SpeakerAccuracy, 0.95},
             {Metric::EntityF1, 0.90},
             {Metric::EventF1, 0.90},
             {Metric::HallucinationRate, 0.05},
             {Metric::ContextConsistency, 0.95},
             {Metric::ProofreadingPrecision, 0.95},
             {Metric::ProofreadingRecall, 0.85}},
            0.02,
            1.25,
            1.50};
}

RegressionResult<RunReport>
ModelRegressionSuite::evaluate(const GoldenCorpus& corpus, const RunManifest& manifest,
                               const QList<CaseObservation>& observations) {
    if (!detail::validCorpus(corpus) || !validManifest(manifest) ||
        manifest.corpusHash != corpus.hash || observations.size() != corpus.cases.size()) {
        return RegressionError{u"$"_s,
                               u"Invalid corpus, run provenance or incomplete case coverage"_s};
    }
    QHash<QString, const GoldenCase*> cases;
    for (const auto& item : corpus.cases) {
        cases.insert(item.id, &item);
    }
    RunReport report;
    report.manifest = manifest;
    QSet<QString> seen;
    QJsonArray captures;
    auto sorted = observations;
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.caseId < b.caseId; });
    for (const auto& observation : sorted) {
        const auto* golden = cases.value(observation.caseId, nullptr);
        if (!golden || seen.contains(observation.caseId) || !observation.captureId.isValid() ||
            report.captureIds.contains(observation.captureId.toString()) ||
            observation.sourceHash != golden->sourceHash || !observation.contextHash.isValid() ||
            !observation.responseHash.isValid() ||
            !detail::validSegmentation(*golden, observation.segmentation) ||
            !detail::validStrings(observation.entities) ||
            !detail::validStrings(observation.events) ||
            !detail::validStrings(observation.contextAssertions) ||
            !detail::validEdits(*golden, observation.edits)) {
            return RegressionError{observation.caseId,
                                   u"Invalid, duplicated or stale case capture"_s};
        }
        QJsonValue usage;
        if (observation.usage) {
            const auto& tokens = *observation.usage;
            if (tokens.promptTokens < 0 || tokens.completionTokens < 0 || tokens.totalTokens < 0 ||
                static_cast<qint64>(tokens.promptTokens) + tokens.completionTokens !=
                    tokens.totalTokens) {
                return RegressionError{observation.caseId,
                                       u"Token usage is invalid or inconsistent"_s};
            }
            usage = QJsonArray{tokens.promptTokens, tokens.completionTokens, tokens.totalTokens};
        }
        if (observation.latencyMs &&
            (*observation.latencyMs < 0 || *observation.latencyMs > (qint64{1} << 53))) {
            return RegressionError{observation.caseId,
                                   u"Latency is outside the exact measurement range"_s};
        }
        seen.insert(observation.caseId);
        report.captureIds.append(observation.captureId.toString());
        report.contextHashes.insert(observation.caseId, observation.contextHash);
        report.cases.append(score(*golden, observation));
        captures.append(QJsonArray{
            observation.caseId, observation.captureId.toString(), observation.sourceHash.toHex(),
            observation.contextHash.toHex(), observation.responseHash.toHex(),
            detail::segmentationJson(observation.segmentation),
            detail::strings(observation.entities), detail::strings(observation.events),
            detail::strings(observation.contextAssertions), detail::encodeEdits(observation.edits),
            usage,
            observation.latencyMs ? QJsonValue(QString::number(*observation.latencyMs))
                                  : QJsonValue()});
    }
    report.aggregate = aggregate(report.cases);
    report.capturesHash = detail::hash(QJsonDocument(captures).toJson(QJsonDocument::Compact));
    report.hash = runHash(report);
    return report;
}

RegressionResult<ComparisonReport> ModelRegressionSuite::compare(const RunReport& baseline,
                                                                 const RunReport& candidate,
                                                                 const RegressionPolicy& policy) {
    if (!validRun(baseline) || !validRun(candidate) ||
        baseline.manifest.runId == candidate.manifest.runId ||
        baseline.manifest.identity == candidate.manifest.identity ||
        baseline.manifest.corpusHash != candidate.manifest.corpusHash ||
        baseline.manifest.promptHash != candidate.manifest.promptHash ||
        baseline.manifest.schemaHash != candidate.manifest.schemaHash ||
        baseline.manifest.configurationHash != candidate.manifest.configurationHash ||
        baseline.contextHashes != candidate.contextHashes ||
        baseline.cases.size() != candidate.cases.size()) {
        return RegressionError{
            u"$"_s,
            u"Runs must be independent, untampered and use identical corpus/prompt/schema/configuration"_s};
    }
    for (const auto& id : baseline.captureIds) {
        if (candidate.captureIds.contains(id)) {
            return RegressionError{id, u"Comparison reuses a baseline capture"_s};
        }
    }
    if (!std::isfinite(policy.maximumQualityRegression) || policy.maximumQualityRegression < 0.0 ||
        policy.maximumQualityRegression > 1.0 || !std::isfinite(policy.maximumTokenRatio) ||
        policy.maximumTokenRatio < 1.0 || !std::isfinite(policy.maximumLatencyRatio) ||
        policy.maximumLatencyRatio < 1.0 || policy.absoluteLimits.size() != 8) {
        return RegressionError{u"policy"_s, u"Invalid regression tolerance or cost budgets"_s};
    }
    for (const auto metric : metrics()) {
        if (cost(metric)) {
            continue;
        }
        if (!policy.absoluteLimits.contains(metric) ||
            !std::isfinite(policy.absoluteLimits.value(metric)) ||
            policy.absoluteLimits.value(metric) < 0.0 ||
            policy.absoluteLimits.value(metric) > 1.0) {
            return RegressionError{u"policy"_s,
                                   u"Every quality metric requires a finite absolute limit"_s};
        }
    }
    ComparisonReport report{baseline, candidate, policy, {}, {}};
    auto inspect = [&](const QString& caseId, const auto& before, const auto& after,
                       bool required) {
        for (const auto metric : metrics()) {
            const auto oldValue = before.value(metric);
            const auto newValue = after.value(metric);
            auto issue = [&](const QString& reason) {
                report.issues.append({caseId, metricName(metric), reason});
            };
            if (!oldValue || !newValue) {
                if (required || cost(metric)) {
                    issue(
                        u"Metric is unavailable; missing measurements are not zero or a passing score"_s);
                } else if (newValue) {
                    const auto limit = policy.absoluteLimits.value(metric);
                    if (lowerIsBetter(metric) ? *newValue > limit : *newValue < limit) {
                        issue(u"Candidate violates the absolute quality limit"_s);
                    }
                }
                continue;
            }
            if (cost(metric)) {
                const auto ratio = metric == Metric::TokenUsage ? policy.maximumTokenRatio
                                                                : policy.maximumLatencyRatio;
                if (*newValue > *oldValue * ratio) {
                    issue(u"Candidate exceeds the relative cost budget"_s);
                }
            } else {
                const auto limit = policy.absoluteLimits.value(metric);
                if (lowerIsBetter(metric) ? *newValue > limit : *newValue < limit) {
                    issue(u"Candidate violates the absolute quality limit"_s);
                }
                const auto regression =
                    lowerIsBetter(metric) ? *newValue - *oldValue : *oldValue - *newValue;
                if (regression > policy.maximumQualityRegression) {
                    issue(u"Candidate regresses beyond the allowed quality tolerance"_s);
                }
            }
        }
    };
    inspect({}, baseline.aggregate, candidate.aggregate, true);
    QHash<QString, CaseMetrics> candidateCases;
    for (const auto& item : candidate.cases) {
        candidateCases.insert(item.caseId, item);
    }
    auto baselineCases = baseline.cases;
    std::sort(baselineCases.begin(), baselineCases.end(),
              [](const auto& a, const auto& b) { return a.caseId < b.caseId; });
    for (const auto& item : baselineCases) {
        if (!candidateCases.contains(item.caseId)) {
            return RegressionError{item.caseId, u"Case identities differ between runs"_s};
        }
        inspect(item.caseId, item.values, candidateCases.value(item.caseId).values, false);
    }
    report.hash =
        detail::hash(QJsonDocument(comparisonJson(report)).toJson(QJsonDocument::Compact));
    return report;
}

QByteArray ModelRegressionSuite::encodeRun(const RunReport& report) {
    auto object = runJson(report);
    object.insert(u"report_hash"_s, report.hash.toHex());
    return QJsonDocument(object).toJson(QJsonDocument::Indented);
}
RegressionResult<RunReport> ModelRegressionSuite::loadRun(const QByteArray& json) {
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(json, &parseError);
    const auto root = document.object();
    if (parseError.error != QJsonParseError::NoError || !document.isObject() || root.size() != 8 ||
        root.value(u"format"_s) != version() || !root.value(u"manifest"_s).isObject() ||
        !root.value(u"cases"_s).isArray() || !root.value(u"aggregate"_s).isObject() ||
        !root.value(u"capture_ids"_s).isArray() || !root.value(u"context_hashes"_s).isObject()) {
        return RegressionError{u"$"_s, u"Invalid saved regression report envelope"_s};
    }
    auto readHash = [](const QJsonValue& value) {
        return core::ContentHash::fromHex(value.toString());
    };
    auto readMetrics =
        [](const QJsonObject& object) -> std::optional<QMap<Metric, std::optional<double>>> {
        if (object.size() != metrics().size()) {
            return std::nullopt;
        }
        QMap<Metric, std::optional<double>> result;
        for (const auto metric : metrics()) {
            const auto value = object.value(metricName(metric));
            if (!object.contains(metricName(metric)) ||
                (!value.isNull() && (!value.isDouble() || !std::isfinite(value.toDouble())))) {
                return std::nullopt;
            }
            result.insert(metric, value.isNull() ? std::nullopt : std::optional(value.toDouble()));
        }
        return result;
    };
    const auto manifest = root.value(u"manifest"_s).toObject();
    const auto identity = manifest.value(u"model"_s).toObject();
    const auto corpus = readHash(manifest.value(u"corpus_hash"_s));
    const auto prompt = readHash(manifest.value(u"prompt_hash"_s));
    const auto schema = readHash(manifest.value(u"schema_hash"_s));
    const auto configuration = readHash(manifest.value(u"configuration_hash"_s));
    const auto captures = readHash(root.value(u"captures_hash"_s));
    const auto hash = readHash(root.value(u"report_hash"_s));
    const auto totals = readMetrics(root.value(u"aggregate"_s).toObject());
    if (manifest.size() != 7 || identity.size() != 3 || !corpus || !prompt || !schema ||
        !configuration || !captures || !hash || !totals) {
        return RegressionError{u"manifest"_s,
                               u"Invalid saved provenance, hashes or aggregate metrics"_s};
    }
    RunReport result;
    result.manifest = {
        manifest.value(u"run_id"_s).toString(),
        {identity.value(u"backend"_s).toString(), identity.value(u"model"_s).toString(),
         identity.value(u"revision"_s).toString()},
        *corpus,
        *prompt,
        *schema,
        *configuration,
        QDateTime::fromString(manifest.value(u"captured_at"_s).toString(), Qt::ISODateWithMs)};
    result.aggregate = *totals;
    result.capturesHash = *captures;
    result.hash = *hash;
    for (const auto& value : root.value(u"capture_ids"_s).toArray()) {
        if (!value.isString() || !core::LLMRunId::fromString(value.toString())) {
            return RegressionError{u"capture_ids"_s, u"Invalid capture identity"_s};
        }
        result.captureIds.append(value.toString());
    }
    const auto contexts = root.value(u"context_hashes"_s).toObject();
    for (auto it = contexts.begin(); it != contexts.end(); ++it) {
        const auto contextHash = readHash(it.value());
        if (!contextHash) {
            return RegressionError{u"context_hashes"_s, u"Invalid context hash"_s};
        }
        result.contextHashes.insert(it.key(), *contextHash);
    }
    for (const auto& value : root.value(u"cases"_s).toArray()) {
        const auto object = value.toObject();
        const auto values = readMetrics(object.value(u"metrics"_s).toObject());
        if (!value.isObject() || object.size() != 2 || !object.value(u"case_id"_s).isString() ||
            !values) {
            return RegressionError{u"cases"_s, u"Invalid saved per-case metrics"_s};
        }
        result.cases.append({object.value(u"case_id"_s).toString(), *values});
    }
    if (!validRun(result)) {
        return RegressionError{u"$"_s,
                               u"Saved regression report is stale, inconsistent or tampered"_s};
    }
    return result;
}
QByteArray ModelRegressionSuite::encodeComparison(const ComparisonReport& report) {
    auto object = comparisonJson(report);
    object.insert(u"comparison_hash"_s, report.hash.toHex());
    return QJsonDocument(object).toJson(QJsonDocument::Indented);
}
RegressionResult<PromotionAuthorization>
ModelRegressionSuite::authorize(const ComparisonReport& report, const RegressionReview& review,
                                const core::ContentHash& currentBaselineRunHash) {
    const auto recomputed = compare(report.baseline, report.candidate, report.policy);
    if (std::holds_alternative<RegressionError>(recomputed) || !report.hash.isValid() ||
        std::get<ComparisonReport>(recomputed).hash != report.hash ||
        !std::get<ComparisonReport>(recomputed).issues.isEmpty() || !report.issues.isEmpty() ||
        currentBaselineRunHash != report.baseline.hash || !review.approved ||
        review.comparisonHash != report.hash || !detail::text(review.reviewer) ||
        !detail::text(review.reason) || !review.reviewedAt.isValid() ||
        review.reviewedAt < report.baseline.manifest.capturedAt ||
        review.reviewedAt < report.candidate.manifest.capturedAt) {
        return RegressionError{
            u"review"_s,
            u"Promotion requires a passing, unchanged comparison and explicit scoped human review"_s};
    }
    return PromotionAuthorization{report.baseline.manifest.identity,
                                  report.candidate.manifest.identity, report.hash, review.reviewer,
                                  review.reason};
}
} // namespace loreforge::regression
