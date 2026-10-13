#include "loreforge/llm/qwen_client.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>

#include <algorithm>
#include <cmath>
#include <limits>
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

QByteArray requestPayload(const LLMRequest& request, QStringView model, const QString& tokenField) {
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
        payload.insert(tokenField, request.maxCompletionTokens);
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
    if (value.isUndefined() || value.isNull()) {
        return TokenUsage{};
    }
    if (!value.isObject()) {
        return std::nullopt;
    }
    const auto usage = value.toObject();
    const auto prompt = usage.value(QStringLiteral("prompt_tokens"));
    const auto completion = usage.value(QStringLiteral("completion_tokens"));
    const auto total = usage.value(QStringLiteral("total_tokens"));
    const auto validCount = [](const QJsonValue& count) {
        const auto number = count.toDouble(-1);
        return count.isDouble() && std::isfinite(number) && number >= 0 &&
               number <= std::numeric_limits<int>::max() && std::floor(number) == number;
    };
    if (!validCount(prompt) || !validCount(completion) || !validCount(total)) {
        return std::nullopt;
    }
    const TokenUsage result{prompt.toInt(-1), completion.toInt(-1), total.toInt(-1)};
    if (result.promptTokens < 0 || result.completionTokens < 0 || result.totalTokens < 0) {
        return std::nullopt;
    }
    return result;
}

} // namespace

QJsonObject responseDiagnosticsJson(const ResponseDiagnostics& value) {
    if (value.issue == ResponseIssue::None)
        return {};
    const QStringList issues{QStringLiteral("none"),
                             QStringLiteral("envelope_json"),
                             QStringLiteral("envelope_shape"),
                             QStringLiteral("missing_content"),
                             QStringLiteral("content_type"),
                             QStringLiteral("empty_content"),
                             QStringLiteral("invalid_usage"),
                             QStringLiteral("structured_json"),
                             QStringLiteral("structured_type"),
                             QStringLiteral("output_limit")};
    QJsonObject result{{QStringLiteral("issue"), issues.at(static_cast<int>(value.issue))},
                       {QStringLiteral("finish_reason"), value.finishReason},
                       {QStringLiteral("response_bytes"), value.responseBytes},
                       {QStringLiteral("content_bytes"), value.contentBytes},
                       {QStringLiteral("reasoning_bytes"), value.reasoningBytes},
                       {QStringLiteral("json_error_offset"), value.jsonErrorOffset},
                       {QStringLiteral("usage_state"),
                        value.usageReported ? QStringLiteral("reported")
                                            : (value.usageInvalid ? QStringLiteral("invalid")
                                                                  : QStringLiteral("missing"))}};
    if (value.usageReported) {
        result.insert(
            QStringLiteral("usage"),
            QJsonObject{{QStringLiteral("prompt_tokens"), value.usage.promptTokens},
                        {QStringLiteral("completion_tokens"), value.usage.completionTokens},
                        {QStringLiteral("total_tokens"), value.usage.totalTokens}});
    }
    return result;
}

QString responseDiagnosticSummary(const ResponseDiagnostics& value) {
    if (value.issue == ResponseIssue::None)
        return {};
    QString reason;
    switch (value.issue) {
    case ResponseIssue::EnvelopeJson:
        reason = QStringLiteral("响应外层不是合法 JSON");
        break;
    case ResponseIssue::EnvelopeShape:
        reason = QStringLiteral("响应外层结构异常（choices/message）");
        break;
    case ResponseIssue::MissingContent:
        reason = QStringLiteral("响应正文 content 缺失或为 null");
        break;
    case ResponseIssue::ContentType:
        reason = QStringLiteral("响应正文 content 不是字符串");
        break;
    case ResponseIssue::EmptyContent:
        reason = QStringLiteral("响应正文为空");
        break;
    case ResponseIssue::InvalidUsage:
        reason = QStringLiteral("token 用量字段格式异常");
        break;
    case ResponseIssue::StructuredJson:
        reason = QStringLiteral("正文不是合法 JSON（可能截断）");
        break;
    case ResponseIssue::StructuredType:
        reason = QStringLiteral("正文 JSON 顶层不是对象或数组");
        break;
    case ResponseIssue::OutputLimit:
        reason = QStringLiteral("输出未完成，服务报告长度限制");
        break;
    case ResponseIssue::None:
        break;
    }
    reason += QStringLiteral("；结束原因 %1；正文 %2 字节；思考 %3 字节")
                  .arg(value.finishReason)
                  .arg(value.contentBytes < 0 ? QStringLiteral("未提供")
                                              : QString::number(value.contentBytes))
                  .arg(value.reasoningBytes < 0 ? QStringLiteral("未提供")
                                                : QString::number(value.reasoningBytes));
    if (value.finishReason == QStringLiteral("length")) {
        reason += QStringLiteral("；服务报告已达输出/上下文限制，不能视为完整结果");
    }
    if (value.jsonErrorOffset >= 0)
        reason += QStringLiteral("；JSON 错误偏移 %1").arg(value.jsonErrorOffset);
    return reason;
}

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

QByteArray QwenClient::requestBody(const LLMRequest& request) const {
    const auto model = request.model.trimmed().isEmpty() ? options_.defaultModel.trimmed()
                                                         : request.model.trimmed();
    return requestPayload(request, model,
                          options_.completionTokenParameter == CompletionTokenParameter::MaxTokens
                              ? QStringLiteral("max_tokens")
                              : QStringLiteral("max_completion_tokens"));
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
    QNetworkRequest networkRequest(options_.endpoint);
    networkRequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                                QNetworkRequest::ManualRedirectPolicy);
    networkRequest.setHeader(QNetworkRequest::ContentTypeHeader,
                             QStringLiteral("application/json"));
    if (!options_.apiKey.trimmed().isEmpty()) {
        networkRequest.setRawHeader(QByteArrayLiteral("Authorization"),
                                    QByteArrayLiteral("Bearer ") + options_.apiKey);
    }
    networkRequest.setRawHeader(QByteArrayLiteral("Accept"), QByteArrayLiteral("application/json"));
    networkRequest.setRawHeader(QByteArrayLiteral("User-Agent"),
                                QByteArrayLiteral("LoreForge/0.1"));
    activePayload_ = requestBody(active_->request);
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
    ResponseDiagnostics diagnostics;
    diagnostics.responseBytes = payload.size();
    diagnostics.finishReason = QStringLiteral("missing");
    const auto usage = tokenUsage(object.value(QStringLiteral("usage")));
    diagnostics.usageInvalid = !usage.has_value();
    diagnostics.usageReported =
        usage.has_value() && object.value(QStringLiteral("usage")).isObject();
    if (usage)
        diagnostics.usage = *usage;
    const auto invalid = [&](ResponseIssue issue) {
        diagnostics.issue = issue;
        finishActive(LLMError{LLMErrorCode::InvalidResponse,
                              QStringLiteral("The provider response failed validation."),
                              {},
                              false,
                              httpStatus,
                              active_->attemptCount,
                              requestTimer_.elapsed(),
                              activePayload_,
                              payload,
                              diagnostics});
    };
    if (parseError.error != QJsonParseError::NoError || !document.isObject() || choices.isEmpty() ||
        !choices.first().isObject()) {
        if (parseError.error != QJsonParseError::NoError)
            diagnostics.jsonErrorOffset = parseError.offset;
        invalid(parseError.error == QJsonParseError::NoError ? ResponseIssue::EnvelopeShape
                                                             : ResponseIssue::EnvelopeJson);
        return;
    }
    const auto choice = choices.first().toObject();
    const auto finish = choice.value(QStringLiteral("finish_reason")).toString();
    const QStringList allowedFinish{
        QStringLiteral("stop"),           QStringLiteral("length"),
        QStringLiteral("content_filter"), QStringLiteral("tool_calls"),
        QStringLiteral("function_call"),  QStringLiteral("insufficient_system_resource"),
        QStringLiteral("aborted")};
    diagnostics.finishReason =
        allowedFinish.contains(finish)
            ? finish
            : (finish.isEmpty() ? QStringLiteral("missing") : QStringLiteral("other"));
    if (!choice.value(QStringLiteral("message")).isObject()) {
        invalid(ResponseIssue::EnvelopeShape);
        return;
    }
    const auto message = choice.value(QStringLiteral("message")).toObject();
    const auto contentValue = message.value(QStringLiteral("content"));
    if (message.value(QStringLiteral("reasoning_content")).isString()) {
        diagnostics.reasoningBytes =
            message.value(QStringLiteral("reasoning_content")).toString().toUtf8().size();
    }
    if (contentValue.isString())
        diagnostics.contentBytes = contentValue.toString().toUtf8().size();
    if (!contentValue.isString()) {
        invalid(contentValue.isNull() || contentValue.isUndefined() ? ResponseIssue::MissingContent
                                                                    : ResponseIssue::ContentType);
        return;
    }
    if (contentValue.toString().trimmed().isEmpty()) {
        invalid(ResponseIssue::EmptyContent);
        return;
    }
    if (!usage.has_value()) {
        invalid(ResponseIssue::InvalidUsage);
        return;
    }

    QJsonValue parsedContent;
    const auto formatType =
        active_->request.responseFormat.value(QStringLiteral("type")).toString();
    if (!formatType.isEmpty() && formatType != QStringLiteral("text")) {
        QJsonParseError contentError;
        const auto structured =
            QJsonDocument::fromJson(contentValue.toString().toUtf8(), &contentError);
        if (contentError.error != QJsonParseError::NoError) {
            const auto scalar =
                QJsonDocument::fromJson(QByteArray("[") + contentValue.toString().toUtf8() + "]");
            if (scalar.isArray() && scalar.array().size() == 1) {
                invalid(ResponseIssue::StructuredType);
                return;
            }
            diagnostics.jsonErrorOffset = contentError.offset;
            invalid(ResponseIssue::StructuredJson);
            return;
        }
        if (!structured.isObject() && !structured.isArray()) {
            invalid(ResponseIssue::StructuredType);
            return;
        }
        parsedContent = structured.isObject() ? QJsonValue(structured.object())
                                              : QJsonValue(structured.array());
        if (finish == QStringLiteral("length")) {
            invalid(ResponseIssue::OutputLimit);
            return;
        }
    }

    finishActive(
        LLMResponse{object.value(QStringLiteral("id")).toString(),
                    object.value(QStringLiteral("model")).toString(), contentValue.toString(),
                    choice.value(QStringLiteral("finish_reason")).toString(), parsedContent, *usage,
                    active_->attemptCount, requestTimer_.elapsed(), activePayload_, payload,
                    object.value(QStringLiteral("usage")).isObject()});
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
        options_.endpoint.host().isEmpty() || !options_.endpoint.userInfo().isEmpty() ||
        options_.endpoint.hasQuery() || options_.endpoint.hasFragment() ||
        (!options_.allowUnauthenticated && options_.apiKey.trimmed().isEmpty()) ||
        options_.apiKey.contains('\r') || options_.apiKey.contains('\n') ||
        (options_.completionTokenParameter != CompletionTokenParameter::MaxCompletionTokens &&
         options_.completionTokenParameter != CompletionTokenParameter::MaxTokens) ||
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
