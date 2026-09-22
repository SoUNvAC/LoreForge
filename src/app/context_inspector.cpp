#include "context_inspector.h"

#include <QJsonDocument>
#include <QPlainTextEdit>
#include <QTabWidget>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace loreforge::app {
namespace {

QString paragraph(QStringView value) {
    return value.isEmpty() ? QStringLiteral("<i>(none)</i>")
                           : QStringLiteral("<pre>%1</pre>").arg(value.toString().toHtmlEscaped());
}

QString list(const QStringList& values) {
    if (values.isEmpty()) {
        return QStringLiteral("<i>(none)</i>");
    }
    QString html = QStringLiteral("<ul>");
    for (const auto& value : values) {
        html += QStringLiteral("<li>%1</li>").arg(value.toHtmlEscaped());
    }
    return html + QStringLiteral("</ul>");
}

} // namespace

ContextInspectorWidget::ContextInspectorWidget(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* tabs = new QTabWidget(this);
    sections_ = new QTextBrowser(tabs);
    sections_->setObjectName(QStringLiteral("contextInspectorSections"));
    rawPrompt_ = new QPlainTextEdit(tabs);
    rawPrompt_->setObjectName(QStringLiteral("contextRawPrompt"));
    rawPrompt_->setReadOnly(true);
    tabs->addTab(sections_, tr("Sections"));
    tabs->addTab(rawPrompt_, tr("Raw Prompt"));
    layout->addWidget(tabs);
    clear();
}

void ContextInspectorWidget::inspect(const context::ContextInspectorData& context) {
    const auto schema =
        QString::fromUtf8(QJsonDocument(context.outputSchema).toJson(QJsonDocument::Indented));
    const auto html =
        QStringLiteral("<style>h3{margin-bottom:4px}pre{white-space:pre-wrap}</style>"
                       "<h3>System Rules</h3>%1"
                       "<h3>Background</h3>%2"
                       "<h3>Canonical Terminology</h3>%3"
                       "<h3>Character Memory</h3>%4"
                       "<h3>Event Memory</h3>%5"
                       "<h3>Open Threads</h3>%6"
                       "<h3>Recent Summary</h3>%7"
                       "<h3>Current Chapter</h3>%8"
                       "<h3>Task</h3>%9"
                       "<h3>Output Schema</h3>%10"
                       "<h3>Token Budget</h3>"
                       "<p>Estimated: %11 · Prompt limit: %12 · Reserved completion: %13</p>"
                       "<p>Omitted: %14 characters, %15 events, %16 open threads</p>")
            .arg(paragraph(context.systemRules))
            .arg(paragraph(context.background))
            .arg(list(context.canonicalTerminology))
            .arg(list(context.characterMemory))
            .arg(list(context.eventMemory))
            .arg(list(context.openThreads))
            .arg(paragraph(context.recentSummary))
            .arg(paragraph(context.currentChapter))
            .arg(paragraph(context.taskInstructions))
            .arg(paragraph(schema))
            .arg(context.estimatedTokens)
            .arg(context.budget.promptTokenLimit())
            .arg(context.budget.reservedCompletionTokens)
            .arg(context.omittedCharacters)
            .arg(context.omittedEvents)
            .arg(context.omittedOpenThreads);
    sections_->setHtml(html);
    rawPrompt_->setPlainText(context.rawFinalPrompt);
}

void ContextInspectorWidget::clear() {
    sections_->setHtml(tr("<h3>Context Inspector</h3><p>No stored context is selected.</p>"));
    rawPrompt_->clear();
}

} // namespace loreforge::app
