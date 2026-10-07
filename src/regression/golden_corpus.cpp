#include "regression_support.h"

namespace loreforge::regression {
using namespace Qt::StringLiterals;
namespace {
std::optional<QStringList> readStrings(const QJsonValue& value) {
    if (!value.isArray()) {
        return std::nullopt;
    }
    QStringList result;
    for (const auto& item : value.toArray()) {
        if (!item.isString()) {
            return std::nullopt;
        }
        result.append(item.toString());
    }
    return detail::validStrings(result) ? std::optional(result) : std::nullopt;
}
std::optional<GoldenLabels> labels(const QJsonValue& value) {
    if (!value.isObject() || value.toObject().size() != 2) {
        return std::nullopt;
    }
    const auto required = readStrings(value.toObject().value(u"required"_s));
    const auto allowed = readStrings(value.toObject().value(u"allowed"_s));
    if (!required || !allowed) {
        return std::nullopt;
    }
    const GoldenLabels result{*required, *allowed};
    return detail::validLabels(result) ? std::optional(result) : std::nullopt;
}
} // namespace

RegressionResult<GoldenCorpus> ModelRegressionSuite::loadCorpus(const QByteArray& json) {
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(json, &error);
    const auto root = document.object();
    if (error.error != QJsonParseError::NoError || !document.isObject() || root.size() != 4 ||
        root.value(u"format"_s) != u"loreforge-golden-corpus-v1"_s ||
        !root.value(u"id"_s).isString() || !root.value(u"version"_s).isString() ||
        !root.value(u"cases"_s).isArray()) {
        return RegressionError{u"$"_s, u"Invalid golden corpus envelope"_s};
    }
    GoldenCorpus corpus{
        root.value(u"id"_s).toString(), root.value(u"version"_s).toString(), {}, {}};
    for (const auto& value : root.value(u"cases"_s).toArray()) {
        const auto row = value.toObject();
        const auto caseId = row.value(u"id"_s).toString();
        const auto tags = readStrings(row.value(u"tags"_s));
        const auto entities = labels(row.value(u"entities"_s));
        const auto events = labels(row.value(u"events"_s));
        const auto context = labels(row.value(u"context"_s));
        if (!value.isObject() || row.size() != 8 || !row.value(u"id"_s).isString() ||
            !row.value(u"source"_s).isString() || !row.value(u"segments"_s).isArray() ||
            !row.value(u"edits"_s).isArray() || !tags || !entities || !events || !context) {
            return RegressionError{caseId, u"Invalid golden case fields"_s};
        }
        GoldenCase item;
        item.id = caseId;
        item.tags = *tags;
        item.sourceUtf8 = row.value(u"source"_s).toString().toUtf8();
        item.sourceHash = detail::hash(item.sourceUtf8);
        const auto sourceId = u"golden/%1/%2"_s.arg(corpus.id, caseId);
        item.segmentation.chapterId = core::ChapterId::fromStableKey(sourceId);
        item.segmentation.sourceSpan = {sourceId, 0, item.sourceUtf8.size()};
        QByteArray reconstructed;
        for (const auto& segmentValue : row.value(u"segments"_s).toArray()) {
            const auto segment = segmentValue.toObject();
            const auto type = narrative::segmentTypeFromString(segment.value(u"type"_s).toString());
            if (!segmentValue.isObject() || segment.size() != 3 || !type ||
                !segment.value(u"text"_s).isString() ||
                (!segment.value(u"speaker"_s).isNull() &&
                 !segment.value(u"speaker"_s).isString())) {
                return RegressionError{caseId, u"Invalid golden segmentation"_s};
            }
            const auto text = segment.value(u"text"_s).toString();
            const auto start = reconstructed.size();
            reconstructed.append(text.toUtf8());
            item.segmentation.segments.append(
                {*type,
                 text,
                 segment.value(u"speaker"_s).isNull()
                     ? std::nullopt
                     : std::optional(segment.value(u"speaker"_s).toString()),
                 1.0,
                 {sourceId, start, reconstructed.size()}});
        }
        if (reconstructed != item.sourceUtf8) {
            return RegressionError{caseId, u"Golden segments do not reconstruct source"_s};
        }
        item.entities = *entities;
        item.events = *events;
        item.context = *context;
        for (const auto& editValue : row.value(u"edits"_s).toArray()) {
            const auto edit = editValue.toObject();
            if (!editValue.isObject() || edit.size() != 3 ||
                !edit.value(u"original"_s).isString() || !edit.value(u"replacement"_s).isString() ||
                !edit.value(u"occurrence"_s).isDouble() ||
                edit.value(u"occurrence"_s).toDouble() !=
                    edit.value(u"occurrence"_s).toInteger(-1) ||
                edit.value(u"occurrence"_s).toInteger(-1) < 0 ||
                edit.value(u"occurrence"_s).toInteger() > item.sourceUtf8.size()) {
                return RegressionError{caseId, u"Invalid golden edit fields"_s};
            }
            const auto original = edit.value(u"original"_s).toString();
            const auto needle = original.toUtf8();
            qsizetype start = -1;
            for (qint64 index = 0; index <= edit.value(u"occurrence"_s).toInteger(); ++index) {
                start = item.sourceUtf8.indexOf(needle, start + 1);
                if (start < 0) {
                    return RegressionError{caseId, u"Golden edit quote is not in source"_s};
                }
            }
            item.edits.append({{sourceId, start, start + needle.size()},
                               original,
                               edit.value(u"replacement"_s).toString()});
        }
        corpus.cases.append(item);
    }
    corpus.hash = detail::hash(detail::corpusBytes(corpus));
    if (!detail::validCorpus(corpus)) {
        return RegressionError{u"$"_s, u"Golden corpus validation failed"_s};
    }
    return corpus;
}
} // namespace loreforge::regression
