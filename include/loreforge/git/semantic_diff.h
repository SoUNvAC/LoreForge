#pragma once

#include "loreforge/git/git_repository.h"
#include "loreforge/narrative/chapter_analysis.h"

namespace loreforge::git {

enum class RevisionImpact { NoChange, TypoOnly, TextOnly, SemanticChange, Unknown };
[[nodiscard]] QString revisionImpactName(RevisionImpact impact);

struct RevisionChapter final {
    core::ChapterId id;
    qsizetype sequence = -1;
    core::SourceSpan sourceSpan;
    std::optional<narrative::ChapterAnalysis> analysis;
};

struct SourceRevision final {
    QString sourceId;
    QByteArray utf8;
    core::ContentHash sourceHash;
    QList<RevisionChapter> chapters;
};

struct ChangedSourceSpan final {
    core::SourceSpan before;
    core::SourceSpan after;
    QByteArray removedUtf8;
    QByteArray addedUtf8;
    RevisionImpact impact = RevisionImpact::Unknown;
    std::optional<core::ProofreadingCandidateId> candidateId;
};

struct ChapterRevisionImpact final {
    core::ChapterId chapterId;
    qsizetype sequence = -1;
    RevisionImpact impact = RevisionImpact::Unknown;
    QStringList affectedEntities;
    bool entityImpactUncertain = false;
    bool storySemanticsMayChange = true;
};

struct AnalysisInvalidationPlan final {
    QList<core::ChapterId> analysisChapters;
    QList<core::ChapterId> provenanceChapters;
    std::optional<qsizetype> storyStateSnapshotsDirtyFrom;
    bool storySemanticsMayChange = false;
    bool requiresChapterRemap = false;
};

struct SemanticDiffReport final {
    core::ContentHash beforeHash;
    core::ContentHash afterHash;
    RevisionImpact impact = RevisionImpact::NoChange;
    QList<ChangedSourceSpan> changedSpans;
    QList<ChapterRevisionImpact> changedChapters;
    AnalysisInvalidationPlan invalidation;
    bool conservativeSpanFallback = false;
};

class SemanticDiffer final {
  public:
    // Reviews must describe the before revision. Only complete, validated review coverage
    // can certify a text-only revision. Invalidation is a plan; no persisted data is deleted.
    [[nodiscard]] static GitResult<SemanticDiffReport>
    compare(const SourceRevision& before, const SourceRevision& after,
            const QList<proofreading::RepairQueueItem>& reviews = {});
};

} // namespace loreforge::git
