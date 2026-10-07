#include "loreforge/incremental/chapter_pipeline.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>
#include <stdexcept>

using namespace Qt::StringLiterals;
using namespace loreforge;
using namespace loreforge::incremental;

namespace {
core::ContentHash hash(const QString& text) {
    return core::ContentHash::sha256(QStringView(text));
}
const auto project = core::ProjectId::fromStableKey(u"incremental-project"_s);

git::SourceRevision revision(const QList<QByteArray>& contents) {
    git::SourceRevision result;
    result.sourceId = u"novel.txt"_s;
    for (qsizetype i = 0; i < contents.size(); ++i) {
        const auto start = result.utf8.size();
        result.utf8.append(contents[i]);
        result.chapters.append({core::ChapterId::fromStableKey(u"chapter-%1"_s.arg(i)),
                                i,
                                {result.sourceId, start, result.utf8.size()},
                                std::nullopt});
    }
    result.sourceHash = core::ContentHash::sha256(QByteArrayView(result.utf8));
    return result;
}

PipelineVersions versions() {
    return {u"parser-1"_s, u"analyzer-1"_s, u"model-1"_s,          u"prompt-1"_s,
            u"schema-1"_s, u"story-1"_s,    hash(u"settings-1"_s), false};
}

QList<ArtifactSpec> specs(const git::SourceRevision& source, const PipelineVersions& config) {
    return std::get<QList<ArtifactSpec>>(ChapterPipeline::specifications(project, source, config));
}

QString id(const git::SourceRevision& source, qsizetype index, ArtifactKind kind) {
    return ChapterPipeline::artifactId(project, source.chapters[index].id, kind);
}

GraphResult<core::ContentHash> execute(const BuildTicket& ticket) {
    // A deterministic worker result with its dependency receipt, without a network call.
    return hash(ticket.artifactId + ticket.inputFingerprint.toHex());
}

QStringList plan(const DependencyGraph& graph, const QStringList& targets = {}) {
    return std::get<QStringList>(graph.rebuildPlan(targets));
}
} // namespace

class IncrementalAnalysisTest final : public QObject {
    Q_OBJECT
  private slots:
    void selectiveRebuildAndNoOp();
    void sourceAndProvenanceChanges();
    void contextDependenciesPropagate();
    void recipeChangesInvalidateOnlyConsumers();
    void rejectsTopologyAtomically();
    void rejectsStaleAndForeignCompletion();
    void failureRetryAndDirtyPrerequisites();
    void semanticInvalidationBridge();
    void validatesPipelineAndStructuralChanges();
    void fingerprintsBindOrderedDependencyOutputs();
    void persistsVerifiedDirtyAndCleanReceipts();
    void reordersCanonicalChaptersAndHandlesEmptyGraphs();
};

void IncrementalAnalysisTest::selectiveRebuildAndNoOp() {
    const auto source = revision({"first\n", "second\n", "third\n"});
    DependencyGraph graph;
    QVERIFY(!graph.synchronize(specs(source, versions())));
    QCOMPARE(plan(graph).size(), 9);
    const auto first = graph.rebuild({id(source, 1, ArtifactKind::Analysis)}, execute);
    QVERIFY(!first.error);
    QCOMPARE(first.rebuilt, (QStringList{id(source, 1, ArtifactKind::Parse),
                                         id(source, 1, ArtifactKind::Analysis)}));
    QVERIFY(graph.artifact(id(source, 0, ArtifactKind::Parse))->state == ArtifactState::Dirty);
    const auto all = graph.rebuild({}, execute);
    QVERIFY(!all.error);
    QCOMPARE(all.rebuilt.size(), 7);
    QVERIFY(plan(graph).isEmpty());
    const auto snapshot = graph.artifact(id(source, 2, ArtifactKind::StoryState));
    QVERIFY(snapshot->outputHash && snapshot->builtFingerprint);
    QVERIFY(!graph.synchronize(specs(source, versions())));
    QVERIFY(graph.rebuild({}, execute).rebuilt.isEmpty());
    QCOMPARE(graph.artifact(snapshot->spec.id)->outputHash, snapshot->outputHash);
}

void IncrementalAnalysisTest::sourceAndProvenanceChanges() {
    const auto before = revision({"first\n", "second\n", "third\n"});
    DependencyGraph graph;
    QVERIFY(!graph.synchronize(specs(before, versions())));
    QVERIFY(!graph.rebuild({}, execute).error);
    const auto originalFirst = graph.artifact(id(before, 0, ArtifactKind::Analysis))->outputHash;
    // Same-length edit does not shift chapter 3's evidence or rerun its local analysis.
    const auto after = revision({"first\n", "SECOND\n", "third\n"});
    QVERIFY(!graph.synchronize(specs(after, versions())));
    const auto dirty = plan(graph);
    QCOMPARE(dirty.size(), 4);
    QVERIFY(dirty.contains(id(after, 1, ArtifactKind::Parse)));
    QVERIFY(dirty.contains(id(after, 1, ArtifactKind::Analysis)));
    QVERIFY(dirty.contains(id(after, 1, ArtifactKind::StoryState)));
    QVERIFY(dirty.contains(id(after, 2, ArtifactKind::StoryState)));
    QVERIFY(!dirty.contains(id(after, 2, ArtifactKind::Analysis)));
    QVERIFY(!graph.rebuild({id(after, 2, ArtifactKind::StoryState)}, execute).error);
    QCOMPARE(graph.artifact(id(before, 0, ArtifactKind::Analysis))->outputHash, originalFirst);
    // A length-changing UTF-8 edit must invalidate downstream absolute evidence offsets.
    const auto shifted =
        revision({"first\n", QString::fromUtf8("第二章新增文字\n").toUtf8(), "third\n"});
    QVERIFY(!graph.synchronize(specs(shifted, versions())));
    QCOMPARE(plan(graph).size(), 6);
    QVERIFY(plan(graph).contains(id(shifted, 2, ArtifactKind::Parse)));
    QCOMPARE(graph.artifact(id(shifted, 0, ArtifactKind::Analysis))->outputHash, originalFirst);
}

void IncrementalAnalysisTest::contextDependenciesPropagate() {
    const auto before = revision({"first\n", "second\n", "third\n"});
    auto config = versions();
    config.analysisUsesPreviousStoryState = true;
    DependencyGraph graph;
    QVERIFY(!graph.synchronize(specs(before, config)));
    QVERIFY(!graph.rebuild({}, execute).error);
    const auto after = revision({"FIRST\n", "second\n", "third\n"});
    QVERIFY(!graph.synchronize(specs(after, config)));
    QCOMPARE(plan(graph).size(), 7);
    QVERIFY(plan(graph).contains(id(after, 2, ArtifactKind::Analysis)));
    QVERIFY(!plan(graph).contains(id(after, 2, ArtifactKind::Parse)));
    const auto rebuilt = graph.rebuild({id(after, 2, ArtifactKind::Analysis)}, execute);
    QVERIFY(!rebuilt.error);
    // The final story snapshot is not required by its own chapter analysis.
    QCOMPARE(plan(graph), (QStringList{id(after, 2, ArtifactKind::StoryState)}));
}

void IncrementalAnalysisTest::recipeChangesInvalidateOnlyConsumers() {
    const auto source = revision({"one\n", "two\n"});
    DependencyGraph graph;
    auto config = versions();
    QVERIFY(!graph.synchronize(specs(source, config)));
    QVERIFY(!graph.rebuild({}, execute).error);
    config.promptVersion = u"prompt-2"_s;
    QVERIFY(!graph.synchronize(specs(source, config)));
    QCOMPARE(plan(graph).size(), 4);
    QVERIFY(!plan(graph).contains(id(source, 0, ArtifactKind::Parse)));
    QVERIFY(!graph.rebuild({}, execute).error);
    config.storyStateVersion = u"story-2"_s;
    QVERIFY(!graph.synchronize(specs(source, config)));
    QCOMPARE(plan(graph).size(), 2);
    QVERIFY(!graph.rebuild({}, execute).error);
    config.configurationHash = hash(u"temperature-and-context-change"_s);
    QVERIFY(!graph.synchronize(specs(source, config)));
    QCOMPARE(plan(graph).size(), 4);
    QVERIFY(!graph.rebuild({}, execute).error);
    config.parserVersion = u"parser-2"_s;
    QVERIFY(!graph.synchronize(specs(source, config)));
    QCOMPARE(plan(graph).size(), 6);
}

void IncrementalAnalysisTest::rejectsTopologyAtomically() {
    DependencyGraph graph;
    const QList<ArtifactSpec> valid{
        {u"source"_s, ArtifactKind::Source, hash(u"text"_s), {}},
        {u"parse"_s, ArtifactKind::Parse, hash(u"v1"_s), {u"source"_s}}};
    QVERIFY(!graph.synchronize(valid));
    QVERIFY(!graph.rebuild({}, execute).error);
    auto invalid = valid;
    invalid.append(valid.first());
    QVERIFY(graph.synchronize(invalid));
    invalid = valid;
    invalid[1].dependencies = {u"missing"_s};
    QVERIFY(graph.synchronize(invalid));
    invalid[1].dependencies = {u"parse"_s};
    QVERIFY(graph.synchronize(invalid));
    invalid[1].dependencies = {u"source"_s, u"source"_s};
    QVERIFY(graph.synchronize(invalid));
    invalid = valid;
    invalid[0].dependencies = {u"parse"_s};
    QVERIFY(graph.synchronize(invalid));
    invalid = valid;
    invalid[1].inputHash = {};
    QVERIFY(graph.synchronize(invalid));
    invalid = valid;
    invalid[1].kind = static_cast<ArtifactKind>(999);
    QVERIFY(graph.synchronize(invalid));
    QVERIFY(plan(graph).isEmpty());
    QVERIFY(graph.invalidate({u"parse"_s, u"missing"_s}));
    QVERIFY(plan(graph).isEmpty());
    QVERIFY(graph.invalidate({u"source"_s}));
    QVERIFY(std::holds_alternative<GraphError>(graph.rebuildPlan({u"missing"_s})));
}

void IncrementalAnalysisTest::rejectsStaleAndForeignCompletion() {
    const auto source = revision({"first\n"});
    DependencyGraph graph;
    QVERIFY(!graph.synchronize(specs(source, versions())));
    const auto parse = id(source, 0, ArtifactKind::Parse);
    const auto ticket = std::get<BuildTicket>(graph.beginBuild(parse));
    QVERIFY(std::holds_alternative<GraphError>(graph.beginBuild(parse)));
    QVERIFY(graph.completeBuild(ticket, {}));
    QCOMPARE(graph.artifact(parse)->state, ArtifactState::Building);
    QVERIFY(!graph.synchronize(specs(source, versions())));
    QVERIFY(!graph.completeBuild(ticket, hash(u"output"_s)));
    QVERIFY(graph.completeBuild(ticket, hash(u"output"_s)));
    QVERIFY(!graph.invalidate({parse}));
    const auto obsolete = std::get<BuildTicket>(graph.beginBuild(parse));
    QVERIFY(!graph.invalidate({parse}));
    const auto current = std::get<BuildTicket>(graph.beginBuild(parse));
    QVERIFY(graph.completeBuild(obsolete, hash(u"old"_s)));
    QVERIFY(graph.failBuild(obsolete));
    auto forged = current;
    forged.inputFingerprint = hash(u"wrong"_s);
    QVERIFY(graph.completeBuild(forged, hash(u"bad"_s)));
    DependencyGraph other;
    QVERIFY(!other.synchronize(specs(source, versions())));
    const auto foreign = std::get<BuildTicket>(other.beginBuild(parse));
    QVERIFY(graph.completeBuild(foreign, hash(u"foreign"_s)));
    // An input change clears an in-flight ticket even if source later returns to old bytes.
    const auto changed = revision({"FIRST\n"});
    QVERIFY(!graph.synchronize(specs(changed, versions())));
    QVERIFY(!graph.synchronize(specs(source, versions())));
    QVERIFY(graph.completeBuild(current, hash(u"stale"_s)));
    QVERIFY(!graph.rebuild({}, execute).error);
}

void IncrementalAnalysisTest::failureRetryAndDirtyPrerequisites() {
    const auto source = revision({"one\n", "two\n"});
    DependencyGraph graph;
    QVERIFY(!graph.synchronize(specs(source, versions())));
    QVERIFY(std::holds_alternative<GraphError>(
        graph.beginBuild(id(source, 0, ArtifactKind::Analysis))));
    const auto target = id(source, 0, ArtifactKind::StoryState);
    const auto failed =
        graph.rebuild({target}, [&](const BuildTicket& ticket) -> GraphResult<core::ContentHash> {
            if (ticket.artifactId == id(source, 0, ArtifactKind::Analysis)) {
                return GraphError{ticket.artifactId, u"Worker failed"_s};
            }
            return execute(ticket);
        });
    QVERIFY(failed.error);
    QCOMPARE(failed.rebuilt, (QStringList{id(source, 0, ArtifactKind::Parse)}));
    QCOMPARE(graph.artifact(id(source, 0, ArtifactKind::Analysis))->state, ArtifactState::Dirty);
    const auto retried = graph.rebuild({target}, execute);
    QVERIFY(!retried.error);
    QCOMPARE(retried.rebuilt.size(), 2);
    const auto interrupted =
        graph.rebuild({id(source, 1, ArtifactKind::Analysis)},
                      [&](const BuildTicket& ticket) -> GraphResult<core::ContentHash> {
                          (void)graph.invalidate({ticket.artifactId});
                          return execute(ticket);
                      });
    QVERIFY(interrupted.error);
    QVERIFY(interrupted.rebuilt.isEmpty());
    const auto exception =
        graph.rebuild({}, [](const BuildTicket&) -> GraphResult<core::ContentHash> {
            throw std::runtime_error("private provider information");
        });
    QVERIFY(exception.error);
    QVERIFY(!exception.error->message.contains(u"private"_s));
    QVERIFY(graph.rebuild({}, {}).error);
    QVERIFY(!graph.rebuild({}, execute).error);
}

void IncrementalAnalysisTest::semanticInvalidationBridge() {
    const auto before = revision({"one\n", "two\n", "three\n"});
    const auto after = revision({"one\n", "TWO\n", "three\n"});
    const auto diff =
        std::get<git::SemanticDiffReport>(git::SemanticDiffer::compare(before, after));
    DependencyGraph graph;
    QVERIFY(!graph.synchronize(specs(before, versions())));
    QVERIFY(!graph.rebuild({}, execute).error);
    QVERIFY(!ChapterPipeline::invalidate(graph, project, before, diff.invalidation));
    QCOMPARE(plan(graph).size(), 4);
    QVERIFY(!graph.synchronize(specs(after, versions())));
    QCOMPARE(plan(graph).size(), 4);
    QVERIFY(!graph.rebuild({}, execute).error);
    auto invalid = diff.invalidation;
    invalid.requiresChapterRemap = true;
    QVERIFY(ChapterPipeline::invalidate(graph, project, after, invalid));
    invalid.requiresChapterRemap = false;
    invalid.analysisChapters.append(core::ChapterId::fromStableKey(u"outside"_s));
    QVERIFY(ChapterPipeline::invalidate(graph, project, after, invalid));
    invalid = diff.invalidation;
    invalid.storyStateSnapshotsDirtyFrom = -1;
    QVERIFY(ChapterPipeline::invalidate(graph, project, after, invalid));
    const auto otherProject = core::ProjectId::fromStableKey(u"other-project"_s);
    QVERIFY(ChapterPipeline::invalidate(graph, otherProject, after, diff.invalidation));
    QVERIFY(plan(graph).isEmpty());
}

void IncrementalAnalysisTest::validatesPipelineAndStructuralChanges() {
    auto source = revision({"one\n", "two\n"});
    QVERIFY(std::holds_alternative<GraphError>(
        ChapterPipeline::specifications({}, source, versions())));
    auto config = versions();
    config.modelVersion.clear();
    QVERIFY(std::holds_alternative<GraphError>(
        ChapterPipeline::specifications(project, source, config)));
    config = versions();
    config.configurationHash = {};
    QVERIFY(std::holds_alternative<GraphError>(
        ChapterPipeline::specifications(project, source, config)));
    auto broken = source;
    broken.sourceHash = hash(u"stale"_s);
    QVERIFY(std::holds_alternative<GraphError>(
        ChapterPipeline::specifications(project, broken, versions())));
    broken = source;
    broken.chapters[1].sequence = 7;
    QVERIFY(std::holds_alternative<GraphError>(
        ChapterPipeline::specifications(project, broken, versions())));
    DependencyGraph graph;
    QVERIFY(!graph.synchronize(specs(source, versions())));
    QVERIFY(!graph.rebuild({}, execute).error);
    const auto deleted = id(source, 1, ArtifactKind::Analysis);
    const auto ticketSource = id(source, 1, ArtifactKind::Parse);
    QVERIFY(!graph.invalidate({ticketSource}));
    const auto ticket = std::get<BuildTicket>(graph.beginBuild(ticketSource));
    source = revision({"one\n"});
    QVERIFY(!graph.synchronize(specs(source, versions())));
    QVERIFY(!graph.artifact(deleted));
    QVERIFY(graph.completeBuild(ticket, hash(u"removed"_s)));
    QVERIFY(plan(graph).isEmpty());
    auto appended = revision({"one\n", "new\n"});
    QVERIFY(!graph.synchronize(specs(appended, versions())));
    QCOMPARE(plan(graph).size(), 3);
}

void IncrementalAnalysisTest::fingerprintsBindOrderedDependencyOutputs() {
    DependencyGraph graph;
    QList<ArtifactSpec> nodes{
        {u"a"_s, ArtifactKind::Source, hash(u"a"_s), {}},
        {u"b"_s, ArtifactKind::Source, hash(u"b"_s), {}},
        {u"merge"_s, ArtifactKind::Analysis, hash(u"v1"_s), {u"a"_s, u"b"_s}}};
    QVERIFY(!graph.synchronize(nodes));
    QVERIFY(!graph.rebuild({}, execute).error);
    const auto old = graph.artifact(u"merge"_s)->builtFingerprint;
    nodes[2].dependencies = {u"b"_s, u"a"_s};
    QVERIFY(!graph.synchronize(nodes));
    QVERIFY(!graph.rebuild({}, execute).error);
    QVERIFY(graph.artifact(u"merge"_s)->builtFingerprint != old);
    const auto reordered = graph.artifact(u"merge"_s)->builtFingerprint;
    nodes[0].inputHash = hash(u"updated-a"_s);
    QVERIFY(!graph.synchronize(nodes));
    QVERIFY(!graph.rebuild({}, execute).error);
    QVERIFY(graph.artifact(u"merge"_s)->builtFingerprint != reordered);
}

void IncrementalAnalysisTest::persistsVerifiedDirtyAndCleanReceipts() {
    const auto source = revision({"one\n", "two\n"});
    DependencyGraph graph;
    QVERIFY(!graph.synchronize(specs(source, versions())));
    QVERIFY(!graph.rebuild({id(source, 0, ArtifactKind::StoryState)}, execute).error);
    const auto parse = id(source, 1, ArtifactKind::Parse);
    const auto running = std::get<BuildTicket>(graph.beginBuild(parse));
    const auto checkpoint = graph.checkpoint();
    DependencyGraph restored;
    QVERIFY(!restored.synchronize(specs(source, versions())));
    QVERIFY(!restored.restoreCheckpoint(checkpoint));
    QCOMPARE(plan(restored).size(), 3);
    QCOMPARE(restored.artifact(parse)->state, ArtifactState::Dirty);
    QVERIFY(restored.completeBuild(running, hash(u"old-process"_s)));
    QVERIFY(!restored.rebuild({}, execute).error);
    const auto clean = restored.checkpoint();
    QVERIFY(!restored.restoreCheckpoint(clean));
    QVERIFY(plan(restored).isEmpty());
    QVERIFY(restored.restoreCheckpoint("not json"));
    auto root = QJsonDocument::fromJson(clean).object();
    auto entries = root.value(u"artifacts"_s).toArray();
    // Altering a derived output alone invalidates the dependent input fingerprint.
    for (qsizetype i = 0; i < entries.size(); ++i) {
        auto entry = entries[i].toObject();
        if (entry.value(u"id"_s) == parse) {
            entry.insert(u"output_hash"_s, hash(u"tampered"_s).toHex());
            entries[i] = entry;
        }
    }
    root.insert(u"artifacts"_s, entries);
    QVERIFY(restored.restoreCheckpoint(QJsonDocument(root).toJson()));
    QVERIFY(plan(restored).isEmpty());
    auto config = versions();
    config.modelVersion = u"model-2"_s;
    QVERIFY(!restored.synchronize(specs(source, config)));
    const auto dirty = plan(restored);
    QVERIFY(restored.restoreCheckpoint(clean));
    QCOMPARE(plan(restored), dirty);
    QVERIFY(!graph.failBuild(running));
    const auto newRunning = std::get<BuildTicket>(graph.beginBuild(parse));
    QVERIFY(graph.restoreCheckpoint(checkpoint));
    QVERIFY(!graph.failBuild(newRunning));
    // Clean results are never accepted over a dirty prerequisite.
    root = QJsonDocument::fromJson(clean).object();
    entries = root.value(u"artifacts"_s).toArray();
    for (qsizetype i = 0; i < entries.size(); ++i) {
        auto entry = entries[i].toObject();
        if (entry.value(u"id"_s) == parse) {
            entry.insert(u"state"_s, u"dirty"_s);
            entry.remove(u"output_hash"_s);
            entry.remove(u"built_fingerprint"_s);
            entries[i] = entry;
        }
    }
    root.insert(u"artifacts"_s, entries);
    QVERIFY(graph.restoreCheckpoint(QJsonDocument(root).toJson()));
}

void IncrementalAnalysisTest::reordersCanonicalChaptersAndHandlesEmptyGraphs() {
    const auto source = revision({"one\n", "two\n", "end\n"});
    DependencyGraph graph;
    QVERIFY(!graph.synchronize(specs(source, versions())));
    QVERIFY(!graph.rebuild({}, execute).error);
    auto swapped = revision({"one\n", "end\n", "two\n"});
    swapped.chapters[1].id = source.chapters[2].id;
    swapped.chapters[2].id = source.chapters[1].id;
    QVERIFY(!graph.synchronize(specs(swapped, versions())));
    QCOMPARE(plan(graph).size(), 6);
    QVERIFY(!graph.rebuild({}, execute).error);
    QVERIFY(!graph.synchronize({}));
    QVERIFY(plan(graph).isEmpty());
    DependencyGraph empty;
    QVERIFY(!empty.restoreCheckpoint(graph.checkpoint()));
    QVERIFY(!empty.rebuild({}, execute).error);
    QCOMPARE(std::get<QList<ArtifactSpec>>(
                 ChapterPipeline::specifications(project, revision({}), versions()))
                 .size(),
             0);
}

QTEST_GUILESS_MAIN(IncrementalAnalysisTest)
#include "incremental_analysis_test.moc"
