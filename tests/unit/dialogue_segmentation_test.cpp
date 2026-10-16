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
        segments.append(
            QJsonObject{{QStringLiteral("first_unit"), unit.value(QStringLiteral("unit_id"))},
                        {QStringLiteral("last_unit"), unit.value(QStringLiteral("unit_id"))},
                        {QStringLiteral("kind"),
                         quote ? QStringLiteral("dialogue") : QStringLiteral("narration")},
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
    void rejectsMissingRepeatedInventedAndTamperedUnits() {
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
                first.insert(QStringLiteral("speaker"), QStringLiteral("母亲"));
            if (mode == 4) {
                first.insert(QStringLiteral("kind"), QStringLiteral("dialogue"));
                first.insert(QStringLiteral("speaker"), QStringLiteral("捏造人物"));
            }
            if (mode == 5)
                first.insert(QStringLiteral("last_unit"),
                             parts.last().toObject().value(QStringLiteral("last_unit")));
            if (mode == 6)
                first.insert(QStringLiteral("first_unit"), QStringLiteral("u2"));
            parts[0] = first;
            wire.insert(QStringLiteral("segments"), parts);
            QVERIFY2(!resolveDialogue(wire, snap).errors.isEmpty(),
                     qPrintable(QString::number(mode)));
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
        auto quote = parts[2].toObject();
        // Find an actual quotation rather than assuming an index for CRLF units.
        const auto units = snap.value(QStringLiteral("segmentation_units")).toArray();
        for (int i = 0; i < units.size(); ++i) {
            if (!units[i]
                     .toObject()
                     .value(QStringLiteral("text"))
                     .toString()
                     .startsWith(QChar(0x201c)))
                continue;
            quote = parts[i].toObject();
            quote.insert(QStringLiteral("kind"), QStringLiteral("unknown"));
            quote.insert(QStringLiteral("speaker"), QString{});
            parts[i] = quote;
            wire.insert(QStringLiteral("segments"), parts);
            QVERIFY(resolveDialogue(wire, snap).errors.isEmpty());
            quote.insert(QStringLiteral("kind"), QStringLiteral("narration"));
            parts[i] = quote;
            wire.insert(QStringLiteral("segments"), parts);
            QVERIFY(resolveDialogue(wire, snap).errors.isEmpty());
            break;
        }
        QVERIFY(dialoguePrompt().contains(QStringLiteral("呜……呜……")));
        QVERIFY(dialoguePrompt().contains(QStringLiteral("相同")));
    }
};
QTEST_APPLESS_MAIN(DialogueSegmentationTest)
#include "dialogue_segmentation_test.moc"
