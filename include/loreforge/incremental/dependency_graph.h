#pragma once

#include "loreforge/core/content_hash.h"

#include <QHash>
#include <QList>
#include <QString>

#include <functional>
#include <optional>
#include <variant>

namespace loreforge::incremental {

enum class ArtifactKind { Source, Parse, Analysis, StoryState, Context, Diagnostics };
enum class ArtifactState { Clean, Dirty, Building };

struct ArtifactSpec final {
    QString id;
    ArtifactKind kind = ArtifactKind::Source;
    // Source: immutable projection hash. Derived: hash of all recipe/configuration versions.
    core::ContentHash inputHash;
    QStringList dependencies;
    friend bool operator==(const ArtifactSpec&, const ArtifactSpec&) = default;
};

struct GraphError final {
    QString artifactId;
    QString message;
};
template <typename T> using GraphResult = std::variant<T, GraphError>;
using GraphStatus = std::optional<GraphError>;

struct DependencyOutput final {
    QString id;
    core::ContentHash hash;
    friend bool operator==(const DependencyOutput&, const DependencyOutput&) = default;
};

struct BuildTicket final {
    QString graphId;
    QString artifactId;
    quint64 serial = 0;
    core::ContentHash inputFingerprint;
    QList<DependencyOutput> dependencies;
    friend bool operator==(const BuildTicket&, const BuildTicket&) = default;
};

struct ArtifactSnapshot final {
    ArtifactSpec spec;
    ArtifactState state = ArtifactState::Dirty;
    std::optional<core::ContentHash> outputHash;
    std::optional<core::ContentHash> builtFingerprint;
};

struct RebuildReport final {
    QStringList rebuilt;
    std::optional<GraphError> error;
};

// Single-owner coordinator. Workers receive tickets by value; completion is applied on the
// owning thread. Content hashes are receipts, not persisted payloads or automatic LLM calls.
class DependencyGraph final {
  public:
    DependencyGraph();
    DependencyGraph(const DependencyGraph&) = delete;
    DependencyGraph& operator=(const DependencyGraph&) = delete;

    // Atomically validates/replaces the complete topology, retaining only compatible results.
    [[nodiscard]] GraphStatus synchronize(const QList<ArtifactSpec>& specs);
    [[nodiscard]] GraphStatus invalidate(const QStringList& roots);
    [[nodiscard]] std::optional<ArtifactSnapshot> artifact(const QString& id) const;
    // Durable cache receipts; payloads remain in their owning repositories. Running work is
    // saved as dirty. Restore requires the exact current topology/configuration and is atomic.
    [[nodiscard]] QByteArray checkpoint() const;
    [[nodiscard]] GraphStatus restoreCheckpoint(const QByteArray& json);
    // Includes dirty prerequisites, excludes clean and unrelated nodes. Empty targets = all.
    [[nodiscard]] GraphResult<QStringList> rebuildPlan(const QStringList& targets = {}) const;
    [[nodiscard]] GraphResult<BuildTicket> beginBuild(const QString& id);
    [[nodiscard]] GraphStatus completeBuild(const BuildTicket& ticket,
                                            const core::ContentHash& outputHash);
    [[nodiscard]] GraphStatus failBuild(const BuildTicket& ticket);
    [[nodiscard]] RebuildReport
    rebuild(const QStringList& targets,
            const std::function<GraphResult<core::ContentHash>(const BuildTicket&)>& builder);

  private:
    struct Record final {
        ArtifactSnapshot snapshot;
        std::optional<BuildTicket> pending;
    };
    [[nodiscard]] core::ContentHash fingerprint(const QString& id) const;
    void dirtyClosure(const QStringList& roots);
    QHash<QString, Record> records_;
    QStringList order_;
    QString graphId_;
    quint64 serial_ = 0;
};

} // namespace loreforge::incremental
