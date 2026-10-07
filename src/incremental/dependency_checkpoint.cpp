#include "loreforge/incremental/dependency_graph.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

using namespace Qt::StringLiterals;

namespace loreforge::incremental {

QByteArray DependencyGraph::checkpoint() const {
    QJsonArray entries;
    for (const auto& id : order_) {
        const auto& snapshot = records_[id].snapshot;
        QJsonArray dependencies;
        for (const auto& dependency : snapshot.spec.dependencies) {
            dependencies.append(dependency);
        }
        QJsonObject entry{
            {u"id"_s, id},
            {u"kind"_s, static_cast<int>(snapshot.spec.kind)},
            {u"input_hash"_s, snapshot.spec.inputHash.toHex()},
            {u"dependencies"_s, dependencies},
            {u"state"_s, snapshot.state == ArtifactState::Clean ? u"clean"_s : u"dirty"_s}};
        if (snapshot.state == ArtifactState::Clean) {
            entry.insert(u"output_hash"_s, snapshot.outputHash->toHex());
            entry.insert(u"built_fingerprint"_s, snapshot.builtFingerprint->toHex());
        }
        entries.append(entry);
    }
    return QJsonDocument(QJsonObject{{u"format"_s, u"loreforge-dependency-checkpoint-v1"_s},
                                     {u"artifacts"_s, entries}})
        .toJson(QJsonDocument::Compact);
}

GraphStatus DependencyGraph::restoreCheckpoint(const QByteArray& json) {
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(json, &parseError);
    const auto root = document.object();
    if (parseError.error != QJsonParseError::NoError || !document.isObject() || root.size() != 2 ||
        root.value(u"format"_s) != u"loreforge-dependency-checkpoint-v1"_s ||
        !root.value(u"artifacts"_s).isArray()) {
        return GraphError{{}, u"Invalid dependency checkpoint format"_s};
    }
    const auto entries = root.value(u"artifacts"_s).toArray();
    if (entries.size() != records_.size()) {
        return GraphError{{}, u"Checkpoint topology differs from the current graph"_s};
    }
    QList<ArtifactSpec> specs;
    QHash<QString, QJsonObject> receipts;
    for (const auto& id : order_) {
        const auto& record = records_[id];
        if (record.pending) {
            return GraphError{id, u"Cannot restore while a build is running"_s};
        }
        specs.append(record.snapshot.spec);
    }
    for (const auto& value : entries) {
        const auto entry = value.toObject();
        const auto id = entry.value(u"id"_s).toString();
        const auto found = records_.constFind(id);
        if (!value.isObject() || found == records_.cend() || receipts.contains(id)) {
            return GraphError{id, u"Unknown or duplicate checkpoint artifact"_s};
        }
        const auto& spec = found->snapshot.spec;
        QJsonArray dependencies;
        for (const auto& dependency : spec.dependencies) {
            dependencies.append(dependency);
        }
        const auto state = entry.value(u"state"_s).toString();
        if (entry.value(u"kind"_s) != QJsonValue(static_cast<int>(spec.kind)) ||
            entry.value(u"input_hash"_s) != QJsonValue(spec.inputHash.toHex()) ||
            entry.value(u"dependencies"_s) != dependencies ||
            (state != u"clean"_s && state != u"dirty"_s) ||
            entry.size() != (state == u"clean"_s ? 7 : 5)) {
            return GraphError{id, u"Checkpoint specification or state is incompatible"_s};
        }
        receipts.insert(id, entry);
    }
    DependencyGraph candidate;
    if (const auto error = candidate.synchronize(specs)) {
        return error;
    }
    for (const auto& id : order_) {
        const auto& entry = receipts[id];
        const auto source = records_[id].snapshot.spec.kind == ArtifactKind::Source;
        if (entry.value(u"state"_s) == u"dirty"_s) {
            if (source) {
                return GraphError{id, u"Source receipts must be clean"_s};
            }
            continue;
        }
        const auto output = core::ContentHash::fromHex(entry.value(u"output_hash"_s).toString());
        const auto built =
            core::ContentHash::fromHex(entry.value(u"built_fingerprint"_s).toString());
        if (!output || !built) {
            return GraphError{id, u"Checkpoint contains an invalid hash"_s};
        }
        if (source) {
            if (*output != records_[id].snapshot.spec.inputHash || *built != *output) {
                return GraphError{id, u"Source receipt does not match its projection"_s};
            }
            continue;
        }
        const auto started = candidate.beginBuild(id);
        if (std::holds_alternative<GraphError>(started) ||
            std::get<BuildTicket>(started).inputFingerprint != *built) {
            return GraphError{id, u"Checkpoint depends on stale or unavailable inputs"_s};
        }
        if (const auto error = candidate.completeBuild(std::get<BuildTicket>(started), *output)) {
            return error;
        }
    }
    records_ = std::move(candidate.records_);
    return std::nullopt;
}

} // namespace loreforge::incremental
