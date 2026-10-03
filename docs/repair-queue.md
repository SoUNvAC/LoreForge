# Repair Queue

Phase 15 turns proofreading candidates into a human-controlled workflow. Detection still cannot
change source text. A candidate is copied into the queue with its confidence, category, evidence,
exact source span, source hash, and surrounding source context.

## Review lifecycle

New entries start as `REVIEW_REQUIRED`. A reviewer can:

- approve the current suggestion;
- reject the candidate;
- edit the suggestion, which returns the entry to `REVIEW_REQUIRED`;
- ignore the candidate and atomically save its original spelling as a project-wide protected term.

Terminal delivery states (`PATCHED`, `SUBMITTED`, `MERGED`, and `STALE`) are represented for later
phases but cannot be edited by this workflow. Optimistic status checks prevent one reviewer from
silently overwriting a concurrent decision.

## Patch authorization gate

`RepairGate` is the only Phase 15 API that creates a `PatchAuthorization`. It accepts either:

1. an `APPROVED` queue entry, preserving its candidate ID and evidence chain; or
2. an explicit manual edit with a non-empty audit reason.

Pending or rejected candidates cannot authorize patches. Phase 16 must consume this authorization
type when it implements local Git patches.

## Interface

The Repair Queue dock lists category, confidence, current suggestion, and status. Selecting an
entry exposes its evidence and source context. Approve, reject, save-suggestion, and
ignore/protect-term actions update the in-memory workflow and are persisted by the main-window
controller. If persistence fails, the queue is reloaded from storage so the interface cannot drift
from the durable decision state.
