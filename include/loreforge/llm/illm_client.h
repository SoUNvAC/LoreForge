#pragma once

#include "loreforge/llm/llm_types.h"

namespace loreforge::llm {

class ILLMClient {
  public:
    virtual ~ILLMClient() = default;

    [[nodiscard]] virtual QUuid enqueue(LLMRequest request, CompletionHandler completion) = 0;
    [[nodiscard]] virtual bool cancel(const QUuid& requestId) = 0;
    [[nodiscard]] virtual qsizetype pendingRequestCount() const noexcept = 0;
};

} // namespace loreforge::llm
