#pragma once

#include "loreforge/llm/illm_client.h"

#include <QElapsedTimer>
#include <QNetworkAccessManager>
#include <QObject>
#include <QQueue>
#include <QTimer>
#include <QUrl>

#include <memory>
#include <optional>

class QNetworkReply;

namespace loreforge::llm {
class ChatCompletionStream;

enum class CompletionTokenParameter { MaxCompletionTokens, MaxTokens };

struct QwenClientOptions final {
    QUrl endpoint;
    QByteArray apiKey;
    QString defaultModel;
    bool allowUnauthenticated = false;
    CompletionTokenParameter completionTokenParameter =
        CompletionTokenParameter::MaxCompletionTokens;
    // Platform-specific extension; absent preserves compatibility/provider defaults.
    std::optional<bool> enableThinking = std::nullopt;

    friend bool operator==(const QwenClientOptions&, const QwenClientOptions&) = default;
};

class QwenClient final : public QObject, public ILLMClient {
    Q_OBJECT

  public:
    explicit QwenClient(QwenClientOptions options, QObject* parent = nullptr);
    ~QwenClient() override;

    [[nodiscard]] QUuid enqueue(LLMRequest request, CompletionHandler completion) override;
    [[nodiscard]] bool cancel(const QUuid& requestId) override;
    [[nodiscard]] qsizetype pendingRequestCount() const noexcept override;
    // Exact JSON body, without authentication headers; shared by transport and audit storage.
    [[nodiscard]] QByteArray requestBody(const LLMRequest& request) const;

  signals:
    void streamStarted(QUuid requestId);
    // Sanitized current-request tail, not raw wire data or reasoning text.
    void streamProgress(QUuid requestId, QString text, bool generating);
    void streamFinished(QUuid requestId, QString status);

  private:
    struct PendingRequest final {
        QUuid id;
        LLMRequest request;
        CompletionHandler completion;
        int attemptCount = 0;
    };

    void processNext();
    void sendAttempt();
    void handleReplyFinished();
    void readAvailable();
    void reportStreamProgress(bool generating);
    [[nodiscard]] ResponseDiagnostics streamDiagnostics(ResponseIssue issue) const;
    void handleTimeout();
    void retryOrFinish(LLMError error);
    void finishActive(LLMResult result);
    [[nodiscard]] std::optional<LLMError> validate(const LLMRequest& request) const;

    QwenClientOptions options_;
    QNetworkAccessManager network_;
    QQueue<PendingRequest> queue_;
    std::optional<PendingRequest> active_;
    QNetworkReply* reply_ = nullptr;
    QByteArray activePayload_;
    QTimer timeoutTimer_;
    QTimer retryTimer_;
    QElapsedTimer requestTimer_;
    bool attemptTimedOut_ = false;
    std::unique_ptr<ChatCompletionStream> stream_;
    QByteArray responseBuffer_;
    bool receivedSse_ = false;
    bool responseTooLarge_ = false;
};

} // namespace loreforge::llm
