#include "loreforge/git/github_client.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>

#include <algorithm>

namespace loreforge::git {
namespace {

bool validRepository(const GitHubRepository& repository) {
    static const QRegularExpression component(QStringLiteral("^[A-Za-z0-9_.-]+$"));
    return component.match(repository.owner).hasMatch() &&
           component.match(repository.name).hasMatch() && repository.owner != QStringLiteral(".") &&
           repository.owner != QStringLiteral("..") && repository.name != QStringLiteral(".") &&
           repository.name != QStringLiteral("..");
}

bool validRef(const QString& ref) {
    return !ref.isEmpty() && !ref.startsWith(u'-') && !ref.contains(u'\n') &&
           !ref.contains(u'\r') && !ref.contains(QChar::Null);
}

bool protectedHead(const QString& ref) {
    const auto branch = ref.section(u':', -1);
    return branch.compare(QStringLiteral("main"), Qt::CaseInsensitive) == 0 ||
           branch.compare(QStringLiteral("master"), Qt::CaseInsensitive) == 0;
}

QString inlineText(QString text) {
    text.replace(u'\r', u' ');
    text.replace(u'\n', u' ');
    text.replace(u'`', u'\'');
    return text;
}

QString encoded(const QString& text) {
    return QString::fromLatin1(QUrl::toPercentEncoding(text));
}

} // namespace

QByteArray EnvironmentGitHubAuthentication::accessToken() const {
    auto token = qgetenv("GH_TOKEN");
    if (token.isEmpty()) {
        token = qgetenv("GITHUB_TOKEN");
    }
    return token;
}

GitResult<PullRequestDraft>
preparePullRequest(const GitHubRepository& repository, const QString& baseBranch,
                   const CommitReceipt& receipt, const QList<PreparedPatch>& patches,
                   const QString& title, const QStringList& validation) {
    if (!validRepository(repository) || !validRef(baseBranch) || !receipt.branch ||
        !validRef(*receipt.branch) || protectedHead(*receipt.branch) ||
        *receipt.branch == baseBranch || receipt.head.isEmpty() || patches.isEmpty() ||
        title.trimmed().isEmpty() || validation.isEmpty()) {
        return GitError{
            GitErrorCode::InvalidAuthorization,
            QStringLiteral(
                "PR preparation requires a reviewed feature commit and validation results."),
            {}};
    }
    QStringList paths;
    QString body =
        QStringLiteral(
            "Reviewed text repairs are ready for maintainer review.\n\nCommit: `%1`\n\nRepairs:\n")
            .arg(receipt.head);
    for (const auto& patch : patches) {
        if (patch.relativePath.isEmpty() || paths.contains(patch.relativePath) ||
            patch.authorization.auditReason.trimmed().isEmpty() ||
            core::ContentHash::sha256(QByteArrayView(patch.beforeBytes)) != patch.beforeHash ||
            core::ContentHash::sha256(QByteArrayView(patch.afterBytes)) != patch.afterHash) {
            return GitError{GitErrorCode::InvalidAuthorization,
                            QStringLiteral("PR repair metadata is incomplete."),
                            {patch.relativePath}};
        }
        paths.append(patch.relativePath);
        body +=
            QStringLiteral("- `%1`: `%2` → `%3` (%4). Source SHA-256: `%5`.\n")
                .arg(inlineText(patch.relativePath), inlineText(patch.authorization.originalText),
                     inlineText(patch.authorization.replacementText),
                     inlineText(patch.authorization.auditReason), patch.beforeHash.toHex());
        if (patch.authorization.candidateId) {
            body += QStringLiteral("  Candidate: `%1`.\n")
                        .arg(patch.authorization.candidateId->toString());
        }
    }
    auto committed = receipt.committedPaths;
    std::sort(paths.begin(), paths.end());
    std::sort(committed.begin(), committed.end());
    if (paths != committed) {
        return GitError{GitErrorCode::InvalidAuthorization,
                        QStringLiteral("PR patches must exactly match the reviewed commit paths."),
                        paths};
    }
    body += QStringLiteral("\nValidation:\n");
    for (const auto& item : validation) {
        if (item.trimmed().isEmpty()) {
            return GitError{GitErrorCode::InvalidAuthorization,
                            QStringLiteral("Validation descriptions cannot be empty."),
                            {}};
        }
        body += QStringLiteral("- %1\n").arg(inlineText(item));
    }
    return PullRequestDraft{repository, baseBranch, *receipt.branch, receipt.head, title.trimmed(),
                            body,       true};
}

GitHubClient::GitHubClient(std::shared_ptr<IGitHubAuthentication> authentication,
                           GitHubClientOptions options, QObject* parent)
    : QObject(parent), authentication_(std::move(authentication)), options_(std::move(options)) {}

void GitHubClient::request(const GitHubRepository& repository, const QString& suffix,
                           const std::optional<QJsonObject>& payload, GitHubCompletion completion) {
    const auto root = options_.apiRoot;
    const bool localTest =
        root.scheme() == QStringLiteral("http") &&
        (root.host() == QStringLiteral("127.0.0.1") || root.host() == QStringLiteral("localhost"));
    if (!completion) {
        return;
    }
    if (!validRepository(repository) || !root.isValid() || root.host().isEmpty() ||
        (!localTest && root.scheme() != QStringLiteral("https")) || !root.userInfo().isEmpty() ||
        root.hasQuery() || root.hasFragment() || options_.timeoutMilliseconds <= 0) {
        completion(GitHubError{GitHubErrorCode::InvalidRequest,
                               QStringLiteral("Invalid GitHub request configuration."), 0});
        return;
    }
    const auto token = authentication_ ? authentication_->accessToken() : QByteArray{};
    if (payload && token.isEmpty()) {
        completion(GitHubError{GitHubErrorCode::AuthenticationRequired,
                               QStringLiteral("Creating a PR requires GitHub API authentication."),
                               0});
        return;
    }
    if (token.contains('\n') || token.contains('\r') || token.contains('\0')) {
        completion(GitHubError{GitHubErrorCode::InvalidRequest,
                               QStringLiteral("Invalid authentication value."), 0});
        return;
    }
    auto rootText = root.toString(QUrl::FullyEncoded);
    while (rootText.endsWith(u'/')) {
        rootText.chop(1);
    }
    const QUrl url(rootText +
                   QStringLiteral("/repos/%1/%2/%3")
                       .arg(encoded(repository.owner), encoded(repository.name), suffix));
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("User-Agent", "LoreForge");
    request.setRawHeader("X-GitHub-Api-Version", "2026-03-10");
    if (!token.isEmpty()) {
        request.setRawHeader("Authorization", QByteArray("Bearer ") + token);
    }
    QNetworkReply* reply = nullptr;
    if (payload) {
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        reply = network_.post(request, QJsonDocument(*payload).toJson(QJsonDocument::Compact));
    } else {
        reply = network_.get(request);
    }
    auto* timer = new QTimer(reply);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, reply, &QNetworkReply::abort);
    timer->start(options_.timeoutMilliseconds);
    connect(
        reply, &QNetworkReply::finished, this,
        [reply, timer, completion = std::move(completion)]() mutable {
            timer->stop();
            const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const auto bytes = reply->readAll();
            const auto networkError = reply->error();
            reply->deleteLater();
            // Server responses and network diagnostics may contain credentials; never expose them.
            if (status < 200 || status >= 300) {
                completion(GitHubError{
                    status == 0 ? GitHubErrorCode::NetworkError : GitHubErrorCode::HttpError,
                    status == 0 ? QStringLiteral("GitHub request failed or timed out.")
                                : QStringLiteral("GitHub returned HTTP %1.").arg(status),
                    status});
                return;
            }
            if (networkError != QNetworkReply::NoError) {
                completion(GitHubError{GitHubErrorCode::NetworkError,
                                       QStringLiteral("GitHub transport failed."), status});
                return;
            }
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(bytes, &error);
            if (error.error != QJsonParseError::NoError || !document.isObject()) {
                completion(GitHubError{GitHubErrorCode::InvalidResponse,
                                       QStringLiteral("GitHub returned invalid JSON."), status});
                return;
            }
            completion(document.object());
        });
}

void GitHubClient::readCommit(GitHubRepository repository, QString ref,
                              GitHubCompletion completion) {
    if (!validRef(ref)) {
        if (completion) {
            completion(GitHubError{GitHubErrorCode::InvalidRequest,
                                   QStringLiteral("A commit reference is required."), 0});
        }
        return;
    }
    request(repository, QStringLiteral("commits/%1").arg(encoded(ref)), std::nullopt,
            [completion = std::move(completion)](GitHubResult result) mutable {
                if (!completion) {
                    return;
                }
                if (const auto* object = std::get_if<QJsonObject>(&result);
                    object && (object->value(QStringLiteral("sha")).toString().isEmpty() ||
                               !object->value(QStringLiteral("commit")).isObject())) {
                    completion(GitHubError{GitHubErrorCode::InvalidResponse,
                                           QStringLiteral("Missing remote commit metadata."), 200});
                    return;
                }
                completion(std::move(result));
            });
}

void GitHubClient::readPullRequest(GitHubRepository repository, int number,
                                   GitHubCompletion completion) {
    if (number <= 0) {
        if (completion) {
            completion(GitHubError{GitHubErrorCode::InvalidRequest,
                                   QStringLiteral("A positive PR number is required."), 0});
        }
        return;
    }
    request(repository, QStringLiteral("pulls/%1").arg(number), std::nullopt,
            std::move(completion));
}

void GitHubClient::createPullRequest(PullRequestDraft draft, GitHubCompletion completion) {
    if (!completion) {
        return;
    }
    if (!validRepository(draft.repository) || !validRef(draft.baseBranch) ||
        !validRef(draft.headBranch) || draft.headBranch == draft.baseBranch ||
        protectedHead(draft.headBranch) || draft.expectedHead.isEmpty() ||
        draft.title.trimmed().isEmpty() || draft.body.trimmed().isEmpty()) {
        completion(GitHubError{GitHubErrorCode::InvalidRequest,
                               QStringLiteral("Invalid contribution PR."), 0});
        return;
    }
    // Verify the pushed branch still refers to the reviewed local commit before creating a PR.
    const auto repository = draft.repository;
    const auto headBranch = draft.headBranch;
    readCommit(
        repository, headBranch,
        [this, draft = std::move(draft),
         completion = std::move(completion)](GitHubResult result) mutable {
            if (const auto* error = std::get_if<GitHubError>(&result)) {
                completion(*error);
                return;
            }
            if (std::get<QJsonObject>(result).value(QStringLiteral("sha")).toString() !=
                draft.expectedHead) {
                completion(GitHubError{
                    GitHubErrorCode::HeadMismatch,
                    QStringLiteral("The remote branch does not match the reviewed commit."), 0});
                return;
            }
            const QJsonObject payload{{QStringLiteral("title"), draft.title},
                                      {QStringLiteral("head"), draft.headBranch},
                                      {QStringLiteral("base"), draft.baseBranch},
                                      {QStringLiteral("body"), draft.body},
                                      {QStringLiteral("draft"), draft.draft}};
            request(
                draft.repository, QStringLiteral("pulls"), payload,
                [completion = std::move(completion)](GitHubResult response) mutable {
                    if (const auto* object = std::get_if<QJsonObject>(&response);
                        object &&
                        (object->value(QStringLiteral("number")).toInt() <= 0 ||
                         !QUrl(object->value(QStringLiteral("html_url")).toString()).isValid() ||
                         object->value(QStringLiteral("html_url")).toString().isEmpty())) {
                        completion(GitHubError{GitHubErrorCode::InvalidResponse,
                                               QStringLiteral("Missing PR identity."), 201});
                        return;
                    }
                    completion(std::move(response));
                });
        });
}

void GitHubClient::readCommitChecks(GitHubRepository repository, QString ref,
                                    GitHubCompletion completion) {
    if (!validRef(ref)) {
        if (completion) {
            completion(GitHubError{GitHubErrorCode::InvalidRequest,
                                   QStringLiteral("A commit reference is required."), 0});
        }
        return;
    }
    request(repository, QStringLiteral("commits/%1/check-runs?per_page=100").arg(encoded(ref)),
            std::nullopt, [completion = std::move(completion)](GitHubResult result) mutable {
                if (!completion) {
                    return;
                }
                if (const auto* object = std::get_if<QJsonObject>(&result);
                    object && !object->value(QStringLiteral("check_runs")).isArray()) {
                    completion(GitHubError{GitHubErrorCode::InvalidResponse,
                                           QStringLiteral("Missing commit checks."), 200});
                    return;
                }
                completion(std::move(result));
            });
}

} // namespace loreforge::git
