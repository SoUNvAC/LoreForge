#include "loreforge/incremental/dependency_graph.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QUuid>

#include <exception>
#include <set>

using namespace Qt::StringLiterals;

namespace loreforge::incremental {

DependencyGraph::DependencyGraph() : graphId_(QUuid::createUuid().toString()) {}

GraphStatus DependencyGraph::synchronize(const QList<ArtifactSpec>& specs) {
    QHash<QString, ArtifactSpec> proposed;
    for (const auto& spec : specs) {
        if (spec.id.trimmed().isEmpty() || proposed.contains(spec.id) ||
            !spec.inputHash.isValid() ||
            (spec.kind == ArtifactKind::Source && !spec.dependencies.isEmpty())) {
            return GraphError{spec.id, u"Invalid or duplicate artifact specification"_s};
        }
        switch (spec.kind) {
        case ArtifactKind::Source:
        case ArtifactKind::Parse:
        case ArtifactKind::Analysis:
        case ArtifactKind::StoryState:
        case ArtifactKind::Context:
        case ArtifactKind::Diagnostics:
            break;
        default:
            return GraphError{spec.id, u"Unknown artifact kind"_s};
        }
        proposed.insert(spec.id, spec);
    }
    QHash<QString, qsizetype> remaining;
    QHash<QString, QStringList> consumers;
    std::set<QString> ready;
    for (const auto& spec : specs) {
        QSet<QString> seen;
        for (const auto& dependency : spec.dependencies) {
            if (!proposed.contains(dependency) || seen.contains(dependency)) {
                return GraphError{spec.id, u"Missing or duplicate dependency"_s};
            }
            seen.insert(dependency);
            consumers[dependency].append(spec.id);
        }
        remaining.insert(spec.id, spec.dependencies.size());
        if (spec.dependencies.isEmpty()) {
            ready.insert(spec.id);
        }
    }
    QStringList order;
    while (!ready.empty()) {
        const auto id = *ready.begin();
        ready.erase(ready.begin());
        order.append(id);
        for (const auto& consumer : consumers.value(id)) {
            if (--remaining[consumer] == 0) {
                ready.insert(consumer);
            }
        }
    }
    if (order.size() != specs.size()) {
        return GraphError{{}, u"Dependency graph contains a cycle"_s};
    }

    QHash<QString, Record> replacement;
    QStringList changed;
    for (const auto& spec : specs) {
        auto previous = records_.constFind(spec.id);
        Record record;
        if (previous != records_.cend() && previous->snapshot.spec == spec) {
            record = *previous;
        } else {
            record.snapshot.spec = spec;
            changed.append(spec.id);
            if (spec.kind == ArtifactKind::Source) {
                record.snapshot.state = ArtifactState::Clean;
                record.snapshot.outputHash = spec.inputHash;
                record.snapshot.builtFingerprint = spec.inputHash;
            }
        }
        replacement.insert(spec.id, record);
    }
    records_ = std::move(replacement);
    order_ = std::move(order);
    dirtyClosure(changed);
    return std::nullopt;
}

void DependencyGraph::dirtyClosure(const QStringList& roots) {
    QSet<QString> affected(roots.cbegin(), roots.cend());
    // Topological order propagates transitively in a single pass.
    for (const auto& id : order_) {
        auto& record = records_[id];
        for (const auto& dependency : record.snapshot.spec.dependencies) {
            if (affected.contains(dependency)) {
                affected.insert(id);
                break;
            }
        }
        if (affected.contains(id) && record.snapshot.spec.kind != ArtifactKind::Source) {
            record.snapshot.state = ArtifactState::Dirty;
            record.snapshot.outputHash.reset();
            record.snapshot.builtFingerprint.reset();
            record.pending.reset();
        }
    }
}

GraphStatus DependencyGraph::invalidate(const QStringList& roots) {
    for (const auto& id : roots) {
        if (!records_.contains(id) ||
            records_.value(id).snapshot.spec.kind == ArtifactKind::Source) {
            return GraphError{id, u"Invalidation requires a known derived artifact"_s};
        }
    }
    dirtyClosure(roots);
    return std::nullopt;
}

std::optional<ArtifactSnapshot> DependencyGraph::artifact(const QString& id) const {
    const auto found = records_.constFind(id);
    if (found == records_.cend()) {
        return std::nullopt;
    }
    return found->snapshot;
}

GraphResult<QStringList> DependencyGraph::rebuildPlan(const QStringList& targets) const {
    QSet<QString> needed;
    QStringList pending = targets.isEmpty() ? order_ : targets;
    while (!pending.isEmpty()) {
        const auto id = pending.takeLast();
        const auto found = records_.constFind(id);
        if (found == records_.cend()) {
            return GraphError{id, u"Unknown rebuild target"_s};
        }
        if (needed.contains(id)) {
            continue;
        }
        needed.insert(id);
        pending.append(found->snapshot.spec.dependencies);
    }
    QStringList plan;
    for (const auto& id : order_) {
        if (needed.contains(id) && records_.value(id).snapshot.state != ArtifactState::Clean) {
            plan.append(id);
        }
    }
    return plan;
}

core::ContentHash DependencyGraph::fingerprint(const QString& id) const {
    const auto& spec = records_[id].snapshot.spec;
    QJsonArray components{u"loreforge-artifact-v1"_s, spec.id, static_cast<int>(spec.kind),
                          spec.inputHash.toHex()};
    for (const auto& dependency : spec.dependencies) {
        components.append(
            QJsonArray{dependency, records_[dependency].snapshot.outputHash->toHex()});
    }
    const auto bytes = QJsonDocument(components).toJson(QJsonDocument::Compact);
    return core::ContentHash::sha256(QByteArrayView(bytes));
}

GraphResult<BuildTicket> DependencyGraph::beginBuild(const QString& id) {
    auto found = records_.find(id);
    if (found == records_.end() || found->snapshot.state != ArtifactState::Dirty) {
        return GraphError{id, u"Only dirty, non-running artifacts may start a build"_s};
    }
    QList<DependencyOutput> dependencies;
    for (const auto& dependency : found->snapshot.spec.dependencies) {
        const auto& input = records_[dependency].snapshot;
        if (input.state != ArtifactState::Clean || !input.outputHash) {
            return GraphError{id, u"A prerequisite is not clean"_s};
        }
        dependencies.append({dependency, *input.outputHash});
    }
    BuildTicket ticket{graphId_, id, ++serial_, fingerprint(id), dependencies};
    found->pending = ticket;
    found->snapshot.state = ArtifactState::Building;
    return ticket;
}

GraphStatus DependencyGraph::completeBuild(const BuildTicket& ticket,
                                           const core::ContentHash& outputHash) {
    auto found = records_.find(ticket.artifactId);
    if (!outputHash.isValid() || found == records_.end() || !found->pending ||
        *found->pending != ticket || ticket.inputFingerprint != fingerprint(ticket.artifactId)) {
        return GraphError{ticket.artifactId, u"Invalid output or stale build ticket"_s};
    }
    found->snapshot.state = ArtifactState::Clean;
    found->snapshot.outputHash = outputHash;
    found->snapshot.builtFingerprint = ticket.inputFingerprint;
    found->pending.reset();
    return std::nullopt;
}

GraphStatus DependencyGraph::failBuild(const BuildTicket& ticket) {
    auto found = records_.find(ticket.artifactId);
    if (found == records_.end() || !found->pending || *found->pending != ticket) {
        return GraphError{ticket.artifactId, u"Stale build ticket"_s};
    }
    found->snapshot.state = ArtifactState::Dirty;
    found->pending.reset();
    return std::nullopt;
}

RebuildReport DependencyGraph::rebuild(
    const QStringList& targets,
    const std::function<GraphResult<core::ContentHash>(const BuildTicket&)>& builder) {
    RebuildReport report;
    if (!builder) {
        report.error = GraphError{{}, u"A rebuild executor is required"_s};
        return report;
    }
    const auto plan = rebuildPlan(targets);
    if (const auto* error = std::get_if<GraphError>(&plan)) {
        report.error = *error;
        return report;
    }
    for (const auto& id : std::get<QStringList>(plan)) {
        const auto started = beginBuild(id);
        if (const auto* error = std::get_if<GraphError>(&started)) {
            report.error = *error;
            break;
        }
        const auto ticket = std::get<BuildTicket>(started);
        GraphResult<core::ContentHash> result = GraphError{id, u"Build executor failed"_s};
        try {
            result = builder(ticket);
        } catch (const std::exception&) {
            // Do not expose provider exceptions or credentials through graph diagnostics.
        } catch (...) {
        }
        if (const auto* error = std::get_if<GraphError>(&result)) {
            (void)failBuild(ticket);
            report.error = *error;
            break;
        }
        if (const auto error = completeBuild(ticket, std::get<core::ContentHash>(result))) {
            (void)failBuild(ticket);
            report.error = error;
            break;
        }
        report.rebuilt.append(id);
    }
    return report;
}

} // namespace loreforge::incremental
