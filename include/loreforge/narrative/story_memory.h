#pragma once

#include "loreforge/core/content_hash.h"
#include "loreforge/core/identifier.h"
#include "loreforge/narrative/chapter_analysis.h"

#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace loreforge::narrative {

struct ChapterMemoryRecord final {
    core::ProjectId projectId;
    qsizetype chapterSequence = -1;
    ChapterAnalysis analysis;

    friend bool operator==(const ChapterMemoryRecord&, const ChapterMemoryRecord&) = default;
};

struct CharacterMemory final {
    core::CharacterMemoryId id;
    QString name;
    QStringList aliases;
    QList<core::ChapterId> appearances;
    qsizetype firstSeenSequence = -1;
    qsizetype lastSeenSequence = -1;

    friend bool operator==(const CharacterMemory&, const CharacterMemory&) = default;
};

struct EventMemory final {
    core::EventMemoryId id;
    core::ChapterId chapterId;
    qsizetype chapterSequence = -1;
    QString description;
    QList<core::CharacterMemoryId> participants;
    QStringList unresolvedParticipants;
    std::optional<QString> location;
    ClaimSupport support;

    friend bool operator==(const EventMemory&, const EventMemory&) = default;
};

struct RelationshipMemory final {
    core::RelationshipMemoryId id;
    core::CharacterMemoryId firstCharacterId;
    core::CharacterMemoryId secondCharacterId;
    QList<core::EventMemoryId> sharedEvents;
    qsizetype firstSeenSequence = -1;
    qsizetype lastSeenSequence = -1;

    friend bool operator==(const RelationshipMemory&, const RelationshipMemory&) = default;
};

struct TimelineEntry final {
    qsizetype sequence = -1;
    core::ChapterId chapterId;
    core::EventMemoryId eventId;

    friend bool operator==(const TimelineEntry&, const TimelineEntry&) = default;
};

struct OpenThreadMemory final {
    core::OpenThreadMemoryId id;
    QString text;
    QList<core::ChapterId> mentions;
    qsizetype firstMentionSequence = -1;
    qsizetype lastMentionSequence = -1;
    bool inferredOnly = true;
    QList<SourceEvidence> evidence;

    friend bool operator==(const OpenThreadMemory&, const OpenThreadMemory&) = default;
};

struct StoryStateSnapshot final {
    core::StoryStateSnapshotId id;
    core::ProjectId projectId;
    qsizetype throughChapterSequence = -1;
    QList<core::ChapterId> sourceChapters;
    QList<CharacterMemory> characters;
    QList<EventMemory> events;
    QList<RelationshipMemory> relationships;
    QList<TimelineEntry> timeline;
    QList<OpenThreadMemory> openThreads;
    core::ContentHash sourceHash;
    core::ContentHash stateHash;

    friend bool operator==(const StoryStateSnapshot&, const StoryStateSnapshot&) = default;
};

enum class StoryMemoryErrorCode {
    InvalidProject,
    InvalidRecord,
    InvalidSequence,
    DuplicateChapter,
    InvalidClaim,
};

struct StoryMemoryError final {
    StoryMemoryErrorCode code;
    QString path;
    QString message;

    friend bool operator==(const StoryMemoryError&, const StoryMemoryError&) = default;
};

struct StoryStateResult final {
    std::optional<StoryStateSnapshot> snapshot;
    QList<StoryMemoryError> errors;

    [[nodiscard]] bool isValid() const noexcept;
};

class StoryStateRebuilder final {
  public:
    [[nodiscard]] static QList<StoryMemoryError> validateRecord(const ChapterMemoryRecord& record);
    [[nodiscard]] static StoryStateResult rebuild(const core::ProjectId& projectId,
                                                  QList<ChapterMemoryRecord> records);
};

} // namespace loreforge::narrative
