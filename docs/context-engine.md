# Context Engine

Phase 13 builds bounded, inspectable prompts from project-owned structured data. It does not
use model chat history as memory and it does not send a model request itself.

## Context assembly

`ContextBuilder` always renders these sections in a stable order:

1. project background;
2. canonical terminology;
3. selected character memory;
4. selected event memory;
5. selected open threads;
6. recent chapter summary;
7. current chapter;
8. task instructions;
9. output schema.

System rules are kept as a separate system message. Background, terminology, recent summary,
current chapter, task instructions, and output schema are mandatory prompt content. Story
memory is optional under pressure and is considered one item at a time.

`RelevantMemoryRetriever` uses deterministic priorities. Characters mentioned by name or alias
come first, followed by events connected to those characters or otherwise mentioned in the
current work, related open threads, other events, other characters, and other open threads.
Recency breaks ties within a priority class; memory kind and stable ID provide the final stable
ordering. The resulting prompt never dumps previous chapter text.

## Token budget

`ContextBudget` splits a maximum token allowance into a prompt limit and a reserved completion
allowance. Mandatory content must fit the prompt limit. Ranked memory entries are included only
when the complete rerendered prompt still fits; omitted counts remain visible in the snapshot
and inspector.

`TokenEstimator` is deliberately provider-neutral and deterministic. It estimates CJK code
points individually, alphanumeric runs at roughly four characters per token, and punctuation
individually. It is a planning heuristic, not a claim about a provider tokenizer or billable
usage. Provider-reported usage remains authoritative after a call.

## Stored-snapshot gate

Every successful build creates a content-addressed `ContextSnapshot` with kind
`loreforge-context-v1`. `ContextEngine::prepareAndStore` persists that snapshot before returning
it. `ContextEngine::requestFromStoredSnapshot` accepts only a snapshot ID, reloads and validates
the stored context, and then reconstructs the exact system/user messages, structured-output
schema, and completion limit. Application use cases must obtain production `LLMRequest` values
through this boundary. The lower-level `ILLMClient` remains a provider transport and is not a
prompt orchestration API.

The desktop Context Inspector displays system rules, background, canonical terminology,
selected character/event/open-thread memory, recent summary, current chapter, task and schema,
the estimate and budget, omission counts, and the raw final prompt.
