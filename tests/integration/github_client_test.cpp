#include "loreforge/git/github_client.h"

#include <QHostAddress>
#include <QJsonDocument>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>

using namespace Qt::StringLiterals;
using namespace loreforge::git;

namespace {

class TestAuthentication final : public IGitHubAuthentication {
  public:
    QByteArray token = "fixture-token";
    QByteArray accessToken() const override {
        return token;
    }
};

class HttpFixture final : public QObject {
  public:
    QTcpServer server;
    QList<QByteArray> requests;
    QList<QByteArray> bodies;
    int status = 200;
    bool respond = true;
    int dropRequest = 0;

    HttpFixture() {
        const bool listening = server.listen(QHostAddress::LocalHost);
        Q_ASSERT(listening);
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (auto* socket = server.nextPendingConnection()) {
                auto buffer = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer] {
                    *buffer += socket->readAll();
                    const auto split = buffer->indexOf("\r\n\r\n");
                    if (split < 0) {
                        return;
                    }
                    int length = 0;
                    for (const auto& header : buffer->first(split).split('\n')) {
                        if (header.toLower().startsWith("content-length:")) {
                            length = header.sliced(header.indexOf(':') + 1).trimmed().toInt();
                        }
                    }
                    if (buffer->size() < split + 4 + length) {
                        return;
                    }
                    requests.append(*buffer);
                    disconnect(socket, &QTcpSocket::readyRead, nullptr, nullptr);
                    if (!respond || requests.size() == dropRequest) {
                        return;
                    }
                    const auto body = bodies.isEmpty() ? QByteArray("{}") : bodies.takeFirst();
                    socket->write("HTTP/1.1 " + QByteArray::number(status) +
                                  " Fixture\r\nContent-Type: application/json\r\nContent-Length: " +
                                  QByteArray::number(body.size()) +
                                  "\r\nConnection: close\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    QUrl endpoint() const {
        return QUrl(u"http://127.0.0.1:%1"_s.arg(server.serverPort()));
    }
};

PullRequestDraft draft() {
    return {{u"owner"_s, u"novel"_s},
            u"main"_s,
            u"feature/repair"_s,
            QString(40, u'a'),
            u"Reviewed repairs"_s,
            u"Reviewed typo and validation"_s,
            true};
}

} // namespace

class GitHubClientTest final : public QObject {
    Q_OBJECT
  private slots:
    void verifiesRemoteCommitBeforeCreatingDraft();
    void refusesMismatchedRemoteCommit();
    void handlesAuthenticationHttpAndInvalidJson();
    void timesOutWithoutRetryingWrites();
    void generatesDescriptionBoundToReviewedPaths();
    void readsChecksAndRejectsProtectedBranchesAndRedirects();
};

void GitHubClientTest::verifiesRemoteCommitBeforeCreatingDraft() {
    HttpFixture fixture;
    fixture.bodies = {
        QByteArray("{\"sha\":\"") + QByteArray(40, 'a') + "\",\"commit\":{}}",
        QByteArray("{\"number\":7,\"html_url\":\"https://github.com/owner/novel/pull/7\"}")};
    auto authentication = std::make_shared<TestAuthentication>();
    GitHubClient client(authentication, {fixture.endpoint(), 1000});
    std::optional<GitHubResult> result;
    client.createPullRequest(draft(), [&](GitHubResult value) { result = std::move(value); });
    QTRY_VERIFY(result.has_value());
    QVERIFY(std::holds_alternative<QJsonObject>(*result));
    QCOMPARE(fixture.requests.size(), 2);
    QVERIFY(fixture.requests[0].startsWith("GET /repos/owner/novel/commits/feature%2Frepair "));
    QVERIFY(fixture.requests[1].startsWith("POST /repos/owner/novel/pulls "));
    QVERIFY(fixture.requests[1].contains("Bearer fixture-token"));
    const auto wire = fixture.requests[1];
    const auto payload =
        QJsonDocument::fromJson(wire.sliced(wire.indexOf("\r\n\r\n") + 4)).object();
    QCOMPARE(payload.value(u"draft"_s).toBool(), true);
    QCOMPARE(payload.value(u"base"_s).toString(), u"main"_s);
}

void GitHubClientTest::refusesMismatchedRemoteCommit() {
    HttpFixture fixture;
    fixture.bodies = {QByteArray("{\"sha\":\"other\",\"commit\":{}}")};
    GitHubClient client(std::make_shared<TestAuthentication>(), {fixture.endpoint(), 1000});
    std::optional<GitHubResult> result;
    client.createPullRequest(draft(), [&](GitHubResult value) { result = std::move(value); });
    QTRY_VERIFY(result.has_value());
    QVERIFY(std::holds_alternative<GitHubError>(*result));
    QCOMPARE(std::get<GitHubError>(*result).code, GitHubErrorCode::HeadMismatch);
    QCOMPARE(fixture.requests.size(), 1);
}

void GitHubClientTest::handlesAuthenticationHttpAndInvalidJson() {
    HttpFixture fixture;
    auto authentication = std::make_shared<TestAuthentication>();
    authentication->token.clear();
    GitHubClient client(authentication, {fixture.endpoint(), 1000});
    fixture.bodies = {QByteArray("{\"sha\":\"") + QByteArray(40, 'a') + "\",\"commit\":{}}"};
    std::optional<GitHubResult> result;
    client.createPullRequest(draft(), [&](GitHubResult value) { result = std::move(value); });
    QTRY_VERIFY(result.has_value());
    QCOMPARE(std::get<GitHubError>(*result).code, GitHubErrorCode::AuthenticationRequired);
    QCOMPARE(fixture.requests.size(), 1);
    result.reset();
    fixture.status = 401;
    fixture.bodies = {QByteArray("fixture-token")};
    client.readCommit({u"owner"_s, u"novel"_s}, u"main"_s,
                      [&](GitHubResult value) { result = std::move(value); });
    QTRY_VERIFY(result.has_value());
    QCOMPARE(std::get<GitHubError>(*result).httpStatus, 401);
    QVERIFY(!std::get<GitHubError>(*result).message.contains(u"fixture-token"_s));
    result.reset();
    fixture.status = 200;
    fixture.bodies = {QByteArray("broken")};
    client.readPullRequest({u"owner"_s, u"novel"_s}, 7,
                           [&](GitHubResult value) { result = std::move(value); });
    QTRY_VERIFY(result.has_value());
    QCOMPARE(std::get<GitHubError>(*result).code, GitHubErrorCode::InvalidResponse);
}

void GitHubClientTest::timesOutWithoutRetryingWrites() {
    HttpFixture fixture;
    fixture.dropRequest = 2;
    fixture.bodies = {QByteArray("{\"sha\":\"") + QByteArray(40, 'a') + "\",\"commit\":{}}"};
    GitHubClient client(std::make_shared<TestAuthentication>(), {fixture.endpoint(), 50});
    std::optional<GitHubResult> result;
    client.createPullRequest(draft(), [&](GitHubResult value) { result = std::move(value); });
    QTRY_VERIFY(result.has_value());
    QCOMPARE(std::get<GitHubError>(*result).code, GitHubErrorCode::NetworkError);
    QCOMPARE(fixture.requests.size(), 2);
}

void GitHubClientTest::generatesDescriptionBoundToReviewedPaths() {
    const QByteArray before("walk\n");
    const QByteArray after("walks\n");
    const auto hash = loreforge::core::ContentHash::sha256(QByteArrayView(before));
    const auto authorization = loreforge::proofreading::RepairGate::authorizeManualEdit(
        loreforge::core::ChapterId::fromStableKey(u"chapter"_s), {u"chapter.txt"_s, 0, 4},
        u"walk"_s, u"walks"_s, hash, u"Reviewed grammar"_s);
    PreparedPatch patch{std::get<loreforge::proofreading::PatchAuthorization>(authorization),
                        u"chapter.txt"_s,
                        hash,
                        loreforge::core::ContentHash::sha256(QByteArrayView(after)),
                        before,
                        after,
                        {}};
    CommitReceipt receipt{QString(40, u'a'), u"feature/repair"_s, {u"chapter.txt"_s}};
    const auto prepared = preparePullRequest({u"owner"_s, u"novel"_s}, u"main"_s, receipt, {patch},
                                             u"Repair grammar"_s, {u"Tests passed"_s});
    QVERIFY(std::holds_alternative<PullRequestDraft>(prepared));
    QVERIFY(std::get<PullRequestDraft>(prepared).body.contains(hash.toHex()));
    receipt.committedPaths.append(u"unreviewed.txt"_s);
    QVERIFY(std::holds_alternative<GitError>(
        preparePullRequest({u"owner"_s, u"novel"_s}, u"main"_s, receipt, {patch},
                           u"Repair grammar"_s, {u"Tests passed"_s})));
}

void GitHubClientTest::readsChecksAndRejectsProtectedBranchesAndRedirects() {
    HttpFixture fixture;
    fixture.bodies = {QByteArray("{\"total_count\":1,\"check_runs\":[{\"status\":\"completed\","
                                 "\"conclusion\":\"success\"}]}")};
    GitHubClient client(std::make_shared<TestAuthentication>(), {fixture.endpoint(), 1000});
    std::optional<GitHubResult> result;
    client.readCommitChecks({u"owner"_s, u"novel"_s}, u"feature/repair"_s,
                            [&](GitHubResult value) { result = std::move(value); });
    QTRY_VERIFY(result.has_value());
    QVERIFY(std::holds_alternative<QJsonObject>(*result));
    QVERIFY(fixture.requests[0].contains("/check-runs?per_page=100"));
    auto protectedDraft = draft();
    protectedDraft.headBranch = u"main"_s;
    result.reset();
    client.createPullRequest(protectedDraft,
                             [&](GitHubResult value) { result = std::move(value); });
    QVERIFY(result.has_value());
    QCOMPARE(std::get<GitHubError>(*result).code, GitHubErrorCode::InvalidRequest);
    QCOMPARE(fixture.requests.size(), 1);
    fixture.status = 302;
    fixture.bodies = {QByteArray("{}")};
    result.reset();
    client.readCommit({u"owner"_s, u"novel"_s}, u"main"_s,
                      [&](GitHubResult value) { result = std::move(value); });
    QTRY_VERIFY(result.has_value());
    QCOMPARE(std::get<GitHubError>(*result).httpStatus, 302);
}

QTEST_GUILESS_MAIN(GitHubClientTest)
#include "github_client_test.moc"
