#include "loreforge/git/semantic_diff.h"

#include "loreforge/narrative/story_memory.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSet>

#include <algorithm>
#include <vector>

namespace loreforge::git {
namespace {

bool boundary(const QByteArray& bytes, qint64 offset) {
    return offset >= 0 && offset <= bytes.size() &&
           (offset == bytes.size() ||
            (static_cast<unsigned char>(bytes.at(offset)) & 0xc0U) != 0x80U);
}

bool validSpan(const core::SourceSpan& span, const SourceRevision& source) {
    return span.isValid() && span.sourceId == source.sourceId &&
           boundary(source.utf8, span.startByte) && boundary(source.utf8, span.endByte);
}

QList<narrative::ClaimSupport> supports(const narrative::ChapterAnalysis& analysis) {
    QList<narrative::ClaimSupport> result{analysis.summary.support};
    for (const auto& character : analysis.characters) {
        result.append(character.support);
        for (const auto& alias : character.aliases) {
            result.append(alias.support);
        }
    }
    for (const auto& location : analysis.locations) {
        result.append(location.support);
    }
    for (const auto& event : analysis.events) {
        result.append(event.support);
    }
    for (const auto& fact : analysis.importantFacts) {
        result.append(fact.support);
    }
    for (const auto& thread : analysis.openThreads) {
        result.append(thread.support);
    }
    return result;
}

std::optional<GitError> validateSource(const SourceRevision& source) {
    if (source.sourceId.trimmed().isEmpty() || !source.sourceHash.isValid() ||
        core::ContentHash::sha256(QByteArrayView(source.utf8)) != source.sourceHash ||
        QString::fromUtf8(source.utf8).toUtf8() != source.utf8) {
        return GitError{
            GitErrorCode::SourceChanged,
            QStringLiteral("Semantic diff requires hash-verified UTF-8 source revisions."),
            {source.sourceId}};
    }
    QSet<QString> ids;
    QSet<qsizetype> sequences;
    qint64 previousEnd = 0;
    qsizetype previousSequence = -1;
    for (const auto& chapter : source.chapters) {
        if (!chapter.id.isValid() || chapter.sequence < 0 || ids.contains(chapter.id.toString()) ||
            sequences.contains(chapter.sequence) || !validSpan(chapter.sourceSpan, source) ||
            chapter.sourceSpan.startByte < previousEnd || chapter.sequence <= previousSequence) {
            return GitError{
                GitErrorCode::InvalidAuthorization,
                QStringLiteral(
                    "Chapter projections must be ordered, unique, and inside the source."),
                {source.sourceId}};
        }
        ids.insert(chapter.id.toString());
        sequences.insert(chapter.sequence);
        previousEnd = chapter.sourceSpan.endByte;
        previousSequence = chapter.sequence;
        if (!chapter.analysis) {
            continue;
        }
        const auto& analysis = *chapter.analysis;
        const narrative::ChapterMemoryRecord record{
            core::ProjectId::fromStableKey(QStringLiteral("semantic-diff-validation")),
            chapter.sequence, analysis};
        if (analysis.chapterId != chapter.id || analysis.sourceSpan != chapter.sourceSpan ||
            !narrative::StoryStateRebuilder::validateRecord(record).isEmpty()) {
            return GitError{
                GitErrorCode::InvalidAuthorization,
                QStringLiteral("Chapter analysis does not match its revision projection."),
                {source.sourceId}};
        }
        for (const auto& support : supports(analysis)) {
            for (const auto& evidence : support.evidence) {
                const auto& span = evidence.sourceSpan;
                if (!validSpan(span, source) || span.startByte < chapter.sourceSpan.startByte ||
                    span.endByte > chapter.sourceSpan.endByte ||
                    source.utf8.sliced(span.startByte, span.lengthBytes()) !=
                        evidence.text.toUtf8()) {
                    return GitError{
                        GitErrorCode::SourceChanged,
                        QStringLiteral("Analysis evidence no longer matches revision bytes."),
                        {source.sourceId}};
                }
            }
        }
    }
    return std::nullopt;
}

RevisionImpact combine(RevisionImpact first, RevisionImpact second) {
    if (first == RevisionImpact::SemanticChange || second == RevisionImpact::SemanticChange) {
        return RevisionImpact::SemanticChange;
    }
    if (first == RevisionImpact::Unknown || second == RevisionImpact::Unknown) {
        return RevisionImpact::Unknown;
    }
    if (first == RevisionImpact::TextOnly || second == RevisionImpact::TextOnly) {
        return RevisionImpact::TextOnly;
    }
    if (first == RevisionImpact::TypoOnly || second == RevisionImpact::TypoOnly) {
        return RevisionImpact::TypoOnly;
    }
    return RevisionImpact::NoChange;
}

bool semanticRisk(RevisionImpact impact) {
    return impact == RevisionImpact::Unknown || impact == RevisionImpact::SemanticChange;
}

RevisionImpact reviewImpact(const proofreading::RepairQueueItem& item) {
    using namespace proofreading;
    const auto& candidate = item.candidate;
    if (candidate.category == CandidateCategory::NameInconsistency ||
        candidate.category == CandidateCategory::TerminologyInconsistency) {
        return RevisionImpact::SemanticChange;
    }
    switch (candidate.semanticImpact) {
    case SemanticImpact::EntityChange:
    case SemanticImpact::DialogueChange:
    case SemanticImpact::ActionChange:
    case SemanticImpact::TimelineChange:
    case SemanticImpact::StoryStateChange:
        return RevisionImpact::SemanticChange;
    case SemanticImpact::Unknown:
        return RevisionImpact::Unknown;
    case SemanticImpact::TextOnly:
    case SemanticImpact::PunctuationOnly:
        if (candidate.category == CandidateCategory::Ambiguous ||
            candidate.category == CandidateCategory::PossibleSourceDamage) {
            return RevisionImpact::Unknown;
        }
        return candidate.category == CandidateCategory::Typo &&
                       candidate.semanticImpact == SemanticImpact::TextOnly
                   ? RevisionImpact::TypoOnly
                   : RevisionImpact::TextOnly;
    }
    return RevisionImpact::Unknown;
}

ChangedSourceSpan changedSpan(const SourceRevision& before, const SourceRevision& after,
                              qint64 oldStart, qint64 oldEnd, qint64 newStart, qint64 newEnd) {
    // Trim common bytes, then retreat to complete UTF-8 code points on both sides.
    qint64 prefix = 0;
    while (oldStart + prefix < oldEnd && newStart + prefix < newEnd &&
           before.utf8.at(oldStart + prefix) == after.utf8.at(newStart + prefix)) {
        ++prefix;
    }
    while (prefix > 0 && (!boundary(before.utf8, oldStart + prefix) ||
                          !boundary(after.utf8, newStart + prefix))) {
        --prefix;
    }
    oldStart += prefix;
    newStart += prefix;
    qint64 suffix = 0;
    while (oldEnd - suffix > oldStart && newEnd - suffix > newStart &&
           before.utf8.at(oldEnd - suffix - 1) == after.utf8.at(newEnd - suffix - 1)) {
        ++suffix;
    }
    while (suffix > 0 &&
           (!boundary(before.utf8, oldEnd - suffix) || !boundary(after.utf8, newEnd - suffix))) {
        --suffix;
    }
    oldEnd -= suffix;
    newEnd -= suffix;
    return {{before.sourceId, oldStart, oldEnd},
            {after.sourceId, newStart, newEnd},
            before.utf8.sliced(oldStart, oldEnd - oldStart),
            after.utf8.sliced(newStart, newEnd - newStart),
            RevisionImpact::Unknown,
            std::nullopt};
}

struct Line final {
    QByteArray bytes;
    qint64 start = 0;
};

QList<Line> lines(const QByteArray& bytes) {
    QList<Line> result;
    qint64 start = 0;
    while (start < bytes.size()) {
        const auto newline = bytes.indexOf('\n', start);
        const auto end = newline < 0 ? bytes.size() : newline + 1;
        result.append({bytes.sliced(start, end - start), start});
        start = end;
    }
    return result;
}

QList<ChangedSourceSpan> rawChanges(const SourceRevision& before, const SourceRevision& after,
                                    bool& fallback) {
    if (before.utf8 == after.utf8) {
        return {};
    }
    const auto oldLines = lines(before.utf8);
    const auto newLines = lines(after.utf8);
    const auto n = oldLines.size();
    const auto m = newLines.size();
    constexpr qsizetype maxCells = 1'000'000;
    if (n + 1 > maxCells / (m + 1)) {
        fallback = true;
        return {changedSpan(before, after, 0, before.utf8.size(), 0, after.utf8.size())};
    }
    std::vector<qsizetype> matrix(static_cast<size_t>((n + 1) * (m + 1)), 0);
    auto cell = [&](qsizetype i, qsizetype j) -> qsizetype& {
        return matrix[static_cast<size_t>(i * (m + 1) + j)];
    };
    for (auto i = n; i-- > 0;) {
        for (auto j = m; j-- > 0;) {
            cell(i, j) = oldLines[i].bytes == newLines[j].bytes
                             ? cell(i + 1, j + 1) + 1
                             : std::max(cell(i + 1, j), cell(i, j + 1));
        }
    }
    auto offset = [](const QList<Line>& list, qsizetype index, qsizetype size) {
        return index == list.size() ? static_cast<qint64>(size) : list[index].start;
    };
    QList<ChangedSourceSpan> result;
    qsizetype i = 0;
    qsizetype j = 0;
    while (i < n || j < m) {
        if (i < n && j < m && oldLines[i].bytes == newLines[j].bytes) {
            ++i;
            ++j;
            continue;
        }
        const auto oldStart = i;
        const auto newStart = j;
        while (i < n || j < m) {
            if (i < n && j < m && oldLines[i].bytes == newLines[j].bytes) {
                break;
            }
            if (j < m && (i == n || cell(i, j + 1) >= cell(i + 1, j))) {
                ++j;
            } else {
                ++i;
            }
        }
        result.append(changedSpan(before, after, offset(oldLines, oldStart, before.utf8.size()),
                                  offset(oldLines, i, before.utf8.size()),
                                  offset(newLines, newStart, after.utf8.size()),
                                  offset(newLines, j, after.utf8.size())));
    }
    return result;
}

bool touches(const core::SourceSpan& change, const core::SourceSpan& chapter) {
    if (change.sourceId != chapter.sourceId) {
        return false;
    }
    if (change.lengthBytes() == 0) {
        return change.startByte >= chapter.startByte && change.startByte <= chapter.endByte;
    }
    return change.overlaps(chapter);
}

QStringList entityNames(const narrative::ChapterAnalysis& analysis) {
    QStringList result;
    for (const auto& character : analysis.characters) {
        result.append(character.name);
        for (const auto& alias : character.aliases) {
            result.append(alias.name);
        }
    }
    for (const auto& location : analysis.locations) {
        result.append(location.name);
    }
    result.removeDuplicates();
    std::sort(result.begin(), result.end());
    return result;
}

QByteArray semanticSignature(const narrative::ChapterAnalysis& analysis) {
    auto sortedArray = [](QStringList values) {
        std::sort(values.begin(), values.end());
        return QJsonArray::fromStringList(values);
    };
    QStringList characters;
    for (const auto& character : analysis.characters) {
        QStringList aliases;
        for (const auto& alias : character.aliases) {
            aliases.append(alias.name);
        }
        characters.append(QString::fromUtf8(
            QJsonDocument(QJsonObject{{QStringLiteral("name"), character.name},
                                      {QStringLiteral("aliases"), sortedArray(aliases)}})
                .toJson(QJsonDocument::Compact)));
    }
    QJsonArray events;
    for (const auto& event : analysis.events) {
        events.append(
            QJsonObject{{QStringLiteral("description"), event.description},
                        {QStringLiteral("participants"), sortedArray(event.participants)},
                        {QStringLiteral("location"), event.location.value_or(QString{})}});
    }
    QStringList facts;
    QStringList threads;
    for (const auto& fact : analysis.importantFacts) {
        facts.append(fact.text);
    }
    for (const auto& thread : analysis.openThreads) {
        threads.append(thread.text);
    }
    return QJsonDocument(
               QJsonObject{{QStringLiteral("characters"), sortedArray(characters)},
                           {QStringLiteral("entities"), sortedArray(entityNames(analysis))},
                           {QStringLiteral("events"), events},
                           {QStringLiteral("summary"), analysis.summary.text},
                           {QStringLiteral("facts"), sortedArray(facts)},
                           {QStringLiteral("threads"), sortedArray(threads)}})
        .toJson(QJsonDocument::Compact);
}

void appendUnique(QList<core::ChapterId>& list, const core::ChapterId& id) {
    if (!list.contains(id)) {
        list.append(id);
    }
}

void dirtyFrom(AnalysisInvalidationPlan& plan, qsizetype sequence) {
    if (!plan.storyStateSnapshotsDirtyFrom || sequence < *plan.storyStateSnapshotsDirtyFrom) {
        plan.storyStateSnapshotsDirtyFrom = sequence;
    }
}

std::optional<qint64> mappedOffset(qint64 offset, const QList<ChangedSourceSpan>& changes,
                                   bool includeInsertion = false) {
    qint64 delta = 0;
    for (const auto& change : changes) {
        if (includeInsertion && offset == change.before.startByte &&
            change.before.lengthBytes() == 0) {
            return offset + delta + change.after.lengthBytes();
        }
        if (offset <= change.before.startByte) {
            return offset + delta;
        }
        if (offset < change.before.endByte) {
            return std::nullopt;
        }
        delta += change.after.lengthBytes() - change.before.lengthBytes();
    }
    return offset + delta;
}

bool mappedBoundary(qint64 oldOffset, qint64 newOffset, const QList<ChangedSourceSpan>& changes) {
    return mappedOffset(oldOffset, changes) == std::optional<qint64>(newOffset) ||
           mappedOffset(oldOffset, changes, true) == std::optional<qint64>(newOffset);
}

bool namedEvidenceChanged(const narrative::ChapterAnalysis& analysis,
                          const QList<ChangedSourceSpan>& changes, const SourceRevision& before,
                          const SourceRevision& after) {
    QList<ChangedSourceSpan> localizedChanges;
    for (const auto& change : changes) {
        localizedChanges.append(changedSpan(before, after, change.before.startByte,
                                            change.before.endByte, change.after.startByte,
                                            change.after.endByte));
    }
    auto changed = [&](const QString& name, const narrative::ClaimSupport& support) {
        for (const auto& evidence : support.evidence) {
            if (evidence.text != name) {
                continue;
            }
            const auto start = mappedOffset(evidence.sourceSpan.startByte, localizedChanges);
            const auto end = mappedOffset(evidence.sourceSpan.endByte, localizedChanges, true);
            if (!start || !end || *end < *start || *end > after.utf8.size() ||
                after.utf8.sliced(*start, *end - *start) != name.toUtf8()) {
                return true;
            }
        }
        return false;
    };
    for (const auto& character : analysis.characters) {
        if (changed(character.name, character.support)) {
            return true;
        }
        for (const auto& alias : character.aliases) {
            if (changed(alias.name, alias.support)) {
                return true;
            }
        }
    }
    for (const auto& location : analysis.locations) {
        if (changed(location.name, location.support)) {
            return true;
        }
    }
    return false;
}

} // namespace

QString revisionImpactName(RevisionImpact impact) {
    switch (impact) {
    case RevisionImpact::NoChange:
        return QStringLiteral("NO_CHANGE");
    case RevisionImpact::TypoOnly:
        return QStringLiteral("TYPO_ONLY");
    case RevisionImpact::TextOnly:
        return QStringLiteral("TEXT_ONLY");
    case RevisionImpact::SemanticChange:
        return QStringLiteral("SEMANTIC_CHANGE");
    case RevisionImpact::Unknown:
        return QStringLiteral("UNKNOWN");
    }
    return {};
}

GitResult<SemanticDiffReport>
SemanticDiffer::compare(const SourceRevision& before, const SourceRevision& after,
                        const QList<proofreading::RepairQueueItem>& reviews) {
    if (const auto error = validateSource(before)) {
        return *error;
    }
    if (const auto error = validateSource(after)) {
        return *error;
    }
    if (before.sourceId != after.sourceId) {
        return GitError{GitErrorCode::InvalidPath,
                        QStringLiteral("Semantic diff compares revisions of one source identity."),
                        {}};
    }
    SemanticDiffReport report;
    report.beforeHash = before.sourceHash;
    report.afterHash = after.sourceHash;
    auto orderedReviews = reviews;
    std::sort(orderedReviews.begin(), orderedReviews.end(), [](const auto& a, const auto& b) {
        return a.candidate.sourceSpan.startByte < b.candidate.sourceSpan.startByte;
    });
    QByteArray reviewedAfter = before.utf8;
    QList<ChangedSourceSpan> reviewedChanges;
    QSet<QString> candidateIds;
    qint64 previousEnd = 0;
    qint64 delta = 0;
    for (const auto& review : orderedReviews) {
        const auto authorization = proofreading::RepairGate::authorizeApprovedCandidate(review);
        const auto& candidate = review.candidate;
        const auto& span = candidate.sourceSpan;
        const auto chapter =
            std::find_if(before.chapters.cbegin(), before.chapters.cend(),
                         [&](const auto& value) { return value.id == candidate.chapterId; });
        if (std::holds_alternative<proofreading::RepairQueueError>(authorization) ||
            !review.projectId.isValid() || !candidate.id.isValid() ||
            candidateIds.contains(candidate.id.toString()) ||
            candidate.sourceHash != before.sourceHash || !validSpan(span, before) ||
            span.startByte < previousEnd || candidate.originalText.isEmpty() ||
            candidate.originalText == review.currentSuggestion ||
            review.currentSuggestion.isEmpty() ||
            before.utf8.sliced(span.startByte, span.lengthBytes()) !=
                candidate.originalText.toUtf8() ||
            chapter == before.chapters.cend() || span.startByte < chapter->sourceSpan.startByte ||
            span.endByte > chapter->sourceSpan.endByte) {
            return GitError{GitErrorCode::InvalidAuthorization,
                            QStringLiteral("Review provenance does not match the before revision."),
                            {before.sourceId}};
        }
        candidateIds.insert(candidate.id.toString());
        const auto replacement = review.currentSuggestion.toUtf8();
        const auto afterStart = span.startByte + delta;
        reviewedAfter.replace(afterStart, span.lengthBytes(), replacement);
        reviewedChanges.append({span,
                                {after.sourceId, afterStart, afterStart + replacement.size()},
                                candidate.originalText.toUtf8(),
                                replacement,
                                reviewImpact(review),
                                candidate.id});
        delta += replacement.size() - span.lengthBytes();
        previousEnd = span.endByte;
    }
    if (!reviews.isEmpty() && reviewedAfter == after.utf8) {
        report.changedSpans = reviewedChanges;
    } else {
        report.changedSpans = rawChanges(before, after, report.conservativeSpanFallback);
    }
    for (const auto& change : report.changedSpans) {
        report.impact = combine(report.impact, change.impact);
    }

    QMap<QString, const RevisionChapter*> oldChapters;
    QMap<QString, const RevisionChapter*> newChapters;
    for (const auto& chapter : before.chapters) {
        oldChapters.insert(chapter.id.toString(), &chapter);
    }
    for (const auto& chapter : after.chapters) {
        newChapters.insert(chapter.id.toString(), &chapter);
    }
    QStringList ids;
    for (const auto& chapter : before.chapters) {
        ids.append(chapter.id.toString());
    }
    for (const auto& chapter : after.chapters) {
        const auto id = chapter.id.toString();
        if (!ids.contains(id)) {
            ids.append(id);
        }
    }
    for (const auto& id : ids) {
        const auto* oldChapter = oldChapters.value(id, nullptr);
        const auto* newChapter = newChapters.value(id, nullptr);
        const bool identityChanged =
            !oldChapter || !newChapter || oldChapter->sequence != newChapter->sequence;
        const bool layoutUncertain =
            !identityChanged &&
            (!mappedBoundary(oldChapter->sourceSpan.startByte, newChapter->sourceSpan.startByte,
                             report.changedSpans) ||
             !mappedBoundary(oldChapter->sourceSpan.endByte, newChapter->sourceSpan.endByte,
                             report.changedSpans));
        const bool structureChanged = identityChanged || layoutUncertain;
        const bool provenanceChanged =
            structureChanged || oldChapter->sourceSpan != newChapter->sourceSpan;
        const bool textChanged =
            structureChanged || before.utf8.sliced(oldChapter->sourceSpan.startByte,
                                                   oldChapter->sourceSpan.lengthBytes()) !=
                                    after.utf8.sliced(newChapter->sourceSpan.startByte,
                                                      newChapter->sourceSpan.lengthBytes());
        bool knownSemanticChange =
            oldChapter && newChapter && oldChapter->analysis && newChapter->analysis &&
            semanticSignature(*oldChapter->analysis) != semanticSignature(*newChapter->analysis);
        if (oldChapter && oldChapter->analysis &&
            namedEvidenceChanged(*oldChapter->analysis, report.changedSpans, before, after)) {
            knownSemanticChange = true;
        }
        if (!textChanged && !provenanceChanged && !knownSemanticChange) {
            continue;
        }
        const auto& chapter = newChapter ? *newChapter : *oldChapter;
        const auto sequence = oldChapter && newChapter
                                  ? std::min(oldChapter->sequence, newChapter->sequence)
                                  : chapter.sequence;
        appendUnique(report.invalidation.analysisChapters, chapter.id);
        appendUnique(report.invalidation.provenanceChapters, chapter.id);
        dirtyFrom(report.invalidation, sequence);
        report.invalidation.requiresChapterRemap |= structureChanged;
        if (!textChanged && !knownSemanticChange) {
            continue;
        }
        auto impact = identityChanged || knownSemanticChange ? RevisionImpact::SemanticChange
                      : layoutUncertain                      ? RevisionImpact::Unknown
                                                             : RevisionImpact::NoChange;
        for (const auto& change : report.changedSpans) {
            if ((oldChapter && touches(change.before, oldChapter->sourceSpan)) ||
                (newChapter && touches(change.after, newChapter->sourceSpan))) {
                impact = combine(impact, change.impact);
            }
        }
        if (impact == RevisionImpact::NoChange) {
            impact = RevisionImpact::Unknown;
        }
        ChapterRevisionImpact chapterImpact{chapter.id, sequence, impact,
                                            {},         false,    semanticRisk(impact)};
        if (chapterImpact.storySemanticsMayChange) {
            chapterImpact.entityImpactUncertain =
                !oldChapter || !newChapter || !oldChapter->analysis || !newChapter->analysis;
            if (oldChapter && oldChapter->analysis) {
                chapterImpact.affectedEntities += entityNames(*oldChapter->analysis);
            }
            if (newChapter && newChapter->analysis) {
                chapterImpact.affectedEntities += entityNames(*newChapter->analysis);
            }
            chapterImpact.affectedEntities.removeDuplicates();
            std::sort(chapterImpact.affectedEntities.begin(), chapterImpact.affectedEntities.end());
        }
        report.changedChapters.append(chapterImpact);
        report.impact = combine(report.impact, impact);
        report.invalidation.storySemanticsMayChange |= chapterImpact.storySemanticsMayChange;
    }
    // Unmapped edits (e.g. chapter headings or preamble) can alter chapter segmentation.
    for (const auto& change : report.changedSpans) {
        auto covered = [](const SourceRevision& source, const core::SourceSpan& span) {
            return std::any_of(source.chapters.cbegin(), source.chapters.cend(),
                               [&](const auto& chapter) {
                                   return span.startByte >= chapter.sourceSpan.startByte &&
                                          span.endByte <= chapter.sourceSpan.endByte;
                               });
        };
        if (!covered(before, change.before) || !covered(after, change.after)) {
            report.invalidation.requiresChapterRemap = true;
            report.invalidation.storySemanticsMayChange = true;
            qsizetype firstSequence = 0;
            if (!before.chapters.isEmpty()) {
                firstSequence = before.chapters.first().sequence;
            }
            if (!after.chapters.isEmpty()) {
                firstSequence = before.chapters.isEmpty()
                                    ? after.chapters.first().sequence
                                    : std::min(firstSequence, after.chapters.first().sequence);
            }
            dirtyFrom(report.invalidation, firstSequence);
            report.impact = combine(report.impact, RevisionImpact::Unknown);
        }
    }
    if (report.invalidation.requiresChapterRemap) {
        for (const auto& chapter : before.chapters) {
            appendUnique(report.invalidation.analysisChapters, chapter.id);
            appendUnique(report.invalidation.provenanceChapters, chapter.id);
            dirtyFrom(report.invalidation, chapter.sequence);
        }
        for (const auto& chapter : after.chapters) {
            appendUnique(report.invalidation.analysisChapters, chapter.id);
            appendUnique(report.invalidation.provenanceChapters, chapter.id);
            dirtyFrom(report.invalidation, chapter.sequence);
        }
    }
    std::sort(report.changedChapters.begin(), report.changedChapters.end(),
              [](const auto& a, const auto& b) {
                  return a.sequence == b.sequence ? a.chapterId.toString() < b.chapterId.toString()
                                                  : a.sequence < b.sequence;
              });
    return report;
}

} // namespace loreforge::git
