#pragma once

#include "loreforge/narrative/dialogue_extractor.h"
#include "loreforge/regression/model_regression.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <algorithm>

namespace loreforge::regression::detail {
using namespace Qt::StringLiterals;
inline bool text(const QString& value) {
    return !value.isEmpty() && value == value.trimmed();
}
inline core::ContentHash hash(const QByteArray& bytes) {
    return core::ContentHash::sha256(QByteArrayView(bytes));
}
inline QJsonArray strings(QStringList values) {
    std::sort(values.begin(), values.end());
    QJsonArray result;
    for (const auto& value : values) {
        result.append(value);
    }
    return result;
}
inline bool validStrings(const QStringList& values) {
    QSet<QString> seen;
    for (const auto& value : values) {
        if (!text(value) || seen.contains(value)) {
            return false;
        }
        seen.insert(value);
    }
    return true;
}
inline QJsonArray encodeLabels(const GoldenLabels& labels) {
    return {strings(labels.required), strings(labels.allowed)};
}
inline bool validLabels(const GoldenLabels& labels) {
    if (!validStrings(labels.required) || !validStrings(labels.allowed)) {
        return false;
    }
    for (const auto& value : labels.required) {
        if (!labels.allowed.contains(value)) {
            return false;
        }
    }
    return true;
}
inline QJsonObject segmentationJson(const narrative::ChapterSegmentation& segmentation) {
    QJsonArray segments;
    for (const auto& segment : segmentation.segments) {
        segments.append(QJsonObject{
            {u"type"_s, narrative::segmentTypeToString(segment.type)},
            {u"speaker"_s, segment.speaker ? QJsonValue(*segment.speaker) : QJsonValue()},
            {u"source_start"_s, segment.sourceSpan.startByte},
            {u"source_end"_s, segment.sourceSpan.endByte},
            {u"confidence"_s, segment.confidence}});
    }
    return {{u"chapter_id"_s, segmentation.chapterId.toString()}, {u"segments"_s, segments}};
}
inline bool validSegmentation(const GoldenCase& golden,
                              const narrative::ChapterSegmentation& value) {
    if (value.chapterId != golden.segmentation.chapterId ||
        value.sourceSpan != golden.segmentation.sourceSpan) {
        return false;
    }
    for (const auto& segment : value.segments) {
        if (segment.sourceSpan.sourceId != value.sourceSpan.sourceId ||
            (segment.type != narrative::SegmentType::Dialogue &&
             segment.type != narrative::SegmentType::Narration)) {
            return false;
        }
    }
    const auto checked = narrative::DialogueExtractor::extract(value.chapterId, value.sourceSpan,
                                                               QByteArrayView(golden.sourceUtf8),
                                                               segmentationJson(value));
    return checked.isValid() && *checked.extraction == value;
}
inline bool validEdits(const GoldenCase& golden, const QList<ProofreadingEdit>& edits) {
    QList<core::SourceSpan> seen;
    for (const auto& edit : edits) {
        const auto& span = edit.span;
        if (!span.isValid() || span.sourceId != golden.segmentation.sourceSpan.sourceId ||
            span.startByte < 0 || span.endByte > golden.sourceUtf8.size() ||
            edit.original == edit.replacement || edit.original.isEmpty()) {
            return false;
        }
        const auto bytes =
            QByteArrayView(golden.sourceUtf8).sliced(span.startByte, span.lengthBytes());
        if (QString::fromUtf8(bytes).toUtf8() != bytes || edit.original.toUtf8() != bytes ||
            QString::fromUtf8(edit.replacement.toUtf8()) != edit.replacement) {
            return false;
        }
        for (const auto& prior : seen) {
            if (prior == span || prior.overlaps(span)) {
                return false;
            }
        }
        seen.append(span);
    }
    return true;
}
inline QJsonArray encodeEdits(QList<ProofreadingEdit> edits) {
    std::sort(edits.begin(), edits.end(),
              [](const auto& a, const auto& b) { return a.span.startByte < b.span.startByte; });
    QJsonArray result;
    for (const auto& edit : edits) {
        result.append(QJsonArray{edit.span.sourceId, QString::number(edit.span.startByte),
                                 QString::number(edit.span.endByte), edit.original,
                                 edit.replacement});
    }
    return result;
}
inline QByteArray corpusBytes(const GoldenCorpus& corpus) {
    auto cases = corpus.cases;
    std::sort(cases.begin(), cases.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    QJsonArray rows;
    for (const auto& item : cases) {
        rows.append(QJsonArray{item.id, strings(item.tags), QString::fromUtf8(item.sourceUtf8),
                               item.sourceHash.toHex(), segmentationJson(item.segmentation),
                               encodeLabels(item.entities), encodeLabels(item.events),
                               encodeLabels(item.context), encodeEdits(item.edits)});
    }
    return QJsonDocument(
               QJsonArray{u"loreforge-golden-corpus-v1"_s, corpus.id, corpus.version, rows})
        .toJson(QJsonDocument::Compact);
}
inline bool validCorpus(const GoldenCorpus& corpus) {
    if (!text(corpus.id) || !text(corpus.version) || corpus.cases.isEmpty() ||
        !corpus.hash.isValid() || corpus.hash != hash(corpusBytes(corpus))) {
        return false;
    }
    QSet<QString> seen;
    for (const auto& item : corpus.cases) {
        if (!text(item.id) || seen.contains(item.id) || item.tags.isEmpty() ||
            !validStrings(item.tags) || item.sourceUtf8.isEmpty() ||
            QString::fromUtf8(item.sourceUtf8).toUtf8() != item.sourceUtf8 ||
            item.sourceHash != hash(item.sourceUtf8) ||
            item.segmentation.sourceSpan.startByte != 0 ||
            item.segmentation.sourceSpan.endByte != item.sourceUtf8.size() ||
            !validSegmentation(item, item.segmentation) || !validLabels(item.entities) ||
            !validLabels(item.events) || !validLabels(item.context) ||
            !validEdits(item, item.edits)) {
            return false;
        }
        seen.insert(item.id);
    }
    return true;
}
} // namespace loreforge::regression::detail
