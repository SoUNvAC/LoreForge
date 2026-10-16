#include "character_highlight_preview.h"

#include <QComboBox>
#include <QFile>
#include <QFontDatabase>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QSettings>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QtTest>

using namespace loreforge::app;
namespace {
const QString raw = QStringLiteral("# 家中😀\r\n\r\n母亲阻止父亲。\r\n\r\n苏雨晴退到门边。\r\n");
QJsonObject source() {
    return {{QStringLiteral("utf8"), raw},
            {QStringLiteral("start_byte"), 3},
            {QStringLiteral("end_byte"), 3 + raw.toUtf8().size()}};
}
QJsonArray evidence(const QString& quote) {
    const auto start = raw.toUtf8().indexOf(quote.toUtf8());
    return {QJsonObject{{QStringLiteral("source_start"), 3 + start},
                        {QStringLiteral("source_end"), 3 + start + quote.toUtf8().size()}}};
}
QJsonObject analysis(bool reverse = false) {
    const QJsonObject mother{
        {QStringLiteral("name"), QStringLiteral("母亲")},
        {QStringLiteral("evidence"), evidence(QStringLiteral("母亲阻止父亲。"))}};
    const QJsonObject father{
        {QStringLiteral("name"), QStringLiteral("父亲")},
        {QStringLiteral("evidence"), evidence(QStringLiteral("母亲阻止父亲。"))}};
    const QJsonObject girl{
        {QStringLiteral("name"), QStringLiteral("苏雨晴")},
        {QStringLiteral("evidence"), evidence(QStringLiteral("苏雨晴退到门边。"))}};
    return {{QStringLiteral("characters"),
             reverse ? QJsonArray{girl, father, mother} : QJsonArray{mother, father, girl}},
            {QStringLiteral("events"), QJsonArray{}}};
}
} // namespace

class CharacterHighlightPreviewTest final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        const auto id =
            QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
        const auto families = QFontDatabase::applicationFontFamilies(id);
        if (!families.isEmpty())
            QApplication::setFont(QFont(families.first(), 11));
    }
    void mapsUtf8AndOverlappingEvidence();
    void restoresEditableStylesAndBookBindings();
    void clearsStaleDataAndRejectsBadBoundaries();
    void paintsAndChangesOpacityAndWidth();
};

void CharacterHighlightPreviewTest::mapsUtf8AndOverlappingEvidence() {
    QTemporaryDir directory;
    CharacterHighlightPreview panel(nullptr, directory.filePath(QStringLiteral("style.ini")));
    panel.showAnalysis(QStringLiteral("book-a"), source(), analysis(), QStringLiteral("候选结果"));
    auto* text = panel.findChild<CharacterHighlightText*>();
    QCOMPARE(text->spans().size(), 2);
    const auto plain = text->toPlainText();
    QCOMPARE(text->spans()[0].start, plain.indexOf(QStringLiteral("母亲阻止父亲。")));
    QCOMPARE(text->spans()[0].end - text->spans()[0].start,
             QStringLiteral("母亲阻止父亲。").size());
    QCOMPARE(text->spans()[0].people.size(), 2);
    QVERIFY(text->spans()[0].people.contains(QStringLiteral("母亲")));
    QVERIFY(text->spans()[0].people.contains(QStringLiteral("父亲")));
    QCOMPARE(panel.characterColor(QStringLiteral("母亲")).name().toUpper(),
             QStringLiteral("#4A2718"));
    QCOMPARE(panel.characterColor(QStringLiteral("父亲")).name().toUpper(),
             QStringLiteral("#592219"));
    QVERIFY(text->isReadOnly());
    QVERIFY(panel.findChild<QLabel*>(QStringLiteral("highlightPreviewStatus"))
                ->text()
                .contains(QStringLiteral("候选")));
    auto* filter = panel.findChild<QComboBox*>(QStringLiteral("highlightCharacterFilter"));
    QCOMPARE(filter->count(), 4);
    filter->setCurrentIndex(1);
    QCOMPARE(filter->currentData().toString(), QStringLiteral("母亲"));
}

void CharacterHighlightPreviewTest::restoresEditableStylesAndBookBindings() {
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("style.ini"));
    {
        CharacterHighlightPreview panel(nullptr, path);
        panel.showAnalysis(QStringLiteral("book-a"), source(), analysis(), QStringLiteral("候选"));
        auto* palette = panel.findChild<QLineEdit*>(QStringLiteral("highlightPalette0"));
        palette->setText(QStringLiteral("#123456"));
        QVERIFY(QMetaObject::invokeMethod(palette, "editingFinished", Qt::DirectConnection));
        QCOMPARE(panel.characterColor(QStringLiteral("母亲")), QColor(QStringLiteral("#123456")));
        auto* person = panel.findChild<QLineEdit*>(QStringLiteral("highlightPersonColor1"));
        person->setText(QStringLiteral("#ABCDEF"));
        QVERIFY(QMetaObject::invokeMethod(person, "editingFinished", Qt::DirectConnection));
        panel.findChild<QSpinBox*>(QStringLiteral("highlightOpacity"))->setValue(42);
        panel.findChild<QSpinBox*>(QStringLiteral("highlightStrokeWidth"))->setValue(23);
        palette->setText(QStringLiteral("invalid"));
        QVERIFY(QMetaObject::invokeMethod(palette, "editingFinished", Qt::DirectConnection));
        QCOMPARE(panel.characterColor(QStringLiteral("母亲")), QColor(QStringLiteral("#123456")));
    }
    CharacterHighlightPreview restored(nullptr, path);
    restored.showAnalysis(QStringLiteral("book-a"), source(), analysis(true),
                          QStringLiteral("候选"));
    QCOMPARE(restored.characterColor(QStringLiteral("母亲")), QColor(QStringLiteral("#123456")));
    QCOMPARE(restored.characterColor(QStringLiteral("父亲")), QColor(QStringLiteral("#ABCDEF")));
    QCOMPARE(restored.findChild<QSpinBox*>(QStringLiteral("highlightOpacity"))->value(), 42);
    QCOMPARE(restored.findChild<QSpinBox*>(QStringLiteral("highlightStrokeWidth"))->value(), 23);
    restored.showAnalysis(QStringLiteral("book-b"), source(), analysis(), QStringLiteral("候选"));
    QCOMPARE(restored.characterColor(QStringLiteral("父亲")), QColor(QStringLiteral("#592219")));
    QFile settingsFile(path);
    QVERIFY(settingsFile.open(QIODevice::ReadOnly));
    const auto saved = settingsFile.readAll();
    QVERIFY(!saved.contains(raw.toUtf8()));
    QVERIFY(!saved.contains(QStringLiteral("母亲").toUtf8()));
}

void CharacterHighlightPreviewTest::clearsStaleDataAndRejectsBadBoundaries() {
    QTemporaryDir directory;
    CharacterHighlightPreview panel(nullptr, directory.filePath(QStringLiteral("style.ini")));
    panel.showAnalysis(QStringLiteral("book-a"), source(), analysis(), QStringLiteral("候选"));
    auto* text = panel.findChild<CharacterHighlightText*>();
    QVERIFY(!text->spans().isEmpty());
    panel.showImportedText(QStringLiteral("book-b"), QStringLiteral("下一章"));
    QVERIFY(text->spans().isEmpty());
    QCOMPARE(text->toPlainText(), QStringLiteral("下一章"));
    auto output = analysis();
    auto people = output.value(QStringLiteral("characters")).toArray();
    auto mother = people[0].toObject();
    auto refs = mother.value(QStringLiteral("evidence")).toArray();
    auto ref = refs[0].toObject();
    ref.insert(QStringLiteral("source_start"),
               ref.value(QStringLiteral("source_start")).toInteger() + 1);
    refs[0] = ref;
    mother.insert(QStringLiteral("evidence"), refs);
    people = {mother};
    output.insert(QStringLiteral("characters"), people);
    panel.showAnalysis(QStringLiteral("book-a"), source(), output, QStringLiteral("候选"));
    QVERIFY(text->spans().isEmpty());
}

void CharacterHighlightPreviewTest::paintsAndChangesOpacityAndWidth() {
    QTemporaryDir directory;
    CharacterHighlightPreview panel(nullptr, directory.filePath(QStringLiteral("style.ini")));
    panel.resize(600, 800);
    panel.showAnalysis(QStringLiteral("book-a"), source(), analysis(),
                       QStringLiteral("候选：仅显示人物相关证据"));
    panel.show();
    QTest::qWait(30);
    auto* text = panel.findChild<CharacterHighlightText*>();
    auto* opacity = panel.findChild<QSpinBox*>(QStringLiteral("highlightOpacity"));
    auto* width = panel.findChild<QSpinBox*>(QStringLiteral("highlightStrokeWidth"));
    opacity->setValue(0);
    const auto plain = text->viewport()->grab().toImage();
    opacity->setValue(70);
    width->setValue(20);
    const auto highlighted = text->viewport()->grab().toImage();
    QVERIFY(plain != highlighted);
    panel.findChild<QComboBox*>(QStringLiteral("highlightCharacterFilter"))->setCurrentIndex(1);
    QVERIFY(text->viewport()->grab().toImage() != highlighted);
    panel.findChild<QComboBox*>(QStringLiteral("highlightCharacterFilter"))->setCurrentIndex(0);
    width->setValue(3);
    QVERIFY(text->viewport()->grab().toImage() != highlighted);
    if (const auto file = qEnvironmentVariable("LOREFORGE_HIGHLIGHT_PREVIEW_IMAGE");
        !file.isEmpty()) {
        const auto id =
            QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
        const auto families = QFontDatabase::applicationFontFamilies(id);
        if (!families.isEmpty())
            panel.setFont(QFont(families.first(), 11));
        opacity->setValue(35);
        width->setValue(14);
        QTest::qWait(30);
        QVERIFY(panel.grab().save(file));
    }
}

QTEST_MAIN(CharacterHighlightPreviewTest)
#include "character_highlight_preview_test.moc"
