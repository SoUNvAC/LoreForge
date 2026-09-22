#include "loreforge/context/relevant_memory_retriever.h"

#include <QHash>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace loreforge::context {
namespace {

QString normalized(QStringView text) {
    return text.toString().normalized(QString::NormalizationForm_C).toCaseFolded();
}

bool containsCjk(QStringView text) {
    const auto codePoints = text.toString().toUcs4();
    return std::any_of(codePoints.cbegin(), codePoints.cend(), [](char32_t codePoint) {
        return (codePoint >= 0x3400 && codePoint <= 0x4DBF) ||
               (codePoint >= 0x4E00 && codePoint <= 0x9FFF) ||
               (codePoint >= 0xF900 && codePoint <= 0xFAFF) ||
               (codePoint >= 0x20000 && codePoint <= 0x2EBEF) ||
               (codePoint >= 0x3040 && codePoint <= 0x30FF) ||
               (codePoint >= 0xAC00 && codePoint <= 0xD7AF);
    });
}

bool containsMeaningfulTerm(QStringView query, QStringView candidate) {
    const auto normalizedQuery = normalized(query);
    const auto normalizedCandidate = normalized(candidate);
    if ((normalizedCandidate.size() >= 3 || containsCjk(normalizedCandidate)) &&
        normalizedQuery.contains(normalizedCandidate)) {
        return true;
    }
    const auto terms = normalizedCandidate.split(
        QRegularExpression(QStringLiteral("[^\\p{L}\\p{N}_]+")), Qt::SkipEmptyParts);
    return std::any_of(terms.cbegin(), terms.cend(), [&normalizedQuery](const QString& term) {
        return (term.size() >= 4 || containsCjk(term)) && normalizedQuery.contains(term);
    });
}

int recency(qsizetype sequence) {
    return static_cast<int>(qBound(qsizetype{0}, sequence, qsizetype{9'999}));
}

} // namespace

QList<MemoryCandidate>
RelevantMemoryRetriever::rank(const narrative::StoryStateSnapshot& storyState,
                              QStringView currentChapter, QStringView taskInstructions) {
    const auto query = currentChapter.toString() + QLatin1Char('\n') + taskInstructions.toString();
    QHash<QString, QString> namesById;
    QSet<QString> referencedCharacters;
    QList<MemoryCandidate> candidates;

    for (const auto& character : storyState.characters) {
        namesById.insert(character.id.toString(), character.name);
        bool referenced = containsMeaningfulTerm(query, character.name);
        for (const auto& alias : character.aliases) {
            referenced = referenced || containsMeaningfulTerm(query, alias);
        }
        if (referenced) {
            referencedCharacters.insert(character.id.toString());
        }
        QString text = character.name;
        if (!character.aliases.isEmpty()) {
            text +=
                QStringLiteral(" | aliases: %1").arg(character.aliases.join(QStringLiteral(", ")));
        }
        text += QStringLiteral(" | seen in %1 chapter(s), last in chapter %2")
                    .arg(character.appearances.size())
                    .arg(character.lastSeenSequence + 1);
        candidates.append({MemoryKind::Character, character.id.toString(), std::move(text),
                           (referenced ? 500'000 : 100'000) + recency(character.lastSeenSequence)});
    }

    for (const auto& event : storyState.events) {
        QStringList participantNames;
        bool related = false;
        for (const auto& participantId : event.participants) {
            participantNames.append(
                namesById.value(participantId.toString(), participantId.toString()));
            related = related || referencedCharacters.contains(participantId.toString());
        }
        participantNames.append(event.unresolvedParticipants);
        related = related || containsMeaningfulTerm(query, event.description) ||
                  (event.location.has_value() && containsMeaningfulTerm(query, *event.location));
        QString text =
            QStringLiteral("Chapter %1: %2").arg(event.chapterSequence + 1).arg(event.description);
        if (!participantNames.isEmpty()) {
            text += QStringLiteral(" | participants: %1")
                        .arg(participantNames.join(QStringLiteral(", ")));
        }
        if (event.location.has_value()) {
            text += QStringLiteral(" | location: %1").arg(*event.location);
        }
        candidates.append({MemoryKind::Event, event.id.toString(), std::move(text),
                           (related ? 400'000 : 200'000) + recency(event.chapterSequence)});
    }

    for (const auto& thread : storyState.openThreads) {
        const bool related = containsMeaningfulTerm(query, thread.text);
        auto text = QStringLiteral("Open since chapter %1: %2")
                        .arg(thread.firstMentionSequence + 1)
                        .arg(thread.text);
        if (thread.inferredOnly) {
            text += QStringLiteral(" | inferred");
        }
        candidates.append({MemoryKind::OpenThread, thread.id.toString(), std::move(text),
                           (related ? 300'000 : 50'000) + recency(thread.lastMentionSequence)});
    }

    std::sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
        if (left.priority != right.priority) {
            return left.priority > right.priority;
        }
        if (left.kind != right.kind) {
            return left.kind < right.kind;
        }
        return left.stableId < right.stableId;
    });
    return candidates;
}

} // namespace loreforge::context
