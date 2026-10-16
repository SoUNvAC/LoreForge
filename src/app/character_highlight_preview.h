#pragma once

#include <QColor>
#include <QJsonObject>
#include <QMap>
#include <QPlainTextEdit>
#include <QWidget>
#include <functional>
#include <memory>

class QComboBox;
class QLabel;
class QLineEdit;
class QSettings;
class QSpinBox;
class QTableWidget;

namespace loreforge::app {
struct CharacterHighlightSpan final {
    int start = 0;
    int end = 0;
    QStringList people;
};

class CharacterHighlightText final : public QPlainTextEdit {
    Q_OBJECT
  public:
    explicit CharacterHighlightText(QWidget* parent = nullptr);
    void setHighlights(QList<CharacterHighlightSpan> spans, QMap<QString, QColor> colors,
                       QString filter, int opacity, int width);
    [[nodiscard]] const QList<CharacterHighlightSpan>& spans() const {
        return spans_;
    }

  protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;

  private:
    QList<CharacterHighlightSpan> spans_;
    QMap<QString, QColor> colors_;
    QString filter_;
    int opacity_ = 25;
    int width_ = 14;
};

class CharacterHighlightPreview final : public QWidget {
    Q_OBJECT
  public:
    explicit CharacterHighlightPreview(QWidget* parent = nullptr, QString settingsFile = {});
    ~CharacterHighlightPreview() override;
    void showImportedText(QString bookId, QString text);
    void showAnalysis(QString bookId, const QJsonObject& source, const QJsonObject& analysis,
                      QString status);
    [[nodiscard]] QColor characterColor(const QString& name) const;

  private:
    void rebuildLegend();
    void refreshHighlights();
    void saveStyle();
    void updateColor(QLineEdit* editor, const std::function<void(QColor)>& apply);
    QString bindingKey(const QString& name) const;
    std::unique_ptr<QSettings> settings_;
    QList<QColor> palette_;
    QList<QLineEdit*> paletteEditors_;
    QString bookId_;
    QStringList people_;
    QMap<QString, int> slots_;
    QMap<QString, QColor> overrides_;
    QMap<QString, int> counts_;
    QList<CharacterHighlightSpan> spans_;
    CharacterHighlightText* text_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* storageNote_ = nullptr;
    QComboBox* filter_ = nullptr;
    QSpinBox* opacity_ = nullptr;
    QSpinBox* width_ = nullptr;
    QTableWidget* legend_ = nullptr;
};
} // namespace loreforge::app
