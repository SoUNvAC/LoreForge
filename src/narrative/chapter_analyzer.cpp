#include "loreforge/narrative/chapter_analyzer.h"

#include "loreforge/inference/output_validator.h"

#include <QJsonArray>
#include <QSet>
#include <QStringDecoder>

#include <cmath>
#include <utility>

namespace loreforge::narrative {
namespace {

void addError(ChapterAnalysisResult& result, ChapterAnalysisErrorCode code, QString path,
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
    const auto roundTrip = text.toUtf8();
    if (decoder.hasError() || QByteArrayView(roundTrip) != bytes) {
        return std::nullopt;
    }
    return text;
}

QString validatedText(const QJsonValue& value, QString path, ChapterAnalysisResult& result) {
    const auto text = value.toString();
    if (text.isEmpty() || text != text.trimmed()) {
        addError(result, ChapterAnalysisErrorCode::InvalidText, std::move(path),
                 QStringLiteral("Text must be non-empty and trimmed."));
    }
    return text;
}

QJsonObject evidenceSchema() {
    return {
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("required"),
         QJsonArray{QStringLiteral("source_start"), QStringLiteral("source_end")}},
        {QStringLiteral("properties"),
         QJsonObject{
             {QStringLiteral("source_start"),
              QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
             {QStringLiteral("source_end"),
              QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
         }},
        {QStringLiteral("additionalProperties"), false},
    };
}

QJsonObject claimSchema(QJsonObject properties, QJsonArray required) {
    properties.insert(QStringLiteral("inferred"),
                      QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}});
    properties.insert(QStringLiteral("confidence"),
                      QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}});
    properties.insert(QStringLiteral("evidence"),
                      QJsonObject{{QStringLiteral("type"), QStringLiteral("array")},
                                  {QStringLiteral("items"), evidenceSchema()}});
    required.append(QStringLiteral("inferred"));
    required.append(QStringLiteral("confidence"));
    required.append(QStringLiteral("evidence"));
    return {
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("required"), required},
        {QStringLiteral("properties"), properties},
        {QStringLiteral("additionalProperties"), false},
    };
}

QJsonObject namedClaimSchema() {
    return claimSchema(
        QJsonObject{{QStringLiteral("name"),
                     QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}},
        QJsonArray{QStringLiteral("name")});
}

QJsonObject textClaimSchema() {
    return claimSchema(
        QJsonObject{{QStringLiteral("text"),
                     QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}},
        QJsonArray{QStringLiteral("text")});
}

QList<SourceEvidence> parseEvidence(const QJsonArray& values, QStringView path,
                                    const core::SourceSpan& chapterSpan, QByteArrayView sourceUtf8,
                                    ChapterAnalysisResult& result) {
    QList<SourceEvidence> evidence;
    QSet<QString> seen;
    for (qsizetype index = 0; index < values.size(); ++index) {
        const auto evidencePath = QStringLiteral("%1[%2]").arg(path).arg(index);
        const auto object = values.at(index).toObject();
        const auto start = offsetValue(object.value(QStringLiteral("source_start")));
        const auto end = offsetValue(object.value(QStringLiteral("source_end")));
        if (!start.has_value() || !end.has_value() || *start < chapterSpan.startByte ||
            *end > chapterSpan.endByte || *end <= *start) {
            addError(result, ChapterAnalysisErrorCode::InvalidEvidence, evidencePath,
                     QStringLiteral("Evidence must be a non-empty range inside the chapter."));
            continue;
        }
        const auto key = QStringLiteral("%1:%2").arg(*start).arg(*end);
        if (seen.contains(key)) {
            addError(result, ChapterAnalysisErrorCode::DuplicateEvidence, evidencePath,
                     QStringLiteral("Evidence ranges within one claim must be unique."));
            continue;
        }
        seen.insert(key);
        const auto relativeStart = *start - chapterSpan.startByte;
        const auto text = decodeUtf8(sourceUtf8.sliced(relativeStart, *end - *start));
        if (!text.has_value()) {
            addError(result, ChapterAnalysisErrorCode::InvalidEvidence, evidencePath,
                     QStringLiteral("The evidence boundary splits a UTF-8 character."));
            continue;
        }
        evidence.append({{chapterSpan.sourceId, *start, *end}, *text});
    }
    return evidence;
}

ClaimSupport parseSupport(const QJsonObject& object, QStringView path,
                          const core::SourceSpan& chapterSpan, QByteArrayView sourceUtf8,
                          ChapterAnalysisResult& result) {
    const auto inferred = object.value(QStringLiteral("inferred")).toBool();
    const auto confidence = object.value(QStringLiteral("confidence")).toDouble();
    if (!std::isfinite(confidence) || confidence < 0.0 || confidence > 1.0) {
        addError(result, ChapterAnalysisErrorCode::InvalidConfidence,
                 QStringLiteral("%1.confidence").arg(path),
                 QStringLiteral("Confidence must be between zero and one."));
    }
    auto evidence =
        parseEvidence(object.value(QStringLiteral("evidence")).toArray(),
                      QStringLiteral("%1.evidence").arg(path), chapterSpan, sourceUtf8, result);
    ClaimSupport support{inferred ? ClaimBasis::Inference : ClaimBasis::Evidence,
                         std::move(evidence), confidence};
    if (!support.isGrounded()) {
        addError(
            result, ChapterAnalysisErrorCode::UngroundedClaim, path.toString(),
            QStringLiteral("A direct claim requires source evidence; otherwise mark it inferred."));
    }
    return support;
}

QList<CharacterAlias> parseAliases(const QJsonArray& values, QStringView path,
                                   QStringView canonicalName, const core::SourceSpan& chapterSpan,
                                   QByteArrayView sourceUtf8, ChapterAnalysisResult& result) {
    QList<CharacterAlias> aliases;
    QSet<QString> names{canonicalName.toString().toCaseFolded()};
    for (qsizetype index = 0; index < values.size(); ++index) {
        const auto aliasPath = QStringLiteral("%1[%2]").arg(path).arg(index);
        const auto object = values.at(index).toObject();
        auto name = validatedText(object.value(QStringLiteral("name")),
                                  aliasPath + QStringLiteral(".name"), result);
        const auto key = name.toCaseFolded();
        if (!key.isEmpty() && names.contains(key)) {
            addError(result, ChapterAnalysisErrorCode::DuplicateName,
                     aliasPath + QStringLiteral(".name"),
                     QStringLiteral("Character names and aliases must be unique."));
        }
        names.insert(key);
        aliases.append(
            {std::move(name), parseSupport(object, aliasPath, chapterSpan, sourceUtf8, result)});
    }
    return aliases;
}

QList<ChapterCharacter> parseCharacters(const QJsonArray& values, const core::SourceSpan& span,
                                        QByteArrayView sourceUtf8, ChapterAnalysisResult& result) {
    QList<ChapterCharacter> characters;
    QSet<QString> names;
    for (qsizetype index = 0; index < values.size(); ++index) {
        const auto path = QStringLiteral("$.characters[%1]").arg(index);
        const auto object = values.at(index).toObject();
        auto name = validatedText(object.value(QStringLiteral("name")),
                                  path + QStringLiteral(".name"), result);
        const auto key = name.toCaseFolded();
        if (!key.isEmpty() && names.contains(key)) {
            addError(result, ChapterAnalysisErrorCode::DuplicateName,
                     path + QStringLiteral(".name"),
                     QStringLiteral("Character names must be unique within one chapter."));
        }
        names.insert(key);
        auto aliases =
            parseAliases(object.value(QStringLiteral("aliases")).toArray(),
                         path + QStringLiteral(".aliases"), name, span, sourceUtf8, result);
        characters.append({std::move(name), std::move(aliases),
                           parseSupport(object, path, span, sourceUtf8, result)});
    }
    return characters;
}

QList<ChapterLocation> parseLocations(const QJsonArray& values, const core::SourceSpan& span,
                                      QByteArrayView sourceUtf8, ChapterAnalysisResult& result) {
    QList<ChapterLocation> locations;
    QSet<QString> names;
    for (qsizetype index = 0; index < values.size(); ++index) {
        const auto path = QStringLiteral("$.locations[%1]").arg(index);
        const auto object = values.at(index).toObject();
        auto name = validatedText(object.value(QStringLiteral("name")),
                                  path + QStringLiteral(".name"), result);
        const auto key = name.toCaseFolded();
        if (!key.isEmpty() && names.contains(key)) {
            addError(result, ChapterAnalysisErrorCode::DuplicateName,
                     path + QStringLiteral(".name"),
                     QStringLiteral("Location names must be unique within one chapter."));
        }
        names.insert(key);
        locations.append({std::move(name), parseSupport(object, path, span, sourceUtf8, result)});
    }
    return locations;
}

QStringList parseParticipants(const QJsonArray& values, QStringView path,
                              ChapterAnalysisResult& result) {
    QStringList participants;
    QSet<QString> names;
    for (qsizetype index = 0; index < values.size(); ++index) {
        auto name =
            validatedText(values.at(index), QStringLiteral("%1[%2]").arg(path).arg(index), result);
        const auto key = name.toCaseFolded();
        if (!key.isEmpty() && names.contains(key)) {
            addError(result, ChapterAnalysisErrorCode::DuplicateName,
                     QStringLiteral("%1[%2]").arg(path).arg(index),
                     QStringLiteral("Event participants must be unique."));
        }
        names.insert(key);
        participants.append(std::move(name));
    }
    return participants;
}

QList<ChapterEvent> parseEvents(const QJsonArray& values, const core::SourceSpan& span,
                                QByteArrayView sourceUtf8, ChapterAnalysisResult& result) {
    QList<ChapterEvent> events;
    for (qsizetype index = 0; index < values.size(); ++index) {
        const auto path = QStringLiteral("$.events[%1]").arg(index);
        const auto object = values.at(index).toObject();
        auto description = validatedText(object.value(QStringLiteral("description")),
                                         path + QStringLiteral(".description"), result);
        auto participants =
            parseParticipants(object.value(QStringLiteral("participants")).toArray(),
                              path + QStringLiteral(".participants"), result);
        std::optional<QString> location;
        const auto locationValue = object.value(QStringLiteral("location"));
        if (locationValue.isString()) {
            location = validatedText(locationValue, path + QStringLiteral(".location"), result);
        }
        events.append({std::move(description), std::move(participants), std::move(location),
                       parseSupport(object, path, span, sourceUtf8, result)});
    }
    return events;
}

TextClaim parseTextClaim(const QJsonObject& object, QString path, const core::SourceSpan& span,
                         QByteArrayView sourceUtf8, ChapterAnalysisResult& result) {
    auto text =
        validatedText(object.value(QStringLiteral("text")), path + QStringLiteral(".text"), result);
    return {std::move(text), parseSupport(object, path, span, sourceUtf8, result)};
}

QList<TextClaim> parseTextClaims(const QJsonArray& values, QStringView path,
                                 const core::SourceSpan& span, QByteArrayView sourceUtf8,
                                 ChapterAnalysisResult& result) {
    QList<TextClaim> claims;
    for (qsizetype index = 0; index < values.size(); ++index) {
        claims.append(parseTextClaim(values.at(index).toObject(),
                                     QStringLiteral("%1[%2]").arg(path).arg(index), span,
                                     sourceUtf8, result));
    }
    return claims;
}

} // namespace

bool ChapterAnalysisResult::isValid() const noexcept {
    return analysis.has_value() && errors.isEmpty();
}

QJsonObject ChapterAnalyzer::outputSchema() {
    auto aliasSchema = namedClaimSchema();
    auto characterSchema = claimSchema(
        QJsonObject{
            {QStringLiteral("name"),
             QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
            {QStringLiteral("aliases"),
             QJsonObject{{QStringLiteral("type"), QStringLiteral("array")},
                         {QStringLiteral("items"), aliasSchema}}},
        },
        QJsonArray{QStringLiteral("name"), QStringLiteral("aliases")});
    auto eventSchema = claimSchema(
        QJsonObject{
            {QStringLiteral("description"),
             QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
            {QStringLiteral("participants"),
             QJsonObject{{QStringLiteral("type"), QStringLiteral("array")},
                         {QStringLiteral("items"),
                          QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}}},
            {QStringLiteral("location"),
             QJsonObject{{QStringLiteral("type"),
                          QJsonArray{QStringLiteral("string"), QStringLiteral("null")}}}},
        },
        QJsonArray{QStringLiteral("description"), QStringLiteral("participants"),
                   QStringLiteral("location")});
    const auto arrayOf = [](const QJsonObject& itemSchema) {
        return QJsonObject{{QStringLiteral("type"), QStringLiteral("array")},
                           {QStringLiteral("items"), itemSchema}};
    };
    return {
        {QStringLiteral("$schema"), QStringLiteral("https://json-schema.org/draft/2020-12/schema")},
        {QStringLiteral("title"), QStringLiteral("LoreForge chapter semantic analysis")},
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("required"),
         QJsonArray{QStringLiteral("chapter_id"), QStringLiteral("characters"),
                    QStringLiteral("locations"), QStringLiteral("events"),
                    QStringLiteral("summary"), QStringLiteral("important_facts"),
                    QStringLiteral("open_threads")}},
        {QStringLiteral("properties"),
         QJsonObject{
             {QStringLiteral("chapter_id"),
              QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
             {QStringLiteral("characters"), arrayOf(characterSchema)},
             {QStringLiteral("locations"), arrayOf(namedClaimSchema())},
             {QStringLiteral("events"), arrayOf(eventSchema)},
             {QStringLiteral("summary"), textClaimSchema()},
             {QStringLiteral("important_facts"), arrayOf(textClaimSchema())},
             {QStringLiteral("open_threads"), arrayOf(textClaimSchema())},
         }},
        {QStringLiteral("additionalProperties"), false},
    };
}

ChapterAnalysisResult ChapterAnalyzer::analyze(const core::ChapterId& chapterId,
                                               const core::SourceSpan& chapterSpan,
                                               QByteArrayView sourceUtf8,
                                               const QJsonObject& modelOutput) {
    ChapterAnalysisResult result;
    if (!chapterId.isValid()) {
        addError(result, ChapterAnalysisErrorCode::InvalidChapter, QStringLiteral("$.chapter_id"),
                 QStringLiteral("The chapter ID is invalid."));
    }
    if (!chapterSpan.isValid() || chapterSpan.lengthBytes() != sourceUtf8.size()) {
        addError(result, ChapterAnalysisErrorCode::InvalidSourceSpan, QStringLiteral("$.source"),
                 QStringLiteral("The chapter span must match the exact source byte length."));
    }
    if (!decodeUtf8(sourceUtf8).has_value()) {
        addError(result, ChapterAnalysisErrorCode::InvalidUtf8, QStringLiteral("$.source"),
                 QStringLiteral("The chapter source must be valid UTF-8."));
    }
    const auto schemaValidation = inference::OutputValidator::validate(outputSchema(), modelOutput);
    for (const auto& validationError : schemaValidation.errors) {
        addError(result, ChapterAnalysisErrorCode::SchemaViolation, QStringLiteral("$.output"),
                 validationError);
    }
    if (!result.errors.isEmpty()) {
        return result;
    }
    if (modelOutput.value(QStringLiteral("chapter_id")).toString() != chapterId.toString()) {
        addError(result, ChapterAnalysisErrorCode::ChapterMismatch, QStringLiteral("$.chapter_id"),
                 QStringLiteral("The model output belongs to another chapter."));
    }

    auto characters = parseCharacters(modelOutput.value(QStringLiteral("characters")).toArray(),
                                      chapterSpan, sourceUtf8, result);
    auto locations = parseLocations(modelOutput.value(QStringLiteral("locations")).toArray(),
                                    chapterSpan, sourceUtf8, result);
    auto events = parseEvents(modelOutput.value(QStringLiteral("events")).toArray(), chapterSpan,
                              sourceUtf8, result);
    auto summary = parseTextClaim(modelOutput.value(QStringLiteral("summary")).toObject(),
                                  QStringLiteral("$.summary"), chapterSpan, sourceUtf8, result);
    auto importantFacts =
        parseTextClaims(modelOutput.value(QStringLiteral("important_facts")).toArray(),
                        QStringLiteral("$.important_facts"), chapterSpan, sourceUtf8, result);
    auto openThreads =
        parseTextClaims(modelOutput.value(QStringLiteral("open_threads")).toArray(),
                        QStringLiteral("$.open_threads"), chapterSpan, sourceUtf8, result);
    if (!result.errors.isEmpty()) {
        return result;
    }
    result.analysis = ChapterAnalysis{
        chapterId,         chapterSpan,        std::move(characters),     std::move(locations),
        std::move(events), std::move(summary), std::move(importantFacts), std::move(openThreads)};
    return result;
}

} // namespace loreforge::narrative
