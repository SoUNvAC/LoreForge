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

    void dispatch() {
        if (done) {
            issue = ResponseIssue::StreamProtocol;
            return;
        }
        if (data_.trimmed() == "[DONE]") {
            done = true;
            ++activity;
            return;
        }
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(data_, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            issue = ResponseIssue::StreamProtocol;
            return;
        }
        const auto chunk = document.object();
        if (chunk.contains(QStringLiteral("error")) ||
            !chunk.value(QStringLiteral("choices")).isArray()) {
            issue = ResponseIssue::StreamProtocol;
            return;
        }
        const auto choices = chunk.value(QStringLiteral("choices")).toArray();
        if (choices.size() > 1 ||
            (choices.isEmpty() && !chunk.value(QStringLiteral("usage")).isObject())) {
            issue = ResponseIssue::StreamProtocol;
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
            if (!choices[0].isObject() || choice.value(QStringLiteral("index")).toInt(-1) != 0 ||
                !choice.value(QStringLiteral("delta")).isObject()) {
                issue = ResponseIssue::StreamProtocol;
                return;
            }
            const auto delta = choice.value(QStringLiteral("delta")).toObject();
            for (const auto& field :
                 {QStringLiteral("content"), QStringLiteral("reasoning_content")}) {
                const auto value = delta.value(field);
                if (!value.isUndefined() && !value.isNull() && !value.isString()) {
                    issue = ResponseIssue::StreamProtocol;
                    return;
                }
                const auto text = value.toString();
                if (!text.isEmpty()) {
                    if (!finishReason.isEmpty()) {
                        issue = ResponseIssue::StreamProtocol;
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
            if (delta.contains(QStringLiteral("tool_calls")) ||
                delta.contains(QStringLiteral("function_call"))) {
                issue = ResponseIssue::StreamProtocol;
                return;
            }
            const auto finish = choice.value(QStringLiteral("finish_reason"));
            if (!finish.isUndefined() && !finish.isNull()) {
                if (!finish.isString() || finish.toString().isEmpty() || !finishReason.isEmpty()) {
                    issue = ResponseIssue::StreamProtocol;
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
