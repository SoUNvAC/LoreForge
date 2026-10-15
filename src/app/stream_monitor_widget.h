#pragma once

#include <QTimer>
#include <QUuid>
#include <QWidget>

class QLabel;
class QPlainTextEdit;

namespace loreforge::app {
class StreamMonitorWidget final : public QWidget {
    Q_OBJECT
  public:
    explicit StreamMonitorWidget(QWidget* parent = nullptr);
    void beginRequest(QUuid id, QString label);
    void updateRequest(QUuid id, QString text, bool generating);
    void finishRequest(QUuid id, QString status);

  private:
    void flush();
    QUuid current_;
    QString label_;
    QString pending_;
    QLabel* status_ = nullptr;
    QPlainTextEdit* messages_ = nullptr;
    QTimer refresh_;
    QTimer silence_;
};
} // namespace loreforge::app
