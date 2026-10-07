#pragma once

#include "loreforge/git/git_repository.h"

#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QUrl>

#include <functional>
#include <memory>

namespace loreforge::git {

class IGitHubAuthentication {
  public:
    virtual ~IGitHubAuthentication() = default;
    [[nodiscard]] virtual QByteArray accessToken() const = 0;
};

// Tokens are resolved at request time and are never persisted by this adapter.
class EnvironmentGitHubAuthentication final : public IGitHubAuthentication {
  public:
    [[nodiscard]] QByteArray accessToken() const override;
};

struct GitHubRepository final {
    QString owner;
    QString name;
};

struct PullRequestDraft final {
    GitHubRepository repository;
    QString baseBranch;
    QString headBranch;
    QString expectedHead;
    QString title;
    QString body;
    bool draft = true;
};

[[nodiscard]] GitResult<PullRequestDraft>
preparePullRequest(const GitHubRepository& repository, const QString& baseBranch,
                   const CommitReceipt& receipt, const QList<PreparedPatch>& patches,
                   const QString& title, const QStringList& validation);

enum class GitHubErrorCode {
    InvalidRequest,
    AuthenticationRequired,
    NetworkError,
    HttpError,
    InvalidResponse,
    HeadMismatch,
};

struct GitHubError final {
    GitHubErrorCode code;
    QString message;
    int httpStatus = 0;
};

using GitHubResult = std::variant<QJsonObject, GitHubError>;
using GitHubCompletion = std::function<void(GitHubResult)>;

class IGitHubClient {
  public:
    virtual ~IGitHubClient() = default;
    virtual void createPullRequest(PullRequestDraft draft, GitHubCompletion completion) = 0;
    virtual void readCommit(GitHubRepository repository, QString ref,
                            GitHubCompletion completion) = 0;
    virtual void readPullRequest(GitHubRepository repository, int number,
                                 GitHubCompletion completion) = 0;
    virtual void readCommitChecks(GitHubRepository repository, QString ref,
                                  GitHubCompletion completion) = 0;
};

struct GitHubClientOptions final {
    QUrl apiRoot = QUrl(QStringLiteral("https://api.github.com"));
    int timeoutMilliseconds = 15'000;
};

class GitHubClient final : public QObject, public IGitHubClient {
  public:
    explicit GitHubClient(std::shared_ptr<IGitHubAuthentication> authentication,
                          GitHubClientOptions options = {}, QObject* parent = nullptr);
    void createPullRequest(PullRequestDraft draft, GitHubCompletion completion) override;
    void readCommit(GitHubRepository repository, QString ref, GitHubCompletion completion) override;
    void readPullRequest(GitHubRepository repository, int number,
                         GitHubCompletion completion) override;
    void readCommitChecks(GitHubRepository repository, QString ref,
                          GitHubCompletion completion) override;

  private:
    void request(const GitHubRepository& repository, const QString& suffix,
                 const std::optional<QJsonObject>& payload, GitHubCompletion completion);

    std::shared_ptr<IGitHubAuthentication> authentication_;
    GitHubClientOptions options_;
    QNetworkAccessManager network_;
};

} // namespace loreforge::git
