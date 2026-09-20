#pragma once

#include "loreforge/core/identifier.h"
#include "loreforge/core/source_span.h"

#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace loreforge::narrative {

struct SourceEvidence final {
    core::SourceSpan sourceSpan;
    QString text;

    friend bool operator==(const SourceEvidence&, const SourceEvidence&) = default;
};

enum class ClaimBasis {
    Evidence,
    Inference,
};

struct ClaimSupport final {
    ClaimBasis basis = ClaimBasis::Evidence;
    QList<SourceEvidence> evidence;
    double confidence = 0.0;

    [[nodiscard]] bool isGrounded() const noexcept;
    friend bool operator==(const ClaimSupport&, const ClaimSupport&) = default;
};

struct CharacterAlias final {
    QString name;
    ClaimSupport support;

    friend bool operator==(const CharacterAlias&, const CharacterAlias&) = default;
};

struct ChapterCharacter final {
    QString name;
    QList<CharacterAlias> aliases;
    ClaimSupport support;

    friend bool operator==(const ChapterCharacter&, const ChapterCharacter&) = default;
};

struct ChapterLocation final {
    QString name;
    ClaimSupport support;

    friend bool operator==(const ChapterLocation&, const ChapterLocation&) = default;
};

struct ChapterEvent final {
    QString description;
    QStringList participants;
    std::optional<QString> location;
    ClaimSupport support;

    friend bool operator==(const ChapterEvent&, const ChapterEvent&) = default;
};

struct TextClaim final {
    QString text;
    ClaimSupport support;

    friend bool operator==(const TextClaim&, const TextClaim&) = default;
};

struct ChapterAnalysis final {
    core::ChapterId chapterId;
    core::SourceSpan sourceSpan;
    QList<ChapterCharacter> characters;
    QList<ChapterLocation> locations;
    QList<ChapterEvent> events;
    TextClaim summary;
    QList<TextClaim> importantFacts;
    QList<TextClaim> openThreads;

    friend bool operator==(const ChapterAnalysis&, const ChapterAnalysis&) = default;
};

} // namespace loreforge::narrative
