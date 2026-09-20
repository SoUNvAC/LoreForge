#include "loreforge/narrative/story_memory.h"
#include "loreforge/narrative/story_memory_json.h"

#include <QJsonDocument>
#include <QtTest>

#include <algorithm>

using namespace Qt::StringLiterals;

class StoryMemoryTest final : public QObject {
    Q_OBJECT

  private slots:
    void rebuildsDeterministicallyFromOrderedChapterRecords();
    void rejectsGapsAndUngroundedClaims();
    void roundTripsPersistedChapterAnalysis();
};

namespace {

loreforge::narrative::ClaimSupport directSupport(QString sourceId, qint64 start, QString text) {
    const auto bytes = text.toUtf8();
    return {loreforge::narrative::ClaimBasis::Evidence,
            {{{std::move(sourceId), start, start + bytes.size()}, std::move(text)}},
            0.95};
}

loreforge::narrative::ClaimSupport inferredSupport() {
    return {loreforge::narrative::ClaimBasis::Inference, {}, 0.7};
}

loreforge::narrative::ChapterMemoryRecord chapterRecord(const loreforge::core::ProjectId& projectId,
                                                        qsizetype sequence) {
    const auto first = sequence == 0;
    const auto chapterId = loreforge::core::ChapterId::fromStableKey(first ? u"memory:chapter:0"_s
                                                                           : u"memory:chapter:1"_s);
    const auto sourceId = first ? u"memory/chapter-0.txt"_s : u"memory/chapter-1.txt"_s;
    const qint64 start = first ? 0 : 100;
    const auto source = first ? u"Mara met Ivo."_s : u"Mara and Ivo opened the gate."_s;

    loreforge::narrative::ChapterAnalysis analysis;
    analysis.chapterId = chapterId;
    analysis.sourceSpan = {sourceId, start, start + source.toUtf8().size()};
    analysis.characters = {
        {u"Mara"_s,
         first ? QList<loreforge::narrative::CharacterAlias>{{u"She"_s, inferredSupport()}}
               : QList<loreforge::narrative::CharacterAlias>{},
         directSupport(sourceId, start, u"Mara"_s)},
        {u"Ivo"_s, {}, directSupport(sourceId, start + (first ? 9 : 9), u"Ivo"_s)},
    };
    analysis.locations = first ? QList<loreforge::narrative::ChapterLocation>{}
                               : QList<loreforge::narrative::ChapterLocation>{
                                     {u"gate"_s, directSupport(sourceId, 124, u"gate"_s)}};
    analysis.events = {
        {first ? u"Mara meets Ivo."_s : u"Mara and Ivo open the gate."_s,
         {u"Mara"_s, u"Ivo"_s},
         first ? std::optional<QString>{} : std::optional<QString>{u"gate"_s},
         directSupport(sourceId, start, source)},
    };
    analysis.summary = {first ? u"Mara meets Ivo."_s : u"They open a gate."_s, inferredSupport()};
    analysis.openThreads = {
        {u"Why did Ivo come?"_s,
         first ? inferredSupport() : directSupport(sourceId, 109, u"Ivo"_s)},
    };
    return {projectId, sequence, std::move(analysis)};
}

bool containsError(const loreforge::narrative::StoryStateResult& result,
                   loreforge::narrative::StoryMemoryErrorCode code) {
    return std::any_of(result.errors.cbegin(), result.errors.cend(),
                       [code](const auto& error) { return error.code == code; });
}

} // namespace

void StoryMemoryTest::rebuildsDeterministicallyFromOrderedChapterRecords() {
    const auto projectId = loreforge::core::ProjectId::fromStableKey(u"memory-project"_s);
    const auto first = chapterRecord(projectId, 0);
    const auto second = chapterRecord(projectId, 1);
    const auto rebuilt =
        loreforge::narrative::StoryStateRebuilder::rebuild(projectId, {first, second});
    QVERIFY2(rebuilt.isValid(),
             qPrintable(rebuilt.errors.isEmpty() ? QString{} : rebuilt.errors.first().message));

    const auto shuffled =
        loreforge::narrative::StoryStateRebuilder::rebuild(projectId, {second, first});
    QVERIFY(shuffled.isValid());
    QCOMPARE(*rebuilt.snapshot, *shuffled.snapshot);

    const auto& snapshot = *rebuilt.snapshot;
    QCOMPARE(snapshot.throughChapterSequence, 1);
    QCOMPARE(snapshot.sourceChapters.size(), 2);
    QCOMPARE(snapshot.characters.size(), 2);
    const auto mara =
        std::find_if(snapshot.characters.cbegin(), snapshot.characters.cend(),
                     [](const auto& character) { return character.name == u"Mara"_s; });
    QVERIFY(mara != snapshot.characters.cend());
    QCOMPARE(mara->aliases, QStringList{u"She"_s});
    QCOMPARE(mara->appearances.size(), 2);
    QCOMPARE(snapshot.events.size(), 2);
    QCOMPARE(snapshot.timeline.size(), 2);
    QCOMPARE(snapshot.relationships.size(), 1);
    QCOMPARE(snapshot.relationships.first().sharedEvents.size(), 2);
    QCOMPARE(snapshot.openThreads.size(), 1);
    QCOMPARE(snapshot.openThreads.first().mentions.size(), 2);
    QVERIFY(!snapshot.openThreads.first().inferredOnly);
    QVERIFY(snapshot.id.isValid());
    QVERIFY(snapshot.sourceHash.isValid());
    QVERIFY(snapshot.stateHash.isValid());
}

void StoryMemoryTest::rejectsGapsAndUngroundedClaims() {
    const auto projectId = loreforge::core::ProjectId::fromStableKey(u"memory-invalid"_s);
    auto gap = chapterRecord(projectId, 1);
    const auto gapResult = loreforge::narrative::StoryStateRebuilder::rebuild(projectId, {gap});
    QVERIFY(!gapResult.isValid());
    QVERIFY(containsError(gapResult, loreforge::narrative::StoryMemoryErrorCode::InvalidSequence));

    auto ungrounded = chapterRecord(projectId, 0);
    ungrounded.analysis.summary.support = {loreforge::narrative::ClaimBasis::Evidence, {}, 0.8};
    const auto ungroundedResult =
        loreforge::narrative::StoryStateRebuilder::rebuild(projectId, {ungrounded});
    QVERIFY(!ungroundedResult.isValid());
    QVERIFY(
        containsError(ungroundedResult, loreforge::narrative::StoryMemoryErrorCode::InvalidClaim));
}

void StoryMemoryTest::roundTripsPersistedChapterAnalysis() {
    const auto projectId = loreforge::core::ProjectId::fromStableKey(u"memory-codec"_s);
    const auto original = chapterRecord(projectId, 0).analysis;
    const auto encoded = loreforge::narrative::encodeChapterAnalysis(original);
    const auto decoded = loreforge::narrative::decodeChapterAnalysis(encoded);
    QVERIFY2(decoded.isValid(), qPrintable(decoded.error));
    QCOMPARE(*decoded.analysis, original);
    QCOMPARE(loreforge::narrative::encodeChapterAnalysis(*decoded.analysis), encoded);

    const auto malformed = loreforge::narrative::decodeChapterAnalysis("{\"chapter_id\":42}");
    QVERIFY(!malformed.isValid());
}

QTEST_GUILESS_MAIN(StoryMemoryTest)

#include "story_memory_test.moc"
