# Model regression suite

Phase 21 adds `LoreForge::Regression` and `ModelRegressionSuite`. The suite evaluates captured,
normalized outputs against a versioned golden corpus, compares baseline/candidate receipts and
requires scoped human review before issuing a promotion authorization. It never calls a model,
changes active model configuration, rewrites a source or grants its own approval.

## Golden corpus and captured runs

`tests/fixtures/regression/golden-v1.json` contains six original, handcrafted English/Chinese seed
cases: attributed dialogue, unknown speakers, intentional terminology, duplicated words,
flashbacks and character-specific knowledge with a typo. Its checked-in format, corpus identity,
version, exact UTF-8 sources, segmentation, required/allowed semantic labels and approved edits
are content addressed. Text chunks derive exact byte ranges; edit quotes select a zero-based
occurrence. Gaps, mismatched source, duplicates, invalid label sets and overlapping edits fail
validation. Optional allowed labels do not penalize legitimate additional extraction.

This small seed covers distinct failure modes, not the diversity of production novels. Expand
it with authorized, human-reviewed samples before making a production model decision. Update
the corpus version for annotation changes; run both models again against the new corpus hash.
Private/copyrighted corpora and captured responses remain local and ignored, not committed.

Each `RunManifest` identifies backend, model and exact revision, corpus, prompt/schema bundles,
configuration and capture time. Configuration must cover the task adapter/normalizer versions,
decoding parameters, retries, context policy, hardware/measurement protocol and all relevant
settings. A mutable model alias is not an exact revision; local weight hashes or immutable
deployment revisions should be recorded. Multi-task prompt/schema hashes cover all evaluated
tasks, not just one convenient prompt.

`CaseObservation` requires exactly one independent capture per golden case, an LLM run ID,
source hash, exact context snapshot hash, retained response receipt, segmentation, normalized
semantic labels, proposed edits and optional provider token/latency measurements. Predictions
must come from captured results, never copied from golden answers. Domain adapters validate
raw outputs through the existing extractor/analyzer/proofreader before normalization. Unknown
claims must remain unknown labels, not be dropped. Semantic label mappings/accepted paraphrases
are human-reviewed benchmark annotations, not an automatic semantic equivalence oracle.

The suite revalidates segmentation through `DialogueExtractor`, requires reconstructed text to
equal the immutable source and checks edit quotes/ranges against complete UTF-8 bytes. It does
not independently prove a raw response produced normalized semantic labels: capture storage and
the adapter are trusted boundaries. SHA-256 receipts detect accidental changes, not forgery.
Token counts must be nonnegative and internally consistent; missing measurements remain null.
Latency uses a nonnegative measured millisecond count, not a guessed value or a quality score.

## Metrics

| Metric | Definition |
| --- | --- |
| Dialogue accuracy | Fraction of source UTF-8 bytes correctly classified as dialogue/narration. |
| Speaker accuracy | Fraction of golden dialogue bytes with the correct speaker, including unknown/null. |
| Entity/event extraction | F1 over required labels; precision accepts all explicitly allowed labels. |
| Hallucination rate | Unsupported entity/event/context labels divided by predicted labels in this closed benchmark. |
| Context consistency | Required/allowed context-assertion F1, including timeline and knowledge assertions. |
| Proofreading precision | Exact approved edits divided by proposed edits. |
| Proofreading recall | Exact approved edits divided by required edits; prevents no-op precision gaming. |
| Token usage | Total reported prompt + completion tokens per case. |
| Latency | Measured milliseconds per case. |

The hallucination metric is benchmark unsupported-label rate, not a universal detector of fictional
truth or an open-world factuality claim. A new legitimate claim needs annotation review. Proofreading
matches exact span, original and replacement; it does not credit an unreviewed paraphrase. Empty,
inapplicable entity/event/speaker/precision/recall cases are null rather than fictitious perfect
scores. Missing required facts reduce recall. Hallucination rate with no predictions is zero, but
empty extraction cannot pass the other recall/F1 gates. Source is never edited during evaluation.

Aggregates are deterministic macro means of applicable case scores, not a pooled score dominated
by a long chapter. All cases must have token/latency measurements for a complete cost aggregate.
Raw per-case scores remain visible. The default policy checks every applicable case and aggregate:
dialogue/speaker/context/precision >= 0.95, entity/event F1 >= 0.90, proofreading recall >= 0.85,
hallucination <= 0.05, quality regression <= 0.02, token ratio <= 1.25 and latency ratio <= 1.50.
These are configurable engineering defaults, not statistically calibrated universal standards.
Use controlled repeated captures and reviewed tolerances for noisy real-model latency/quality.

## Comparison, persistence and review

Both runs must use identical corpus, prompt/schema/configuration and per-case context hashes.
Changed inputs are a new experiment, not an isolated backend comparison. Run/model identities
must differ and capture IDs cannot be reused. Invalid/tampered receipts or mismatched scopes fail
comparison. Missing aggregate metrics and any absolute/relative/per-case violation become explicit
blocking issues. A zero baseline cost cannot authorize positive candidate cost through division.

`encodeRun`/`loadRun` save and reload `loreforge-model-regression-v1` receipts. Loading validates
hashes, per-case domains, complete identities and recalculated aggregates. `encodeComparison`
exports both runs, policy, issues and a comparison hash. Keep raw captures separately through
the existing inference snapshot storage; exported receipts contain hashes, not prompts, response
text, provider tokens or secrets. Save reports atomically in derived/runtime data, not source.

`authorize` recalculates the comparison, requires no blocking issues, verifies the current baseline
run hash and binds an approved review to the exact comparison hash, reviewer, reason and review
time after both captures. Modified policy, inputs, runs or baseline invalidate earlier review.
Review is provided by the human-facing caller; the suite cannot authenticate a person by a string
and must not be used to manufacture automatic approval. Authorization is only an immutable receipt.
The caller still needs the user's explicit permission to change active model configuration.

## Verified scope

CTest runs an offline fixture suite with explicitly synthetic observations. It verifies scoring,
failure behavior, deterministic receipts, report persistence, comparable provenance and the human
review gate without live inference. Those tests are not baseline/candidate quality results for
Qwen or any real backend. No actual model benchmark, promotion, database migration or new desktop
benchmark button is performed by this phase. Production captures continue through the stored
Context Engine and existing validated model transport/domain adapters.
