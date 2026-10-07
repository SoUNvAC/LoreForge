#include "loreforge/incremental/chapter_pipeline.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>

using namespace Qt::StringLiterals;

namespace loreforge::incremental {
namespace {
core::ContentHash hash(const QJsonArray& values) {
    const auto bytes = QJsonDocument(values).toJson(QJsonDocument::Compact);
    return core::ContentHash::sha256(QByteArrayView(bytes));
}

GraphStatus validateRevision(const core::ProjectId& projectId,
                             const git::SourceRevision& revision) {
    if (!projectId.isValid()) {
        return GraphError{{}, u"Invalid project identifier"_s};
    }
    const auto validated = git::SemanticDiffer::compare(revision, revision);
    if (const auto* error = std::get_if<git::GitError>(&validated)) {
        return GraphError{{}, error->message};
    }
    for (qsizetype sequence = 0; sequence < revision.chapters.size(); ++sequence) {
        if (revision.chapters[sequence].sequence != sequence) {
            return GraphError{{}, u"Pipeline requires a contiguous canonical chapter sequence"_s};
        }
    }
    return std::nullopt;
}
} // namespace

QString ChapterPipeline::artifactId(const core::ProjectId& projectId,
                                    const core::ChapterId& chapterId, ArtifactKind kind) {
    QString suffix;
    switch (kind) {
    case ArtifactKind::Source:
        suffix = u"source"_s;
        break;
    case ArtifactKind::Parse:
        suffix = u"parse"_s;
        break;
    case ArtifactKind::Analysis:
        suffix = u"analysis"_s;
        break;
    case ArtifactKind::StoryState:
        suffix = u"story"_s;
        break;
    case ArtifactKind::Context:
        suffix = u"context"_s;
        break;
    case ArtifactKind::Diagnostics:
        suffix = u"diagnostics"_s;
        break;
    default:
        return {};
    }
    if (!projectId.isValid() || !chapterId.isValid()) {
        return {};
    }
    return projectId.toString() + u"/"_s + chapterId.toString() + u"/"_s + suffix;
}

GraphResult<QList<ArtifactSpec>>
ChapterPipeline::specifications(const core::ProjectId& projectId,
                                const git::SourceRevision& revision,
                                const PipelineVersions& versions) {
    if (const auto error = validateRevision(projectId, revision)) {
        return *error;
    }
    for (const auto& version :
         {versions.parserVersion, versions.analyzerVersion, versions.modelVersion,
          versions.promptVersion, versions.schemaVersion, versions.storyStateVersion}) {
        if (version.trimmed().isEmpty()) {
            return GraphError{{}, u"Every pipeline version must be explicit"_s};
        }
    }
    if (!versions.configurationHash.isValid()) {
        return GraphError{{}, u"Configuration hash is required"_s};
    }
    const auto parseRecipe = hash({u"parse-v1"_s, versions.parserVersion});
    const auto analysisRecipe =
        hash({u"analysis-v1"_s, versions.analyzerVersion, versions.modelVersion,
              versions.promptVersion, versions.schemaVersion, versions.configurationHash.toHex(),
              versions.analysisUsesPreviousStoryState});
    const auto storyRecipe = hash({u"story-v1"_s, versions.storyStateVersion});
    QList<ArtifactSpec> specs;
    QString previousStory;
    for (const auto& chapter : revision.chapters) {
        const auto source = artifactId(projectId, chapter.id, ArtifactKind::Source);
        const auto parse = artifactId(projectId, chapter.id, ArtifactKind::Parse);
        const auto analysis = artifactId(projectId, chapter.id, ArtifactKind::Analysis);
        const auto story = artifactId(projectId, chapter.id, ArtifactKind::StoryState);
        const auto& span = chapter.sourceSpan;
        const auto bytes = QByteArrayView(revision.utf8).sliced(span.startByte, span.lengthBytes());
        // Exact byte ranges participate in provenance. No unrelated full-novel hash dependency.
        const auto projection =
            hash({u"chapter-projection-v1"_s, revision.sourceId, QString::number(span.startByte),
                  QString::number(span.endByte), core::ContentHash::sha256(bytes).toHex()});
        specs.append({source, ArtifactKind::Source, projection, {}});
        specs.append({parse, ArtifactKind::Parse, parseRecipe, {source}});
        QStringList analysisDependencies{parse};
        if (versions.analysisUsesPreviousStoryState && !previousStory.isEmpty()) {
            analysisDependencies.append(previousStory);
        }
        specs.append({analysis, ArtifactKind::Analysis, analysisRecipe, analysisDependencies});
        QStringList storyDependencies{analysis};
        if (!previousStory.isEmpty()) {
            storyDependencies.append(previousStory);
        }
        specs.append({story, ArtifactKind::StoryState, storyRecipe, storyDependencies});
        previousStory = story;
    }
    return specs;
}

GraphStatus ChapterPipeline::invalidate(DependencyGraph& graph, const core::ProjectId& projectId,
                                        const git::SourceRevision& canonicalRevision,
                                        const git::AnalysisInvalidationPlan& plan) {
    if (const auto error = validateRevision(projectId, canonicalRevision)) {
        return error;
    }
    if (plan.requiresChapterRemap) {
        return GraphError{{},
                          u"Resolve canonical chapter remapping before incremental invalidation"_s};
    }
    if (plan.storyStateSnapshotsDirtyFrom &&
        (*plan.storyStateSnapshotsDirtyFrom < 0 ||
         *plan.storyStateSnapshotsDirtyFrom >= canonicalRevision.chapters.size())) {
        return GraphError{{}, u"Invalid story-state cutoff"_s};
    }
    QSet<core::ChapterId> known;
    for (const auto& chapter : canonicalRevision.chapters) {
        known.insert(chapter.id);
    }
    QStringList roots;
    // Provenance shifts require reparsing even if the chapter's words have not changed.
    for (const auto& id : plan.analysisChapters + plan.provenanceChapters) {
        if (!known.contains(id)) {
            return GraphError{id.toString(), u"Invalidation chapter is outside the revision"_s};
        }
        const auto root = artifactId(projectId, id, ArtifactKind::Parse);
        if (!roots.contains(root)) {
            roots.append(root);
        }
    }
    if (plan.storyStateSnapshotsDirtyFrom) {
        for (const auto& chapter : canonicalRevision.chapters) {
            if (chapter.sequence >= *plan.storyStateSnapshotsDirtyFrom) {
                roots.append(artifactId(projectId, chapter.id, ArtifactKind::StoryState));
            }
        }
    }
    return graph.invalidate(roots);
}

} // namespace loreforge::incremental
