# Local Git Integration

Phase 16 connects reviewed text repairs to a local Git repository without adding any remote or
GitHub behavior.

## Repository snapshot

`GitRepository::discover()` accepts a repository directory, a nested directory, or a file path and
resolves the worktree root with Git. `snapshot()` reports:

- the canonical repository root;
- the current branch, or no branch for a detached HEAD;
- the full HEAD object ID;
- index and worktree status for tracked, renamed, copied, and untracked paths.

`diff()` returns Git's binary-safe working-tree diff and can be restricted to validated relative
paths. Git is launched directly through `QProcess`; arguments never pass through a command shell.

## Reviewed patch boundary

Patch preparation requires a `PatchAuthorization` created by the Phase 15 `RepairGate`. The
authorized source ID must equal the repository-relative target path. The target must already be
tracked, and all of these values are checked before a preview is produced:

1. authorization origin and candidate-ID shape;
2. repository containment and tracked-file identity;
3. SHA-256 of the complete current file;
4. exact original UTF-8 bytes at the authorized byte span;
5. replacement bytes, resulting SHA-256, and generated unified diff.

The current target must also match its `HEAD` blob before patch preparation. This prevents a
reviewed replacement from accidentally committing older, unrelated edits in the same file.
Application repeats the integrity checks and writes with `QSaveFile`, so a file changed after
preview is rejected rather than overwritten. Commit repeats the prepared-patch validation and
checks the applied file hash again.

The full-file hash requirement is deliberate: proofreading should run against the complete
file-backed chapter source that will be patched. A stale analysis must be rerun instead of trying
to relocate a suggestion heuristically.

## Commit safety gate

A commit request contains reviewed patches, a message, and explicit human author name and email.
There is no generated or default contributor identity.

By default, any status entry outside the reviewed patch paths causes `UnsafeWorkingTree`. A caller
may acknowledge unrelated paths individually after presenting them to the reviewer. Acknowledging
a path permits the operation to continue but does not add that path to the commit. The adapter
uses `git commit --only -- <reviewed paths>`, so unrelated staged and unstaged content remains in
the working tree/index.

Remote synchronization, pull-request creation, CI status, and merge state belong to Phase 17.
