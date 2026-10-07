#pragma once

#include "loreforge/core/content_hash.h"
#include "loreforge/proofreading/repair_queue.h"

#include <QList>
#include <QSet>
#include <QString>

#include <optional>
#include <variant>

namespace loreforge::git {

enum class GitErrorCode {
    RepositoryNotFound,
    ProcessStartFailed,
    ProcessTimedOut,
    ProcessFailed,
    InvalidPath,
    InvalidAuthorization,
    SourceChanged,
    IoError,
    UnsafeWorkingTree,
    NothingToCommit,
    InvalidBranch,
    RemoteMismatch,
};

struct GitError final {
    GitErrorCode code;
    QString message;
    QStringList paths;

    friend bool operator==(const GitError&, const GitError&) = default;
};

template <typename T> using GitResult = std::variant<T, GitError>;

struct GitFileStatus final {
    QString path;
    std::optional<QString> originalPath;
    QChar indexStatus;
    QChar workTreeStatus;

    [[nodiscard]] bool isUntracked() const noexcept;
    [[nodiscard]] bool isStaged() const noexcept;

    friend bool operator==(const GitFileStatus&, const GitFileStatus&) = default;
};

struct RepositorySnapshot final {
    QString rootPath;
    std::optional<QString> branch;
    QString head;
    QList<GitFileStatus> changes;

    [[nodiscard]] bool isClean() const noexcept;

    friend bool operator==(const RepositorySnapshot&, const RepositorySnapshot&) = default;
};

struct PreparedPatch final {
    proofreading::PatchAuthorization authorization;
    QString relativePath;
    core::ContentHash beforeHash;
    core::ContentHash afterHash;
    QByteArray beforeBytes;
    QByteArray afterBytes;
    QString unifiedDiff;

    friend bool operator==(const PreparedPatch&, const PreparedPatch&) = default;
};

struct CommitMetadata final {
    QString authorName;
    QString authorEmail;
    QString message;

    friend bool operator==(const CommitMetadata&, const CommitMetadata&) = default;
};

struct CommitRequest final {
    QList<PreparedPatch> patches;
    CommitMetadata metadata;
    QSet<QString> acknowledgedUnrelatedPaths;
};

struct CommitReceipt final {
    QString head;
    std::optional<QString> branch;
    QStringList committedPaths;

    friend bool operator==(const CommitReceipt&, const CommitReceipt&) = default;
};

struct RemoteTracking final {
    QString remote;
    QString branch;
    QString localHead;
    QString remoteHead;
    int ahead = 0;
    int behind = 0;
};

class GitRepository final {
  public:
    [[nodiscard]] static GitResult<GitRepository> discover(const QString& path);

    [[nodiscard]] const QString& rootPath() const noexcept;
    [[nodiscard]] GitResult<RepositorySnapshot> snapshot() const;
    [[nodiscard]] GitResult<QString> diff(const QStringList& paths = {}) const;

    [[nodiscard]] GitResult<PreparedPatch>
    preparePatch(const QString& relativePath,
                 const proofreading::PatchAuthorization& authorization) const;
    [[nodiscard]] std::optional<GitError> applyPatch(const PreparedPatch& patch) const;
    [[nodiscard]] GitResult<CommitReceipt> commit(const CommitRequest& request) const;
    [[nodiscard]] std::optional<GitError> createContributionBranch(const QString& branch) const;
    [[nodiscard]] std::optional<GitError>
    fetch(const QString& remote = QStringLiteral("origin")) const;
    [[nodiscard]] GitResult<RemoteTracking> remoteTracking(const QString& remote,
                                                           const QString& branch) const;
    [[nodiscard]] std::optional<GitError> sync(const QString& remote, const QString& branch) const;
    [[nodiscard]] std::optional<GitError>
    pushContribution(const CommitReceipt& receipt,
                     const QString& remote = QStringLiteral("origin")) const;

  private:
    explicit GitRepository(QString rootPath);

    QString rootPath_;
};

} // namespace loreforge::git
