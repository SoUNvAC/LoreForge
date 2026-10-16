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
    QByteArray extraHeaders;
    QList<QByteArray> chunks;
    int chunkDelayMs = 10;
    bool keepOpen = false;
    QByteArray contentType = "application/json";
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
        auto body = response.body;
        if (!response.chunks.isEmpty()) {
            body.clear();
            for (const auto& chunk : response.chunks)
                body += chunk;
        }
        const auto wire =
            QByteArrayLiteral("HTTP/1.1 ") + QByteArray::number(response.status) +
            QByteArrayLiteral(" ") + reason + QByteArrayLiteral("\r\n") + response.extraHeaders +
            QByteArrayLiteral("Content-Type: ") + response.contentType +
            (response.keepOpen
                 ? QByteArray("\r\nConnection: keep-alive\r\n\r\n")
                 : QByteArray("\r\nContent-Length: ") + QByteArray::number(body.size()) +
                       "\r\nConnection: close\r\n\r\n");
        socket->write(wire);
        if (response.chunks.isEmpty()) {
            socket->write(body);
            if (!response.keepOpen)
                socket->disconnectFromHost();
        } else {
            for (int index = 0; index < response.chunks.size(); ++index) {
                const auto chunk = response.chunks[index];
                const auto last = index == response.chunks.size() - 1;
                QTimer::singleShot(index * response.chunkDelayMs, socket,
                                   [socket, chunk, last, keepOpen = response.keepOpen] {
                                       if (socket->state() == QAbstractSocket::ConnectedState) {
                                           socket->write(chunk);
                                           if (last && !keepOpen)
                                               socket->disconnectFromHost();
                                       }
                                   });
            }
        }
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

QByteArray streamEvent(const QJsonObject& delta, const QJsonValue& finish = QJsonValue()) {
    return "data: " +
           QJsonDocument(
               QJsonObject{{u"id"_s, u"stream-fixture"_s},
                           {u"model"_s, u"fixture-model"_s},
                           {u"choices"_s, QJsonArray{QJsonObject{{u"index"_s, 0},
                                                                 {u"delta"_s, delta},
                                                                 {u"finish_reason"_s, finish}}}}})
               .toJson(QJsonDocument::Compact) +
           "\r\n\r\n";
}
QByteArray streamUsage() {
    return "data: "
           "{\"choices\":[],\"usage\":{\"prompt_tokens\":4,\"completion_tokens\":3,\"total_"
           "tokens\":7}}\n\n";
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
    void requiresExplicitUnauthenticatedConfiguration();
    void doesNotFollowRedirects();
    void reportsSafeResponseDiagnostics_data();
    void reportsSafeResponseDiagnostics();
    void receivesIncrementalSseWithoutWaitingForDisconnect();
    void rejectsIncompleteAndMalformedStreams_data();
    void rejectsIncompleteAndMalformedStreams();
    void cancelsAndTimesOutStreamsWithoutRetry();
    void redactsCredentialsAcrossStreamDeltas();
    void acceptsMultilineDataAndCarriageReturns();
    void retainsReportedUsageWhenStreamingIsCancelled();
    void acceptsEmptyCompatibilityFields_data();
    void acceptsEmptyCompatibilityFields();
    void identifiesTheRejectedStreamEventWithoutStoringIt();
};

void LLMTransportTest::acceptsEmptyCompatibilityFields_data() {
    QTest::addColumn<int>("mode");
    QTest::newRow("null-tool-placeholders") << 0;
    QTest::newRow("empty-tool-placeholders") << 1;
    QTest::newRow("single-choice-without-index") << 2;
    QTest::newRow("null-terminal-delta") << 3;
    QTest::newRow("missing-terminal-delta") << 4;
    QTest::newRow("null-usage-control-event") << 5;
    QTest::newRow("empty-choice-control-event") << 6;
    QTest::newRow("empty-finish-is-not-finished") << 7;
    QTest::newRow("combined-relay-placeholders") << 8;
}

void LLMTransportTest::acceptsEmptyCompatibilityFields() {
    QFETCH(int, mode);
    auto first =
        QJsonDocument::fromJson(
            streamEvent({{u"reasoning_content"_s, u"hidden-reasoning"_s}}).sliced(6).trimmed())
            .object();
    auto choices = first.value(u"choices"_s).toArray();
    auto choice = choices[0].toObject();
    auto delta = choice.value(u"delta"_s).toObject();
    if (mode == 0 || mode == 8) {
        delta.insert(u"tool_calls"_s, QJsonValue());
        delta.insert(u"function_call"_s, QJsonValue());
        first.insert(u"error"_s, QJsonValue());
    }
    if (mode == 1) {
        delta.insert(u"tool_calls"_s, QJsonArray{});
        delta.insert(u"function_call"_s, QJsonObject{});
    }
    if (mode == 2 || mode == 8)
        choice.remove(u"index"_s);
    if (mode == 7 || mode == 8)
        choice.insert(u"finish_reason"_s, QString());
    choice.insert(u"delta"_s, delta);
    choices[0] = choice;
    first.insert(u"choices"_s, choices);
    HttpResponse response;
    response.contentType = "text/event-stream";
    response.body = "data: " + QJsonDocument(first).toJson(QJsonDocument::Compact) + "\n\n";
    if (mode == 5 || mode == 8)
        response.body += "data: {\"choices\":[],\"usage\":null}\n\n";
    if (mode == 6)
        response.body += "data: {\"choices\":[]}\n\n";
    response.body += streamEvent({{u"content"_s, u"{\"ok\":true}"_s}});
    if (mode == 3 || mode == 8)
        response.body +=
            "data: {\"choices\":[{\"index\":0,\"delta\":null,\"finish_reason\":\"stop\"}]}\n\n";
    else if (mode == 4)
        response.body += "data: {\"choices\":[{\"index\":0,\"finish_reason\":\"stop\"}]}\n\n";
    else
        response.body += streamEvent({}, u"stop"_s);
    response.body += streamUsage() + "data: [DONE]\n\n";
    ScriptedHttpServer server;
    server.enqueue(response);
    auto client = makeClient(server);
    auto input = request();
    input.stream = true;
    input.responseFormat = {{u"type"_s, u"json_object"_s}};
    std::optional<loreforge::llm::LLMResult> result;
    QSignalSpy progress(&client, &loreforge::llm::QwenClient::streamProgress);
    static_cast<void>(
        client.enqueue(input, [&](const QUuid&, auto value) { result = std::move(value); }));
    QTRY_VERIFY(result.has_value());
    QVERIFY(std::holds_alternative<loreforge::llm::LLMResponse>(*result));
    const auto output = std::get<loreforge::llm::LLMResponse>(*result);
    QVERIFY(output.streamed);
    QVERIFY(output.structuredContent.toObject().value(u"ok"_s).toBool());
    QCOMPARE(output.usage.totalTokens, 7);
    QVERIFY(!output.rawResponse.contains("hidden-reasoning"));
    for (const auto& event : progress)
        QVERIFY(!event[1].toString().contains(u"hidden-reasoning"_s));
    QCOMPARE(server.requestCount(), 1);
}

void LLMTransportTest::identifiesTheRejectedStreamEventWithoutStoringIt() {
    ScriptedHttpServer server;
    HttpResponse response;
    response.contentType = "text/event-stream";
    response.body =
        streamEvent({{u"reasoning_content"_s, QString(757, QLatin1Char('r'))}}) +
        streamEvent({{u"function_call"_s, QJsonObject{{u"name"_s, u"fixture-secret"_s}}}});
    server.enqueue(response);
    auto client = makeClient(server);
    auto input = request();
    input.stream = true;
    std::optional<loreforge::llm::LLMResult> result;
    static_cast<void>(
        client.enqueue(input, [&](const QUuid&, auto value) { result = std::move(value); }));
    QTRY_VERIFY(result.has_value());
    const auto error = std::get<loreforge::llm::LLMError>(*result);
    QCOMPARE(error.diagnostics.reasoningBytes, 757);
    QCOMPARE(error.diagnostics.contentBytes, 0);
    const auto diagnostics = loreforge::llm::responseDiagnosticsJson(error.diagnostics);
    QCOMPARE(diagnostics.value(u"response_transport"_s).toString(), u"sse"_s);
    QCOMPARE(diagnostics.value(u"stream_problem"_s).toString(), u"function_call"_s);
    QCOMPARE(diagnostics.value(u"stream_event_index"_s).toInteger(), 2);
    QVERIFY(diagnostics.value(u"stream_event_bytes"_s).toInteger() > 0);
    QVERIFY(error.rawResponse.isEmpty());
    QVERIFY(!QJsonDocument(diagnostics).toJson().contains("fixture-secret"));
}

void LLMTransportTest::acceptsMultilineDataAndCarriageReturns() {
    ScriptedHttpServer server;
    HttpResponse response;
    response.contentType = "text/event-stream";
    auto first = streamEvent({{u"content"_s, u"{}"_s}});
    first.replace("data: {", "event: message\nid: ignored\nretry: 1\ndata: {\ndata: ");
    response.body = first + streamEvent({}, u"stop"_s) + "data: [DONE]\r\r";
    response.body.replace("\r\n", "\r");
    server.enqueue(response);
    auto client = makeClient(server);
    auto input = request();
    input.stream = true;
    input.responseFormat = {{u"type"_s, u"json_object"_s}};
    std::optional<loreforge::llm::LLMResult> result;
    static_cast<void>(
        client.enqueue(input, [&](const QUuid&, auto value) { result = std::move(value); }));
    QTRY_VERIFY(result.has_value());
    QVERIFY(std::holds_alternative<loreforge::llm::LLMResponse>(*result));
    const auto output = std::get<loreforge::llm::LLMResponse>(*result);
    QVERIFY(output.streamed);
    QVERIFY(!output.usageReported);
}

void LLMTransportTest::retainsReportedUsageWhenStreamingIsCancelled() {
    ScriptedHttpServer server;
    HttpResponse response;
    response.contentType = "text/event-stream";
    response.body =
        streamEvent({{u"content"_s, u"{}"_s}}) + streamEvent({}, u"stop"_s) + streamUsage();
    response.keepOpen = true;
    server.enqueue(response);
    auto client = makeClient(server);
    auto input = request();
    input.stream = true;
    input.timeoutMs = 3000;
    std::optional<loreforge::llm::LLMResult> result;
    QSignalSpy progress(&client, &loreforge::llm::QwenClient::streamProgress);
    const auto id =
        client.enqueue(input, [&](const QUuid&, auto value) { result = std::move(value); });
    QTRY_VERIFY(!progress.isEmpty());
    QVERIFY(client.cancel(id));
    QVERIFY(result.has_value());
    const auto error = std::get<loreforge::llm::LLMError>(*result);
    QCOMPARE(error.code, loreforge::llm::LLMErrorCode::Cancelled);
    QVERIFY(error.diagnostics.usageReported);
    QCOMPARE(error.diagnostics.usage.totalTokens, 7);
}

void LLMTransportTest::receivesIncrementalSseWithoutWaitingForDisconnect() {
    ScriptedHttpServer server;
    HttpResponse response;
    response.contentType = "text/event-stream; charset=utf-8";
    response.keepOpen = true;
    response.chunkDelayMs = 25;
    const auto first = streamEvent({{u"content"_s, u"{\"name\":\"中文"_s}});
    const auto split = first.indexOf(QStringLiteral("中").toUtf8()) + 1;
    response.chunks = {QByteArray("\xef\xbb\xbf: heartbeat\r\n\r\n"),
                       streamEvent({{u"role"_s, u"assistant"_s}}),
                       streamEvent({{u"reasoning_content"_s, u"hidden-thinking"_s}}),
                       first.first(split),
                       first.sliced(split),
                       streamEvent({{u"content"_s, u"\"}"_s}}),
                       streamEvent({}, u"stop"_s),
                       streamUsage(),
                       QByteArray("data: [DONE]\n\n")};
    server.enqueue(response);
    auto client = makeClient(server);
    QSignalSpy started(&client, &loreforge::llm::QwenClient::streamStarted);
    QSignalSpy progress(&client, &loreforge::llm::QwenClient::streamProgress);
    QSignalSpy finished(&client, &loreforge::llm::QwenClient::streamFinished);
    auto input = request();
    input.stream = true;
    input.timeoutMs = 2000;
    input.responseFormat = {{u"type"_s, u"json_object"_s}};
    std::optional<loreforge::llm::LLMResult> result;
    const auto id =
        client.enqueue(input, [&](const QUuid&, auto value) { result = std::move(value); });
    QTRY_VERIFY(!progress.isEmpty());
    QVERIFY(!result.has_value());
    QTRY_VERIFY(result.has_value());
    QVERIFY(std::holds_alternative<loreforge::llm::LLMResponse>(*result));
    const auto output = std::get<loreforge::llm::LLMResponse>(*result);
    QCOMPARE(output.structuredContent.toObject().value(u"name"_s).toString(), u"中文"_s);
    QCOMPARE(output.usage.totalTokens, 7);
    QVERIFY(output.usageReported);
    QVERIFY(!output.rawResponse.contains("hidden-thinking"));
    QCOMPARE(started.size(), 1);
    QCOMPARE(finished.size(), 1);
    QCOMPARE(started[0][0].toUuid(), id);
    bool generating = false;
    for (const auto& event : progress) {
        QCOMPARE(event[0].toUuid(), id);
        QVERIFY(!event[1].toString().contains(u"hidden-thinking"_s));
        generating |= event[2].toBool();
    }
    QVERIFY(generating);
    QVERIFY(output.rawRequest.contains("\"stream\":true"));
    QVERIFY(output.rawRequest.contains("\"include_usage\":true"));
    QVERIFY(server.requestAt(0).toLower().contains("accept: text/event-stream"));
}

void LLMTransportTest::rejectsIncompleteAndMalformedStreams_data() {
    using loreforge::llm::ResponseIssue;
    QTest::addColumn<QByteArray>("body");
    QTest::addColumn<int>("issue");
    const auto valid =
        streamEvent({{u"content"_s, u"{}"_s}}) + streamEvent({}, u"stop"_s) + streamUsage();
    QTest::newRow("no-done") << valid << int(ResponseIssue::StreamIncomplete);
    QTest::newRow("no-finish") << streamEvent({{u"content"_s, u"{}"_s}}) + "data: [DONE]\n\n"
                               << int(ResponseIssue::StreamIncomplete);
    QTest::newRow("unfinished-event")
        << valid + "data: [DONE]" << int(ResponseIssue::StreamIncomplete);
    QTest::newRow("bad-json") << QByteArray("data: nope\n\n") << int(ResponseIssue::StreamProtocol);
    QTest::newRow("error-event") << QByteArray("data: {\"error\":\"fixture-secret\"}\n\n")
                                 << int(ResponseIssue::StreamProtocol);
    QTest::newRow("bad-content") << streamEvent({{u"content"_s, QJsonArray{}}})
                                 << int(ResponseIssue::StreamProtocol);
    QTest::newRow("tool-call")
        << streamEvent({{u"tool_calls"_s, QJsonArray{QJsonObject{{u"id"_s, u"fixture-secret"_s}}}}})
        << int(ResponseIssue::StreamProtocol);
    QTest::newRow("wrong-tool-type")
        << streamEvent({{u"tool_calls"_s, QString()}}) << int(ResponseIssue::StreamProtocol);
    QTest::newRow("bad-delta") << QByteArray(
                                      "data: {\"choices\":[{\"index\":0,\"delta\":null}]}\n\n")
                               << int(ResponseIssue::StreamProtocol);
    QTest::newRow("nonzero-index")
        << QByteArray("data: {\"choices\":[{\"index\":1,\"delta\":{}}]}\n\n")
        << int(ResponseIssue::StreamProtocol);
    QTest::newRow("fractional-index")
        << QByteArray("data: {\"choices\":[{\"index\":0.5,\"delta\":{}}]}\n\n")
        << int(ResponseIssue::StreamProtocol);
    QTest::newRow("bad-finish") << streamEvent({}, 3) << int(ResponseIssue::StreamProtocol);
    QTest::newRow("duplicate-finish") << streamEvent({}, u"stop"_s) + streamEvent({}, u"stop"_s)
                                      << int(ResponseIssue::StreamProtocol);
    QTest::newRow("event-too-large") << QByteArray("data: ") + QByteArray(1024 * 1024 + 1, 'x')
                                     << int(ResponseIssue::ResponseLimit);
    QTest::newRow("after-done") << valid + "data: [DONE]\n\ndata: {}\n\n"
                                << int(ResponseIssue::StreamProtocol);
    QTest::newRow("reasoning-only-length")
        << streamEvent({{u"reasoning_content"_s, u"private-thought"_s}}) +
               streamEvent({}, u"length"_s) + streamUsage() + "data: [DONE]\n\n"
        << int(ResponseIssue::EmptyContent);
    QTest::newRow("length-valid-json")
        << streamEvent({{u"content"_s, u"{}"_s}}) + streamEvent({}, u"length"_s) + streamUsage() +
               "data: [DONE]\n\n"
        << int(ResponseIssue::OutputLimit);
    QTest::newRow("invalid-usage")
        << streamEvent({{u"content"_s, u"{}"_s}}) + streamEvent({}, u"stop"_s) +
               "data: {\"choices\":[],\"usage\":{\"total_tokens\":1.5}}\n\ndata: [DONE]\n\n"
        << int(ResponseIssue::InvalidUsage);
}

void LLMTransportTest::rejectsIncompleteAndMalformedStreams() {
    QFETCH(QByteArray, body);
    QFETCH(int, issue);
    ScriptedHttpServer server;
    HttpResponse response;
    response.body = body;
    response.contentType = "text/event-stream";
    server.enqueue(response);
    auto client = makeClient(server);
    auto input = request();
    input.stream = true;
    input.responseFormat = {{u"type"_s, u"json_object"_s}};
    std::optional<loreforge::llm::LLMResult> result;
    static_cast<void>(
        client.enqueue(input, [&](const QUuid&, auto value) { result = std::move(value); }));
    QTRY_VERIFY(result.has_value());
    QVERIFY(std::holds_alternative<loreforge::llm::LLMError>(*result));
    const auto error = std::get<loreforge::llm::LLMError>(*result);
    QCOMPARE(int(error.diagnostics.issue), issue);
    QVERIFY(error.diagnostics.streamed);
    if (issue == int(loreforge::llm::ResponseIssue::StreamProtocol)) {
        QVERIFY(error.diagnostics.streamProblem != loreforge::llm::StreamProblem::None);
        QVERIFY(error.diagnostics.streamEventIndex > 0);
    }
    QVERIFY(!loreforge::llm::responseDiagnosticSummary(error.diagnostics)
                 .contains(u"private-thought"_s));
    QCOMPARE(server.requestCount(), 1);
}

void LLMTransportTest::cancelsAndTimesOutStreamsWithoutRetry() {
    for (const bool cancel : {false, true}) {
        ScriptedHttpServer server;
        HttpResponse response;
        response.contentType = "text/event-stream";
        response.body = streamEvent({{u"content"_s, u"partial"_s}});
        response.keepOpen = true;
        server.enqueue(response);
        auto client = makeClient(server);
        auto input = request();
        input.stream = true;
        input.timeoutMs = cancel ? 3000 : 500;
        std::optional<loreforge::llm::LLMResult> result;
        QSignalSpy progress(&client, &loreforge::llm::QwenClient::streamProgress);
        const auto id =
            client.enqueue(input, [&](const QUuid&, auto value) { result = std::move(value); });
        if (cancel) {
            QTRY_VERIFY(!progress.isEmpty());
            QVERIFY(client.cancel(id));
        }
        QTRY_VERIFY(result.has_value());
        const auto error = std::get<loreforge::llm::LLMError>(*result);
        QCOMPARE(error.code, cancel ? loreforge::llm::LLMErrorCode::Cancelled
                                    : loreforge::llm::LLMErrorCode::Timeout);
        QCOMPARE(server.requestCount(), 1);
    }
}

void LLMTransportTest::redactsCredentialsAcrossStreamDeltas() {
    ScriptedHttpServer server;
    HttpResponse response;
    response.contentType = "text/event-stream";
    response.chunkDelayMs = 40;
    response.chunks = {streamEvent({{u"content"_s, u"prefix fixture-"_s}}),
                       streamEvent({{u"content"_s, u"secret suffix"_s}}),
                       streamEvent({}, u"stop"_s), QByteArray("data: [DONE]\n\n")};
    server.enqueue(response);
    auto client = makeClient(server);
    auto input = request();
    input.stream = true;
    QSignalSpy progress(&client, &loreforge::llm::QwenClient::streamProgress);
    std::optional<loreforge::llm::LLMResult> result;
    static_cast<void>(
        client.enqueue(input, [&](const QUuid&, auto value) { result = std::move(value); }));
    QTRY_VERIFY(result.has_value());
    QVERIFY(!progress.isEmpty());
    for (const auto& event : progress)
        QVERIFY(!event[1].toString().contains(u"fixture-secret"_s));
    QVERIFY(progress.last()[1].toString().contains(QStringLiteral("[凭据已隐藏]")));
}

void LLMTransportTest::reportsSafeResponseDiagnostics_data() {
    using loreforge::llm::ResponseIssue;
    QTest::addColumn<QByteArray>("body");
    QTest::addColumn<int>("issue");
    QTest::addColumn<bool>("usageKnown");
    QTest::addColumn<QString>("finish");
    const auto row = [](const char* name, const QJsonValue& content, ResponseIssue issue,
                        const QString& finish = u"stop"_s, int usageMode = 0) {
        auto object = QJsonDocument::fromJson(successResponse(u"{}"_s)).object();
        QJsonObject message{{u"reasoning_content"_s, u"fixture-secret hidden thinking"_s}};
        if (!content.isUndefined())
            message.insert(u"content"_s, content);
        object.insert(u"choices"_s, QJsonArray{QJsonObject{{u"message"_s, message},
                                                           {u"finish_reason"_s, finish}}});
        if (usageMode == 1)
            object.remove(u"usage"_s);
        if (usageMode == 2)
            object.insert(u"usage"_s, QJsonObject{{u"prompt_tokens"_s, 1.5},
                                                  {u"completion_tokens"_s, 3},
                                                  {u"total_tokens"_s, 5}});
        QTest::newRow(name) << QJsonDocument(object).toJson(QJsonDocument::Compact)
                            << static_cast<int>(issue) << (usageMode == 0)
                            << (finish == u"fixture-secret"_s ? u"other"_s : finish);
    };
    QTest::newRow("bad-envelope-json")
        << QByteArray("<html>fixture-secret</html>")
        << static_cast<int>(ResponseIssue::EnvelopeJson) << false << u"missing"_s;
    QTest::newRow("missing-choices")
        << QByteArray("{}") << static_cast<int>(ResponseIssue::EnvelopeShape) << false
        << u"missing"_s;
    row("missing-content", QJsonValue(QJsonValue::Undefined), ResponseIssue::MissingContent);
    row("null-content", QJsonValue(QJsonValue::Null), ResponseIssue::MissingContent);
    row("array-content", QJsonArray{}, ResponseIssue::ContentType);
    row("empty-content", u""_s, ResponseIssue::EmptyContent);
    row("empty-length", u""_s, ResponseIssue::EmptyContent, u"length"_s);
    row("whitespace", u" \n"_s, ResponseIssue::EmptyContent);
    row("truncated-json", u"{\"value\":"_s, ResponseIssue::StructuredJson, u"length"_s);
    row("scalar-json", u"true"_s, ResponseIssue::StructuredType);
    row("missing-usage", u"invalid json"_s, ResponseIssue::StructuredJson, u"stop"_s, 1);
    row("invalid-usage", u"{}"_s, ResponseIssue::InvalidUsage, u"stop"_s, 2);
    row("complete-json-length", u"{}"_s, ResponseIssue::OutputLimit, u"length"_s);
    row("reflected-finish", u"invalid json"_s, ResponseIssue::StructuredJson, u"fixture-secret"_s);
}

void LLMTransportTest::reportsSafeResponseDiagnostics() {
    QFETCH(QByteArray, body);
    QFETCH(int, issue);
    QFETCH(bool, usageKnown);
    QFETCH(QString, finish);
    ScriptedHttpServer server;
    server.enqueue({200, body});
    auto client = makeClient(server);
    auto input = request();
    input.responseFormat = {{u"type"_s, u"json_object"_s}};
    std::optional<loreforge::llm::LLMResult> result;
    const auto id = client.enqueue(
        input, [&](const QUuid&, loreforge::llm::LLMResult value) { result = std::move(value); });
    QVERIFY(!id.isNull());
    QTRY_VERIFY(result.has_value());
    QVERIFY(std::holds_alternative<loreforge::llm::LLMError>(*result));
    const auto error = std::get<loreforge::llm::LLMError>(*result);
    QCOMPARE(error.code, loreforge::llm::LLMErrorCode::InvalidResponse);
    QCOMPARE(error.httpStatus, 200);
    QCOMPARE(static_cast<int>(error.diagnostics.issue), issue);
    QCOMPARE(error.diagnostics.usageReported, usageKnown);
    QCOMPARE(error.diagnostics.finishReason, finish);
    if (usageKnown)
        QCOMPARE(error.diagnostics.usage.totalTokens, 7);
    const auto safe =
        QJsonDocument(loreforge::llm::responseDiagnosticsJson(error.diagnostics)).toJson();
    QVERIFY(!safe.contains("fixture-secret"));
    QVERIFY(!safe.contains("hidden thinking"));
    const auto summary = loreforge::llm::responseDiagnosticSummary(error.diagnostics);
    QVERIFY(!summary.contains(u"fixture-secret"_s));
    QVERIFY(!summary.isEmpty());
    QCOMPARE(server.requestCount(), 1);
}

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

void LLMTransportTest::requiresExplicitUnauthenticatedConfiguration() {
    ScriptedHttpServer server;
    loreforge::llm::QwenClient client({server.endpoint(), {}, u"fixture"_s});
    std::optional<loreforge::llm::LLMResult> result;
    const auto id =
        client.enqueue(request(), [&result](const QUuid&, loreforge::llm::LLMResult value) {
            result = std::move(value);
        });
    QVERIFY(!id.isNull());
    QTRY_VERIFY(result.has_value());
    QVERIFY(std::holds_alternative<loreforge::llm::LLMError>(*result));
    QCOMPARE(std::get<loreforge::llm::LLMError>(*result).code,
             loreforge::llm::LLMErrorCode::InvalidConfiguration);
    QCOMPARE(server.requestCount(), 0);
}

void LLMTransportTest::doesNotFollowRedirects() {
    ScriptedHttpServer origin;
    ScriptedHttpServer destination;
    origin.enqueue({302, QByteArrayLiteral("{}"), true,
                    QByteArrayLiteral("Location: ") + destination.endpoint().toEncoded() + "\r\n"});
    auto client = makeClient(origin);
    std::optional<loreforge::llm::LLMResult> result;
    const auto id =
        client.enqueue(request(), [&result](const QUuid&, loreforge::llm::LLMResult value) {
            result = std::move(value);
        });
    QVERIFY(!id.isNull());
    QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 1000);
    QVERIFY(std::holds_alternative<loreforge::llm::LLMError>(*result));
    QCOMPARE(std::get<loreforge::llm::LLMError>(*result).httpStatus, 302);
    QCOMPARE(destination.requestCount(), 0);
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
    QVERIFY(response.usageReported);
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
