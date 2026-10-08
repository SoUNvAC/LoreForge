#pragma once

#include "loreforge/llm/qwen_client.h"

#include <QWidget>

#include <memory>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSettings;
class QSpinBox;

namespace loreforge::app {

class LlmWorkbench final : public QWidget {
    Q_OBJECT
  public:
    explicit LlmWorkbench(QWidget* parent = nullptr, QString settingsFile = {});
    ~LlmWorkbench() override;

  private:
    void saveConfiguration();
    void testConnection();
    void cancelTest();
    void resetTokenCounts();
    void updateTokenTotals();
    void setBusy(bool busy);
    void finishTest(llm::LLMResult result);
    [[nodiscard]] QString configurationError() const;

    std::unique_ptr<QSettings> settings_;
    std::unique_ptr<llm::QwenClient> client_;
    QLineEdit* endpoint_ = nullptr;
    QComboBox* modelSize_ = nullptr;
    QLineEdit* modelId_ = nullptr;
    QLineEdit* apiKey_ = nullptr;
    QCheckBox* allowHttp_ = nullptr;
    QComboBox* format_ = nullptr;
    QComboBox* tokenField_ = nullptr;
    QSpinBox* timeout_ = nullptr;
    QSpinBox* outputBudget_ = nullptr;
    QPushButton* save_ = nullptr;
    QPushButton* test_ = nullptr;
    QPushButton* cancel_ = nullptr;
    QPushButton* resetTokens_ = nullptr;
    QLabel* connection_ = nullptr;
    QLabel* metrics_ = nullptr;
    QLabel* totals_ = nullptr;
    QPlainTextEdit* log_ = nullptr;
    QUuid requestId_;
    qint64 inputTokens_ = 0;
    qint64 outputTokens_ = 0;
    qint64 missingUsage_ = 0;
    bool closing_ = false;
};

} // namespace loreforge::app
