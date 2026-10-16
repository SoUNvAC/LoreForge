#pragma once

#include "loreforge/llm/llm_types.h"

#include <QJsonArray>
#include <QJsonDocument>

namespace loreforge::llm {
// Incremental SSE framing on bytes, so UTF-8 and CRLF may cross network reads.
// Never retain or expose reasoning text; only its UTF-8 byte count is kept.
class ChatCompletionStream final {
  public:
    void append(const QByteArray& bytes) {
        wireBytes += bytes.size();
        if (wireBytes > maximumWireBytes) {
            issue = ResponseIssue::ResponseLimit;
            return;
        }
        if (issue != ResponseIssue::None)
            return;
        pending_ += bytes;
        qsizetype offset = 0;
        while (offset < pending_.size()) {
            auto end = offset;
            while (end < pending_.size() && pending_[end] != '\r' && pending_[end] != '\n')
                ++end;
            if (end == pending_.size() || (pending_[end] == '\r' && end + 1 == pending_.size()))
                break;
            const auto line = pending_.mid(offset, end - offset);
            offset = end + ((pending_[end] == '\r' && pending_[end + 1] == '\n') ? 2 : 1);
            consumeLine(line);
            if (issue != ResponseIssue::None)
                break;
        }
        pending_.remove(0, offset);
        if (pending_.size() + data_.size() > maximumEventBytes)
            issue = ResponseIssue::ResponseLimit;
    }

    void finish() {
        // A terminal CR is a complete line ending; other partial events are not.
        if (pending_.endsWith('\r')) {
            append(QByteArray("\n"));
            --wireBytes;
        }
        if (issue == ResponseIssue::None &&
            (!done || finishReason.isEmpty() || !pending_.trimmed().isEmpty() || hasData_))
            issue = ResponseIssue::StreamIncomplete;
    }

    [[nodiscard]] QJsonObject envelope() const {
        QJsonObject result{
            {QStringLiteral("id"), id},
            {QStringLiteral("model"), model},
            {QStringLiteral("choices"),
             QJsonArray{QJsonObject{
                 {QStringLiteral("message"), QJsonObject{{QStringLiteral("content"), content}}},
                 {QStringLiteral("finish_reason"), finishReason}}}}};
        if (!usage.isUndefined())
            result.insert(QStringLiteral("usage"), usage);
        return result;
    }

    static constexpr qint64 maximumWireBytes = 16 * 1024 * 1024;
    static constexpr qsizetype maximumEventBytes = 1024 * 1024;
    ResponseIssue issue = ResponseIssue::None;
    QString content;
    QString id;
    QString model;
    QString finishReason;
    QJsonValue usage{QJsonValue::Undefined};
    qint64 wireBytes = 0;
    qint64 contentBytes = 0;
    qint64 reasoningBytes = 0;
    quint64 activity = 0;
    quint64 generation = 0;
    bool done = false;
    StreamProblem problem = StreamProblem::None;
    qint64 eventIndex = 0;
    qint64 eventBytes = 0;
    qint64 jsonErrorOffset = -1;

  private:
    void consumeLine(QByteArray line) {
        if (firstLine_) {
            if (line.startsWith("\xef\xbb\xbf"))
                line.remove(0, 3);
            firstLine_ = false;
        }
        if (line.isEmpty()) {
            if (hasData_) {
                dispatch();
                data_.clear();
                hasData_ = false;
            }
            return;
        }
        if (line.startsWith(':')) {
            ++activity;
            return;
        }
        const auto colon = line.indexOf(':');
        const auto field = colon < 0 ? line : line.first(colon);
        if (field != "data")
            return; // event/id/retry are metadata, not model output or retry instructions.
        auto value = colon < 0 ? QByteArray{} : line.sliced(colon + 1);
        if (value.startsWith(' '))
            value.remove(0, 1);
        if (hasData_)
            data_ += '\n';
        data_ += value;
        hasData_ = true;
        if (data_.size() > maximumEventBytes)
            issue = ResponseIssue::ResponseLimit;
    }

    void reject(StreamProblem value) {
        problem = value;
        issue = ResponseIssue::StreamProtocol;
    }

    void dispatch() {
        ++eventIndex;
        eventBytes = data_.size();
        if (done) {
            reject(StreamProblem::EventAfterDone);
            return;
        }
        if (data_.trimmed() == "[DONE]") {
            done = true;
            ++activity;
            return;
        }
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(data_, &error);
        if (error.error != QJsonParseError::NoError) {
            jsonErrorOffset = error.offset;
            reject(StreamProblem::EventJson);
            return;
        }
        if (!document.isObject()) {
            reject(StreamProblem::EventType);
            return;
        }
        const auto chunk = document.object();
        const auto providerError = chunk.value(QStringLiteral("error"));
        if (!providerError.isUndefined() && !providerError.isNull()) {
            reject(StreamProblem::ProviderError);
            return;
        }
        if (!chunk.value(QStringLiteral("choices")).isArray()) {
            reject(StreamProblem::ChoicesType);
            return;
        }
        const auto choices = chunk.value(QStringLiteral("choices")).toArray();
        if (choices.size() > 1) {
            reject(StreamProblem::MultipleChoices);
            return;
        }
        const auto reportedUsage = chunk.value(QStringLiteral("usage"));
        if (!reportedUsage.isUndefined() && !reportedUsage.isNull() && !reportedUsage.isObject()) {
            reject(StreamProblem::UsageType);
            return;
        }
        if (chunk.value(QStringLiteral("id")).isString())
            id = chunk.value(QStringLiteral("id")).toString();
        if (chunk.value(QStringLiteral("model")).isString())
            model = chunk.value(QStringLiteral("model")).toString();
        if (chunk.contains(QStringLiteral("usage")) &&
            !chunk.value(QStringLiteral("usage")).isNull())
            usage = chunk.value(QStringLiteral("usage"));
        if (!choices.isEmpty()) {
            const auto choice = choices[0].toObject();
            if (!choices[0].isObject()) {
                reject(StreamProblem::ChoiceType);
                return;
            }
            // With exactly one choice, an omitted index is unambiguous. Explicit wrong
            // indices/types are still rejected; never merge multiple candidates.
            const auto index = choice.value(QStringLiteral("index"));
            if (!index.isUndefined() && (!index.isDouble() || index.toDouble() != 0)) {
                reject(StreamProblem::ChoiceIndex);
                return;
            }
            const auto finish = choice.value(QStringLiteral("finish_reason"));
            const auto deltaValue = choice.value(QStringLiteral("delta"));
            const auto emptyTerminal = (deltaValue.isUndefined() || deltaValue.isNull()) &&
                                       finish.isString() && !finish.toString().isEmpty() &&
                                       !choice.contains(QStringLiteral("message")) &&
                                       !choice.contains(QStringLiteral("text")) &&
                                       !choice.contains(QStringLiteral("tool_calls")) &&
                                       !choice.contains(QStringLiteral("function_call"));
            if (!deltaValue.isObject() && !emptyTerminal) {
                reject(StreamProblem::DeltaType);
                return;
            }
            const auto delta = deltaValue.toObject();
            for (const auto& field :
                 {QStringLiteral("content"), QStringLiteral("reasoning_content")}) {
                const auto value = delta.value(field);
                if (!value.isUndefined() && !value.isNull() && !value.isString()) {
                    reject(field == QStringLiteral("content") ? StreamProblem::ContentType
                                                              : StreamProblem::ReasoningType);
                    return;
                }
                const auto text = value.toString();
                if (!text.isEmpty()) {
                    if (!finishReason.isEmpty()) {
                        reject(StreamProblem::ContentAfterFinish);
                        return;
                    }
                    ++generation;
                    if (field == QStringLiteral("content")) {
                        content += text;
                        contentBytes += text.toUtf8().size();
                    } else
                        reasoningBytes += text.toUtf8().size();
                }
            }
            // Compatibility serializers often emit null/empty placeholders. These are
            // not tool invocations. Any actual tool/function payload remains unsupported.
            const auto tools = delta.value(QStringLiteral("tool_calls"));
            if (!tools.isUndefined() && !tools.isNull() &&
                !(tools.isArray() && tools.toArray().isEmpty())) {
                reject(StreamProblem::ToolCalls);
                return;
            }
            const auto function = delta.value(QStringLiteral("function_call"));
            if (!function.isUndefined() && !function.isNull() &&
                !(function.isObject() && function.toObject().isEmpty())) {
                reject(StreamProblem::FunctionCall);
                return;
            }
            if (!finish.isUndefined() && !finish.isNull() &&
                !(finish.isString() && finish.toString().isEmpty())) {
                if (!finish.isString() || finish.toString().isEmpty()) {
                    reject(StreamProblem::FinishType);
                    return;
                }
                if (!finishReason.isEmpty()) {
                    reject(StreamProblem::DuplicateFinish);
                    return;
                }
                finishReason = finish.toString();
            }
        }
        ++activity;
    }

    QByteArray pending_;
    QByteArray data_;
    bool hasData_ = false;
    bool firstLine_ = true;
};
} // namespace loreforge::llm
