#include "loreforge/llm/qwen_client.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>

#include <algorithm>
#include <utility>

namespace loreforge::llm {
namespace {

QString roleName(LLMRole role) {
    switch (role) {
    case LLMRole::System:
        return QStringLiteral("system");
    case LLMRole::User:
        return QStringLiteral("user");
    case LLMRole::Assistant:
        return QStringLiteral("assistant");
    }
    return {};
}

QByteArray requestPayload(const LLMRequest& request, QStringView model) {
    QJsonArray messages;
    for (const auto& message : request.messages) {
        messages.append(QJsonObject{{QStringLiteral("role"), roleName(message.role)},
                                    {QStringLiteral("content"), message.content}});
    }

    QJsonObject payload{{QStringLiteral("model"), model.toString()},
                        {QStringLiteral("messages"), messages},
                        {QStringLiteral("stream"), false}};
    if (!request.responseFormat.isEmpty()) {
        payload.insert(QStringLiteral("response_format"), request.responseFormat);
    }
    if (request.maxCompletionTokens > 0) {
        payload.insert(QStringLiteral("max_completion_tokens"), request.maxCompletionTokens);
    }
    return QJsonDocument(payload).toJson(QJsonDocument::Compact);
}

QString providerErrorMessage(const QByteArray& payload, QStringView fallback) {
    const auto document = QJsonDocument::fromJson(payload);
    const auto message = document.object()
                             .value(QStringLiteral("error"))
                             .toObject()
                             .value(QStringLiteral("message"))
                             .toString();
    return message.isEmpty() ? fallback.toString() : message;
}

bool isRetryableHttpStatus(int status) {
    return status == 408 || status == 425 || status == 429 || status >= 500;
}

std::optional<TokenUsage> tokenUsage(const QJsonValue& value) {
    if (value.isUndefined()) {
        return TokenUsage{};
    }
    if (!value.isObject()) {
        return std::nullopt;
    }
    const auto usage = value.toObject();
    const auto prompt = usage.value(QStringLiteral("prompt_tokens"));
    const auto completion = usage.value(QStringLiteral("completion_tokens"));
    const auto total = usage.value(QStringLiteral("total_tokens"));
    if (!prompt.isDouble() || !completion.isDouble() || !total.isDouble()) {
        return std::nullopt;
    }
    const TokenUsage result{prompt.toInt(-1), completion.toInt(-1), total.toInt(-1)};
    if (result.promptTokens < 0 || result.completionTokens < 0 || result.totalTokens < 0) {
        return std::nullopt;
    }
    return result;
}

std::optional<QJsonValue> structuredContent(QStringView content) {
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(content.toString().toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        return std::nullopt;
    }
    if (document.isObject()) {
        return QJsonValue(document.object());
    }
    if (document.isArray()) {
        return QJsonValue(document.array());
    }
    return std::nullopt;
}

} // namespace

QString errorCodeName(LLMErrorCode code) {
    switch (code) {
    case LLMErrorCode::InvalidConfiguration:
        return QStringLiteral("invalid_configuration");
    case LLMErrorCode::InvalidRequest:
        return QStringLiteral("invalid_request");
    case LLMErrorCode::Network:
        return QStringLiteral("network");
    case LLMErrorCode::Timeout:
        return QStringLiteral("timeout");
    case LLMErrorCode::Cancelled:
        return QStringLiteral("cancelled");
    case LLMErrorCode::Http:
        return QStringLiteral("http");
    case LLMErrorCode::InvalidResponse:
        return QStringLiteral("invalid_response");
    }
    return QStringLiteral("unknown");
}

QwenClient::QwenClient(QwenClientOptions options, QObject* parent)
    : QObject(parent), options_(std::move(options)) {
    timeoutTimer_.setSingleShot(true);
    retryTimer_.setSingleShot(true);
    connect(&timeoutTimer_, &QTimer::timeout, this, &QwenClient::handleTimeout);
    connect(&retryTimer_, &QTimer::timeout, this, &QwenClient::sendAttempt);
}

QwenClient::~QwenClient() {
    timeoutTimer_.stop();
    retryTimer_.stop();
    if (reply_ != nullptr) {
        disconnect(reply_, nullptr, this, nullptr);
        reply_->abort();
        reply_ = nullptr;
    }
}

QUuid QwenClient::enqueue(LLMRequest request, CompletionHandler completion) {
    const auto requestId = QUuid::createUuid();
    queue_.enqueue({requestId, std::move(request), std::move(completion), 0});
    QTimer::singleShot(0, this, &QwenClient::processNext);
    return requestId;
}

bool QwenClient::cancel(const QUuid& requestId) {
    for (auto iterator = queue_.begin(); iterator != queue_.end(); ++iterator) {
        if (iterator->id != requestId) {
            continue;
        }
        auto pending = std::move(*iterator);
        queue_.erase(iterator);
        if (pending.completion) {
            pending.completion(pending.id,
                               LLMError{LLMErrorCode::Cancelled,
                                        QStringLiteral("The LLM request was cancelled."),
                                        {},
                                        false,
                                        0,
                                        0,
                                        0});
        }
        return true;
    }

    if (!active_.has_value() || active_->id != requestId) {
        return false;
    }
    timeoutTimer_.stop();
    retryTimer_.stop();
    if (reply_ != nullptr) {
        disconnect(reply_, nullptr, this, nullptr);
        reply_->abort();
        reply_->deleteLater();
        reply_ = nullptr;
    }
    finishActive(LLMError{LLMErrorCode::Cancelled,
                          QStringLiteral("The LLM request was cancelled."),
                          {},
                          false,
                          0,
                          active_->attemptCount,
                          requestTimer_.isValid() ? requestTimer_.elapsed() : 0,
                          activePayload_,
                          {}});
    return true;
}

qsizetype QwenClient::pendingRequestCount() const noexcept {
    return queue_.size() + (active_.has_value() ? 1 : 0);
}

void QwenClient::processNext() {
    if (active_.has_value() || queue_.isEmpty()) {
        return;
    }
    active_ = queue_.dequeue();
    requestTimer_.start();
    if (auto error = validate(active_->request); error.has_value()) {
        finishActive(std::move(*error));
        return;
    }
    sendAttempt();
}

void QwenClient::sendAttempt() {
    if (!active_.has_value()) {
        return;
    }
    ++active_->attemptCount;
    attemptTimedOut_ = false;
    const auto model = active_->request.model.trimmed().isEmpty()
                           ? options_.defaultModel.trimmed()
                           : active_->request.model.trimmed();
    QNetworkRequest networkRequest(options_.endpoint);
    networkRequest.setHeader(QNetworkRequest::ContentTypeHeader,
                             QStringLiteral("application/json"));
    networkRequest.setRawHeader(QByteArrayLiteral("Authorization"),
                                QByteArrayLiteral("Bearer ") + options_.apiKey);
    networkRequest.setRawHeader(QByteArrayLiteral("Accept"), QByteArrayLiteral("application/json"));
    networkRequest.setRawHeader(QByteArrayLiteral("User-Agent"),
                                QByteArrayLiteral("LoreForge/0.1"));
    activePayload_ = requestPayload(active_->request, model);
    reply_ = network_.post(networkRequest, activePayload_);
    connect(reply_, &QNetworkReply::finished, this, &QwenClient::handleReplyFinished);
    timeoutTimer_.start(active_->request.timeoutMs);
}

void QwenClient::handleReplyFinished() {
    if (!active_.has_value() || reply_ == nullptr) {
        return;
    }
    timeoutTimer_.stop();
    auto* completedReply = reply_;
    reply_ = nullptr;
    const auto networkError = completedReply->error();
    const auto networkErrorText = completedReply->errorString();
    const auto httpStatus =
        completedReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const auto payload = attemptTimedOut_ ? QByteArray{} : completedReply->readAll();
    completedReply->deleteLater();

    if (attemptTimedOut_) {
        retryOrFinish(LLMError{LLMErrorCode::Timeout,
                               QStringLiteral("The Qwen request timed out."),
                               {},
                               true,
                               0,
                               active_->attemptCount,
                               requestTimer_.elapsed(),
                               activePayload_,
                               {}});
        return;
    }
    if (httpStatus == 0 && networkError != QNetworkReply::NoError) {
        retryOrFinish(LLMError{LLMErrorCode::Network,
                               QStringLiteral("The Qwen network request failed."), networkErrorText,
                               true, 0, active_->attemptCount, requestTimer_.elapsed(),
                               activePayload_, payload});
        return;
    }
    if (httpStatus < 200 || httpStatus >= 300) {
        const auto fallback = httpStatus == 0
                                  ? QStringLiteral("The Qwen request failed.")
                                  : QStringLiteral("Qwen returned HTTP %1.").arg(httpStatus);
        retryOrFinish(LLMError{LLMErrorCode::Http, providerErrorMessage(payload, fallback),
                               QString::fromUtf8(payload), isRetryableHttpStatus(httpStatus),
                               httpStatus, active_->attemptCount, requestTimer_.elapsed(),
                               activePayload_, payload});
        return;
    }
    if (networkError != QNetworkReply::NoError) {
        retryOrFinish(LLMError{LLMErrorCode::Network,
                               QStringLiteral("The Qwen network request failed."), networkErrorText,
                               true, httpStatus, active_->attemptCount, requestTimer_.elapsed(),
                               activePayload_, payload});
        return;
    }

    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(payload, &parseError);
    const auto object = document.object();
    const auto choices = object.value(QStringLiteral("choices")).toArray();
    if (parseError.error != QJsonParseError::NoError || !document.isObject() || choices.isEmpty() ||
        !choices.first().isObject()) {
        finishActive(LLMError{LLMErrorCode::InvalidResponse,
                              QStringLiteral("Qwen returned an invalid response."),
                              parseError.errorString(), false, httpStatus, active_->attemptCount,
                              requestTimer_.elapsed(), activePayload_, payload});
        return;
    }
    const auto choice = choices.first().toObject();
    const auto message = choice.value(QStringLiteral("message")).toObject();
    const auto contentValue = message.value(QStringLiteral("content"));
    const auto usage = tokenUsage(object.value(QStringLiteral("usage")));
    if (!contentValue.isString() || !usage.has_value()) {
        finishActive(LLMError{LLMErrorCode::InvalidResponse,
                              QStringLiteral("Qwen returned incomplete response data."),
                              {},
                              false,
                              httpStatus,
                              active_->attemptCount,
                              requestTimer_.elapsed(),
                              activePayload_,
                              payload});
        return;
    }

    QJsonValue parsedContent;
    const auto formatType =
        active_->request.responseFormat.value(QStringLiteral("type")).toString();
    if (!formatType.isEmpty() && formatType != QStringLiteral("text")) {
        const auto structured = structuredContent(contentValue.toString());
        if (!structured.has_value()) {
            finishActive(LLMError{LLMErrorCode::InvalidResponse,
                                  QStringLiteral("Qwen returned invalid structured JSON."),
                                  {},
                                  false,
                                  httpStatus,
                                  active_->attemptCount,
                                  requestTimer_.elapsed(),
                                  activePayload_,
                                  payload});
            return;
        }
        parsedContent = *structured;
    }

    finishActive(
        LLMResponse{object.value(QStringLiteral("id")).toString(),
                    object.value(QStringLiteral("model")).toString(), contentValue.toString(),
                    choice.value(QStringLiteral("finish_reason")).toString(), parsedContent, *usage,
                    active_->attemptCount, requestTimer_.elapsed(), activePayload_, payload});
}

void QwenClient::handleTimeout() {
    if (reply_ == nullptr) {
        return;
    }
    attemptTimedOut_ = true;
    reply_->abort();
}

void QwenClient::retryOrFinish(LLMError error) {
    if (!active_.has_value() || !error.retryable ||
        active_->attemptCount > active_->request.retryPolicy.maxRetries) {
        finishActive(std::move(error));
        return;
    }

    qint64 delay = active_->request.retryPolicy.initialDelayMs;
    for (int retry = 1; retry < active_->attemptCount; ++retry) {
        delay = std::min<qint64>(delay * 2, active_->request.retryPolicy.maximumDelayMs);
    }
    retryTimer_.start(static_cast<int>(delay));
}

void QwenClient::finishActive(LLMResult result) {
    if (!active_.has_value()) {
        return;
    }
    timeoutTimer_.stop();
    retryTimer_.stop();
    auto completed = std::move(*active_);
    active_.reset();
    activePayload_.clear();
    QTimer::singleShot(0, this, &QwenClient::processNext);
    if (completed.completion) {
        completed.completion(completed.id, std::move(result));
    }
}

std::optional<LLMError> QwenClient::validate(const LLMRequest& request) const {
    const auto scheme = options_.endpoint.scheme().toLower();
    if (!options_.endpoint.isValid() ||
        (scheme != QStringLiteral("https") && scheme != QStringLiteral("http")) ||
        options_.apiKey.trimmed().isEmpty() ||
        (request.model.trimmed().isEmpty() && options_.defaultModel.trimmed().isEmpty())) {
        return LLMError{LLMErrorCode::InvalidConfiguration,
                        QStringLiteral("Qwen endpoint, API key, and model are required."),
                        {},
                        false,
                        0,
                        0,
                        requestTimer_.elapsed()};
    }
    if (request.messages.isEmpty() || request.timeoutMs <= 0 || request.maxCompletionTokens < 0 ||
        request.retryPolicy.maxRetries < 0 || request.retryPolicy.initialDelayMs < 0 ||
        request.retryPolicy.maximumDelayMs < request.retryPolicy.initialDelayMs) {
        return LLMError{LLMErrorCode::InvalidRequest,
                        QStringLiteral("The LLM request options are invalid."),
                        {},
                        false,
                        0,
                        0,
                        requestTimer_.elapsed()};
    }
    for (const auto& message : request.messages) {
        if (message.content.trimmed().isEmpty()) {
            return LLMError{LLMErrorCode::InvalidRequest,
                            QStringLiteral("LLM messages cannot be empty."),
                            {},
                            false,
                            0,
                            0,
                            requestTimer_.elapsed()};
        }
    }
    if (!request.responseFormat.isEmpty()) {
        const auto type = request.responseFormat.value(QStringLiteral("type")).toString();
        if (type != QStringLiteral("text") && type != QStringLiteral("json_object") &&
            type != QStringLiteral("json_schema")) {
            return LLMError{LLMErrorCode::InvalidRequest,
                            QStringLiteral("The structured response format is unsupported."),
                            {},
                            false,
                            0,
                            0,
                            requestTimer_.elapsed()};
        }
    }
    return std::nullopt;
}

} // namespace loreforge::llm
