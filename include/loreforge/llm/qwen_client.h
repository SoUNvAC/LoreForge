#pragma once

#include "loreforge/llm/illm_client.h"

#include <QElapsedTimer>
#include <QNetworkAccessManager>
#include <QObject>
#include <QQueue>
#include <QTimer>
#include <QUrl>

#include <optional>

class QNetworkReply;

namespace loreforge::llm {

struct QwenClientOptions final {
    QUrl endpoint;
    QByteArray apiKey;
    QString defaultModel;

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
    void handleTimeout();
    void retryOrFinish(LLMError error);
    void finishActive(LLMResult result);
    [[nodiscard]] std::optional<LLMError> validate(const LLMRequest& request) const;

    QwenClientOptions options_;
    QNetworkAccessManager network_;
    QQueue<PendingRequest> queue_;
    std::optional<PendingRequest> active_;
    QNetworkReply* reply_ = nullptr;
    QTimer timeoutTimer_;
    QTimer retryTimer_;
    QElapsedTimer requestTimer_;
    bool attemptTimedOut_ = false;
};

} // namespace loreforge::llm
