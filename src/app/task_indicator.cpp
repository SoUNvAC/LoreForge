#include "task_indicator.h"

#include <QColor>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <cmath>

namespace loreforge::app {
TaskIndicator::TaskIndicator(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("taskIndicator"));
    setMaximumWidth(700);
    setMinimumWidth(380);
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 0, 4, 0);
    lamp_ = new QPushButton(this);
    lamp_->setObjectName(QStringLiteral("taskStatusLamp"));
    lamp_->setFixedSize(22, 22);
    label_ = new QLabel(this);
    label_->setObjectName(QStringLiteral("taskStatusText"));
    label_->setTextFormat(Qt::PlainText);
    label_->setMinimumWidth(0);
    label_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(lamp_);
    layout->addWidget(label_);
    connect(lamp_, &QPushButton::clicked, this, &TaskIndicator::acknowledgeFailure);
    timer_ = new QTimer(this);
    timer_->setInterval(80);
    connect(timer_, &QTimer::timeout, this, &TaskIndicator::refresh);
    animationClock_.start();
    refresh();
}

void TaskIndicator::report(const QUuid& id, TaskState state, const QString& text) {
    if (id.isNull())
        return;
    const bool running = state == TaskState::Local || state == TaskState::Waiting;
    if (running) {
        if (!active_.contains(id)) {
            ActiveTask task{text, state, {}};
            task.elapsed.start();
            active_.insert(id, task);
            order_.append(id);
        }
        active_[id].text = text;
        active_[id].state = state;
        current_ = order_.last();
    } else {
        // A late callback cannot replace the status of a newer task or an acknowledged error.
        if (!active_.contains(id))
            return;
        active_.remove(id);
        order_.removeAll(id);
        if (current_ != id)
            return;
        if (!order_.isEmpty()) {
            current_ = order_.last();
        } else {
            state_ = state;
            text_ = text;
        }
    }
    refresh();
    // Local operations are synchronous; paint without accepting reentrant user input.
    repaint();
    if (state_ == TaskState::Success) {
        QTimer::singleShot(4000, this, [this, id] {
            if (current_ == id && state_ == TaskState::Success) {
                state_ = TaskState::Idle;
                text_.clear();
                refresh();
            }
        });
    }
}

void TaskIndicator::acknowledgeFailure() {
    if (state_ != TaskState::Failed)
        return;
    state_ = TaskState::Idle;
    text_.clear();
    refresh();
}

void TaskIndicator::setAnimationEnabled(bool enabled) {
    animationEnabled_ = enabled;
    refresh();
}

void TaskIndicator::refresh() {
    QString display = text_.isEmpty() ? tr("就绪") : text_;
    if (active_.contains(current_)) {
        const auto& task = active_[current_];
        state_ = task.state;
        const auto seconds = task.elapsed.elapsed() / 1000;
        display = tr("%1  ·  %2:%3")
                      .arg(task.text)
                      .arg(seconds / 60, 2, 10, QLatin1Char('0'))
                      .arg(seconds % 60, 2, 10, QLatin1Char('0'));
    }
    QColor color;
    switch (state_) {
    case TaskState::Local:
        color = QColor("#2878d0");
        break;
    case TaskState::Waiting:
        color = QColor("#009ca6");
        break;
    case TaskState::Success:
        color = QColor("#26853b");
        break;
    case TaskState::Attention:
        color = QColor("#bc8500");
        break;
    case TaskState::Failed:
        color = QColor("#d32f2f");
        break;
    default:
        color = QColor("#808080");
        break;
    }
    const bool running = state_ == TaskState::Local || state_ == TaskState::Waiting;
    if (running && animationEnabled_) {
        const double wave =
            (1.0 + std::sin(animationClock_.elapsed() * 6.283185307 / 3000.0)) / 2.0;
        color = color.lighter(100 + static_cast<int>(wave * 65));
    }
    lamp_->setStyleSheet(
        QStringLiteral(
            "QPushButton { background: %1; border: 1px solid #666; border-radius: 11px; }")
            .arg(color.name()));
    lamp_->setEnabled(state_ == TaskState::Failed);
    lamp_->setAccessibleName(state_ == TaskState::Failed ? tr("失败：点击确认并恢复就绪")
                                                         : display);
    const auto tooltip =
        QStringLiteral("<qt>") +
        display.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")) +
        QStringLiteral("</qt>");
    lamp_->setToolTip(state_ == TaskState::Failed ? tr("点击恢复就绪；不会删除错误记录或重试任务。")
                                                  : tooltip);
    // Keep the status bar on one line; full review reasons remain in the page and tooltip.
    label_->setText(display.simplified());
    label_->setToolTip(tooltip);
    if (running)
        timer_->start();
    else
        timer_->stop();
}
} // namespace loreforge::app
