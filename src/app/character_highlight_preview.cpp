#include "character_highlight_preview.h"

#include <QColorDialog>
#include <QComboBox>
#include <QCryptographicHash>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QRegularExpressionValidator>
#include <QSet>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTextBlock>
#include <QTextLayout>
#include <QToolTip>
#include <QVBoxLayout>

namespace loreforge::app {
namespace {
const QStringList initialColors{QStringLiteral("#4A2718"), QStringLiteral("#592219"),
                                QStringLiteral("#7A4E47"), QStringLiteral("#CDB0A1"),
                                QStringLiteral("#B98E61"), QStringLiteral("#C7B89C"),
                                QStringLiteral("#DDDDCF"), QStringLiteral("#DBCEC3")};
const QString narrationKey = QChar(0x1f) + QStringLiteral("叙述 / 动作 / 心理");
const QString unknownKey = QChar(0x1f) + QStringLiteral("待确认（类型或说话人不明）");
QString displayName(QString key) {
    if (key.startsWith(QChar(0x1f)))
        key.remove(0, 1);
    return key;
}
QString hashed(const QString& value) {
    return QString::fromLatin1(
        QCryptographicHash::hash(value.toUtf8(), QCryptographicHash::Sha256).toHex());
}
bool validHex(const QString& text) {
    static const QRegularExpression expression(QStringLiteral("^#[0-9A-Fa-f]{6}$"));
    return expression.match(text).hasMatch();
}
void decorate(QLineEdit* editor, QColor color) {
    editor->setText(color.name().toUpper());
    editor->setStyleSheet({});
    auto palette = editor->palette();
    palette.setColor(QPalette::Base, color);
    palette.setColor(QPalette::Text, color.lightness() < 128 ? Qt::white : Qt::black);
    editor->setPalette(palette);
    editor->setToolTip(QStringLiteral("输入 #RRGGBB 修改颜色。"));
}
} // namespace

CharacterHighlightText::CharacterHighlightText(QWidget* parent) : QPlainTextEdit(parent) {
    setObjectName(QStringLiteral("segmentationPreviewText"));
    setReadOnly(true);
    setMouseTracking(true);
    auto paper = palette();
    paper.setColor(QPalette::Base, QColor(Qt::white));
    paper.setColor(QPalette::Text, QColor(Qt::black));
    setPalette(paper);
    setPlaceholderText(tr("选择章节后显示原文；完成新版分析后按正文分割结果染色。"));
}
void CharacterHighlightText::setHighlights(QList<CharacterHighlightSpan> spans,
                                           QMap<QString, QColor> colors, QString filter,
                                           int opacity, int width) {
    spans_ = std::move(spans);
    colors_ = std::move(colors);
    filter_ = std::move(filter);
    opacity_ = opacity;
    width_ = width;
    viewport()->update();
}
void CharacterHighlightText::paintEvent(QPaintEvent* event) {
    QPlainTextEdit::paintEvent(event);
    if (opacity_ == 0 || spans_.isEmpty())
        return;
    QPainter painter(viewport());
    painter.setClipRect(event->rect());
    // Multiply preserves black glyphs on the white paper, like a physical highlighter.
    painter.setCompositionMode(QPainter::CompositionMode_Multiply);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    auto block = firstVisibleBlock();
    while (block.isValid()) {
        const auto geometry = blockBoundingGeometry(block).translated(contentOffset());
        if (geometry.top() > viewport()->height())
            break;
        auto* layout = block.layout();
        if (block.isVisible() && geometry.bottom() >= 0 && layout) {
            for (int index = 0; index < layout->lineCount(); ++index) {
                const auto line = layout->lineAt(index);
                const auto first = block.position() + line.textStart();
                const auto last = first + line.textLength();
                for (const auto& span : spans_) {
                    const auto start = qMax(first, span.start);
                    const auto end = qMin(last, span.end);
                    if (start >= end || (!filter_.isEmpty() && !span.people.contains(filter_)))
                        continue;
                    const auto names = filter_.isEmpty() ? span.people : QStringList{filter_};
                    const auto x1 = line.cursorToX(start - block.position());
                    const auto x2 = line.cursorToX(end - block.position());
                    const auto height = static_cast<qreal>(width_) / names.size();
                    const auto top =
                        geometry.top() + line.y() + line.height() * 0.58 - width_ / 2.0;
                    for (int stripe = 0; stripe < names.size(); ++stripe) {
                        auto color = colors_.value(names[stripe]);
                        if (!color.isValid())
                            continue;
                        color.setAlphaF(opacity_ / 100.0);
                        painter.setBrush(color);
                        painter.drawRoundedRect(QRectF(geometry.left() + qMin(x1, x2),
                                                       top + stripe * height, qAbs(x2 - x1),
                                                       height),
                                                2, qMin<qreal>(2, height / 2));
                    }
                }
            }
        }
        block = block.next();
    }
}
void CharacterHighlightText::mouseMoveEvent(QMouseEvent* event) {
    QPlainTextEdit::mouseMoveEvent(event);
    const auto position = cursorForPosition(event->pos()).position();
    for (const auto& span : spans_) {
        if (position >= span.start && position < span.end &&
            (filter_.isEmpty() || span.people.contains(filter_))) {
            QToolTip::showText(event->globalPosition().toPoint(),
                               QStringLiteral("<qt>分割归属：%1</qt>")
                                   .arg(displayName(span.people.value(0)).toHtmlEscaped()),
                               this);
            return;
        }
    }
    QToolTip::hideText();
}

CharacterHighlightPreview::CharacterHighlightPreview(QWidget* parent, QString settingsFile)
    : QWidget(parent) {
    setObjectName(QStringLiteral("characterHighlightPreview"));
    if (settingsFile.isEmpty())
        settingsFile = qEnvironmentVariable("LOREFORGE_HIGHLIGHT_SETTINGS");
    if (settingsFile.isEmpty())
        settingsFile = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) +
                       QStringLiteral("/highlight-preview.ini");
    settings_ = std::make_unique<QSettings>(settingsFile, QSettings::IniFormat);
    const auto saved = settings_->value(QStringLiteral("palette")).toStringList();
    for (int index = 0; index < initialColors.size(); ++index)
        palette_.append(QColor(
            index < saved.size() && validHex(saved[index]) ? saved[index] : initialColors[index]));
    auto* layout = new QVBoxLayout(this);
    auto* title = new QLabel(tr("分割预览"), this);
    title->setObjectName(QStringLiteral("segmentationPreviewTitle"));
    auto font = title->font();
    font.setBold(true);
    font.setPointSize(font.pointSize() + 2);
    title->setFont(font);
    layout->addWidget(title);
    auto* meaning = new QLabel(tr("完整正文按分割结果显示：人物颜色仅用于其对白；灰色为叙述、动作或"
                                  "心理，琥珀色为待确认。鼠标悬停查看归属。"),
                               this);
    meaning->setWordWrap(true);
    layout->addWidget(meaning);
    auto* options = new QGroupBox(tr("荧光笔设置（点击展开）"), this);
    options->setObjectName(QStringLiteral("highlightOptions"));
    options->setCheckable(true);
    options->setChecked(false);
    auto* optionsLayout = new QVBoxLayout(options);
    auto* body = new QWidget(options);
    optionsLayout->addWidget(body);
    auto* settingsLayout = new QVBoxLayout(body);
    auto* grid = new QGridLayout;
    for (int index = 0; index < palette_.size(); ++index) {
        auto* editor = new QLineEdit(body);
        editor->setObjectName(QStringLiteral("highlightPalette%1").arg(index));
        editor->setMaximumWidth(110);
        editor->setValidator(new QRegularExpressionValidator(
            QRegularExpression(QStringLiteral("#[0-9A-Fa-f]{6}")), editor));
        decorate(editor, palette_[index]);
        paletteEditors_.append(editor);
        grid->addWidget(editor, index / 4, index % 4);
        connect(editor, &QLineEdit::editingFinished, this, [this, editor, index] {
            updateColor(editor, [this, index](QColor color) {
                palette_[index] = color;
                saveStyle();
                rebuildLegend();
            });
        });
    }
    settingsLayout->addLayout(grid);
    auto* form = new QFormLayout;
    opacity_ = new QSpinBox(body);
    opacity_->setObjectName(QStringLiteral("highlightOpacity"));
    opacity_->setRange(0, 100);
    opacity_->setSuffix(QStringLiteral(" %"));
    opacity_->setValue(qBound(0, settings_->value(QStringLiteral("opacity"), 25).toInt(), 100));
    width_ = new QSpinBox(body);
    width_->setObjectName(QStringLiteral("highlightStrokeWidth"));
    width_->setRange(1, 40);
    width_->setSuffix(QStringLiteral(" px"));
    width_->setValue(qBound(1, settings_->value(QStringLiteral("width"), 14).toInt(), 40));
    form->addRow(tr("不透明度"), opacity_);
    form->addRow(tr("笔触粗细"), width_);
    settingsLayout->addLayout(form);
    body->hide();
    connect(options, &QGroupBox::toggled, body, &QWidget::setVisible);
    connect(opacity_, &QSpinBox::valueChanged, this, [this] {
        saveStyle();
        refreshHighlights();
    });
    connect(width_, &QSpinBox::valueChanged, this, [this] {
        saveStyle();
        refreshHighlights();
    });
    layout->addWidget(options);
    filter_ = new QComboBox(this);
    filter_->setObjectName(QStringLiteral("highlightCharacterFilter"));
    filter_->addItem(tr("全部分割"), QString{});
    connect(filter_, &QComboBox::currentIndexChanged, this, [this] { refreshHighlights(); });
    layout->addWidget(filter_);
    legend_ = new QTableWidget(0, 2, this);
    legend_->setObjectName(QStringLiteral("highlightCharacterLegend"));
    legend_->setHorizontalHeaderLabels({tr("说话人 / 类型 · 分段数"), tr("颜色（可编辑）")});
    legend_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    legend_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    legend_->verticalHeader()->hide();
    legend_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    legend_->setToolTip(
        tr("点击人物名筛选，双击人物名打开选色器；也可直接编辑右侧 #RRGGBB 色值。"));
    legend_->setMaximumHeight(150);
    legend_->setMinimumHeight(70);
    connect(legend_, &QTableWidget::cellClicked, this, [this](int row, int column) {
        if (column == 0 && row >= 0 && row < people_.size())
            filter_->setCurrentIndex(row + 1);
    });
    connect(legend_, &QTableWidget::cellDoubleClicked, this, [this](int row, int column) {
        if (column != 0 || row < 0 || row >= people_.size())
            return;
        const auto name = people_[row];
        const auto color = QColorDialog::getColor(characterColor(name), this, tr("人物颜色"));
        if (!color.isValid())
            return;
        overrides_[name] = color;
        settings_->setValue(bindingKey(name) + QStringLiteral("/color"), color.name());
        saveStyle();
        rebuildLegend();
    });
    layout->addWidget(legend_);
    status_ = new QLabel(tr("尚无分析结果。"), this);
    status_->setObjectName(QStringLiteral("highlightPreviewStatus"));
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    layout->addWidget(status_);
    text_ = new CharacterHighlightText(this);
    layout->addWidget(text_, 1);
    storageNote_ = new QLabel(tr("配色保存在本机，人物绑定按书籍记忆；不修改工程或小说。超过 8 "
                                 "人会循环使用配色，可单独调整。"),
                              this);
    storageNote_->setObjectName(QStringLiteral("highlightStorageNote"));
    storageNote_->setWordWrap(true);
    layout->addWidget(storageNote_);
}
CharacterHighlightPreview::~CharacterHighlightPreview() = default;
QString CharacterHighlightPreview::bindingKey(const QString& name) const {
    return QStringLiteral("bindings/") + hashed(bookId_) + QLatin1Char('/') + hashed(name);
}
QColor CharacterHighlightPreview::characterColor(const QString& name) const {
    if (!overrides_.contains(name) && name == narrationKey)
        return QColor(QStringLiteral("#B0B0B0"));
    if (!overrides_.contains(name) && name == unknownKey)
        return QColor(QStringLiteral("#E2A63B"));
    return overrides_.contains(name) ? overrides_.value(name) : palette_[slots_.value(name, 0)];
}
void CharacterHighlightPreview::saveStyle() {
    QStringList colors;
    for (const auto& color : palette_)
        colors.append(color.name().toUpper());
    settings_->setValue(QStringLiteral("palette"), colors);
    settings_->setValue(QStringLiteral("opacity"), opacity_->value());
    settings_->setValue(QStringLiteral("width"), width_->value());
    settings_->sync();
    if (settings_->status() != QSettings::NoError)
        storageNote_->setText(tr("配色保存失败，本次会话仍可使用；请检查本机配置目录权限。"));
}
void CharacterHighlightPreview::updateColor(QLineEdit* editor,
                                            const std::function<void(QColor)>& apply) {
    if (!validHex(editor->text())) {
        editor->setStyleSheet(QStringLiteral("QLineEdit { border: 1px solid #d32f2f; }"));
        editor->setToolTip(tr("请输入完整的 #RRGGBB 颜色。"));
        return;
    }
    const QColor color(editor->text());
    decorate(editor, color);
    apply(color);
    refreshHighlights();
}
void CharacterHighlightPreview::rebuildLegend() {
    const auto filter = filter_->currentData().toString();
    const QSignalBlocker blocker(filter_);
    filter_->clear();
    filter_->addItem(tr("全部分割"), QString{});
    legend_->setRowCount(0);
    for (int row = 0; row < people_.size(); ++row) {
        const auto name = people_[row];
        filter_->addItem(displayName(name), name);
        legend_->insertRow(row);
        legend_->setItem(
            row, 0,
            new QTableWidgetItem(tr("%1 · %2 段").arg(displayName(name)).arg(counts_.value(name))));
        auto* color = new QLineEdit(legend_);
        color->setObjectName(QStringLiteral("highlightPersonColor%1").arg(row));
        color->setMaximumWidth(115);
        color->setValidator(new QRegularExpressionValidator(
            QRegularExpression(QStringLiteral("#[0-9A-Fa-f]{6}")), color));
        decorate(color, characterColor(name));
        connect(color, &QLineEdit::editingFinished, this, [this, color, name] {
            updateColor(color, [this, name](QColor chosen) {
                overrides_[name] = chosen;
                settings_->setValue(bindingKey(name) + QStringLiteral("/color"), chosen.name());
                saveStyle();
            });
        });
        legend_->setCellWidget(row, 1, color);
    }
    const auto selected = filter_->findData(filter);
    filter_->setCurrentIndex(selected >= 0 ? selected : 0);
    legend_->setFixedHeight(qBound(55,
                                   legend_->horizontalHeader()->height() +
                                       qMin(people_.size(), qsizetype(4)) *
                                           legend_->verticalHeader()->defaultSectionSize() +
                                       2,
                                   150));
    refreshHighlights();
}
void CharacterHighlightPreview::refreshHighlights() {
    QMap<QString, QColor> colors;
    for (const auto& name : people_)
        colors.insert(name, characterColor(name));
    text_->setHighlights(spans_, colors, filter_->currentData().toString(), opacity_->value(),
                         width_->value());
}
void CharacterHighlightPreview::showImportedText(QString bookId, QString text) {
    bookId_ = std::move(bookId);
    people_.clear();
    counts_.clear();
    overrides_.clear();
    slots_.clear();
    spans_.clear();
    text_->setPlainText(std::move(text));
    status_->setText(tr("尚无正文分割结果；当前仅显示未分割原文。请运行新版单章分析。"));
    rebuildLegend();
}
void CharacterHighlightPreview::showAnalysis(QString bookId, const QJsonObject& source,
                                             const QJsonObject& analysis, QString status) {
    showImportedText(std::move(bookId), source.value(QStringLiteral("utf8")).toString());
    const auto bytes = source.value(QStringLiteral("utf8")).toString().toUtf8();
    const auto base = source.value(QStringLiteral("start_byte")).toInteger(-1);
    const auto end = source.value(QStringLiteral("end_byte")).toInteger(-1);
    if (base < 0 || end < base || end - base != bytes.size()) {
        status_->setText(tr("原文快照范围无效，未染色。"));
        return;
    }
    const auto segments = analysis.value(QStringLiteral("segments")).toArray();
    if (segments.isEmpty()) {
        status_->setText(status + QLatin1Char('\n') +
                         tr("所选任务没有可显示的正文分割结果（旧版分析、本轮尚未完成或校验未通过）"
                            "。当前仅显示未分割原文，不会用人物证据替代。"));
        return;
    }
    qint64 covered = base;
    for (const auto& item : segments) {
        const auto part = item.toObject();
        const auto start = part.value(QStringLiteral("source_start")).toInteger(-1);
        const auto stop = part.value(QStringLiteral("source_end")).toInteger(-1);
        const auto kind = part.value(QStringLiteral("kind")).toString();
        const auto speaker = part.value(QStringLiteral("speaker")).toString();
        if (start != covered || stop <= start || stop > end ||
            (kind != QStringLiteral("dialogue") && kind != QStringLiteral("narration") &&
             kind != QStringLiteral("unknown")) ||
            (kind != QStringLiteral("dialogue") && !speaker.isEmpty()) ||
            bytes.mid(start - base, stop - start) !=
                part.value(QStringLiteral("text")).toString().toUtf8()) {
            status_->setText(tr("正文分割范围或原文校验失败，未染色。"));
            return;
        }
        covered = stop;
    }
    if (covered != end) {
        status_->setText(tr("正文分割不完整，未染色。"));
        return;
    }
    struct Reference {
        qint64 start;
        qint64 end;
        QString person;
    };
    QList<Reference> references;
    QSet<QString> seen;
    QSet<qint64> wanted;
    const auto add = [&](const QString& person, const QJsonArray& evidence) {
        if (!people_.contains(person))
            return;
        for (const auto& item : evidence) {
            const auto left = item.toObject().value(QStringLiteral("source_start"));
            const auto right = item.toObject().value(QStringLiteral("source_end"));
            const auto rawStart = left.toInteger(-1);
            const auto rawStop = right.toInteger(-1);
            if (rawStart < base || rawStop > end || rawStop <= rawStart || !left.isDouble() ||
                !right.isDouble() || left.toDouble() != static_cast<double>(rawStart) ||
                right.toDouble() != static_cast<double>(rawStop))
                continue;
            const auto start = rawStart - base;
            const auto stop = rawStop - base;
            const auto key = person + QLatin1Char(':') + QString::number(start) + QLatin1Char(':') +
                             QString::number(stop);
            if (seen.contains(key))
                continue;
            seen.insert(key);
            references.append({start, stop, person});
            wanted.insert(start);
            wanted.insert(stop);
        }
    };
    settings_->beginGroup(QStringLiteral("bindings/") + hashed(bookId_));
    int nextSlot = 0;
    for (const auto& group : settings_->childGroups()) {
        const auto sequence = settings_->value(group + QStringLiteral("/sequence"), -1).toInt();
        if (sequence >= 0 && sequence <= 1000000)
            nextSlot = qMax(nextSlot, sequence + 1);
    }
    settings_->endGroup();
    for (const auto& value : analysis.value(QStringLiteral("characters")).toArray()) {
        const auto character = value.toObject();
        const auto name = character.value(QStringLiteral("name")).toString();
        if (name.isEmpty() || people_.contains(name))
            continue;
        people_.append(name);
        const auto key = bindingKey(name);
        auto sequence = settings_->value(key + QStringLiteral("/sequence"), -1).toInt();
        if (sequence < 0 || sequence > 1000000) {
            sequence = nextSlot++;
            settings_->setValue(key + QStringLiteral("/sequence"), sequence);
        }
        slots_[name] = sequence % palette_.size();
        const auto custom = settings_->value(key + QStringLiteral("/color")).toString();
        if (validHex(custom))
            overrides_[name] = QColor(custom);
    }
    const auto characterCount = people_.size();
    for (const auto& key : {narrationKey, unknownKey}) {
        people_.append(key);
        const auto custom = settings_->value(bindingKey(key) + QStringLiteral("/color")).toString();
        if (validHex(custom))
            overrides_[key] = QColor(custom);
    }
    for (const auto& value : segments) {
        const auto part = value.toObject();
        const auto kind = part.value(QStringLiteral("kind")).toString();
        const auto speaker = part.value(QStringLiteral("speaker")).toString();
        const auto key = kind == QStringLiteral("narration") ? narrationKey
                         : kind == QStringLiteral("dialogue") && people_.contains(speaker) &&
                                 !speaker.startsWith(QChar(0x1f))
                             ? speaker
                             : unknownKey;
        add(key, QJsonArray{QJsonObject{
                     {QStringLiteral("source_start"), part.value(QStringLiteral("source_start"))},
                     {QStringLiteral("source_end"), part.value(QStringLiteral("source_end"))}}});
    }
    // A single UTF-8 pass maps needed boundaries to Qt's normalized UTF-16 cursor positions.
    QMap<qint64, int> positions;
    int cursor = 0;
    bool previousCr = false;
    for (qint64 offset = 0; offset <= bytes.size();) {
        if (wanted.contains(offset))
            positions.insert(offset, cursor);
        if (offset == bytes.size())
            break;
        const auto lead = static_cast<unsigned char>(bytes[offset]);
        const int length = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
        if (!(lead == '\n' && previousCr))
            cursor += length == 4 ? 2 : 1;
        previousCr = lead == '\r';
        offset += length;
    }
    QMap<int, QMap<QString, int>> edges;
    for (const auto& reference : references) {
        if (!positions.contains(reference.start) || !positions.contains(reference.end))
            continue;
        const auto start = positions.value(reference.start);
        const auto stop = positions.value(reference.end);
        if (start >= stop)
            continue;
        ++edges[start][reference.person];
        --edges[stop][reference.person];
        ++counts_[reference.person];
    }
    QMap<QString, int> active;
    int previous = 0;
    for (auto edge = edges.cbegin(); edge != edges.cend(); ++edge) {
        if (previous < edge.key() && !active.isEmpty())
            spans_.append({previous, edge.key(), active.keys()});
        for (auto delta = edge->cbegin(); delta != edge->cend(); ++delta) {
            active[delta.key()] += delta.value();
            if (active[delta.key()] <= 0)
                active.remove(delta.key());
        }
        previous = edge.key();
    }
    int dialogueCount = 0, narrationCount = 0, uncertainCount = 0;
    for (const auto& value : segments) {
        const auto part = value.toObject();
        const auto kind = part.value(QStringLiteral("kind")).toString();
        if (kind == QStringLiteral("dialogue"))
            ++dialogueCount;
        if (kind == QStringLiteral("narration"))
            ++narrationCount;
        if (kind == QStringLiteral("unknown") ||
            (kind == QStringLiteral("dialogue") &&
             part.value(QStringLiteral("speaker")).toString().isEmpty()))
            ++uncertainCount;
    }
    status_->setText(std::move(status) +
                     tr("\n%1 位人物 · %2 段正文分割；原文为所选任务快照。")
                         .arg(characterCount)
                         .arg(segments.size()) +
                     tr("\n对白 %1 段 · 叙述 %2 段 · 待确认 %3 "
                        "段（含说话人不明的对白）；人物栏仅统计该人物的对白。")
                         .arg(dialogueCount)
                         .arg(narrationCount)
                         .arg(uncertainCount));
    if (dialogueCount == 0)
        status_->setText(status_->text() + tr("\n注意：模型没有识别出任何对白，请人工核对；叙述不分"
                                              "配给人物，0 段不等于读取失败。"));
    rebuildLegend();
    settings_->sync();
    if (settings_->status() != QSettings::NoError)
        storageNote_->setText(tr("人物配色绑定保存失败，本会话仍可预览；请检查配置目录权限。"));
}
} // namespace loreforge::app
