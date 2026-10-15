#include "stream_monitor_widget.h"

#include <QLabel>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QVBoxLayout>

namespace loreforge::app {
StreamMonitorWidget::StreamMonitorWidget(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("streamMonitor"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    status_ = new QLabel(tr("SSE · 当前请求（尚未发送）"), this);
    status_->setObjectName(QStringLiteral("streamMonitorStatus"));
    status_->setTextFormat(Qt::PlainText);
    layout->addWidget(status_);
    messages_ = new QPlainTextEdit(this);
    messages_->setObjectName(QStringLiteral("streamMonitorMessages"));
    messages_->setReadOnly(true);
    messages_->setPlaceholderText(
        tr("只显示本次请求的 SSE 正文与接收统计；新请求自动清空，不回放历史。"));
    messages_->setMinimumHeight(65);
    messages_->setMaximumHeight(105);
    layout->addWidget(messages_);
    refresh_.setSingleShot(true);
    refresh_.setInterval(100);
    silence_.setSingleShot(true);
    silence_.setInterval(5000);
    connect(&refresh_, &QTimer::timeout, this, &StreamMonitorWidget::flush);
    connect(&silence_, &QTimer::timeout, this, [this] {
        status_->setText(tr("SSE · %1 · 等待后续数据（已静默 5 秒，不代表失败）").arg(label_));
    });
}

void StreamMonitorWidget::beginRequest(QUuid id, QString label) {
    refresh_.stop();
    silence_.stop();
    current_ = id;
    label_ = std::move(label);
    pending_.clear();
    messages_->clear();
    status_->setText(tr("SSE · %1 · 等待服务响应").arg(label_));
    status_->setToolTip(id.toString());
}

void StreamMonitorWidget::updateRequest(QUuid id, QString text, bool generating) {
    if (id != current_ || current_.isNull())
        return;
    pending_ = std::move(text);
    status_->setText(
        tr("SSE · %1 · %2").arg(label_, generating ? tr("分析中") : tr("连接活跃 / 接收事件")));
    silence_.start();
    if (!refresh_.isActive())
        refresh_.start();
}

void StreamMonitorWidget::finishRequest(QUuid id, QString status) {
    if (id != current_ || current_.isNull())
        return;
    refresh_.stop();
    silence_.stop();
    flush();
    status_->setText(tr("SSE · %1 · %2").arg(label_, status));
    current_ = {};
}

void StreamMonitorWidget::flush() {
    const auto* scroll = messages_->verticalScrollBar();
    const auto follow = scroll->value() >= scroll->maximum();
    const auto previous = scroll->value();
    messages_->setPlainText(pending_);
    messages_->verticalScrollBar()->setValue(follow ? messages_->verticalScrollBar()->maximum()
                                                    : previous);
}
} // namespace loreforge::app
