#include "loreforge/narrative/integrity_checker.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QSet>

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

namespace loreforge::narrative {
namespace {
bool validText(const QString& text) {
    return !text.isEmpty() && text == text.trimmed();
}
bool validMoment(const StoryMoment& moment) {
    return moment.tick >= 0 && moment.order >= 0;
}
QString nameKey(const QString& name) {
    return name.normalized(QString::NormalizationForm_C).toCaseFolded();
}
QJsonArray encodeMoment(const StoryMoment& moment) {
    return {QString::number(moment.tick), QString::number(moment.order)};
}
QJsonArray encodeEvidence(const SourceEvidence& evidence) {
    return {evidence.sourceSpan.sourceId, QString::number(evidence.sourceSpan.startByte),
            QString::number(evidence.sourceSpan.endByte), evidence.text};
}
template <typename T, typename Encoder>
QJsonArray canonicalArray(const QList<T>& values, Encoder encoder) {
    QList<QByteArray> bytes;
    for (const auto& value : values) {
        bytes.append(QJsonDocument(encoder(value)).toJson(QJsonDocument::Compact));
    }
    std::sort(bytes.begin(), bytes.end());
    QJsonArray result;
    for (const auto& value : bytes) {
        result.append(QJsonDocument::fromJson(value).array());
    }
    return result;
}
QJsonArray encodeAnchor(const IntegrityAnchor& anchor) {
    return {anchor.chapterId.toString(), static_cast<int>(anchor.support.basis),
            anchor.support.confidence, canonicalArray(anchor.support.evidence, encodeEvidence)};
}
core::ContentHash inputHash(const IntegrityInput& input) {
    const QJsonArray values{
        NarrativeIntegrityChecker::version(),
        input.projectId.toString(),
        input.clockId,
        canonicalArray(input.chapters,
                       [](const auto& chapter) -> QJsonArray {
                           return {chapter.id.toString(), chapter.span.sourceId,
                                   QString::number(chapter.span.startByte),
                                   QString::number(chapter.span.endByte), chapter.hash.toHex()};
                       }),
        canonicalArray(
            input.characters,
            [](const auto& character) -> QJsonArray {
                return {character.id.toString(), character.canonicalName,
                        encodeMoment(character.existsFrom), encodeAnchor(character.anchor),
                        canonicalArray(character.variants, [](const auto& variant) -> QJsonArray {
                            return {variant.name, encodeMoment(variant.validFrom),
                                    variant.validUntil
                                        ? QJsonValue(encodeMoment(*variant.validUntil))
                                        : QJsonValue(),
                                    encodeAnchor(variant.anchor)};
                        })};
            }),
        canonicalArray(input.information,
                       [](const auto& item) -> QJsonArray { return {item.id, item.description}; }),
        canonicalArray(input.revelations,
                       [](const auto& item) -> QJsonArray {
                           return {item.informationId, item.characterId.toString(),
                                   encodeMoment(item.availableFrom), encodeAnchor(item.anchor)};
                       }),
        canonicalArray(input.locations,
                       [](const auto& item) -> QJsonArray { return {item.id, item.name}; }),
        canonicalArray(input.travelConstraints,
                       [](const auto& item) -> QJsonArray {
                           return {item.from, item.to, QString::number(item.minimumTicks),
                                   encodeAnchor(item.anchor)};
                       }),
        canonicalArray(input.events,
                       [](const auto& event) -> QJsonArray {
                           return {event.id,
                                   static_cast<int>(event.kind),
                                   event.characterId.toString(),
                                   encodeMoment(event.moment),
                                   encodeAnchor(event.anchor),
                                   event.usedName ? QJsonValue(*event.usedName) : QJsonValue(),
                                   event.informationId ? QJsonValue(*event.informationId)
                                                       : QJsonValue(),
                                   event.locationId ? QJsonValue(*event.locationId) : QJsonValue()};
                       }),
        canonicalArray(input.explanations, [](const auto& item) -> QJsonArray {
            return {item.eventId, static_cast<int>(item.rule), item.reason,
                    encodeAnchor(item.anchor)};
        })};
    const auto bytes = QJsonDocument(values).toJson(QJsonDocument::Compact);
    return core::ContentHash::sha256(QByteArrayView(bytes));
}

class Validator final {
  public:
    explicit Validator(const IntegrityInput& input) : input_(input) {}
    QList<IntegrityInputError> validate() {
        if (!input_.projectId.isValid() || !validText(input_.clockId)) {
            error(u"$"_s, u"Project identity and an explicit story clock are required"_s);
        }
        for (const auto& chapter : input_.chapters) {
            const auto path = u"chapters/%1"_s.arg(chapter.id.toString());
            if (!chapter.id.isValid() || chapters_.contains(chapter.id) ||
                !chapter.span.isValid() || !validText(chapter.span.sourceId) ||
                chapter.span.lengthBytes() != chapter.utf8.size() ||
                QString::fromUtf8(chapter.utf8).toUtf8() != chapter.utf8 ||
                !chapter.hash.isValid() ||
                chapter.hash != core::ContentHash::sha256(QByteArrayView(chapter.utf8))) {
                error(path, u"Invalid, duplicate or stale UTF-8 chapter projection"_s);
            }
            chapters_.insert(chapter.id, &chapter);
        }
        QList<core::SourceSpan> spans;
        for (const auto& chapter : input_.chapters) {
            spans.append(chapter.span);
        }
        std::sort(spans.begin(), spans.end(), [](const auto& a, const auto& b) {
            return a.sourceId != b.sourceId ? a.sourceId < b.sourceId : a.startByte < b.startByte;
        });
        for (qsizetype i = 1; i < spans.size(); ++i) {
            if (spans[i].sourceId == spans[i - 1].sourceId && spans[i].overlaps(spans[i - 1])) {
                error(u"chapters"_s, u"Canonical chapter projections must not overlap"_s);
            }
        }
        // Never slice evidence against a malformed projection, even while collecting errors.
        if (!errors_.isEmpty()) {
            return errors_;
        }
        for (const auto& character : input_.characters) {
            const auto path = u"characters/%1"_s.arg(character.id.toString());
            if (!character.id.isValid() || characters_.contains(character.id) ||
                !validText(character.canonicalName) || !validMoment(character.existsFrom)) {
                error(path, u"Invalid or duplicate character definition"_s);
            }
            characters_.insert(character.id);
            anchor(character.anchor, path);
            for (const auto& variant : character.variants) {
                if (!validText(variant.name) || !validMoment(variant.validFrom) ||
                    (variant.validUntil && (!validMoment(*variant.validUntil) ||
                                            *variant.validUntil <= variant.validFrom))) {
                    error(path + u"/variants"_s, u"Invalid name variant or validity interval"_s);
                }
                anchor(variant.anchor, path + u"/variants"_s);
            }
        }
        QSet<QString> information;
        for (const auto& item : input_.information) {
            if (!validText(item.id) || !validText(item.description) ||
                information.contains(item.id)) {
                error(u"information"_s, u"Invalid or duplicate information definition"_s);
            }
            information.insert(item.id);
        }
        QSet<QString> locations;
        for (const auto& location : input_.locations) {
            if (!validText(location.id) || !validText(location.name) ||
                locations.contains(location.id)) {
                error(u"locations"_s, u"Invalid or duplicate location definition"_s);
            }
            locations.insert(location.id);
        }
        QMap<QString, QSet<core::CharacterMemoryId>> access;
        for (const auto& revelation : input_.revelations) {
            if (!information.contains(revelation.informationId) ||
                !characters_.contains(revelation.characterId) ||
                !validMoment(revelation.availableFrom) ||
                access[revelation.informationId].contains(revelation.characterId)) {
                error(u"revelations"_s,
                      u"Invalid or duplicate character-specific knowledge access"_s);
            }
            access[revelation.informationId].insert(revelation.characterId);
            anchor(revelation.anchor, u"revelations"_s);
        }
        QMap<QString, QSet<QString>> routes;
        for (const auto& constraint : input_.travelConstraints) {
            if (!locations.contains(constraint.from) || !locations.contains(constraint.to) ||
                constraint.from == constraint.to || constraint.minimumTicks < 0 ||
                routes[constraint.from].contains(constraint.to)) {
                error(u"travelConstraints"_s, u"Invalid or duplicate directed travel constraint"_s);
            }
            routes[constraint.from].insert(constraint.to);
            anchor(constraint.anchor, u"travelConstraints"_s);
        }
        QSet<QString> events;
        QHash<core::CharacterMemoryId, QSet<QString>> positions;
        for (const auto& event : input_.events) {
            const auto path = u"events/%1"_s.arg(event.id);
            bool validKind = true;
            switch (event.kind) {
            case IntegrityEventKind::Appearance:
            case IntegrityEventKind::Speech:
            case IntegrityEventKind::Death:
            case IntegrityEventKind::Revival:
            case IntegrityEventKind::Knowledge:
            case IntegrityEventKind::LocationPresence:
                break;
            default:
                validKind = false;
            }
            const auto position =
                QString::number(event.moment.tick) + u"/"_s + QString::number(event.moment.order);
            if (!validText(event.id) || events.contains(event.id) || !event.characterId.isValid() ||
                !validMoment(event.moment) || !validKind ||
                positions[event.characterId].contains(position) ||
                (event.usedName && !validText(*event.usedName)) ||
                (event.kind == IntegrityEventKind::Knowledge) != event.informationId.has_value() ||
                (event.kind == IntegrityEventKind::LocationPresence) !=
                    event.locationId.has_value() ||
                (event.informationId && !validText(*event.informationId)) ||
                (event.locationId && !validText(*event.locationId))) {
                error(path, u"Invalid event identity, chronology, kind or typed payload"_s);
            }
            positions[event.characterId].insert(position);
            events.insert(event.id);
            anchor(event.anchor, path);
        }
        QMap<QString, QSet<int>> exceptions;
        for (const auto& explanation : input_.explanations) {
            if (!events.contains(explanation.eventId) || !validText(explanation.reason) ||
                integrityRuleName(explanation.rule).isEmpty() ||
                exceptions[explanation.eventId].contains(static_cast<int>(explanation.rule)) ||
                explanation.anchor.support.basis != ClaimBasis::Evidence) {
                error(u"explanations"_s,
                      u"An exception requires a unique event/rule scope and direct evidence"_s);
            }
            exceptions[explanation.eventId].insert(static_cast<int>(explanation.rule));
            anchor(explanation.anchor, u"explanations"_s);
        }
        return errors_;
    }

  private:
    void error(QString path, QString message) {
        errors_.append({std::move(path), std::move(message)});
    }
    void anchor(const IntegrityAnchor& value, const QString& path) {
        const auto chapter = chapters_.value(value.chapterId, nullptr);
        const auto& support = value.support;
        if (!chapter ||
            (support.basis != ClaimBasis::Evidence && support.basis != ClaimBasis::Inference) ||
            !std::isfinite(support.confidence) || support.confidence < 0.0 ||
            support.confidence > 1.0 ||
            (support.basis == ClaimBasis::Evidence && support.evidence.isEmpty())) {
            error(path, u"Invalid claim support or chapter anchor"_s);
            return;
        }
        QList<core::SourceSpan> seen;
        for (const auto& evidence : support.evidence) {
            const auto& span = evidence.sourceSpan;
            if (!span.isValid() || span.sourceId != chapter->span.sourceId ||
                span.startByte < chapter->span.startByte || span.endByte > chapter->span.endByte ||
                seen.contains(span) || evidence.text.isEmpty()) {
                error(path, u"Invalid or duplicate evidence range"_s);
                continue;
            }
            const auto bytes =
                QByteArrayView(chapter->utf8)
                    .sliced(span.startByte - chapter->span.startByte, span.lengthBytes());
            if (QString::fromUtf8(bytes).toUtf8() != bytes || evidence.text.toUtf8() != bytes) {
                error(path, u"Evidence must match exact, complete UTF-8 source bytes"_s);
            }
            seen.append(span);
        }
    }
    const IntegrityInput& input_;
    QHash<core::ChapterId, const IntegrityChapter*> chapters_;
    QSet<core::CharacterMemoryId> characters_;
    QList<IntegrityInputError> errors_;
};

bool activeVariant(const IntegrityNameVariant& variant, const StoryMoment& moment) {
    return variant.validFrom <= moment && (!variant.validUntil || moment < *variant.validUntil);
}
} // namespace

QString integrityRuleName(IntegrityRule rule) {
    switch (rule) {
    case IntegrityRule::AppearanceBeforeExistence:
        return u"APPEARANCE_BEFORE_EXISTENCE"_s;
    case IntegrityRule::SpeechAfterDeath:
        return u"SPEECH_AFTER_DEATH"_s;
    case IntegrityRule::KnowledgeBeforeRevelation:
        return u"KNOWLEDGE_BEFORE_REVELATION"_s;
    case IntegrityRule::ImpossibleLocationTransition:
        return u"IMPOSSIBLE_LOCATION_TRANSITION"_s;
    case IntegrityRule::InvalidNameVariant:
        return u"INVALID_NAME_VARIANT"_s;
    case IntegrityRule::AmbiguousNameVariant:
        return u"AMBIGUOUS_NAME_VARIANT"_s;
    case IntegrityRule::UnresolvedCharacter:
        return u"UNRESOLVED_CHARACTER"_s;
    case IntegrityRule::UnresolvedInformation:
        return u"UNRESOLVED_INFORMATION"_s;
    case IntegrityRule::UnknownKnowledgeAccess:
        return u"UNKNOWN_KNOWLEDGE_ACCESS"_s;
    case IntegrityRule::UnresolvedLocation:
        return u"UNRESOLVED_LOCATION"_s;
    case IntegrityRule::UnknownTravelConstraint:
        return u"UNKNOWN_TRAVEL_CONSTRAINT"_s;
    }
    return {};
}

QString NarrativeIntegrityChecker::version() {
    return u"loreforge-narrative-integrity-v1"_s;
}

IntegrityReport NarrativeIntegrityChecker::check(const IntegrityInput& input) {
    IntegrityReport report;
    report.errors = Validator(input).validate();
    if (!report.errors.isEmpty()) {
        return report;
    }
    report.inputHash = inputHash(input);
    QHash<core::CharacterMemoryId, const IntegrityCharacter*> characters;
    for (const auto& character : input.characters) {
        characters.insert(character.id, &character);
    }
    QSet<QString> information;
    for (const auto& item : input.information) {
        information.insert(item.id);
    }
    QSet<QString> locations;
    for (const auto& item : input.locations) {
        locations.insert(item.id);
    }
    auto events = input.events;
    std::sort(events.begin(), events.end(), [](const auto& a, const auto& b) {
        return a.moment != b.moment ? a.moment < b.moment : a.id < b.id;
    });
    auto emitFinding = [&](const IntegrityEvent& event, IntegrityRule rule,
                           IntegritySeverity severity, QString message,
                           QList<IntegrityAnchor> supports) {
        supports.prepend(event.anchor);
        const auto identity =
            QJsonDocument(QJsonArray{version(), input.projectId.toString(), input.clockId, event.id,
                                     integrityRuleName(rule)})
                .toJson(QJsonDocument::Compact);
        IntegrityDiagnostic diagnostic{core::ContentHash::sha256(QByteArrayView(identity)).toHex(),
                                       rule,
                                       severity,
                                       event.id,
                                       event.anchor.chapterId,
                                       event.characterId,
                                       event.moment,
                                       std::move(message),
                                       ClaimBasis::Evidence,
                                       1.0,
                                       {}};
        for (const auto& anchor : supports) {
            diagnostic.confidence = std::min(diagnostic.confidence, anchor.support.confidence);
            if (anchor.support.basis == ClaimBasis::Inference) {
                diagnostic.basis = ClaimBasis::Inference;
            }
            for (const auto& evidence : anchor.support.evidence) {
                if (!diagnostic.evidence.contains(evidence)) {
                    diagnostic.evidence.append(evidence);
                }
            }
        }
        std::sort(diagnostic.evidence.begin(), diagnostic.evidence.end(),
                  [](const auto& a, const auto& b) {
                      if (a.sourceSpan.sourceId != b.sourceSpan.sourceId) {
                          return a.sourceSpan.sourceId < b.sourceSpan.sourceId;
                      }
                      if (a.sourceSpan.startByte != b.sourceSpan.startByte) {
                          return a.sourceSpan.startByte < b.sourceSpan.startByte;
                      }
                      return a.sourceSpan.endByte < b.sourceSpan.endByte;
                  });
        for (const auto& explanation : input.explanations) {
            if (explanation.eventId == event.id && explanation.rule == rule) {
                report.explained.append({diagnostic, explanation.reason, explanation.anchor});
                return;
            }
        }
        report.diagnostics.append(std::move(diagnostic));
    };
    QHash<core::CharacterMemoryId, IntegrityEvent> deaths;
    QHash<core::CharacterMemoryId, IntegrityEvent> lastLocations;
    for (const auto& event : events) {
        const auto* character = characters.value(event.characterId, nullptr);
        if (!character) {
            emitFinding(
                event, IntegrityRule::UnresolvedCharacter, IntegritySeverity::Notice,
                u"Character identity is not present in the supplied registry; do not merge by name."_s,
                {});
            continue;
        }
        if (event.moment < character->existsFrom) {
            emitFinding(event, IntegrityRule::AppearanceBeforeExistence, IntegritySeverity::Warning,
                        u"Character is referenced before its explicit existence boundary."_s,
                        {character->anchor});
        }
        if (event.usedName) {
            const auto key = nameKey(*event.usedName);
            bool valid = key == nameKey(character->canonicalName);
            QList<IntegrityAnchor> aliasSupport;
            for (const auto& variant : character->variants) {
                if (key == nameKey(variant.name) && activeVariant(variant, event.moment)) {
                    valid = true;
                    aliasSupport.append(variant.anchor);
                }
            }
            if (!valid) {
                emitFinding(
                    event, IntegrityRule::InvalidNameVariant, IntegritySeverity::Warning,
                    u"Name is not a canonical name or a valid variant at this story moment."_s,
                    {character->anchor});
            } else {
                QList<IntegrityAnchor> collisions;
                for (const auto& other : input.characters) {
                    if (other.id == character->id || event.moment < other.existsFrom) {
                        continue;
                    }
                    if (key == nameKey(other.canonicalName)) {
                        collisions.append(other.anchor);
                    }
                    for (const auto& variant : other.variants) {
                        if (key == nameKey(variant.name) && activeVariant(variant, event.moment)) {
                            collisions.append(variant.anchor);
                        }
                    }
                }
                if (!collisions.isEmpty()) {
                    collisions.append(character->anchor);
                    collisions.append(aliasSupport);
                    emitFinding(
                        event, IntegrityRule::AmbiguousNameVariant, IntegritySeverity::Notice,
                        u"Name also denotes another registered identity; explicit IDs remain distinct."_s,
                        collisions);
                }
            }
        }
        if (event.kind == IntegrityEventKind::Death) {
            deaths.insert(event.characterId, event);
        } else if (event.kind == IntegrityEventKind::Revival &&
                   event.anchor.support.basis == ClaimBasis::Evidence) {
            deaths.remove(event.characterId);
        } else if (event.kind == IntegrityEventKind::Speech && deaths.contains(event.characterId)) {
            emitFinding(
                event, IntegrityRule::SpeechAfterDeath, IntegritySeverity::Warning,
                u"Speech follows a recorded death without a recorded evidence-backed revival."_s,
                {deaths.value(event.characterId).anchor});
        }
        if (event.kind == IntegrityEventKind::Knowledge) {
            if (!information.contains(*event.informationId)) {
                emitFinding(event, IntegrityRule::UnresolvedInformation, IntegritySeverity::Notice,
                            u"Knowledge references information outside the supplied registry."_s,
                            {});
            } else {
                const IntegrityRevelation* access = nullptr;
                for (const auto& revelation : input.revelations) {
                    if (revelation.characterId == event.characterId &&
                        revelation.informationId == *event.informationId) {
                        access = &revelation;
                        break;
                    }
                }
                if (!access) {
                    emitFinding(
                        event, IntegrityRule::UnknownKnowledgeAccess, IntegritySeverity::Notice,
                        u"Character-specific knowledge access is unspecified; early knowledge is not proven."_s,
                        {});
                } else if (event.moment < access->availableFrom) {
                    emitFinding(
                        event, IntegrityRule::KnowledgeBeforeRevelation, IntegritySeverity::Warning,
                        u"Character uses information before its explicit knowledge-access boundary."_s,
                        {access->anchor});
                }
            }
        }
        if (event.kind == IntegrityEventKind::LocationPresence) {
            if (!locations.contains(*event.locationId)) {
                emitFinding(
                    event, IntegrityRule::UnresolvedLocation, IntegritySeverity::Notice,
                    u"Location is outside the supplied registry; travel cannot be checked."_s, {});
                lastLocations.remove(event.characterId);
                continue;
            }
            const auto previous = lastLocations.constFind(event.characterId);
            if (previous != lastLocations.cend() && previous->locationId != event.locationId) {
                const IntegrityTravelConstraint* constraint = nullptr;
                for (const auto& route : input.travelConstraints) {
                    if (route.from == *previous->locationId && route.to == *event.locationId) {
                        constraint = &route;
                        break;
                    }
                }
                if (!constraint) {
                    emitFinding(
                        event, IntegrityRule::UnknownTravelConstraint, IntegritySeverity::Notice,
                        u"No directed travel bound is supplied; an impossible transition is not proven."_s,
                        {previous->anchor});
                } else if (event.moment.tick - previous->moment.tick < constraint->minimumTicks) {
                    emitFinding(
                        event, IntegrityRule::ImpossibleLocationTransition,
                        IntegritySeverity::Warning,
                        u"Elapsed story time is below the explicit directed travel bound."_s,
                        {previous->anchor, constraint->anchor});
                }
            }
            lastLocations.insert(event.characterId, event);
        }
    }
    return report;
}

} // namespace loreforge::narrative
