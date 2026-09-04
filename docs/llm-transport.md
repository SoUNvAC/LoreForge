# LLM transport

Phase 8 provides model transport only. It does not select narrative context, construct
versioned prompts, validate narrative schemas, or apply model output to project data.

## Interface

`ILLMClient` accepts an `LLMRequest`, returns a request UUID immediately, and invokes the
completion handler once with either `LLMResponse` or `LLMError`. Both test mocks and
`QwenClient` implement this interface. Callers can cancel either queued or active requests.

`QwenClient` processes requests in FIFO order. Each network attempt has its own timeout.
Timeouts, network failures, HTTP 408/425/429, and 5xx responses are retryable within the
request's bounded exponential-backoff policy. Cancellation and invalid inputs are never
retried.

## Qwen configuration

The adapter uses Alibaba Cloud Model Studio's OpenAI-compatible Chat Completions endpoint.
The endpoint, model name, and API key are runtime constructor inputs because endpoints and
model availability vary by region and workspace. The API key is sent only as a Bearer header;
it is not logged or persisted. Keep it in a local environment or secret store, never in the
repository.

Structured output is requested with the provider's `response_format` object. LoreForge
accepts `text`, `json_object`, and `json_schema` transport modes and parses structured model
content as JSON before returning it. The provider-independent inference layer performs the
schema-semantic validation.

Provider references:

- [OpenAI-compatible Qwen Chat API](https://www.alibabacloud.com/help/en/model-studio/qwen-api-via-openai-chat-completions)
- [Qwen structured output](https://www.alibabacloud.com/help/en/model-studio/qwen-structured-output)
- [Model Studio error codes](https://www.alibabacloud.com/help/en/model-studio/error-code)

## Run persistence

`LLMRunRepository` stores project and request identity, provider/model, lifecycle status,
attempt count, token usage, timestamps, latency, and terminal error information. Qwen results
also expose the exact JSON request and provider response bytes. `InferenceRepository` stores
those bytes with the versioned inference inputs and validation result in schema v4. HTTP
authorization headers and API keys are never part of the captured request payload.
