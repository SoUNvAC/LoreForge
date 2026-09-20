#include "loreforge/narrative/story_memory_json.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#include <cmath>
#include <utility>

namespace loreforge::narrative {
namespace {

QJsonObject encodeEvidence(const SourceEvidence& evidence) {
    return {
        {QStringLiteral("source_id"), evidence.sourceSpan.sourceId},
        {QStringLiteral("source_start"), evidence.sourceSpan.startByte},
        {QStringLiteral("source_end"), evidence.sourceSpan.endByte},
        {QStringLiteral("text"), evidence.text},
    };
}

QJsonObject encodeSupport(const ClaimSupport& support) {
    QJsonArray evidence;
    for (const auto& item : support.evidence) {
        evidence.append(encodeEvidence(item));
    }
    return {
        {QStringLiteral("basis"), support.basis == ClaimBasis::Evidence
                                      ? QStringLiteral("evidence")
                                      : QStringLiteral("inference")},
        {QStringLiteral("confidence"), support.confidence},
        {QStringLiteral("evidence"), evidence},
    };
}

void insertSupport(QJsonObject& object, const ClaimSupport& support) {
    const auto encoded = encodeSupport(support);
    object.insert(QStringLiteral("basis"), encoded.value(QStringLiteral("basis")));
    object.insert(QStringLiteral("confidence"), encoded.value(QStringLiteral("confidence")));
    object.insert(QStringLiteral("evidence"), encoded.value(QStringLiteral("evidence")));
}

QJsonObject encodeTextClaim(const TextClaim& claim) {
    QJsonObject object{{QStringLiteral("text"), claim.text}};
    insertSupport(object, claim.support);
    return object;
}

QJsonArray encodeIdentifiers(const auto& identifiers) {
    QJsonArray array;
    for (const auto& identifier : identifiers) {
        array.append(identifier.toString());
    }
    return array;
}

QJsonArray encodeStrings(const auto& strings) {
    QJsonArray array;
    for (const auto& string : strings) {
        array.append(string);
    }
    return array;
}

class Decoder final {
  public:
    explicit Decoder(QJsonObject root) : root_(std::move(root)) {}

    ChapterMemoryDecodeResult decode() {
        ChapterAnalysis analysis;
        const auto chapterId =
            core::ChapterId::fromString(string(root_, QStringLiteral("chapter_id")));
        if (!chapterId.has_value()) {
            fail(QStringLiteral("chapter_id is invalid."));
        } else {
            analysis.chapterId = *chapterId;
        }
        const auto source = object(root_, QStringLiteral("source"));
        analysis.sourceSpan = {string(source, QStringLiteral("source_id")),
                               integer(source, QStringLiteral("start")),
                               integer(source, QStringLiteral("end"))};

        const auto characters = array(root_, QStringLiteral("characters"));
        for (const auto& value : characters) {
            const auto characterObject = requireObject(value);
            ChapterCharacter character;
            character.name = string(characterObject, QStringLiteral("name"));
            character.support = support(characterObject);
            for (const auto& aliasValue : array(characterObject, QStringLiteral("aliases"))) {
                const auto aliasObject = requireObject(aliasValue);
                character.aliases.append(
                    {string(aliasObject, QStringLiteral("name")), support(aliasObject)});
            }
            analysis.characters.append(std::move(character));
        }

        for (const auto& value : array(root_, QStringLiteral("locations"))) {
            const auto location = requireObject(value);
            analysis.locations.append(
                {string(location, QStringLiteral("name")), support(location)});
        }

        for (const auto& value : array(root_, QStringLiteral("events"))) {
            const auto eventObject = requireObject(value);
            QStringList participants;
            for (const auto& participant : array(eventObject, QStringLiteral("participants"))) {
                if (!participant.isString()) {
                    fail(QStringLiteral("An event participant is not a string."));
                }
                participants.append(participant.toString());
            }
            std::optional<QString> location;
            const auto locationValue = eventObject.value(QStringLiteral("location"));
            if (locationValue.isString()) {
                location = locationValue.toString();
            } else if (!locationValue.isNull()) {
                fail(QStringLiteral("An event location must be a string or null."));
            }
            analysis.events.append({string(eventObject, QStringLiteral("description")),
                                    std::move(participants), std::move(location),
                                    support(eventObject)});
        }

        analysis.summary = textClaim(object(root_, QStringLiteral("summary")));
        for (const auto& value : array(root_, QStringLiteral("important_facts"))) {
            analysis.importantFacts.append(textClaim(requireObject(value)));
        }
        for (const auto& value : array(root_, QStringLiteral("open_threads"))) {
            analysis.openThreads.append(textClaim(requireObject(value)));
        }
        if (!error_.isEmpty()) {
            return {std::nullopt, error_};
        }
        return {std::move(analysis), {}};
    }

  private:
    void fail(QString message) {
        if (error_.isEmpty()) {
            error_ = std::move(message);
        }
    }

    QJsonObject requireObject(const QJsonValue& value) {
        if (!value.isObject()) {
            fail(QStringLiteral("A required value is not an object."));
            return {};
        }
        return value.toObject();
    }

    QJsonObject object(const QJsonObject& parent, const QString& key) {
        if (!parent.contains(key) || !parent.value(key).isObject()) {
            fail(QStringLiteral("%1 is missing or is not an object.").arg(key));
            return {};
        }
        return parent.value(key).toObject();
    }

    QJsonArray array(const QJsonObject& parent, const QString& key) {
        if (!parent.contains(key) || !parent.value(key).isArray()) {
            fail(QStringLiteral("%1 is missing or is not an array.").arg(key));
            return {};
        }
        return parent.value(key).toArray();
    }

    QString string(const QJsonObject& parent, const QString& key) {
        if (!parent.contains(key) || !parent.value(key).isString()) {
            fail(QStringLiteral("%1 is missing or is not a string.").arg(key));
            return {};
        }
        return parent.value(key).toString();
    }

    qint64 integer(const QJsonObject& parent, const QString& key) {
        const auto value = parent.value(key);
        if (!parent.contains(key) || !value.isDouble() || !std::isfinite(value.toDouble()) ||
            std::floor(value.toDouble()) != value.toDouble()) {
            fail(QStringLiteral("%1 is missing or is not an integer.").arg(key));
            return -1;
        }
        return value.toInteger(-1);
    }

    ClaimSupport support(const QJsonObject& object) {
        ClaimSupport result;
        const auto basis = string(object, QStringLiteral("basis"));
        if (basis == QStringLiteral("evidence")) {
            result.basis = ClaimBasis::Evidence;
        } else if (basis == QStringLiteral("inference")) {
            result.basis = ClaimBasis::Inference;
        } else {
            fail(QStringLiteral("Claim basis is invalid."));
        }
        const auto confidence = object.value(QStringLiteral("confidence"));
        if (!confidence.isDouble()) {
            fail(QStringLiteral("Claim confidence is missing or invalid."));
        } else {
            result.confidence = confidence.toDouble();
        }
        for (const auto& value : array(object, QStringLiteral("evidence"))) {
            const auto evidenceObject = requireObject(value);
            result.evidence.append({{string(evidenceObject, QStringLiteral("source_id")),
                                     integer(evidenceObject, QStringLiteral("source_start")),
                                     integer(evidenceObject, QStringLiteral("source_end"))},
                                    string(evidenceObject, QStringLiteral("text"))});
        }
        return result;
    }

    TextClaim textClaim(const QJsonObject& object) {
        return {string(object, QStringLiteral("text")), support(object)};
    }

    QJsonObject root_;
    QString error_;
};

} // namespace

bool ChapterMemoryDecodeResult::isValid() const noexcept {
    return analysis.has_value() && error.isEmpty();
}

QByteArray encodeChapterAnalysis(const ChapterAnalysis& analysis) {
    QJsonArray characters;
    for (const auto& character : analysis.characters) {
        QJsonArray aliases;
        for (const auto& alias : character.aliases) {
            QJsonObject aliasObject{{QStringLiteral("name"), alias.name}};
            insertSupport(aliasObject, alias.support);
            aliases.append(aliasObject);
        }
        QJsonObject object{{QStringLiteral("name"), character.name},
                           {QStringLiteral("aliases"), aliases}};
        insertSupport(object, character.support);
        characters.append(object);
    }

    QJsonArray locations;
    for (const auto& location : analysis.locations) {
        QJsonObject object{{QStringLiteral("name"), location.name}};
        insertSupport(object, location.support);
        locations.append(object);
    }

    QJsonArray events;
    for (const auto& event : analysis.events) {
        QJsonObject object{
            {QStringLiteral("description"), event.description},
            {QStringLiteral("participants"), encodeStrings(event.participants)},
            {QStringLiteral("location"), event.location.has_value() ? QJsonValue(*event.location)
                                                                    : QJsonValue(QJsonValue::Null)},
        };
        insertSupport(object, event.support);
        events.append(object);
    }

    QJsonArray importantFacts;
    for (const auto& fact : analysis.importantFacts) {
        importantFacts.append(encodeTextClaim(fact));
    }
    QJsonArray openThreads;
    for (const auto& thread : analysis.openThreads) {
        openThreads.append(encodeTextClaim(thread));
    }

    const QJsonObject root{
        {QStringLiteral("chapter_id"), analysis.chapterId.toString()},
        {QStringLiteral("source"),
         QJsonObject{{QStringLiteral("source_id"), analysis.sourceSpan.sourceId},
                     {QStringLiteral("start"), analysis.sourceSpan.startByte},
                     {QStringLiteral("end"), analysis.sourceSpan.endByte}}},
        {QStringLiteral("characters"), characters},
        {QStringLiteral("locations"), locations},
        {QStringLiteral("events"), events},
        {QStringLiteral("summary"), encodeTextClaim(analysis.summary)},
        {QStringLiteral("important_facts"), importantFacts},
        {QStringLiteral("open_threads"), openThreads},
    };
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

ChapterMemoryDecodeResult decodeChapterAnalysis(QByteArrayView json) {
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(json.toByteArray(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return {std::nullopt, QStringLiteral("Stored chapter analysis is not valid JSON.")};
    }
    return Decoder(document.object()).decode();
}

QByteArray encodeStoryStateSnapshot(const StoryStateSnapshot& snapshot) {
    QJsonArray characters;
    for (const auto& character : snapshot.characters) {
        characters.append(QJsonObject{
            {QStringLiteral("id"), character.id.toString()},
            {QStringLiteral("name"), character.name},
            {QStringLiteral("aliases"), encodeStrings(character.aliases)},
            {QStringLiteral("appearances"), encodeIdentifiers(character.appearances)},
            {QStringLiteral("first_seen"), character.firstSeenSequence},
            {QStringLiteral("last_seen"), character.lastSeenSequence},
        });
    }

    QJsonArray events;
    for (const auto& event : snapshot.events) {
        events.append(QJsonObject{
            {QStringLiteral("id"), event.id.toString()},
            {QStringLiteral("chapter_id"), event.chapterId.toString()},
            {QStringLiteral("chapter_sequence"), event.chapterSequence},
            {QStringLiteral("description"), event.description},
            {QStringLiteral("participants"), encodeIdentifiers(event.participants)},
            {QStringLiteral("unresolved_participants"),
             encodeStrings(event.unresolvedParticipants)},
            {QStringLiteral("location"), event.location.has_value() ? QJsonValue(*event.location)
                                                                    : QJsonValue(QJsonValue::Null)},
            {QStringLiteral("support"), encodeSupport(event.support)},
        });
    }

    QJsonArray relationships;
    for (const auto& relationship : snapshot.relationships) {
        relationships.append(QJsonObject{
            {QStringLiteral("id"), relationship.id.toString()},
            {QStringLiteral("first_character_id"), relationship.firstCharacterId.toString()},
            {QStringLiteral("second_character_id"), relationship.secondCharacterId.toString()},
            {QStringLiteral("shared_events"), encodeIdentifiers(relationship.sharedEvents)},
            {QStringLiteral("first_seen"), relationship.firstSeenSequence},
            {QStringLiteral("last_seen"), relationship.lastSeenSequence},
        });
    }

    QJsonArray timeline;
    for (const auto& entry : snapshot.timeline) {
        timeline.append(QJsonObject{{QStringLiteral("sequence"), entry.sequence},
                                    {QStringLiteral("chapter_id"), entry.chapterId.toString()},
                                    {QStringLiteral("event_id"), entry.eventId.toString()}});
    }

    QJsonArray threads;
    for (const auto& thread : snapshot.openThreads) {
        QJsonArray evidence;
        for (const auto& item : thread.evidence) {
            evidence.append(encodeEvidence(item));
        }
        threads.append(QJsonObject{
            {QStringLiteral("id"), thread.id.toString()},
            {QStringLiteral("text"), thread.text},
            {QStringLiteral("mentions"), encodeIdentifiers(thread.mentions)},
            {QStringLiteral("first_mention"), thread.firstMentionSequence},
            {QStringLiteral("last_mention"), thread.lastMentionSequence},
            {QStringLiteral("inferred_only"), thread.inferredOnly},
            {QStringLiteral("evidence"), evidence},
        });
    }

    const QJsonObject root{
        {QStringLiteral("id"), snapshot.id.toString()},
        {QStringLiteral("project_id"), snapshot.projectId.toString()},
        {QStringLiteral("through_chapter_sequence"), snapshot.throughChapterSequence},
        {QStringLiteral("source_chapters"), encodeIdentifiers(snapshot.sourceChapters)},
        {QStringLiteral("characters"), characters},
        {QStringLiteral("events"), events},
        {QStringLiteral("relationships"), relationships},
        {QStringLiteral("timeline"), timeline},
        {QStringLiteral("open_threads"), threads},
        {QStringLiteral("source_hash"), snapshot.sourceHash.toHex()},
        {QStringLiteral("state_hash"), snapshot.stateHash.toHex()},
    };
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

} // namespace loreforge::narrative
