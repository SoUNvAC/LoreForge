#include "loreforge/llm/illm_client.h"
#include "loreforge/llm/qwen_client.h"

#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QtTest>

#include <optional>
#include <type_traits>
#include <utility>

using namespace Qt::StringLiterals;

namespace {

struct HttpResponse final {
    int status = 200;
    QByteArray body;
    bool respond = true;
};

class ScriptedHttpServer final : public QObject {
  public:
    explicit ScriptedHttpServer(QObject* parent = nullptr) : QObject(parent) {
        connect(&server_, &QTcpServer::newConnection, this, [this] {
            while (auto* socket = server_.nextPendingConnection()) {
                buffers_.insert(socket, {});
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    auto& buffer = buffers_[socket];
                    buffer += socket->readAll();
                    consumeRequest(socket, buffer);
                });
                connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
                    buffers_.remove(socket);
                    socket->deleteLater();
                });
            }
        });
        const auto listening = server_.listen(QHostAddress::LocalHost, 0);
        Q_ASSERT(listening);
    }

    ~ScriptedHttpServer() override {
        server_.close();
        for (auto* socket : buffers_.keys()) {
            disconnect(socket, nullptr, this, nullptr);
            socket->abort();
        }
    }

    void enqueue(HttpResponse response) {
        responses_.enqueue(std::move(response));
    }

    [[nodiscard]] QUrl endpoint() const {
        return QUrl(u"http://127.0.0.1:%1/v1/chat/completions"_s.arg(server_.serverPort()));
    }

    [[nodiscard]] int requestCount() const noexcept {
        return requests_.size();
    }

    [[nodiscard]] const QByteArray& requestAt(qsizetype index) const {
        return requests_.at(index);
    }

  private:
    void consumeRequest(QTcpSocket* socket, const QByteArray& buffer) {
        const auto separator = buffer.indexOf("\r\n\r\n");
        if (separator < 0) {
            return;
        }
        int contentLength = 0;
        const auto headers = buffer.first(separator).split('\n');
        for (const auto& rawHeader : headers) {
            const auto header = rawHeader.trimmed();
            if (header.toLower().startsWith("content-length:")) {
                contentLength = header.sliced(header.indexOf(':') + 1).trimmed().toInt();
            }
        }
        const auto bodyOffset = separator + 4;
        if (buffer.size() < bodyOffset + contentLength) {
            return;
        }

        requests_.append(buffer.first(bodyOffset + contentLength));
        disconnect(socket, &QTcpSocket::readyRead, this, nullptr);
        const auto response = responses_.isEmpty()
                                  ? HttpResponse{500, QByteArrayLiteral("{}"), true}
                                  : responses_.dequeue();
        if (!response.respond) {
            return;
        }
        const auto reason = response.status == 200 ? QByteArrayLiteral("OK")
                                                   : QByteArrayLiteral("Service Unavailable");
        const auto wire =
            QByteArrayLiteral("HTTP/1.1 ") + QByteArray::number(response.status) +
            QByteArrayLiteral(" ") + reason +
            QByteArrayLiteral("\r\nContent-Type: application/json\r\nContent-Length: ") +
            QByteArray::number(response.body.size()) +
            QByteArrayLiteral("\r\nConnection: close\r\n\r\n") + response.body;
        socket->write(wire);
        socket->disconnectFromHost();
    }

    QTcpServer server_;
    QQueue<HttpResponse> responses_;
    QHash<QTcpSocket*, QByteArray> buffers_;
    QList<QByteArray> requests_;
};

QByteArray successResponse(QString content, int promptTokens = 4, int completionTokens = 3) {
    const QJsonObject response{
        {u"id"_s, u"chatcmpl-fixture"_s},
        {u"model"_s, u"qwen-fixture"_s},
        {u"choices"_s,
         QJsonArray{QJsonObject{{u"message"_s, QJsonObject{{u"role"_s, u"assistant"_s},
                                                           {u"content"_s, std::move(content)}}},
                                {u"finish_reason"_s, u"stop"_s}}}},
        {u"usage"_s, QJsonObject{{u"prompt_tokens"_s, promptTokens},
                                 {u"completion_tokens"_s, completionTokens},
                                 {u"total_tokens"_s, promptTokens + completionTokens}}},
    };
    return QJsonDocument(response).toJson(QJsonDocument::Compact);
}

loreforge::llm::LLMRequest request(QString content = u"Return JSON."_s) {
    return {u"qwen-fixture"_s, {{loreforge::llm::LLMRole::User, std::move(content)}}, {}, 0, 500,
            {0, 1, 4}};
}

loreforge::llm::QwenClient makeClient(ScriptedHttpServer& server) {
    return loreforge::llm::QwenClient(
        {server.endpoint(), QByteArrayLiteral("fixture-secret"), u"qwen-default"_s});
}

class MockLLMClient final : public loreforge::llm::ILLMClient {
  public:
    QUuid enqueue(loreforge::llm::LLMRequest request,
                  loreforge::llm::CompletionHandler completion) override {
        const auto id = QUuid::createUuid();
        if (completion) {
            completion(id, loreforge::llm::LLMResponse{
                               {}, request.model, u"mock"_s, u"stop"_s, {}, {}, 1, 0});
        }
        return id;
    }

    bool cancel(const QUuid&) override {
        return false;
    }

    qsizetype pendingRequestCount() const noexcept override {
        return 0;
    }
};

} // namespace

class LLMTransportTest final : public QObject {
    Q_OBJECT

  private slots:
    void mockAndQwenUseTheSameInterface();
    void sendsStructuredRequestsAndReadsUsage();
    void processesRequestsInFifoOrder();
    void retriesRetryableHttpFailures();
    void timesOutUnresponsiveRequests();
    void cancelsActiveAndQueuedRequests();
};

void LLMTransportTest::mockAndQwenUseTheSameInterface() {
    static_assert(std::is_base_of_v<loreforge::llm::ILLMClient, loreforge::llm::QwenClient>);
    MockLLMClient mock;
    loreforge::llm::ILLMClient& client = mock;
    std::optional<loreforge::llm::LLMResult> result;
    const auto requestId =
        client.enqueue(request(), [&result](const QUuid&, loreforge::llm::LLMResult value) {
            result = std::move(value);
        });
    QVERIFY(!requestId.isNull());
    QVERIFY(result.has_value());
    QVERIFY(std::holds_alternative<loreforge::llm::LLMResponse>(*result));
}

void LLMTransportTest::sendsStructuredRequestsAndReadsUsage() {
    ScriptedHttpServer server;
    const auto rawResponse = successResponse(uR"({"name":"Lin"})"_s);
    server.enqueue({200, rawResponse});
    auto client = makeClient(server);
    auto structuredRequest = request();
    structuredRequest.responseFormat = {{u"type"_s, u"json_object"_s}};
    structuredRequest.maxCompletionTokens = 128;
    std::optional<loreforge::llm::LLMResult> result;

    const auto requestId = client.enqueue(
        std::move(structuredRequest),
        [&result](const QUuid&, loreforge::llm::LLMResult value) { result = std::move(value); });
    QVERIFY(!requestId.isNull());
    QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 1'000);
    QVERIFY(std::holds_alternative<loreforge::llm::LLMResponse>(*result));
    const auto& response = std::get<loreforge::llm::LLMResponse>(*result);
    QCOMPARE(response.providerRequestId, u"chatcmpl-fixture"_s);
    QCOMPARE(response.model, u"qwen-fixture"_s);
    QCOMPARE(response.usage, (loreforge::llm::TokenUsage{4, 3, 7}));
    QCOMPARE(response.structuredContent.toObject().value(u"name"_s).toString(), u"Lin"_s);
    QCOMPARE(response.attemptCount, 1);

    QCOMPARE(server.requestCount(), 1);
    const auto rawRequest = server.requestAt(0);
    QVERIFY(rawRequest.toLower().contains("authorization: bearer fixture-secret"));
    const auto body = rawRequest.sliced(rawRequest.indexOf("\r\n\r\n") + 4);
    const auto payload = QJsonDocument::fromJson(body).object();
    QCOMPARE(payload.value(u"model"_s).toString(), u"qwen-fixture"_s);
    QCOMPARE(payload.value(u"max_completion_tokens"_s).toInt(), 128);
    QCOMPARE(payload.value(u"response_format"_s).toObject().value(u"type"_s).toString(),
             u"json_object"_s);
    QCOMPARE(response.rawRequest, body);
    QCOMPARE(response.rawResponse, rawResponse);
}

void LLMTransportTest::processesRequestsInFifoOrder() {
    ScriptedHttpServer server;
    server.enqueue({200, successResponse(u"first"_s)});
    server.enqueue({200, successResponse(u"second"_s)});
    auto client = makeClient(server);
    QStringList completions;

    const auto firstId = client.enqueue(
        request(u"one"_s), [&completions](const QUuid&, loreforge::llm::LLMResult value) {
            completions.append(std::get<loreforge::llm::LLMResponse>(value).content);
        });
    const auto secondId = client.enqueue(
        request(u"two"_s), [&completions](const QUuid&, loreforge::llm::LLMResult value) {
            completions.append(std::get<loreforge::llm::LLMResponse>(value).content);
        });
    QVERIFY(!firstId.isNull());
    QVERIFY(!secondId.isNull());
    QCOMPARE(client.pendingRequestCount(), 2);
    QTRY_COMPARE_WITH_TIMEOUT(completions.size(), 2, 1'000);
    QCOMPARE(completions, QStringList({u"first"_s, u"second"_s}));
    QCOMPARE(client.pendingRequestCount(), 0);
}

void LLMTransportTest::retriesRetryableHttpFailures() {
    ScriptedHttpServer server;
    server.enqueue({503, QByteArrayLiteral(R"({"error":{"message":"busy"}})")});
    server.enqueue({200, successResponse(u"recovered"_s)});
    auto client = makeClient(server);
    auto retryingRequest = request();
    retryingRequest.retryPolicy = {1, 1, 1};
    std::optional<loreforge::llm::LLMResult> result;

    const auto requestId = client.enqueue(
        std::move(retryingRequest),
        [&result](const QUuid&, loreforge::llm::LLMResult value) { result = std::move(value); });
    QVERIFY(!requestId.isNull());
    QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 1'000);
    QCOMPARE(server.requestCount(), 2);
    const auto& response = std::get<loreforge::llm::LLMResponse>(*result);
    QCOMPARE(response.content, u"recovered"_s);
    QCOMPARE(response.attemptCount, 2);
}

void LLMTransportTest::timesOutUnresponsiveRequests() {
    ScriptedHttpServer server;
    server.enqueue({200, {}, false});
    auto client = makeClient(server);
    auto timedRequest = request();
    timedRequest.timeoutMs = 25;
    std::optional<loreforge::llm::LLMResult> result;

    const auto requestId = client.enqueue(
        std::move(timedRequest),
        [&result](const QUuid&, loreforge::llm::LLMResult value) { result = std::move(value); });
    QVERIFY(!requestId.isNull());
    QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 1'000);
    const auto& error = std::get<loreforge::llm::LLMError>(*result);
    QCOMPARE(error.code, loreforge::llm::LLMErrorCode::Timeout);
    QCOMPARE(error.attemptCount, 1);
}

void LLMTransportTest::cancelsActiveAndQueuedRequests() {
    ScriptedHttpServer server;
    server.enqueue({200, {}, false});
    auto client = makeClient(server);
    QList<loreforge::llm::LLMErrorCode> errors;
    const auto activeId = client.enqueue(
        request(u"active"_s), [&errors](const QUuid&, loreforge::llm::LLMResult value) {
            errors.append(std::get<loreforge::llm::LLMError>(value).code);
        });
    const auto queuedId = client.enqueue(
        request(u"queued"_s), [&errors](const QUuid&, loreforge::llm::LLMResult value) {
            errors.append(std::get<loreforge::llm::LLMError>(value).code);
        });
    QTRY_COMPARE_WITH_TIMEOUT(server.requestCount(), 1, 1'000);
    QVERIFY(client.cancel(queuedId));
    QVERIFY(client.cancel(activeId));
    QCOMPARE(errors, QList({loreforge::llm::LLMErrorCode::Cancelled,
                            loreforge::llm::LLMErrorCode::Cancelled}));
    QCOMPARE(client.pendingRequestCount(), 0);
    QVERIFY(!client.cancel(QUuid::createUuid()));
}

QTEST_GUILESS_MAIN(LLMTransportTest)

#include "llm_transport_test.moc"
