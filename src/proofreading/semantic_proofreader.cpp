#include "loreforge/proofreading/semantic_proofreader.h"

#include "loreforge/inference/output_validator.h"
#include "loreforge/proofreading/protected_term_registry.h"
#include "proofreading_support.h"

#include <QJsonArray>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <utility>

namespace loreforge::proofreading {
namespace {

constexpr auto kDetectorVersion = "semantic-v1";

std::optional<qint64> offset(const QJsonValue& value) {
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

QJsonArray strings(std::initializer_list<QString> values) {
    QJsonArray result;
    for (auto& value : values) {
        result.append(std::move(value));
    }
    return result;
}

} // namespace

QJsonObject SemanticProofreader::outputSchema() {
    const QJsonObject candidateSchema{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("required"),
         strings({QStringLiteral("source_start"), QStringLiteral("source_end"),
                  QStringLiteral("suggested_text"), QStringLiteral("category"),
                  QStringLiteral("confidence"), QStringLiteral("evidence"),
                  QStringLiteral("semantic_impact")})},
        {QStringLiteral("properties"),
         QJsonObject{
             {QStringLiteral("source_start"),
              QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
             {QStringLiteral("source_end"),
              QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
             {QStringLiteral("suggested_text"),
              QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
             {QStringLiteral("category"),
              QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                          {QStringLiteral("enum"),
                           strings({QStringLiteral("TYPO"), QStringLiteral("MISSING_WORD"),
                                    QStringLiteral("EXTRA_WORD"), QStringLiteral("DUPLICATED_TEXT"),
                                    QStringLiteral("SPACING"), QStringLiteral("PUNCTUATION"),
                                    QStringLiteral("NAME_INCONSISTENCY"),
                                    QStringLiteral("TERMINOLOGY_INCONSISTENCY"),
                                    QStringLiteral("POSSIBLE_SOURCE_DAMAGE"),
                                    QStringLiteral("AMBIGUOUS")})}}},
             {QStringLiteral("confidence"),
              QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
             {QStringLiteral("evidence"),
              QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
             {QStringLiteral("semantic_impact"),
              QJsonObject{
                  {QStringLiteral("type"), QStringLiteral("string")},
                  {QStringLiteral("enum"),
                   strings({QStringLiteral("TEXT_ONLY"), QStringLiteral("PUNCTUATION_ONLY"),
                            QStringLiteral("ENTITY_CHANGE"), QStringLiteral("DIALOGUE_CHANGE"),
                            QStringLiteral("ACTION_CHANGE"), QStringLiteral("TIMELINE_CHANGE"),
                            QStringLiteral("STORY_STATE_CHANGE"), QStringLiteral("UNKNOWN")})}}},
         }},
        {QStringLiteral("additionalProperties"), false},
    };
    return {
        {QStringLiteral("$schema"), QStringLiteral("https://json-schema.org/draft/2020-12/schema")},
        {QStringLiteral("title"), QStringLiteral("LoreForge semantic proofreading candidates")},
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("required"),
         strings({QStringLiteral("chapter_id"), QStringLiteral("candidates")})},
        {QStringLiteral("properties"),
         QJsonObject{
             {QStringLiteral("chapter_id"),
              QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
             {QStringLiteral("candidates"),
              QJsonObject{{QStringLiteral("type"), QStringLiteral("array")},
                          {QStringLiteral("items"), candidateSchema}}},
         }},
        {QStringLiteral("additionalProperties"), false},
    };
}

ProofreadingResult SemanticProofreader::extract(const ProofreadingSource& source,
                                                const QJsonObject& modelOutput,
                                                const ProofreadingPolicy& policy) {
    ProofreadingResult result;
    detail::validateSource(source, QStringLiteral("$.source"), result.errors);
    detail::validatePolicy(policy, result.errors);
    const auto validation = inference::OutputValidator::validate(outputSchema(), modelOutput);
    for (const auto& error : validation.errors) {
        result.errors.append(
            {ProofreadingErrorCode::SchemaViolation, QStringLiteral("$.output"), error});
    }
    if (!result.errors.isEmpty()) {
        return result;
    }
    if (modelOutput.value(QStringLiteral("chapter_id")).toString() != source.chapterId.toString()) {
        result.errors.append({ProofreadingErrorCode::ChapterMismatch,
                              QStringLiteral("$.output.chapter_id"),
                              QStringLiteral("The semantic result belongs to another chapter.")});
        return result;
    }

    ProtectedTermRegistry protectedTerms(policy.protectedTerms);
    ProofreadingReport report;
    report.sourceHash = source.sourceHash;
    QSet<QString> candidateIds;
    const auto values = modelOutput.value(QStringLiteral("candidates")).toArray();
    for (qsizetype index = 0; index < values.size(); ++index) {
        const auto path = QStringLiteral("$.output.candidates[%1]").arg(index);
        const auto object = values.at(index).toObject();
        const auto start = offset(object.value(QStringLiteral("source_start")));
        const auto end = offset(object.value(QStringLiteral("source_end")));
        const auto category =
            candidateCategoryFromName(object.value(QStringLiteral("category")).toString());
        const auto impact =
            semanticImpactFromName(object.value(QStringLiteral("semantic_impact")).toString());
        const auto confidence = object.value(QStringLiteral("confidence")).toDouble();
        const auto evidence = object.value(QStringLiteral("evidence")).toString();
        const auto suggestedText = object.value(QStringLiteral("suggested_text")).toString();
        if (!start.has_value() || !end.has_value() || *start < source.sourceSpan.startByte ||
            *end < *start || *end > source.sourceSpan.endByte || !category.has_value() ||
            !impact.has_value() || !std::isfinite(confidence) || confidence < 0.0 ||
            confidence > 1.0 || evidence.trimmed().isEmpty() || evidence != evidence.trimmed()) {
            result.errors.append({ProofreadingErrorCode::InvalidCandidate, path,
                                  QStringLiteral("The semantic candidate fields are invalid.")});
            continue;
        }
        const core::SourceSpan span{source.sourceSpan.sourceId, *start, *end};
        const auto original = detail::sourceText(source, span);
        if (!original.has_value() || *original == suggestedText) {
            result.errors.append(
                {ProofreadingErrorCode::InvalidCandidate, path,
                 QStringLiteral("The candidate must use UTF-8 boundaries and propose a change.")});
            continue;
        }
        if (protectedTerms.protects(source, span)) {
            result.errors.append(
                {ProofreadingErrorCode::ProtectedTerm, path,
                 QStringLiteral("A semantic candidate overlaps a protected term.")});
            continue;
        }
        auto candidate = detail::makeCandidate(
            source, span, *original, suggestedText, *category, confidence, evidence, *impact,
            CandidateOrigin::Semantic, QString::fromLatin1(kDetectorVersion));
        if (candidateIds.contains(candidate.id.toString())) {
            result.errors.append({ProofreadingErrorCode::DuplicateCandidate, path,
                                  QStringLiteral("Semantic candidates must be unique.")});
            continue;
        }
        candidateIds.insert(candidate.id.toString());
        report.candidates.append(std::move(candidate));
    }
    if (!result.errors.isEmpty()) {
        return result;
    }
    std::sort(report.candidates.begin(), report.candidates.end(), detail::candidateLess);
    result.report = std::move(report);
    return result;
}

} // namespace loreforge::proofreading
