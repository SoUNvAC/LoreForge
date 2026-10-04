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

  private:
    explicit GitRepository(QString rootPath);

    QString rootPath_;
};

} // namespace loreforge::git
