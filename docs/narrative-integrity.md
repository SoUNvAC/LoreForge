# Narrative integrity diagnostics

Phase 20 adds `NarrativeIntegrityChecker` to `LoreForge::Narrative`: deterministic literary
unit tests over explicit, source-backed structured facts. It returns advisory diagnostics;
it does not rewrite text, create repairs, invent missing facts, or call a model.

## Input and provenance

`IntegrityInput` is project scoped. It contains exact chapter UTF-8 projections and their
SHA-256 hashes, canonical character IDs/names, explicit existence boundaries, time-bounded
name variants, character-specific knowledge access, locations, directed travel bounds,
typed events and optional event/rule-specific explanations. Every assertion has an
`IntegrityAnchor` using the existing evidence-or-inference `ClaimSupport` contract.

All evidence must match the anchored chapter's exact source bytes and complete UTF-8 character
boundaries. Duplicate ranges, unknown chapter anchors, stale hashes, malformed UTF-8, overlapping
chapter projections, invalid confidence, duplicate identities, ambiguous event ordering and
invalid typed payloads fail closed: input errors are returned, with no partial diagnostics.
Inference stays explicitly labeled; it is not promoted to evidence. Diagnostic confidence is
the minimum confidence of the participating claims, not a calibrated probability of a defect.

`StoryMoment` is an explicit nonnegative tick plus same-tick order in `clockId`. All facts and
travel bounds use that same story clock. Chapter order is not story time: flashbacks remain valid.
Adapters must normalize relative dates/calendars before calling the checker. Unknown chronology
must not be guessed or encoded using chapter numbers. Existence means an explicitly supplied
boundary (for example birth), not a first mention or first appearance in Story Memory.

## Rules

| Rule | When it reports |
| --- | --- |
| Appearance before existence | Any character reference precedes its explicit existence boundary. |
| Speech after death | Speech follows a recorded death with no intervening direct-evidence revival. |
| Knowledge before revelation | A character uses information before its explicit access boundary. |
| Impossible location transition | Consecutive known locations allow less time than an explicit directed travel bound. |
| Invalid name variant | A used name matches neither the canonical name nor an active variant. |

Missing character/information/location identity, missing character-specific access, missing directed
travel bounds, and name collisions produce `Notice` diagnostics, not claims that a contradiction
has been proven. A reader's late revelation is not a character's late knowledge. Routes are not
assumed symmetric, transitively inferred, or calculated from imagined geography. Unknown location
observations interrupt travel checking instead of implying an unobserved route. Name comparison
uses Unicode NFC and case folding, not fuzzy correction; variants never merge global identities.
Alias validity end points are exclusive. Events for one character need distinct tick/order pairs.

## Exceptions and review

`IntegrityExplanation` requires direct, exact source evidence, a reason, and one event/rule scope.
An explained finding remains in `report.explained` together with the original diagnostic and
explanation anchor; it is not silently discarded. A recording, ghost, dream or explicit special
travel rule can therefore explain one observation without granting a permanent exception.
An inferred explanation cannot suppress a finding. A direct-evidence `Revival` event changes
subsequent life state; an inferred revival does not clear a recorded death.

Warnings remain review candidates, including fully evidenced findings. Fiction can contain
deliberate contradictions, unreliable narrators and unusual rules. The checker proves consistency
only relative to the supplied typed facts; exact quotes do not independently establish that an
adapter interpreted them correctly. No rule authorizes a patch or an automatic name rewrite.

## Determinism and incremental integration

`version()` is `loreforge-narrative-integrity-v1`. A valid report includes an input hash covering
checker version, project, clock, chapter hashes and all supplied facts, confidence, evidence and
exceptions. Canonical framing and sorted collections make the hash insensitive to list insertion
order. Chronological event processing and evidence ordering make diagnostics deterministic.
Each diagnostic has a stable project/clock/event/rule identity, chapter, character, story moment,
message, severity, basis, confidence and source evidence. Review decisions must also bind the
report input hash: stable diagnostic IDs alone cannot authorize reuse after a source change.

Phase 19's generic `Diagnostics` artifact can depend on the actual source/analysis/Story State
artifacts and use the checker version plus structured fact/configuration hashes in its recipe.
Workers must run only over compatible clean inputs, and publish a report only after accepting the
current build ticket. Do not omit manually curated fact/explanation hashes from the recipe.

Existing `ChapterAnalysis` and `StoryStateSnapshot` do not contain normalized death, resurrection,
knowledge-access times or travel bounds. This phase deliberately adds a typed validation boundary,
not free-text heuristics to manufacture those missing semantics. Human annotation or a future
validated extraction adapter must provide these facts from canonical source/analysis. There is
no new database schema, desktop button, implicit model invocation or automatic Story Memory merge.
The deterministic test corpus covers the five rules, grounded exceptions, flashbacks, same-tick
ordering, unknown facts, Unicode names, inference, stale evidence and immutable input.
