#pragma once

#include "loreforge/context/context_types.h"

#include <QWidget>

class QPlainTextEdit;
class QTextBrowser;

namespace loreforge::app {

class ContextInspectorWidget final : public QWidget {
    Q_OBJECT

  public:
    explicit ContextInspectorWidget(QWidget* parent = nullptr);

    void inspect(const context::ContextInspectorData& context);
    void clear();

  private:
    QTextBrowser* sections_ = nullptr;
    QPlainTextEdit* rawPrompt_ = nullptr;
};

} // namespace loreforge::app
