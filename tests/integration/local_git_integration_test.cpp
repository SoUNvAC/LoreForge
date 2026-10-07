#include "loreforge/git/git_repository.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <variant>

using namespace Qt::StringLiterals;

class LocalGitIntegrationTest final : public QObject {
    Q_OBJECT

  private slots:
    void discoversRepositoryAndReportsSnapshotAndDiff();
    void appliesOnlyAnExactAuthorizedPatch();
    void refusesUnrelatedEditsAndCommitsOnlyReviewedPaths();
    void pushesReviewedFeatureBranchAndTracksRemote();
};

namespace {

struct CommandResult final {
    int exitCode;
    QByteArray output;
    QByteArray error;
};

CommandResult git(const QString& root, QStringList arguments) {
    arguments.prepend(root);
    arguments.prepend(QStringLiteral("-C"));
    QProcess process;
    process.start(QStringLiteral("git"), arguments);
    if (!process.waitForStarted() || !process.waitForFinished(10'000)) {
        return {-1, process.readAllStandardOutput(), process.errorString().toUtf8()};
    }
    return {process.exitCode(), process.readAllStandardOutput(), process.readAllStandardError()};
}

bool writeFile(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
           file.write(bytes) == bytes.size();
}

bool initializeRepository(const QString& root, const QByteArray& chapter) {
    if (!QDir(root).mkpath(QStringLiteral("story")) ||
        !writeFile(QDir(root).filePath(QStringLiteral("story/chapter.txt")), chapter) ||
        !writeFile(QDir(root).filePath(QStringLiteral("README.md")), QByteArray("fixture\n"))) {
        return false;
    }
    return git(root, {QStringLiteral("init"), QStringLiteral("-b"), QStringLiteral("feature/test")})
                   .exitCode == 0 &&
           git(root, {QStringLiteral("config"), QStringLiteral("user.name"),
                      QStringLiteral("Fixture Author")})
                   .exitCode == 0 &&
           git(root, {QStringLiteral("config"), QStringLiteral("user.email"),
                      QStringLiteral("fixture@example.invalid")})
                   .exitCode == 0 &&
           git(root, {QStringLiteral("add"), QStringLiteral("--"), QStringLiteral(".")}).exitCode ==
               0 &&
           git(root, {QStringLiteral("commit"), QStringLiteral("-m"),
                      QStringLiteral("fixture: initialize")})
                   .exitCode == 0;
}

loreforge::proofreading::PatchAuthorization authorizationFor(const QByteArray& source, qint64 start,
                                                             qint64 end, QString replacement) {
    const auto result = loreforge::proofreading::RepairGate::authorizeManualEdit(
        loreforge::core::ChapterId::fromStableKey(u"git-test:chapter"_s),
        {u"story/chapter.txt"_s, start, end}, QString::fromUtf8(source.sliced(start, end - start)),
        std::move(replacement), loreforge::core::ContentHash::sha256(QByteArrayView(source)),
        u"Integration test reviewed edit"_s);
    return std::get<loreforge::proofreading::PatchAuthorization>(result);
}

} // namespace

void LocalGitIntegrationTest::discoversRepositoryAndReportsSnapshotAndDiff() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QByteArray chapter("Mara walk home.\n");
    QVERIFY(initializeRepository(directory.path(), chapter));

    const auto nested = QDir(directory.path()).filePath(QStringLiteral("story"));
    const auto discovered = loreforge::git::GitRepository::discover(nested);
    QVERIFY(std::holds_alternative<loreforge::git::GitRepository>(discovered));
    const auto repository = std::get<loreforge::git::GitRepository>(discovered);

    const auto initial = repository.snapshot();
    QVERIFY(std::holds_alternative<loreforge::git::RepositorySnapshot>(initial));
    const auto& initialState = std::get<loreforge::git::RepositorySnapshot>(initial);
    QCOMPARE(initialState.branch, std::optional<QString>(u"feature/test"_s));
    QCOMPARE(initialState.head.size(), 40);
    QVERIFY(initialState.isClean());

    QVERIFY(writeFile(QDir(directory.path()).filePath(QStringLiteral("README.md")),
                      QByteArray("changed\n")));
    QVERIFY(writeFile(QDir(directory.path()).filePath(QStringLiteral("notes.txt")),
                      QByteArray("untracked\n")));
    const auto changed = repository.snapshot();
    QVERIFY(std::holds_alternative<loreforge::git::RepositorySnapshot>(changed));
    const auto& changedState = std::get<loreforge::git::RepositorySnapshot>(changed);
    QCOMPARE(changedState.changes.size(), 2);

    const auto diff = repository.diff({u"README.md"_s});
    QVERIFY(std::holds_alternative<QString>(diff));
    QVERIFY(std::get<QString>(diff).contains(u"-fixture"_s));
    QVERIFY(std::get<QString>(diff).contains(u"+changed"_s));
}

void LocalGitIntegrationTest::appliesOnlyAnExactAuthorizedPatch() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QByteArray chapter("Mara walk home.\n");
    QVERIFY(initializeRepository(directory.path(), chapter));
    const auto repositoryResult = loreforge::git::GitRepository::discover(directory.path());
    QVERIFY(std::holds_alternative<loreforge::git::GitRepository>(repositoryResult));
    const auto repository = std::get<loreforge::git::GitRepository>(repositoryResult);

    const auto authorization = authorizationFor(chapter, 5, 9, u"walks"_s);
    const auto prepared = repository.preparePatch(u"story/chapter.txt"_s, authorization);
    QVERIFY(std::holds_alternative<loreforge::git::PreparedPatch>(prepared));
    const auto& patch = std::get<loreforge::git::PreparedPatch>(prepared);
    QVERIFY(patch.unifiedDiff.contains(u"-Mara walk home."_s));
    QVERIFY(patch.unifiedDiff.contains(u"+Mara walks home."_s));
    auto forged = patch;
    forged.afterBytes = QByteArray("forged\n");
    const auto forgedError = repository.applyPatch(forged);
    QVERIFY(forgedError.has_value());
    QCOMPARE(forgedError->code, loreforge::git::GitErrorCode::InvalidAuthorization);
    QVERIFY(!repository.applyPatch(patch).has_value());

    QFile changed(QDir(directory.path()).filePath(QStringLiteral("story/chapter.txt")));
    QVERIFY(changed.open(QIODevice::ReadOnly));
    QCOMPARE(changed.readAll(), QByteArray("Mara walks home.\n"));

    const auto stale = repository.preparePatch(u"story/chapter.txt"_s, authorization);
    QVERIFY(std::holds_alternative<loreforge::git::GitError>(stale));
    QCOMPARE(std::get<loreforge::git::GitError>(stale).code,
             loreforge::git::GitErrorCode::SourceChanged);

    auto wrongSource = authorization;
    wrongSource.sourceSpan.sourceId = u"README.md"_s;
    const auto mismatched = repository.preparePatch(u"story/chapter.txt"_s, wrongSource);
    QVERIFY(std::holds_alternative<loreforge::git::GitError>(mismatched));
    QCOMPARE(std::get<loreforge::git::GitError>(mismatched).code,
             loreforge::git::GitErrorCode::InvalidAuthorization);

    const auto dirtyAuthorization =
        authorizationFor(QByteArray("Mara walks home.\n"), 5, 10, u"runs"_s);
    const auto dirtyTarget = repository.preparePatch(u"story/chapter.txt"_s, dirtyAuthorization);
    QVERIFY(std::holds_alternative<loreforge::git::GitError>(dirtyTarget));
    QCOMPARE(std::get<loreforge::git::GitError>(dirtyTarget).code,
             loreforge::git::GitErrorCode::UnsafeWorkingTree);
}

void LocalGitIntegrationTest::refusesUnrelatedEditsAndCommitsOnlyReviewedPaths() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QByteArray chapter("Mara walk home.\n");
    QVERIFY(initializeRepository(directory.path(), chapter));
    const auto repositoryResult = loreforge::git::GitRepository::discover(directory.path());
    QVERIFY(std::holds_alternative<loreforge::git::GitRepository>(repositoryResult));
    const auto repository = std::get<loreforge::git::GitRepository>(repositoryResult);

    const auto prepared = repository.preparePatch(u"story/chapter.txt"_s,
                                                  authorizationFor(chapter, 5, 9, u"walks"_s));
    QVERIFY(std::holds_alternative<loreforge::git::PreparedPatch>(prepared));
    const auto patch = std::get<loreforge::git::PreparedPatch>(prepared);
    QVERIFY(!repository.applyPatch(patch).has_value());
    QVERIFY(writeFile(QDir(directory.path()).filePath(QStringLiteral("notes.tmp")),
                      QByteArray("keep me out\n")));
    QVERIFY(writeFile(QDir(directory.path()).filePath(QStringLiteral("README.md")),
                      QByteArray("staged but unrelated\n")));
    QCOMPARE(git(directory.path(),
                 {QStringLiteral("add"), QStringLiteral("--"), QStringLiteral("README.md")})
                 .exitCode,
             0);

    loreforge::git::CommitRequest request{
        {patch},
        {u"Project Maintainer"_s, u"maintainer@example.invalid"_s,
         u"git: generate reviewed patch"_s},
        {},
    };
    const auto refused = repository.commit(request);
    QVERIFY(std::holds_alternative<loreforge::git::GitError>(refused));
    const auto& refusal = std::get<loreforge::git::GitError>(refused);
    QCOMPARE(refusal.code, loreforge::git::GitErrorCode::UnsafeWorkingTree);
    QCOMPARE(refusal.paths, (QStringList{u"README.md"_s, u"notes.tmp"_s}));

    request.acknowledgedUnrelatedPaths.insert(u"notes.tmp"_s);
    request.acknowledgedUnrelatedPaths.insert(u"README.md"_s);
    const auto committed = repository.commit(request);
    QVERIFY(std::holds_alternative<loreforge::git::CommitReceipt>(committed));
    const auto& receipt = std::get<loreforge::git::CommitReceipt>(committed);
    QCOMPARE(receipt.committedPaths, QStringList{u"story/chapter.txt"_s});
    QCOMPARE(receipt.branch, std::optional<QString>(u"feature/test"_s));

    const auto author =
        git(directory.path(), {QStringLiteral("show"), QStringLiteral("-s"),
                               QStringLiteral("--format=%an <%ae>"), QStringLiteral("HEAD")});
    QCOMPARE(author.exitCode, 0);
    QCOMPARE(author.output.trimmed(),
             QByteArray("Project Maintainer <maintainer@example.invalid>"));
    const auto files =
        git(directory.path(), {QStringLiteral("show"), QStringLiteral("--format="),
                               QStringLiteral("--name-only"), QStringLiteral("HEAD")});
    QCOMPARE(files.exitCode, 0);
    QCOMPARE(files.output.trimmed(), QByteArray("story/chapter.txt"));

    const auto finalState = repository.snapshot();
    QVERIFY(std::holds_alternative<loreforge::git::RepositorySnapshot>(finalState));
    const auto& changes = std::get<loreforge::git::RepositorySnapshot>(finalState).changes;
    QCOMPARE(changes.size(), 2);
    const auto readme = std::find_if(changes.cbegin(), changes.cend(), [](const auto& change) {
        return change.path == u"README.md"_s;
    });
    QVERIFY(readme != changes.cend());
    QVERIFY(readme->isStaged());
    const auto notes = std::find_if(changes.cbegin(), changes.cend(), [](const auto& change) {
        return change.path == u"notes.tmp"_s;
    });
    QVERIFY(notes != changes.cend());
    QVERIFY(notes->isUntracked());
    const auto previousBytes = repository.readCommittedFile(u"HEAD^"_s, u"story/chapter.txt"_s);
    const auto committedBytes = repository.readCommittedFile(u"HEAD"_s, u"story/chapter.txt"_s);
    QVERIFY(std::holds_alternative<QByteArray>(previousBytes));
    QVERIFY(std::holds_alternative<QByteArray>(committedBytes));
    QCOMPARE(std::get<QByteArray>(previousBytes), chapter);
    QCOMPARE(std::get<QByteArray>(committedBytes), QByteArray("Mara walks home.\n"));
    QVERIFY(writeFile(QDir(directory.path()).filePath(u"story/chapter.txt"_s),
                      QByteArray("working edit\n")));
    QCOMPARE(std::get<QByteArray>(repository.readCommittedFile(u"HEAD"_s, u"story/chapter.txt"_s)),
             QByteArray("Mara walks home.\n"));
    QVERIFY(std::holds_alternative<loreforge::git::GitError>(
        repository.readCommittedFile(u"HEAD"_s, u"../outside.txt"_s)));
    QVERIFY(std::holds_alternative<loreforge::git::GitError>(
        repository.readCommittedFile(u"HEAD"_s, u"story"_s)));
}

void LocalGitIntegrationTest::pushesReviewedFeatureBranchAndTracksRemote() {
    using namespace loreforge::git;
    QTemporaryDir directory;
    QTemporaryDir remoteDirectory;
    QVERIFY(directory.isValid());
    QVERIFY(remoteDirectory.isValid());
    const QByteArray chapter("Mara walk home.\n");
    QVERIFY(initializeRepository(directory.path(), chapter));
    QCOMPARE(git(remoteDirectory.path(), {u"init"_s, u"--bare"_s}).exitCode, 0);
    QCOMPARE(git(directory.path(), {u"remote"_s, u"add"_s, u"origin"_s, remoteDirectory.path()})
                 .exitCode,
             0);
    QCOMPARE(git(directory.path(), {u"push"_s, u"origin"_s, u"feature/test"_s}).exitCode, 0);
    const auto result = GitRepository::discover(directory.path());
    QVERIFY(std::holds_alternative<GitRepository>(result));
    const auto repository = std::get<GitRepository>(result);
    QVERIFY(repository.createContributionBranch(u"main"_s).has_value());
    QVERIFY(!repository.createContributionBranch(u"feature/repair"_s).has_value());
    const auto prepared = repository.preparePatch(u"story/chapter.txt"_s,
                                                  authorizationFor(chapter, 5, 9, u"walks"_s));
    QVERIFY(std::holds_alternative<PreparedPatch>(prepared));
    const auto patch = std::get<PreparedPatch>(prepared);
    QVERIFY(!repository.applyPatch(patch).has_value());
    const auto committed = repository.commit(
        {{patch}, {u"Maintainer"_s, u"maintainer@example.invalid"_s, u"repair verb"_s}, {}});
    QVERIFY(std::holds_alternative<CommitReceipt>(committed));
    const auto receipt = std::get<CommitReceipt>(committed);
    auto stale = receipt;
    stale.head = QString(40, u'0');
    const auto staleError = repository.pushContribution(stale);
    QVERIFY(staleError.has_value());
    QCOMPARE(staleError->code, GitErrorCode::SourceChanged);
    QVERIFY(!repository.pushContribution(receipt).has_value());
    QVERIFY(!repository.fetch().has_value());
    const auto tracking = repository.remoteTracking(u"origin"_s, u"feature/repair"_s);
    QVERIFY(std::holds_alternative<RemoteTracking>(tracking));
    QCOMPARE(std::get<RemoteTracking>(tracking).remoteHead, receipt.head);
    QCOMPARE(std::get<RemoteTracking>(tracking).ahead, 0);
    QCOMPARE(std::get<RemoteTracking>(tracking).behind, 0);
    QVERIFY(!repository.sync(u"origin"_s, u"feature/repair"_s).has_value());
    QTemporaryDir collaborator;
    QVERIFY(collaborator.isValid());
    QCOMPARE(git(collaborator.path(),
                 {u"clone"_s, u"--branch"_s, u"feature/repair"_s, remoteDirectory.path(), u"."_s})
                 .exitCode,
             0);
    QCOMPARE(git(collaborator.path(), {u"config"_s, u"user.name"_s, u"Collaborator"_s}).exitCode,
             0);
    QCOMPARE(
        git(collaborator.path(), {u"config"_s, u"user.email"_s, u"collaborator@example.invalid"_s})
            .exitCode,
        0);
    QVERIFY(writeFile(QDir(collaborator.path()).filePath(u"README.md"_s),
                      QByteArray("remote update\n")));
    QCOMPARE(git(collaborator.path(), {u"add"_s, u"README.md"_s}).exitCode, 0);
    QCOMPARE(git(collaborator.path(), {u"commit"_s, u"-m"_s, u"remote update"_s}).exitCode, 0);
    QCOMPARE(git(collaborator.path(), {u"push"_s, u"origin"_s, u"feature/repair"_s}).exitCode, 0);
    QVERIFY(!repository.fetch().has_value());
    const auto behind = repository.remoteTracking(u"origin"_s, u"feature/repair"_s);
    QVERIFY(std::holds_alternative<RemoteTracking>(behind));
    QCOMPARE(std::get<RemoteTracking>(behind).behind, 1);
    QVERIFY(!repository.sync(u"origin"_s, u"feature/repair"_s).has_value());
    const auto caughtUp = repository.remoteTracking(u"origin"_s, u"feature/repair"_s);
    QVERIFY(std::holds_alternative<RemoteTracking>(caughtUp));
    QCOMPARE(std::get<RemoteTracking>(caughtUp).behind, 0);
    QVERIFY(writeFile(QDir(directory.path()).filePath(u"notes.txt"_s), QByteArray("dirty\n")));
    QVERIFY(repository.sync(u"origin"_s, u"feature/repair"_s).has_value());
}

QTEST_GUILESS_MAIN(LocalGitIntegrationTest)

#include "local_git_integration_test.moc"
