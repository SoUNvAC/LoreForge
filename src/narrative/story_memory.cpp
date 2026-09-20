#include "loreforge/narrative/story_memory.h"

#include "loreforge/narrative/story_memory_json.h"

#include <QHash>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <utility>

namespace loreforge::narrative {
namespace {

QString normalizedKey(QStringView value) {
    return value.toString().normalized(QString::NormalizationForm_C).toCaseFolded();
}

void addError(QList<StoryMemoryError>& errors, StoryMemoryErrorCode code, QString path,
              QString message) {
    errors.append({code, std::move(path), std::move(message)});
}

bool validText(QStringView text) {
    return !text.isEmpty() && text == text.trimmed();
}

void validateSupport(const ClaimSupport& support, const ChapterAnalysis& analysis, QString path,
                     QList<StoryMemoryError>& errors) {
    if (!std::isfinite(support.confidence) || support.confidence < 0.0 ||
        support.confidence > 1.0 || !support.isGrounded()) {
        addError(errors, StoryMemoryErrorCode::InvalidClaim, path,
                 QStringLiteral("Claim support is not valid or grounded."));
    }
    QSet<QString> ranges;
    for (qsizetype index = 0; index < support.evidence.size(); ++index) {
        const auto& evidence = support.evidence.at(index);
        const auto evidencePath = QStringLiteral("%1.evidence[%2]").arg(path).arg(index);
        const auto key = QStringLiteral("%1:%2:%3")
                             .arg(evidence.sourceSpan.sourceId)
                             .arg(evidence.sourceSpan.startByte)
                             .arg(evidence.sourceSpan.endByte);
        if (!evidence.sourceSpan.isValid() ||
            evidence.sourceSpan.sourceId != analysis.sourceSpan.sourceId ||
            evidence.sourceSpan.startByte < analysis.sourceSpan.startByte ||
            evidence.sourceSpan.endByte > analysis.sourceSpan.endByte || evidence.text.isEmpty() ||
            evidence.text.toUtf8().size() != evidence.sourceSpan.lengthBytes() ||
            ranges.contains(key)) {
            addError(errors, StoryMemoryErrorCode::InvalidClaim, evidencePath,
                     QStringLiteral("Claim evidence is invalid or duplicated."));
        }
        ranges.insert(key);
    }
}

void validateTextClaim(const TextClaim& claim, const ChapterAnalysis& analysis, QString path,
                       QList<StoryMemoryError>& errors) {
    if (!validText(claim.text)) {
        addError(errors, StoryMemoryErrorCode::InvalidClaim, path + QStringLiteral(".text"),
                 QStringLiteral("Claim text must be non-empty and trimmed."));
    }
    validateSupport(claim.support, analysis, std::move(path), errors);
}

core::ContentHash sourceHash(const QList<ChapterMemoryRecord>& records) {
    QByteArray bytes;
    for (const auto& record : records) {
        bytes.append(QByteArray::number(record.chapterSequence));
        bytes.append('\0');
        bytes.append(encodeChapterAnalysis(record.analysis));
        bytes.append('\0');
    }
    return core::ContentHash::sha256(bytes);
}

void appendUniqueChapter(QList<core::ChapterId>& chapters, const core::ChapterId& chapterId) {
    if (!chapters.contains(chapterId)) {
        chapters.append(chapterId);
    }
}

void appendUniqueEvidence(QList<SourceEvidence>& target, const QList<SourceEvidence>& additions) {
    for (const auto& evidence : additions) {
        if (!target.contains(evidence)) {
            target.append(evidence);
        }
    }
}

template <typename T, typename Id> void sortById(QList<T>& values, Id T::* member) {
    std::sort(values.begin(), values.end(), [member](const T& left, const T& right) {
        return (left.*member).toString() < (right.*member).toString();
    });
}

} // namespace

bool StoryStateResult::isValid() const noexcept {
    return snapshot.has_value() && errors.isEmpty();
}

QList<StoryMemoryError> StoryStateRebuilder::validateRecord(const ChapterMemoryRecord& record) {
    QList<StoryMemoryError> errors;
    if (!record.projectId.isValid() || record.chapterSequence < 0 ||
        !record.analysis.chapterId.isValid() || !record.analysis.sourceSpan.isValid()) {
        addError(errors, StoryMemoryErrorCode::InvalidRecord, QStringLiteral("$"),
                 QStringLiteral("The chapter-memory record identity and source must be valid."));
        return errors;
    }

    QSet<QString> characterNames;
    for (qsizetype index = 0; index < record.analysis.characters.size(); ++index) {
        const auto& character = record.analysis.characters.at(index);
        const auto path = QStringLiteral("$.characters[%1]").arg(index);
        const auto key = normalizedKey(character.name);
        if (!validText(character.name) || characterNames.contains(key)) {
            addError(errors, StoryMemoryErrorCode::InvalidClaim, path + QStringLiteral(".name"),
                     QStringLiteral("Character names must be trimmed and unique."));
        }
        characterNames.insert(key);
        validateSupport(character.support, record.analysis, path, errors);

        QSet<QString> aliases{key};
        for (qsizetype aliasIndex = 0; aliasIndex < character.aliases.size(); ++aliasIndex) {
            const auto& alias = character.aliases.at(aliasIndex);
            const auto aliasPath = QStringLiteral("%1.aliases[%2]").arg(path).arg(aliasIndex);
            const auto aliasKey = normalizedKey(alias.name);
            if (!validText(alias.name) || aliases.contains(aliasKey)) {
                addError(errors, StoryMemoryErrorCode::InvalidClaim,
                         aliasPath + QStringLiteral(".name"),
                         QStringLiteral("Aliases must be trimmed and unique per character."));
            }
            aliases.insert(aliasKey);
            validateSupport(alias.support, record.analysis, aliasPath, errors);
        }
    }

    QSet<QString> locations;
    for (qsizetype index = 0; index < record.analysis.locations.size(); ++index) {
        const auto& location = record.analysis.locations.at(index);
        const auto path = QStringLiteral("$.locations[%1]").arg(index);
        const auto key = normalizedKey(location.name);
        if (!validText(location.name) || locations.contains(key)) {
            addError(errors, StoryMemoryErrorCode::InvalidClaim, path + QStringLiteral(".name"),
                     QStringLiteral("Location names must be trimmed and unique."));
        }
        locations.insert(key);
        validateSupport(location.support, record.analysis, path, errors);
    }

    for (qsizetype index = 0; index < record.analysis.events.size(); ++index) {
        const auto& event = record.analysis.events.at(index);
        const auto path = QStringLiteral("$.events[%1]").arg(index);
        if (!validText(event.description) ||
            (event.location.has_value() && !validText(*event.location))) {
            addError(errors, StoryMemoryErrorCode::InvalidClaim, path,
                     QStringLiteral("Event text must be non-empty and trimmed."));
        }
        QSet<QString> participants;
        for (const auto& participant : event.participants) {
            const auto key = normalizedKey(participant);
            if (!validText(participant) || participants.contains(key)) {
                addError(errors, StoryMemoryErrorCode::InvalidClaim,
                         path + QStringLiteral(".participants"),
                         QStringLiteral("Event participants must be trimmed and unique."));
            }
            participants.insert(key);
        }
        validateSupport(event.support, record.analysis, path, errors);
    }

    validateTextClaim(record.analysis.summary, record.analysis, QStringLiteral("$.summary"),
                      errors);
    for (qsizetype index = 0; index < record.analysis.importantFacts.size(); ++index) {
        validateTextClaim(record.analysis.importantFacts.at(index), record.analysis,
                          QStringLiteral("$.important_facts[%1]").arg(index), errors);
    }
    for (qsizetype index = 0; index < record.analysis.openThreads.size(); ++index) {
        validateTextClaim(record.analysis.openThreads.at(index), record.analysis,
                          QStringLiteral("$.open_threads[%1]").arg(index), errors);
    }
    return errors;
}

StoryStateResult StoryStateRebuilder::rebuild(const core::ProjectId& projectId,
                                              QList<ChapterMemoryRecord> records) {
    StoryStateResult result;
    if (!projectId.isValid()) {
        addError(result.errors, StoryMemoryErrorCode::InvalidProject, QStringLiteral("$.project"),
                 QStringLiteral("A valid project ID is required."));
        return result;
    }
    if (records.isEmpty()) {
        addError(result.errors, StoryMemoryErrorCode::InvalidSequence, QStringLiteral("$.chapters"),
                 QStringLiteral("At least one persisted chapter record is required."));
        return result;
    }
    std::sort(records.begin(), records.end(), [](const auto& left, const auto& right) {
        return left.chapterSequence < right.chapterSequence;
    });

    QSet<QString> chapterIds;
    for (qsizetype index = 0; index < records.size(); ++index) {
        const auto& record = records.at(index);
        if (record.projectId != projectId) {
            addError(result.errors, StoryMemoryErrorCode::InvalidProject,
                     QStringLiteral("$.chapters[%1].project_id").arg(index),
                     QStringLiteral("Every chapter record must belong to the rebuilt project."));
        }
        if (record.chapterSequence != index) {
            addError(result.errors, StoryMemoryErrorCode::InvalidSequence,
                     QStringLiteral("$.chapters[%1].sequence").arg(index),
                     QStringLiteral("Chapter records must form the contiguous sequence 0..N."));
        }
        const auto chapterKey = record.analysis.chapterId.toString();
        if (chapterIds.contains(chapterKey)) {
            addError(result.errors, StoryMemoryErrorCode::DuplicateChapter,
                     QStringLiteral("$.chapters[%1].chapter_id").arg(index),
                     QStringLiteral("A chapter may occur only once in story memory."));
        }
        chapterIds.insert(chapterKey);
        result.errors.append(validateRecord(record));
    }
    if (!result.errors.isEmpty()) {
        return result;
    }

    QHash<QString, CharacterMemory> characterByKey;
    QHash<QString, OpenThreadMemory> threadByKey;
    QHash<QString, RelationshipMemory> relationshipByKey;
    QList<EventMemory> events;
    QList<TimelineEntry> timeline;
    QList<core::ChapterId> sourceChapters;

    for (const auto& record : records) {
        const auto& analysis = record.analysis;
        sourceChapters.append(analysis.chapterId);
        QHash<QString, core::CharacterMemoryId> localCharacters;
        QSet<QString> ambiguousNames;

        for (const auto& character : analysis.characters) {
            const auto key = normalizedKey(character.name);
            const auto id = core::CharacterMemoryId::fromStableKey(projectId.toString() +
                                                                   QLatin1Char(':') + key);
            if (!characterByKey.contains(key)) {
                characterByKey.insert(key, {id,
                                            character.name,
                                            {},
                                            {analysis.chapterId},
                                            record.chapterSequence,
                                            record.chapterSequence});
            } else {
                auto& memory = characterByKey[key];
                appendUniqueChapter(memory.appearances, analysis.chapterId);
                memory.lastSeenSequence = record.chapterSequence;
            }
            auto& memory = characterByKey[key];
            for (const auto& alias : character.aliases) {
                const auto aliasKey = normalizedKey(alias.name);
                bool exists = false;
                for (const auto& current : memory.aliases) {
                    exists = exists || normalizedKey(current) == aliasKey;
                }
                if (!exists) {
                    memory.aliases.append(alias.name);
                }
            }

            const auto addLocalName = [&localCharacters, &ambiguousNames](const QString& nameKey,
                                                                          const auto& memoryId) {
                if (ambiguousNames.contains(nameKey)) {
                    return;
                }
                if (localCharacters.contains(nameKey) &&
                    localCharacters.value(nameKey) != memoryId) {
                    localCharacters.remove(nameKey);
                    ambiguousNames.insert(nameKey);
                    return;
                }
                localCharacters.insert(nameKey, memoryId);
            };
            addLocalName(key, id);
            for (const auto& alias : character.aliases) {
                addLocalName(normalizedKey(alias.name), id);
            }
        }

        for (qsizetype eventIndex = 0; eventIndex < analysis.events.size(); ++eventIndex) {
            const auto& event = analysis.events.at(eventIndex);
            EventMemory memory;
            memory.id = core::EventMemoryId::fromStableKey(
                analysis.chapterId.toString() + QLatin1Char(':') + QString::number(eventIndex));
            memory.chapterId = analysis.chapterId;
            memory.chapterSequence = record.chapterSequence;
            memory.description = event.description;
            memory.location = event.location;
            memory.support = event.support;
            for (const auto& participant : event.participants) {
                const auto key = normalizedKey(participant);
                if (!ambiguousNames.contains(key) && localCharacters.contains(key)) {
                    const auto id = localCharacters.value(key);
                    if (!memory.participants.contains(id)) {
                        memory.participants.append(id);
                    }
                } else {
                    memory.unresolvedParticipants.append(participant);
                }
            }
            std::sort(memory.participants.begin(), memory.participants.end(),
                      [](const auto& left, const auto& right) {
                          return left.toString() < right.toString();
                      });
            events.append(memory);
            timeline.append({timeline.size(), analysis.chapterId, memory.id});

            for (qsizetype first = 0; first < memory.participants.size(); ++first) {
                for (qsizetype second = first + 1; second < memory.participants.size(); ++second) {
                    const auto& firstId = memory.participants.at(first);
                    const auto& secondId = memory.participants.at(second);
                    const auto key = firstId.toString() + QLatin1Char(':') + secondId.toString();
                    if (!relationshipByKey.contains(key)) {
                        relationshipByKey.insert(key,
                                                 {core::RelationshipMemoryId::fromStableKey(key),
                                                  firstId,
                                                  secondId,
                                                  {memory.id},
                                                  record.chapterSequence,
                                                  record.chapterSequence});
                    } else {
                        auto& relationship = relationshipByKey[key];
                        relationship.sharedEvents.append(memory.id);
                        relationship.lastSeenSequence = record.chapterSequence;
                    }
                }
            }
        }

        for (const auto& thread : analysis.openThreads) {
            const auto key = normalizedKey(thread.text);
            const auto id = core::OpenThreadMemoryId::fromStableKey(projectId.toString() +
                                                                    QLatin1Char(':') + key);
            if (!threadByKey.contains(key)) {
                threadByKey.insert(key, {id,
                                         thread.text,
                                         {analysis.chapterId},
                                         record.chapterSequence,
                                         record.chapterSequence,
                                         thread.support.basis == ClaimBasis::Inference,
                                         thread.support.evidence});
            } else {
                auto& memory = threadByKey[key];
                appendUniqueChapter(memory.mentions, analysis.chapterId);
                memory.lastMentionSequence = record.chapterSequence;
                memory.inferredOnly =
                    memory.inferredOnly && thread.support.basis == ClaimBasis::Inference;
                appendUniqueEvidence(memory.evidence, thread.support.evidence);
            }
        }
    }

    QList<CharacterMemory> characters = characterByKey.values();
    for (auto& character : characters) {
        std::sort(character.aliases.begin(), character.aliases.end(),
                  [](const auto& left, const auto& right) {
                      return normalizedKey(left) < normalizedKey(right);
                  });
    }
    sortById(characters, &CharacterMemory::id);
    sortById(events, &EventMemory::id);
    QList<RelationshipMemory> relationships = relationshipByKey.values();
    sortById(relationships, &RelationshipMemory::id);
    QList<OpenThreadMemory> openThreads = threadByKey.values();
    sortById(openThreads, &OpenThreadMemory::id);

    StoryStateSnapshot snapshot;
    snapshot.projectId = projectId;
    snapshot.throughChapterSequence = records.last().chapterSequence;
    snapshot.sourceChapters = std::move(sourceChapters);
    snapshot.characters = std::move(characters);
    snapshot.events = std::move(events);
    snapshot.relationships = std::move(relationships);
    snapshot.timeline = std::move(timeline);
    snapshot.openThreads = std::move(openThreads);
    snapshot.sourceHash = sourceHash(records);
    auto statePayload = snapshot;
    statePayload.sourceHash = {};
    snapshot.stateHash = core::ContentHash::sha256(encodeStoryStateSnapshot(statePayload));
    snapshot.id = core::StoryStateSnapshotId::fromStableKey(
        projectId.toString() + QLatin1Char(':') + QString::number(snapshot.throughChapterSequence) +
        QLatin1Char(':') + snapshot.sourceHash.toHex() + QLatin1Char(':') +
        snapshot.stateHash.toHex());
    result.snapshot = std::move(snapshot);
    return result;
}

} // namespace loreforge::narrative
