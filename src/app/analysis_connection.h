#pragma once

#include "loreforge/llm/qwen_client.h"

namespace loreforge::app {
struct AnalysisConnection final {
    llm::QwenClientOptions options;
    QString responseFormat;
    int timeoutMs = 120000;
};
} // namespace loreforge::app
