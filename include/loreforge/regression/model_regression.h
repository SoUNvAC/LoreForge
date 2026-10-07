#pragma once

#include "loreforge/core/content_hash.h"
#include "loreforge/llm/llm_types.h"
#include "loreforge/narrative/narrative_types.h"

#include <QDateTime>
#include <QMap>

#include <optional>
#include <variant>

namespace loreforge::regression {

struct RegressionError final {
    QString path;
    QString message;
};
template <typename T> using RegressionResult = std::variant<T, RegressionError>;

// Closed, human-reviewed label sets. Optional allowed labels do not count as false positives.
struct GoldenLabels final {
    QStringList required;
    QStringList allowed;
};
struct ProofreadingEdit final {
    core::SourceSpan span;
    QString original;
    QString replacement;
    friend bool operator==(const ProofreadingEdit&, const ProofreadingEdit&) = default;
};
struct GoldenCase final {
    QString id;
    QStringList tags;
    QByteArray sourceUtf8;
    core::ContentHash sourceHash;
    narrative::ChapterSegmentation segmentation;
    GoldenLabels entities;
    GoldenLabels events;
    GoldenLabels context;
    QList<ProofreadingEdit> edits;
};
struct GoldenCorpus final {
    QString id;
    QString version;
    QList<GoldenCase> cases;
    core::ContentHash hash;
};

struct ModelIdentity final {
    QString backend;
    QString model;
    QString revision; // Exact weights/backend revision, not only a mutable alias.
    friend bool operator==(const ModelIdentity&, const ModelIdentity&) = default;
};
struct RunManifest final {
    QString runId;
    ModelIdentity identity;
    core::ContentHash corpusHash;
    core::ContentHash promptHash;
    core::ContentHash schemaHash;
    core::ContentHash configurationHash;
    QDateTime capturedAt;
};
struct CaseObservation final {
    QString caseId;
    core::LLMRunId captureId;
    core::ContentHash sourceHash;
    core::ContentHash contextHash;
    core::ContentHash responseHash; // Retained capture receipt, not a claim of authenticity.
    narrative::ChapterSegmentation segmentation;
    QStringList entities;
    QStringList events;
    QStringList contextAssertions;
    QList<ProofreadingEdit> edits;
    std::optional<llm::TokenUsage> usage; // Missing is not zero.
    std::optional<qint64> latencyMs;
};

enum class Metric {
    DialogueAccuracy,
    SpeakerAccuracy,
    EntityF1,
    EventF1,
    HallucinationRate,
    ContextConsistency,
    ProofreadingPrecision,
    ProofreadingRecall,
    TokenUsage,
    Latency
};
[[nodiscard]] QString metricName(Metric metric);
[[nodiscard]] QList<Metric> metrics();

struct CaseMetrics final {
    QString caseId;
    QMap<Metric, std::optional<double>> values;
};
struct RunReport final {
    RunManifest manifest;
    QList<CaseMetrics> cases;
    QMap<Metric, std::optional<double>> aggregate;
    core::ContentHash capturesHash;
    QStringList captureIds;
    QMap<QString, core::ContentHash> contextHashes;
    core::ContentHash hash;
};
struct RegressionPolicy final {
    // Minimums for higher-is-better quality; maximums for hallucination rate.
    QMap<Metric, double> absoluteLimits;
    double maximumQualityRegression = 0.02;
    double maximumTokenRatio = 1.25;
    double maximumLatencyRatio = 1.50;
};
struct RegressionIssue final {
    QString caseId; // Empty = aggregate/provenance.
    QString metric;
    QString reason;
};
struct ComparisonReport final {
    RunReport baseline;
    RunReport candidate;
    RegressionPolicy policy;
    QList<RegressionIssue> issues;
    core::ContentHash hash;
};
struct RegressionReview final {
    core::ContentHash comparisonHash;
    QString reviewer;
    QString reason;
    QDateTime reviewedAt;
    bool approved = false;
};
struct PromotionAuthorization final {
    ModelIdentity baseline;
    ModelIdentity candidate;
    core::ContentHash comparisonHash;
    QString reviewer;
    QString reason;
};

class ModelRegressionSuite final {
  public:
    [[nodiscard]] static QString version();
    [[nodiscard]] static RegressionPolicy defaultPolicy();
    [[nodiscard]] static RegressionResult<GoldenCorpus> loadCorpus(const QByteArray& json);
    [[nodiscard]] static RegressionResult<RunReport>
    evaluate(const GoldenCorpus& corpus, const RunManifest& manifest,
             const QList<CaseObservation>& observations);
    [[nodiscard]] static RegressionResult<ComparisonReport>
    compare(const RunReport& baseline, const RunReport& candidate, const RegressionPolicy& policy);
    [[nodiscard]] static QByteArray encodeRun(const RunReport& report);
    [[nodiscard]] static RegressionResult<RunReport> loadRun(const QByteArray& json);
    [[nodiscard]] static QByteArray encodeComparison(const ComparisonReport& report);
    // Returns a receipt only. Never writes active model configuration or approves its own run.
    [[nodiscard]] static RegressionResult<PromotionAuthorization>
    authorize(const ComparisonReport& report, const RegressionReview& review,
              const core::ContentHash& currentBaselineRunHash);
};

} // namespace loreforge::regression
