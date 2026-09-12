# Inference snapshots

Phase 9 defines a provider-independent record of everything needed to inspect an LLM run
later. Phase 10 may consume a stored structured response as narrative annotations, but the
snapshot itself does not promise that a remote model will return identical output when called
again.

An inspectable run contains:

- an immutable `PromptTemplate` identity and numbered `PromptVersion`;
- an immutable numbered `OutputSchema`;
- an immutable, project-owned `ContextSnapshot`;
- the exact JSON request body sent to the provider;
- the exact response bytes when a response was received;
- the parsed JSON response when parsing succeeded;
- a validation status and deterministic list of validation errors.

Prompt text, schema JSON, and context JSON carry SHA-256 content hashes. Stored records are
revalidated against those hashes when loaded. A run initially has `pending` artifacts and can
be finalized once as `valid`, `invalid`, or `unavailable`. Finalization recomputes validation
against the stored schema when parsed JSON exists. `InferenceRepository::inspectRun` restores
the complete prompt, schema, context, raw payloads, parsed output, and validation result.

## Supported output-schema subset

Schemas use the JSON Schema Draft 2020-12 vocabulary, limited deliberately to:

- annotations: `$schema`, `title`, and `description`;
- `type`, including a unique non-empty array of supported JSON types;
- `enum` with unique values;
- object keywords `required`, `properties`, and Boolean `additionalProperties`;
- array keyword `items` with an object schema.

The supported JSON types are `null`, `boolean`, `object`, `array`, `number`, `integer`, and
`string`. Unknown keywords and unsupported forms are schema errors rather than being ignored.
This small subset is sufficient for the first structured contracts and prevents accidental
acceptance of a constraint LoreForge does not enforce.

The implementation follows [JSON Schema Draft 2020-12](https://json-schema.org/draft/2020-12),
its [validation vocabulary](https://json-schema.org/draft/2020-12/json-schema-validation),
and the [object reference](https://json-schema.org/understanding-json-schema/reference/object).

## Secret boundary

Captured requests contain only the JSON HTTP body. Authentication headers and API keys are
not included and must never be placed in prompts, contexts, or stored artifacts.
