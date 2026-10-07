#pragma once

#include "loreforge/git/semantic_diff.h"
#include "loreforge/incremental/dependency_graph.h"

namespace loreforge::incremental {

struct PipelineVersions final {
    QString parserVersion;
    QString analyzerVersion;
    QString modelVersion;
    QString promptVersion;
    QString schemaVersion;
    QString storyStateVersion;
    // Canonical hash of inference parameters, context policy and other relevant settings.
    core::ContentHash configurationHash;
    bool analysisUsesPreviousStoryState = false;
};

class ChapterPipeline final {
  public:
    [[nodiscard]] static QString artifactId(const core::ProjectId& projectId,
                                            const core::ChapterId& chapterId, ArtifactKind kind);
    // Revision chapters must have a canonical, contiguous zero-based novel sequence.
    [[nodiscard]] static GraphResult<QList<ArtifactSpec>>
    specifications(const core::ProjectId& projectId, const git::SourceRevision& revision,
                   const PipelineVersions& versions);
    // Bridge Phase 18 invalidation plans. Structural ambiguity fails closed without mutation.
    [[nodiscard]] static GraphStatus invalidate(DependencyGraph& graph,
                                                const core::ProjectId& projectId,
                                                const git::SourceRevision& canonicalRevision,
                                                const git::AnalysisInvalidationPlan& plan);
};

} // namespace loreforge::incremental
