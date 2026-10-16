#include "dialogue_segmentation.h"
#include "loreforge/inference/output_validator.h"
#include <QtTest>
using namespace loreforge::app;
namespace {
QJsonObject snapshot() {
    const auto text = QStringLiteral("# "
                                     "门边😀\r\n\r\n“这是什么药？”母亲的眼神变得严肃。\r\n“……”小晴"
                                     "低下头。\r\n“呜……呜……”\r\n“……”\r\n“我……我不……”\r\n");
    const QJsonObject source{{QStringLiteral("utf8"), text},
                             {QStringLiteral("start_byte"), 3},
                             {QStringLiteral("end_byte"), 3 + text.toUtf8().size()}};
    return {{QStringLiteral("chapter_id"), QStringLiteral("chapter-test")},
            {QStringLiteral("schema_version"), chapterWireSchemaVersion},
            {QStringLiteral("task_kind"), QStringLiteral("dialogue")},
            {QStringLiteral("source"), source},
            {QStringLiteral("segmentation_units"), dialogueUnits(source)}};
}
QJsonObject output(const QJsonObject& snap) {
    QJsonArray segments;
    for (const auto& value : snap.value(QStringLiteral("segmentation_units")).toArray()) {
        const auto unit = value.toObject();
        const auto text = unit.value(QStringLiteral("text")).toString();
        const auto quote = text.startsWith(QChar(0x201c));
        if (!quote)
            continue;
        segments.append(
            QJsonObject{{QStringLiteral("first_unit"), unit.value(QStringLiteral("unit_id"))},
                        {QStringLiteral("last_unit"), unit.value(QStringLiteral("unit_id"))},
                        {QStringLiteral("speaker"),
                         quote ? (text.contains(QStringLiteral("药")) ? QStringLiteral("母亲")
                                                                      : QStringLiteral("苏雨晴"))
                               : QString{}}});
    }
    return {
        {QStringLiteral("chapter_id"), snap.value(QStringLiteral("chapter_id"))},
        {QStringLiteral("characters"),
         QJsonArray{QJsonObject{{QStringLiteral("name"), QStringLiteral("母亲")},
                                {QStringLiteral("aliases"), QJsonArray{}}},
                    QJsonObject{{QStringLiteral("name"), QStringLiteral("苏雨晴")},
                                {QStringLiteral("aliases"), QJsonArray{QStringLiteral("小晴")}}}}},
        {QStringLiteral("segments"), segments}};
}
} // namespace
class DialogueSegmentationTest : public QObject {
    Q_OBJECT
  private slots:
    void skipsNarrationAndTrailingPunctuationWithoutLosingSource() {
        auto snap = snapshot();
        auto source = snap.value(QStringLiteral("source")).toObject();
        const auto text = source.value(QStringLiteral("utf8")).toString() + QStringLiteral("……");
        source.insert(QStringLiteral("utf8"), text);
        source.insert(QStringLiteral("end_byte"), 3 + text.toUtf8().size());
        snap.insert(QStringLiteral("source"), source);
        snap.insert(QStringLiteral("segmentation_units"), dialogueUnits(source));
        auto wire = output(snap);
        auto parts = wire.value(QStringLiteral("segments")).toArray();
        QJsonArray reversed;
        for (int i = parts.size() - 1; i >= 0; --i)
            reversed.append(parts[i]);
        wire.insert(QStringLiteral("segments"), reversed);
        const auto mapped = resolveDialogue(wire, snap);
        QVERIFY(mapped.errors.isEmpty());
        QVERIFY(mapped.output.value(QStringLiteral("extraction_issues")).toArray().isEmpty());
        const auto normalized = mapped.output.value(QStringLiteral("segments")).toArray();
        QCOMPARE(normalized.last().toObject().value(QStringLiteral("kind")).toString(),
                 QStringLiteral("unassigned"));
        QVERIFY(normalized.last()
                    .toObject()
                    .value(QStringLiteral("text"))
                    .toString()
                    .endsWith(QStringLiteral("……")));
        int turns = 0;
        QString restored;
        for (const auto& value : normalized) {
            const auto part = value.toObject();
            restored += part.value(QStringLiteral("text")).toString();
            if (part.value(QStringLiteral("kind")).toString() == QStringLiteral("dialogue"))
                ++turns;
        }
        QCOMPARE(turns, 5);
        QCOMPARE(restored, text);
    }
    void restoresExactTextAndSeparateSpeakers() {
        const auto snap = snapshot();
        const auto wire = output(snap);
        QVERIFY(loreforge::inference::OutputValidator::validate(dialogueSchema(), wire).isValid());
        const auto result = resolveDialogue(wire, snap);
        QVERIFY2(result.errors.isEmpty(), qPrintable(result.errors.join('\n')));
        QString restored;
        QList<qint64> repeated;
        int mother = 0, girl = 0;
        qint64 previous = 3;
        for (const auto& value : result.output.value(QStringLiteral("segments")).toArray()) {
            const auto part = value.toObject();
            QCOMPARE(part.value(QStringLiteral("source_start")).toInteger(), previous);
            previous = part.value(QStringLiteral("source_end")).toInteger();
            const auto text = part.value(QStringLiteral("text")).toString();
            restored += text;
            if (part.value(QStringLiteral("speaker")).toString() == QStringLiteral("母亲")) {
                ++mother;
                QCOMPARE(text, QStringLiteral("“这是什么药？”"));
            }
            if (part.value(QStringLiteral("speaker")).toString() == QStringLiteral("苏雨晴"))
                ++girl;
            if (text == QStringLiteral("“……”"))
                repeated.append(part.value(QStringLiteral("source_start")).toInteger());
        }
        QCOMPARE(restored, snap.value(QStringLiteral("source"))
                               .toObject()
                               .value(QStringLiteral("utf8"))
                               .toString());
        QCOMPARE(mother, 1);
        QCOMPARE(girl, 4);
        QCOMPARE(repeated.size(), 2);
        QVERIFY(repeated[0] != repeated[1]);
    }
    void retainsGoodEntriesAndReportsBadEntries() {
        const auto snap = snapshot();
        const auto valid = output(snap);
        for (int mode = 0; mode < 7; ++mode) {
            auto wire = valid;
            auto parts = wire.value(QStringLiteral("segments")).toArray();
            auto first = parts[0].toObject();
            if (mode == 0)
                parts.removeLast();
            if (mode == 1)
                parts.append(parts.last());
            if (mode == 2)
                first.insert(QStringLiteral("last_unit"), QStringLiteral("u9999"));
            if (mode == 3)
                first.insert(QStringLiteral("first_unit"), QStringLiteral("u9999"));
            if (mode == 4) {
                first.insert(QStringLiteral("speaker"), QStringLiteral("捏造人物"));
            }
            if (mode == 5)
                first.insert(QStringLiteral("last_unit"),
                             parts.last().toObject().value(QStringLiteral("last_unit")));
            if (mode == 6)
                first.insert(QStringLiteral("first_unit"), QStringLiteral("u2"));
            parts[0] = first;
            wire.insert(QStringLiteral("segments"), parts);
            const auto mapped = resolveDialogue(wire, snap);
            QVERIFY2(mapped.errors.isEmpty(), qPrintable(mapped.errors.join('\n')));
            if (mode == 0)
                QVERIFY(
                    mapped.output.value(QStringLiteral("extraction_issues")).toArray().isEmpty());
            else
                QVERIFY2(
                    !mapped.output.value(QStringLiteral("extraction_issues")).toArray().isEmpty(),
                    qPrintable(QString::number(mode)));
            QString restored;
            for (const auto& part : mapped.output.value(QStringLiteral("segments")).toArray())
                restored += part.toObject().value(QStringLiteral("text")).toString();
            QCOMPARE(restored, snap.value(QStringLiteral("source"))
                                   .toObject()
                                   .value(QStringLiteral("utf8"))
                                   .toString());
        }
        auto corrupt = snap;
        auto units = corrupt.value(QStringLiteral("segmentation_units")).toArray();
        auto unit = units[0].toObject();
        unit.insert(QStringLiteral("start_byte"), 4);
        units[0] = unit;
        corrupt.insert(QStringLiteral("segmentation_units"), units);
        QVERIFY(!resolveDialogue(valid, corrupt).errors.isEmpty());
    }
    void preservesUnknownAndQuotedTerms() {
        const auto snap = snapshot();
        auto wire = output(snap);
        auto parts = wire.value(QStringLiteral("segments")).toArray();
        auto quote = parts[0].toObject();
        quote.insert(QStringLiteral("speaker"), QString{});
        parts[0] = quote;
        wire.insert(QStringLiteral("segments"), parts);
        const auto unknown = resolveDialogue(wire, snap);
        QVERIFY(unknown.errors.isEmpty());
        int pending = 0;
        for (const auto& value : unknown.output.value(QStringLiteral("segments")).toArray())
            if (value.toObject().value(QStringLiteral("kind")).toString() ==
                    QStringLiteral("dialogue") &&
                value.toObject().value(QStringLiteral("speaker")).toString().isEmpty())
                ++pending;
        QCOMPARE(pending, 1);
        wire.insert(QStringLiteral("segments"), QJsonArray{});
        const auto empty = resolveDialogue(wire, snap);
        QVERIFY(empty.errors.isEmpty());
        QCOMPARE(empty.output.value(QStringLiteral("segments")).toArray().size(), 1);
        QCOMPARE(empty.output.value(QStringLiteral("segments"))
                     .toArray()[0]
                     .toObject()
                     .value(QStringLiteral("kind"))
                     .toString(),
                 QStringLiteral("unassigned"));
        QVERIFY(dialoguePrompt().contains(QStringLiteral("呜……呜……")));
        QVERIFY(dialoguePrompt().contains(QStringLiteral("相同")));
    }
};
QTEST_APPLESS_MAIN(DialogueSegmentationTest)
#include "dialogue_segmentation_test.moc"
