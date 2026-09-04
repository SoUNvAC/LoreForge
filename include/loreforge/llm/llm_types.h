#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QUuid>

#include <functional>
#include <variant>

namespace loreforge::llm {

enum class LLMRole {
    System,
    User,
    Assistant,
};

struct LLMMessage final {
    LLMRole role = LLMRole::User;
    QString content;

    friend bool operator==(const LLMMessage&, const LLMMessage&) = default;
};

struct RetryPolicy final {
    int maxRetries = 2;
    int initialDelayMs = 250;
    int maximumDelayMs = 2'000;

    friend bool operator==(const RetryPolicy&, const RetryPolicy&) = default;
};

struct LLMRequest final {
    QString model;
    QList<LLMMessage> messages;
    QJsonObject responseFormat;
    int maxCompletionTokens = 0;
    int timeoutMs = 30'000;
    RetryPolicy retryPolicy;

    friend bool operator==(const LLMRequest&, const LLMRequest&) = default;
};

struct TokenUsage final {
    int promptTokens = 0;
    int completionTokens = 0;
    int totalTokens = 0;

    friend bool operator==(const TokenUsage&, const TokenUsage&) = default;
};

struct LLMResponse final {
    QString providerRequestId;
    QString model;
    QString content;
    QString finishReason;
    QJsonValue structuredContent;
    TokenUsage usage;
    int attemptCount = 0;
    qint64 latencyMs = 0;
    QByteArray rawRequest;
    QByteArray rawResponse;

    friend bool operator==(const LLMResponse&, const LLMResponse&) = default;
};

enum class LLMErrorCode {
    InvalidConfiguration,
    InvalidRequest,
    Network,
    Timeout,
    Cancelled,
    Http,
    InvalidResponse,
};

struct LLMError final {
    LLMErrorCode code = LLMErrorCode::InvalidRequest;
    QString message;
    QString technicalDetails;
    bool retryable = false;
    int httpStatus = 0;
    int attemptCount = 0;
    qint64 latencyMs = 0;
    QByteArray rawRequest;
    QByteArray rawResponse;

    friend bool operator==(const LLMError&, const LLMError&) = default;
};

using LLMResult = std::variant<LLMResponse, LLMError>;
using CompletionHandler = std::function<void(const QUuid&, LLMResult)>;

[[nodiscard]] QString errorCodeName(LLMErrorCode code);

} // namespace loreforge::llm
