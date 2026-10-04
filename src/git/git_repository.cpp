#include "loreforge/git/git_repository.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSaveFile>

#include <algorithm>

namespace loreforge::git {
namespace {

struct ProcessResult final {
    int exitCode = -1;
    QByteArray standardOutput;
    QByteArray standardError;
};

using ProcessOutcome = std::variant<ProcessResult, GitError>;

ProcessOutcome runGit(const QString& workingDirectory, QStringList arguments,
                      const QByteArray& standardInput = {}, int timeoutMilliseconds = 15'000) {
    arguments.prepend(QStringLiteral("-C"));
    arguments.insert(1, workingDirectory);

    QProcess process;
    process.setProgram(QStringLiteral("git"));
    process.setArguments(arguments);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start();
    if (!process.waitForStarted()) {
        return GitError{GitErrorCode::ProcessStartFailed,
                        QStringLiteral("Git could not be started: %1").arg(process.errorString()),
                        {}};
    }
    if (!standardInput.isEmpty()) {
        process.write(standardInput);
    }
    process.closeWriteChannel();
    if (!process.waitForFinished(timeoutMilliseconds)) {
        process.kill();
        process.waitForFinished();
        return GitError{GitErrorCode::ProcessTimedOut,
                        QStringLiteral("Git did not finish within the allowed time."),
                        {}};
    }
    return ProcessResult{process.exitCode(), process.readAllStandardOutput(),
                         process.readAllStandardError()};
}

GitError processFailure(const ProcessResult& result, QString action) {
    auto detail = QString::fromUtf8(result.standardError).trimmed();
    if (detail.isEmpty()) {
        detail = QString::fromUtf8(result.standardOutput).trimmed();
    }
    return {GitErrorCode::ProcessFailed,
            QStringLiteral("Git could not %1%2")
                .arg(std::move(action),
                     detail.isEmpty() ? QStringLiteral(".") : QStringLiteral(": %1").arg(detail)),
            {}};
}

QString normalizedPath(QString path) {
    return QDir::fromNativeSeparators(QDir::cleanPath(std::move(path)));
}

bool isSafeRelativePath(const QString& path) {
    const auto clean = normalizedPath(path);
    return !clean.isEmpty() && clean != QStringLiteral(".") && !QDir::isAbsolutePath(clean) &&
           clean != QStringLiteral("..") && !clean.startsWith(QStringLiteral("../")) &&
           !clean.contains(u'\n') && !clean.contains(u'\r') && !clean.contains(QChar::Null);
}

GitResult<QString> resolveTrackedPath(const QString& rootPath, const QString& relativePath) {
    const auto clean = normalizedPath(relativePath);
    if (!isSafeRelativePath(clean)) {
        return GitError{GitErrorCode::InvalidPath,
                        QStringLiteral("Patch paths must stay inside the repository."),
                        {relativePath}};
    }

    const QFileInfo rootInfo(rootPath);
    const QFileInfo fileInfo(QDir(rootPath).absoluteFilePath(clean));
    const auto canonicalRoot = normalizedPath(rootInfo.canonicalFilePath());
    const auto canonicalFile = normalizedPath(fileInfo.canonicalFilePath());
    const auto prefix = canonicalRoot + u'/';
    if (canonicalRoot.isEmpty() || canonicalFile.isEmpty() ||
        !canonicalFile.startsWith(prefix, Qt::CaseInsensitive)) {
        return GitError{GitErrorCode::InvalidPath,
                        QStringLiteral("The patch target must be an existing repository file."),
                        {clean}};
    }

    const auto tracked =
        runGit(rootPath, {QStringLiteral("ls-files"), QStringLiteral("--error-unmatch"),
                          QStringLiteral("--"), clean});
    if (const auto* error = std::get_if<GitError>(&tracked)) {
        return *error;
    }
    if (std::get<ProcessResult>(tracked).exitCode != 0) {
        return GitError{GitErrorCode::InvalidPath,
                        QStringLiteral("Only tracked files can receive reviewed patches."),
                        {clean}};
    }
    return canonicalFile;
}

QString fullFileDiff(const QString& path, const QByteArray& before, const QByteArray& after) {
    const auto beforeText = QString::fromUtf8(before);
    const auto afterText = QString::fromUtf8(after);
    auto lines = [](const QString& text, const QByteArray& bytes) {
        if (bytes.isEmpty()) {
            return QStringList{};
        }
        auto result = text.split(u'\n', Qt::KeepEmptyParts);
        if (bytes.endsWith('\n')) {
            result.removeLast();
        }
        return result;
    };
    const auto beforeLines = lines(beforeText, before);
    const auto afterLines = lines(afterText, after);
    const auto beforeStart = beforeLines.isEmpty() ? 0 : 1;
    const auto afterStart = afterLines.isEmpty() ? 0 : 1;
    QString result = QStringLiteral("--- a/%1\n+++ b/%1\n@@ -%2,%3 +%4,%5 @@\n")
                         .arg(path)
                         .arg(beforeStart)
                         .arg(beforeLines.size())
                         .arg(afterStart)
                         .arg(afterLines.size());
    for (const auto& line : beforeLines) {
        result += u'-' + line + u'\n';
    }
    if (!before.isEmpty() && !before.endsWith('\n')) {
        result += QStringLiteral("\\ No newline at end of file\n");
    }
    for (const auto& line : afterLines) {
        result += u'+' + line + u'\n';
    }
    if (!after.isEmpty() && !after.endsWith('\n')) {
        result += QStringLiteral("\\ No newline at end of file\n");
    }
    return result;
}

GitResult<QByteArray> readFile(const QString& path, const QString& displayPath) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return GitError{
            GitErrorCode::IoError,
            QStringLiteral("Could not read %1: %2").arg(displayPath, file.errorString()),
            {displayPath}};
    }
    return file.readAll();
}

GitResult<QByteArray> readHeadFile(const QString& rootPath, const QString& relativePath) {
    const auto outcome =
        runGit(rootPath, {QStringLiteral("show"), QStringLiteral("HEAD:%1").arg(relativePath)});
    if (const auto* error = std::get_if<GitError>(&outcome)) {
        return *error;
    }
    const auto& result = std::get<ProcessResult>(outcome);
    if (result.exitCode != 0) {
        return processFailure(result, QStringLiteral("read the patch target from HEAD"));
    }
    return result.standardOutput;
}

std::optional<GitError> validatePreparedPatch(const PreparedPatch& patch) {
    const auto& authorization = patch.authorization;
    const bool validOrigin =
        (authorization.origin == proofreading::PatchOrigin::ApprovedCandidate &&
         authorization.candidateId.has_value() && authorization.candidateId->isValid()) ||
        (authorization.origin == proofreading::PatchOrigin::ExplicitManualEdit &&
         !authorization.candidateId.has_value());
    if (!validOrigin || !authorization.chapterId.isValid() || !authorization.sourceSpan.isValid() ||
        authorization.originalText.isEmpty() ||
        authorization.originalText == authorization.replacementText ||
        authorization.auditReason.trimmed().isEmpty() ||
        normalizedPath(authorization.sourceSpan.sourceId) != patch.relativePath ||
        authorization.sourceHash != patch.beforeHash ||
        core::ContentHash::sha256(QByteArrayView(patch.beforeBytes)) != patch.beforeHash) {
        return GitError{
            GitErrorCode::InvalidAuthorization,
            QStringLiteral(
                "The prepared patch is not backed by a valid Repair Gate authorization."),
            {patch.relativePath}};
    }

    const auto start = authorization.sourceSpan.startByte;
    const auto end = authorization.sourceSpan.endByte;
    const auto original = authorization.originalText.toUtf8();
    if (start < 0 || end < start || end > patch.beforeBytes.size() ||
        end - start != original.size() ||
        patch.beforeBytes.sliced(start, end - start) != original) {
        return GitError{
            GitErrorCode::InvalidAuthorization,
            QStringLiteral("The prepared patch does not contain the authorized source bytes."),
            {patch.relativePath}};
    }

    auto expectedAfter = patch.beforeBytes;
    expectedAfter.replace(start, end - start, authorization.replacementText.toUtf8());
    if (patch.afterBytes != expectedAfter ||
        core::ContentHash::sha256(QByteArrayView(patch.afterBytes)) != patch.afterHash ||
        patch.unifiedDiff !=
            fullFileDiff(patch.relativePath, patch.beforeBytes, patch.afterBytes)) {
        return GitError{GitErrorCode::InvalidAuthorization,
                        QStringLiteral("The prepared patch content failed integrity validation."),
                        {patch.relativePath}};
    }
    return std::nullopt;
}

} // namespace

bool GitFileStatus::isUntracked() const noexcept {
    return indexStatus == u'?' && workTreeStatus == u'?';
}

bool GitFileStatus::isStaged() const noexcept {
    return indexStatus != u' ' && indexStatus != u'?';
}

bool RepositorySnapshot::isClean() const noexcept {
    return changes.isEmpty();
}

GitRepository::GitRepository(QString rootPath) : rootPath_(std::move(rootPath)) {}

GitResult<GitRepository> GitRepository::discover(const QString& path) {
    const QFileInfo start(path);
    const auto directory = start.isDir() ? start.absoluteFilePath() : start.absolutePath();
    const auto outcome =
        runGit(directory, {QStringLiteral("rev-parse"), QStringLiteral("--show-toplevel")});
    if (const auto* error = std::get_if<GitError>(&outcome)) {
        return *error;
    }
    const auto& result = std::get<ProcessResult>(outcome);
    if (result.exitCode != 0) {
        return GitError{GitErrorCode::RepositoryNotFound,
                        QStringLiteral("No Git repository contains %1.").arg(path),
                        {path}};
    }
    return GitRepository(normalizedPath(QString::fromUtf8(result.standardOutput).trimmed()));
}

const QString& GitRepository::rootPath() const noexcept {
    return rootPath_;
}

GitResult<RepositorySnapshot> GitRepository::snapshot() const {
    const auto headOutcome =
        runGit(rootPath_, {QStringLiteral("rev-parse"), QStringLiteral("HEAD")});
    if (const auto* error = std::get_if<GitError>(&headOutcome)) {
        return *error;
    }
    const auto& headResult = std::get<ProcessResult>(headOutcome);
    if (headResult.exitCode != 0) {
        return processFailure(headResult, QStringLiteral("read HEAD"));
    }

    std::optional<QString> branch;
    const auto branchOutcome =
        runGit(rootPath_, {QStringLiteral("symbolic-ref"), QStringLiteral("--quiet"),
                           QStringLiteral("--short"), QStringLiteral("HEAD")});
    if (const auto* error = std::get_if<GitError>(&branchOutcome)) {
        return *error;
    }
    const auto& branchResult = std::get<ProcessResult>(branchOutcome);
    if (branchResult.exitCode == 0) {
        branch = QString::fromUtf8(branchResult.standardOutput).trimmed();
    } else if (branchResult.exitCode != 1) {
        return processFailure(branchResult, QStringLiteral("read the current branch"));
    }

    const auto statusOutcome =
        runGit(rootPath_, {QStringLiteral("status"), QStringLiteral("--porcelain=v1"),
                           QStringLiteral("-z"), QStringLiteral("--untracked-files=all")});
    if (const auto* error = std::get_if<GitError>(&statusOutcome)) {
        return *error;
    }
    const auto& statusResult = std::get<ProcessResult>(statusOutcome);
    if (statusResult.exitCode != 0) {
        return processFailure(statusResult, QStringLiteral("read repository status"));
    }

    QList<GitFileStatus> changes;
    const auto records = statusResult.standardOutput.split('\0');
    for (qsizetype index = 0; index < records.size(); ++index) {
        const auto& record = records.at(index);
        if (record.size() < 4) {
            continue;
        }
        GitFileStatus change{normalizedPath(QString::fromUtf8(record.sliced(3))), std::nullopt,
                             QChar::fromLatin1(record.at(0)), QChar::fromLatin1(record.at(1))};
        if ((change.indexStatus == u'R' || change.indexStatus == u'C') &&
            index + 1 < records.size() && !records.at(index + 1).isEmpty()) {
            change.originalPath = normalizedPath(QString::fromUtf8(records.at(++index)));
        }
        changes.append(std::move(change));
    }

    return RepositorySnapshot{rootPath_, branch,
                              QString::fromUtf8(headResult.standardOutput).trimmed(), changes};
}

GitResult<QString> GitRepository::diff(const QStringList& paths) const {
    QStringList arguments{QStringLiteral("diff"), QStringLiteral("--no-ext-diff"),
                          QStringLiteral("--binary"), QStringLiteral("--")};
    for (const auto& path : paths) {
        if (!isSafeRelativePath(path)) {
            return GitError{GitErrorCode::InvalidPath,
                            QStringLiteral("Diff paths must stay inside the repository."),
                            {path}};
        }
        arguments.append(normalizedPath(path));
    }
    const auto outcome = runGit(rootPath_, arguments);
    if (const auto* error = std::get_if<GitError>(&outcome)) {
        return *error;
    }
    const auto& result = std::get<ProcessResult>(outcome);
    if (result.exitCode != 0) {
        return processFailure(result, QStringLiteral("generate a diff"));
    }
    return QString::fromUtf8(result.standardOutput);
}

GitResult<PreparedPatch>
GitRepository::preparePatch(const QString& relativePath,
                            const proofreading::PatchAuthorization& authorization) const {
    const auto clean = normalizedPath(relativePath);
    const bool validOrigin =
        (authorization.origin == proofreading::PatchOrigin::ApprovedCandidate &&
         authorization.candidateId.has_value() && authorization.candidateId->isValid()) ||
        (authorization.origin == proofreading::PatchOrigin::ExplicitManualEdit &&
         !authorization.candidateId.has_value());
    if (!validOrigin || !authorization.chapterId.isValid() || !authorization.sourceSpan.isValid() ||
        !authorization.sourceHash.isValid() || authorization.originalText.isEmpty() ||
        authorization.originalText == authorization.replacementText ||
        authorization.auditReason.trimmed().isEmpty()) {
        return GitError{GitErrorCode::InvalidAuthorization,
                        QStringLiteral("The Repair Gate authorization is incomplete."),
                        {clean}};
    }
    if (normalizedPath(authorization.sourceSpan.sourceId) != clean) {
        return GitError{GitErrorCode::InvalidAuthorization,
                        QStringLiteral("The authorized source does not match the patch target."),
                        {clean, authorization.sourceSpan.sourceId}};
    }

    const auto resolved = resolveTrackedPath(rootPath_, clean);
    if (const auto* error = std::get_if<GitError>(&resolved)) {
        return *error;
    }
    const auto bytesResult = readFile(std::get<QString>(resolved), clean);
    if (const auto* error = std::get_if<GitError>(&bytesResult)) {
        return *error;
    }
    const auto& before = std::get<QByteArray>(bytesResult);
    const auto beforeHash = core::ContentHash::sha256(QByteArrayView(before));
    if (beforeHash != authorization.sourceHash) {
        return GitError{GitErrorCode::SourceChanged,
                        QStringLiteral("The file changed after proofreading; review it again."),
                        {clean}};
    }
    if (QString::fromUtf8(before).toUtf8() != before) {
        return GitError{GitErrorCode::InvalidAuthorization,
                        QStringLiteral("Reviewed patches support valid UTF-8 text files only."),
                        {clean}};
    }
    const auto headBytesResult = readHeadFile(rootPath_, clean);
    if (const auto* error = std::get_if<GitError>(&headBytesResult)) {
        return *error;
    }
    if (std::get<QByteArray>(headBytesResult) != before) {
        return GitError{GitErrorCode::UnsafeWorkingTree,
                        QStringLiteral("The patch target already contains unreviewed edits."),
                        {clean}};
    }

    const auto start = authorization.sourceSpan.startByte;
    const auto end = authorization.sourceSpan.endByte;
    const auto original = authorization.originalText.toUtf8();
    if (start < 0 || end < start || end > before.size() || end - start != original.size() ||
        before.sliced(start, end - start) != original) {
        return GitError{GitErrorCode::SourceChanged,
                        QStringLiteral("The authorized byte span no longer matches the source."),
                        {clean}};
    }

    auto after = before;
    after.replace(start, end - start, authorization.replacementText.toUtf8());
    return PreparedPatch{authorization,
                         clean,
                         beforeHash,
                         core::ContentHash::sha256(QByteArrayView(after)),
                         before,
                         after,
                         fullFileDiff(clean, before, after)};
}

std::optional<GitError> GitRepository::applyPatch(const PreparedPatch& patch) const {
    if (const auto validationError = validatePreparedPatch(patch)) {
        return validationError;
    }
    const auto resolved = resolveTrackedPath(rootPath_, patch.relativePath);
    if (const auto* error = std::get_if<GitError>(&resolved)) {
        return *error;
    }
    const auto path = std::get<QString>(resolved);
    const auto currentResult = readFile(path, patch.relativePath);
    if (const auto* error = std::get_if<GitError>(&currentResult)) {
        return *error;
    }
    const auto& current = std::get<QByteArray>(currentResult);
    if (current != patch.beforeBytes ||
        core::ContentHash::sha256(QByteArrayView(current)) != patch.beforeHash) {
        return GitError{GitErrorCode::SourceChanged,
                        QStringLiteral("The file changed after the patch preview was prepared."),
                        {patch.relativePath}};
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(patch.afterBytes) != patch.afterBytes.size() || !file.commit()) {
        return GitError{GitErrorCode::IoError,
                        QStringLiteral("Could not atomically write %1: %2")
                            .arg(patch.relativePath, file.errorString()),
                        {patch.relativePath}};
    }
    return std::nullopt;
}

GitResult<CommitReceipt> GitRepository::commit(const CommitRequest& request) const {
    if (request.patches.isEmpty() || request.metadata.authorName.trimmed().isEmpty() ||
        request.metadata.authorEmail.trimmed().isEmpty() ||
        request.metadata.message.trimmed().isEmpty()) {
        return GitError{
            GitErrorCode::NothingToCommit,
            QStringLiteral("A commit requires reviewed patches and explicit author metadata."),
            {}};
    }

    QSet<QString> reviewedPaths;
    for (const auto& patch : request.patches) {
        if (const auto validationError = validatePreparedPatch(patch)) {
            return *validationError;
        }
        const auto clean = normalizedPath(patch.relativePath);
        if (reviewedPaths.contains(clean)) {
            return GitError{GitErrorCode::InvalidAuthorization,
                            QStringLiteral("A commit cannot contain duplicate patch targets."),
                            {clean}};
        }
        const auto resolved = resolveTrackedPath(rootPath_, clean);
        if (const auto* error = std::get_if<GitError>(&resolved)) {
            return *error;
        }
        const auto bytesResult = readFile(std::get<QString>(resolved), clean);
        if (const auto* error = std::get_if<GitError>(&bytesResult)) {
            return *error;
        }
        const auto& bytes = std::get<QByteArray>(bytesResult);
        if (core::ContentHash::sha256(QByteArrayView(bytes)) != patch.afterHash) {
            return GitError{GitErrorCode::SourceChanged,
                            QStringLiteral("A reviewed file changed after its patch was applied."),
                            {clean}};
        }
        reviewedPaths.insert(clean);
    }

    const auto snapshotResult = snapshot();
    if (const auto* error = std::get_if<GitError>(&snapshotResult)) {
        return *error;
    }
    QStringList unreviewed;
    const auto& state = std::get<RepositorySnapshot>(snapshotResult);
    for (const auto& change : state.changes) {
        if (!reviewedPaths.contains(change.path) &&
            !request.acknowledgedUnrelatedPaths.contains(change.path)) {
            unreviewed.append(change.path);
        }
    }
    if (!unreviewed.isEmpty()) {
        std::sort(unreviewed.begin(), unreviewed.end());
        return GitError{
            GitErrorCode::UnsafeWorkingTree,
            QStringLiteral(
                "Unrelated working-tree edits must be handled explicitly before committing."),
            unreviewed};
    }

    auto paths = reviewedPaths.values();
    std::sort(paths.begin(), paths.end());
    QStringList arguments{
        QStringLiteral("-c"),
        QStringLiteral("user.name=%1").arg(request.metadata.authorName.trimmed()),
        QStringLiteral("-c"),
        QStringLiteral("user.email=%1").arg(request.metadata.authorEmail.trimmed()),
        QStringLiteral("commit"),
        QStringLiteral("--only"),
        QStringLiteral("--no-gpg-sign"),
        QStringLiteral("-m"),
        request.metadata.message.trimmed(),
        QStringLiteral("--")};
    arguments.append(paths);
    const auto commitOutcome = runGit(rootPath_, arguments);
    if (const auto* error = std::get_if<GitError>(&commitOutcome)) {
        return *error;
    }
    const auto& commitResult = std::get<ProcessResult>(commitOutcome);
    if (commitResult.exitCode != 0) {
        return processFailure(commitResult, QStringLiteral("create the reviewed commit"));
    }

    const auto finalSnapshot = snapshot();
    if (const auto* error = std::get_if<GitError>(&finalSnapshot)) {
        return *error;
    }
    const auto& finalState = std::get<RepositorySnapshot>(finalSnapshot);
    return CommitReceipt{finalState.head, finalState.branch, paths};
}

} // namespace loreforge::git
