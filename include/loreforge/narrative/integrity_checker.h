#pragma once

#include "loreforge/core/content_hash.h"
#include "loreforge/narrative/chapter_analysis.h"

#include <compare>

namespace loreforge::narrative {

// Explicit story chronology, independent of chapter/narration order. Travel durations use
// this same clock's ticks. Order disambiguates events at the same tick.
struct StoryMoment final {
    qint64 tick = -1;
    qint64 order = 0;
    friend bool operator==(const StoryMoment&, const StoryMoment&) = default;
    friend auto operator<=>(const StoryMoment&, const StoryMoment&) = default;
};

struct IntegrityChapter final {
    core::ChapterId id;
    core::SourceSpan span;
    QByteArray utf8; // Exactly the chapter projection, not a padded full-source buffer.
    core::ContentHash hash;
};

struct IntegrityAnchor final {
    core::ChapterId chapterId;
    ClaimSupport support;
};

struct IntegrityNameVariant final {
    QString name;
    StoryMoment validFrom;
    std::optional<StoryMoment> validUntil; // Exclusive.
    IntegrityAnchor anchor;
};

struct IntegrityCharacter final {
    core::CharacterMemoryId id;
    QString canonicalName;
    StoryMoment existsFrom;
    IntegrityAnchor anchor;
    QList<IntegrityNameVariant> variants;
};

struct IntegrityInformation final {
    QString id;
    QString description;
};

// Access for a particular character, not when the reader learns a secret.
struct IntegrityRevelation final {
    QString informationId;
    core::CharacterMemoryId characterId;
    StoryMoment availableFrom;
    IntegrityAnchor anchor;
};

struct IntegrityLocation final {
    QString id;
    QString name;
};

struct IntegrityTravelConstraint final {
    QString from;
    QString to;
    qint64 minimumTicks = -1; // Explicit directed bound; no guessed geography or route inference.
    IntegrityAnchor anchor;
};

enum class IntegrityEventKind { Appearance, Speech, Death, Revival, Knowledge, LocationPresence };

struct IntegrityEvent final {
    QString id;
    IntegrityEventKind kind = IntegrityEventKind::Appearance;
    core::CharacterMemoryId characterId;
    StoryMoment moment;
    IntegrityAnchor anchor;
    std::optional<QString> usedName;
    std::optional<QString> informationId; // Knowledge only.
    std::optional<QString> locationId;    // LocationPresence only.
};

enum class IntegrityRule {
    AppearanceBeforeExistence,
    SpeechAfterDeath,
    KnowledgeBeforeRevelation,
    ImpossibleLocationTransition,
    InvalidNameVariant,
    AmbiguousNameVariant,
    UnresolvedCharacter,
    UnresolvedInformation,
    UnknownKnowledgeAccess,
    UnresolvedLocation,
    UnknownTravelConstraint,
};
enum class IntegritySeverity { Warning, Notice };
[[nodiscard]] QString integrityRuleName(IntegrityRule rule);

// An explicit, evidence-backed exception applies only to this event/rule pair. It does
// not globally permit later speech, unexplained teleportation, or early knowledge.
struct IntegrityExplanation final {
    QString eventId;
    IntegrityRule rule = IntegrityRule::SpeechAfterDeath;
    QString reason;
    IntegrityAnchor anchor;
};

struct IntegrityInput final {
    core::ProjectId projectId;
    QString clockId;
    QList<IntegrityChapter> chapters;
    QList<IntegrityCharacter> characters;
    QList<IntegrityInformation> information;
    QList<IntegrityRevelation> revelations;
    QList<IntegrityLocation> locations;
    QList<IntegrityTravelConstraint> travelConstraints;
    QList<IntegrityEvent> events;
    QList<IntegrityExplanation> explanations;
};

struct IntegrityDiagnostic final {
    QString id;
    IntegrityRule rule;
    IntegritySeverity severity;
    QString eventId;
    core::ChapterId chapterId;
    core::CharacterMemoryId characterId;
    StoryMoment moment;
    QString message;
    ClaimBasis basis = ClaimBasis::Inference;
    double confidence = 0.0;
    QList<SourceEvidence> evidence;
    friend bool operator==(const IntegrityDiagnostic&, const IntegrityDiagnostic&) = default;
};

struct ExplainedIntegrityDiagnostic final {
    IntegrityDiagnostic diagnostic;
    QString reason;
    IntegrityAnchor explanation;
};

struct IntegrityInputError final {
    QString path;
    QString message;
};

struct IntegrityReport final {
    core::ContentHash inputHash;
    QList<IntegrityDiagnostic> diagnostics;
    QList<ExplainedIntegrityDiagnostic> explained;
    QList<IntegrityInputError> errors;
    [[nodiscard]] bool isValid() const noexcept {
        return errors.isEmpty() && inputHash.isValid();
    }
};

class NarrativeIntegrityChecker final {
  public:
    [[nodiscard]] static QString version();
    // Invalid/stale inputs return errors and no diagnostics. Valid findings are advisory.
    [[nodiscard]] static IntegrityReport check(const IntegrityInput& input);
};

} // namespace loreforge::narrative
