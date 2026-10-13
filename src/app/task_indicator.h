#pragma once

#include <QElapsedTimer>
#include <QMap>
#include <QUuid>
#include <QWidget>

class QLabel;
class QPushButton;
class QTimer;

namespace loreforge::app {
enum class TaskState { Idle, Local, Waiting, Success, Attention, Failed, Cancelled };

class TaskIndicator final : public QWidget {
    Q_OBJECT
  public:
    explicit TaskIndicator(QWidget* parent = nullptr);
    void report(const QUuid& id, TaskState state, const QString& text);
    [[nodiscard]] TaskState state() const {
        return state_;
    }
    void setAnimationEnabled(bool enabled);
  public slots:
    void acknowledgeFailure();

  private:
    void refresh();
    struct ActiveTask {
        QString text;
        TaskState state;
        QElapsedTimer elapsed;
    };
    QMap<QUuid, ActiveTask> active_;
    QList<QUuid> order_;
    QUuid current_;
    TaskState state_ = TaskState::Idle;
    QString text_;
    QPushButton* lamp_ = nullptr;
    QLabel* label_ = nullptr;
    QTimer* timer_ = nullptr;
    QElapsedTimer animationClock_;
    bool animationEnabled_ = true;
};
} // namespace loreforge::app
