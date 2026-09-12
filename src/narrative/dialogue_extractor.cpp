#include "loreforge/narrative/dialogue_extractor.h"

#include "loreforge/inference/output_validator.h"

#include <QJsonArray>
#include <QStringDecoder>

#include <cmath>
#include <utility>

namespace loreforge::narrative {
namespace {

void addError(DialogueExtractionResult& result, ExtractionErrorCode code, QString path,
              QString message) {
    result.errors.append({code, std::move(path), std::move(message)});
}

std::optional<qint64> offsetValue(const QJsonValue& value) {
    if (!value.isDouble()) {
        return std::nullopt;
    }
    const auto number = value.toDouble();
    const auto integer = value.toInteger(-1);
    if (!std::isfinite(number) || std::floor(number) != number || integer < 0 ||
        static_cast<double>(integer) != number) {
        return std::nullopt;
    }
    return integer;
}

std::optional<QString> decodeUtf8(QByteArrayView bytes) {
    QStringDecoder decoder(QStringDecoder::Utf8);
    QString text = decoder.decode(bytes);
    if (decoder.hasError()) {
        return std::nullopt;
    }
    return text;
}

} // namespace

bool DialogueExtractionResult::isValid() const noexcept {
    return extraction.has_value() && errors.isEmpty();
}

QJsonObject DialogueExtractor::outputSchema() {
    const QJsonObject segmentSchema{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("required"),
         QJsonArray{QStringLiteral("type"), QStringLiteral("source_start"),
                    QStringLiteral("source_end"), QStringLiteral("confidence")}},
        {QStringLiteral("properties"),
         QJsonObject{
             {QStringLiteral("type"),
              QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                          {QStringLiteral("enum"),
                           QJsonArray{QStringLiteral("dialogue"), QStringLiteral("narration")}}}},
             {QStringLiteral("speaker"),
              QJsonObject{{QStringLiteral("type"),
                           QJsonArray{QStringLiteral("string"), QStringLiteral("null")}}}},
             {QStringLiteral("source_start"),
              QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
             {QStringLiteral("source_end"),
              QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
             {QStringLiteral("confidence"),
              QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
         }},
        {QStringLiteral("additionalProperties"), false},
    };
    return {
        {QStringLiteral("$schema"), QStringLiteral("https://json-schema.org/draft/2020-12/schema")},
        {QStringLiteral("title"), QStringLiteral("LoreForge narrative segmentation")},
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("required"),
         QJsonArray{QStringLiteral("chapter_id"), QStringLiteral("segments")}},
        {QStringLiteral("properties"),
         QJsonObject{
             {QStringLiteral("chapter_id"),
              QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
             {QStringLiteral("segments"),
              QJsonObject{{QStringLiteral("type"), QStringLiteral("array")},
                          {QStringLiteral("items"), segmentSchema}}},
         }},
        {QStringLiteral("additionalProperties"), false},
    };
}

DialogueExtractionResult DialogueExtractor::extract(const core::ChapterId& chapterId,
                                                    const core::SourceSpan& chapterSpan,
                                                    QByteArrayView sourceUtf8,
                                                    const QJsonObject& modelOutput) {
    DialogueExtractionResult result;
    if (!chapterId.isValid()) {
        addError(result, ExtractionErrorCode::InvalidChapter, QStringLiteral("$.chapter_id"),
                 QStringLiteral("The chapter ID is invalid."));
    }
    if (!chapterSpan.isValid() || chapterSpan.lengthBytes() != sourceUtf8.size()) {
        addError(result, ExtractionErrorCode::InvalidSourceSpan, QStringLiteral("$.source"),
                 QStringLiteral("The chapter span must match the exact source byte length."));
    }
    if (!decodeUtf8(sourceUtf8).has_value()) {
        addError(result, ExtractionErrorCode::InvalidUtf8, QStringLiteral("$.source"),
                 QStringLiteral("The chapter source must be valid UTF-8."));
    }
    const auto schemaValidation = inference::OutputValidator::validate(outputSchema(), modelOutput);
    for (const auto& validationError : schemaValidation.errors) {
        addError(result, ExtractionErrorCode::SchemaViolation, QStringLiteral("$.output"),
                 validationError);
    }
    if (!result.errors.isEmpty()) {
        return result;
    }

    if (modelOutput.value(QStringLiteral("chapter_id")).toString() != chapterId.toString()) {
        addError(result, ExtractionErrorCode::ChapterMismatch, QStringLiteral("$.chapter_id"),
                 QStringLiteral("The model output belongs to another chapter."));
    }
    const auto segmentValues = modelOutput.value(QStringLiteral("segments")).toArray();
    if (segmentValues.isEmpty() && chapterSpan.lengthBytes() > 0) {
        addError(result, ExtractionErrorCode::MissingSegments, QStringLiteral("$.segments"),
                 QStringLiteral("A non-empty chapter must contain segments."));
    }

    QList<NarrativeSegment> segments;
    qint64 expectedStart = chapterSpan.startByte;
    for (qsizetype index = 0; index < segmentValues.size(); ++index) {
        const auto segmentPath = QStringLiteral("$.segments[%1]").arg(index);
        const auto object = segmentValues.at(index).toObject();
        const auto start = offsetValue(object.value(QStringLiteral("source_start")));
        const auto end = offsetValue(object.value(QStringLiteral("source_end")));
        if (!start.has_value() || !end.has_value() || *start < chapterSpan.startByte ||
            *end > chapterSpan.endByte || *end <= *start) {
            addError(result, ExtractionErrorCode::InvalidSegmentSpan,
                     segmentPath + QStringLiteral(".source_span"),
                     QStringLiteral("The segment must be a non-empty range inside the chapter."));
            continue;
        }
        if (*start > expectedStart) {
            addError(result, ExtractionErrorCode::CoverageGap,
                     segmentPath + QStringLiteral(".source_start"),
                     QStringLiteral("Source bytes are missing before this segment."));
        } else if (*start < expectedStart) {
            addError(result, ExtractionErrorCode::CoverageOverlap,
                     segmentPath + QStringLiteral(".source_start"),
                     QStringLiteral("This segment overlaps an earlier segment."));
        }
        expectedStart = qMax(expectedStart, *end);

        const auto relativeStart = *start - chapterSpan.startByte;
        const auto length = *end - *start;
        const auto segmentText = decodeUtf8(sourceUtf8.sliced(relativeStart, length));
        if (!segmentText.has_value()) {
            addError(result, ExtractionErrorCode::InvalidUtf8,
                     segmentPath + QStringLiteral(".source_span"),
                     QStringLiteral("The segment boundary splits a UTF-8 character."));
            continue;
        }

        const auto segmentType =
            segmentTypeFromString(object.value(QStringLiteral("type")).toString());
        const auto confidence = object.value(QStringLiteral("confidence")).toDouble();
        if (!std::isfinite(confidence) || confidence < 0.0 || confidence > 1.0) {
            addError(result, ExtractionErrorCode::InvalidConfidence,
                     segmentPath + QStringLiteral(".confidence"),
                     QStringLiteral("Confidence must be between zero and one."));
        }

        std::optional<QString> speaker;
        const auto speakerValue = object.value(QStringLiteral("speaker"));
        if (speakerValue.isString()) {
            const auto name = speakerValue.toString();
            if (name.isEmpty() || name != name.trimmed()) {
                addError(result, ExtractionErrorCode::InvalidSpeaker,
                         segmentPath + QStringLiteral(".speaker"),
                         QStringLiteral("A speaker name must be non-empty and trimmed."));
            } else {
                speaker = name;
            }
        }
        if (segmentType == SegmentType::Narration && speaker.has_value()) {
            addError(result, ExtractionErrorCode::InvalidSpeaker,
                     segmentPath + QStringLiteral(".speaker"),
                     QStringLiteral("Narration cannot have a speaker."));
        }
        if (segmentType.has_value()) {
            segments.append({*segmentType,
                             *segmentText,
                             std::move(speaker),
                             confidence,
                             {chapterSpan.sourceId, *start, *end}});
        }
    }
    if (expectedStart < chapterSpan.endByte) {
        addError(result, ExtractionErrorCode::CoverageGap, QStringLiteral("$.segments"),
                 QStringLiteral("Source bytes are missing after the final segment."));
    }
    if (!result.errors.isEmpty()) {
        return result;
    }
    result.extraction = ChapterSegmentation{chapterId, chapterSpan, std::move(segments)};
    return result;
}

} // namespace loreforge::narrative
