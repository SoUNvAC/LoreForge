# GitHub Integration

Phase 17 delivers reviewed repairs through local feature branches and GitHub draft pull requests.
The Git adapter owns local/remote Git commands; the GitHub adapter owns HTTP metadata and PRs.

## Contribution sequence

1. Discover a repository and create a contribution branch while the worktree is clean.
2. Prepare/apply repairs with Phase 15 authorization and the Phase 16 integrity checks.
3. Commit the reviewed paths with explicit maintainer identity and validation.
4. Push the returned commit receipt with `pushContribution()`. The current branch and HEAD must
   still match the receipt. Direct pushes to `main` and `master` are refused; push never forces.
5. Generate a `PullRequestDraft` with `preparePullRequest()`. Repair paths must exactly match the
   receipt's committed paths. The description includes original/replacement text, audit reasons,
   source hashes, candidate IDs where available, and caller-supplied validation results.
6. Call `IGitHubClient::createPullRequest()`. The REST adapter reads the remote head first and
   refuses to create the PR if it differs from the reviewed commit. Draft mode is the default.

These are explicit adapter operations for a use-case controller; this phase does not add a
submission UI or an automatic merge operation. The receipt and authorization are trusted
in-process domain values, not cryptographically signed approval capabilities.

## Authentication

Git fetch/push uses configured Git credentials, including the user's SSH agent/key configuration.
GitHub REST authentication is separate: `IGitHubAuthentication` can resolve a token from an OS
credential store, GitHub App integration, or another provider. The included environment provider
resolves `GH_TOKEN`, then `GITHUB_TOKEN`, at request time. No token is persisted or included in
errors, PR text, or Git arguments. Public read operations may be anonymous; PR creation requires
authentication with repository pull-request write permission.

The client requires HTTPS except for loopback HTTP mock servers. It disables redirects, validates
repository identifiers, uses encoded URL path components, sends the GitHub version/accept headers,
and bounds each request with a timeout. HTTP errors, invalid JSON, and remote head mismatches have
distinct results. PR creation is never automatically retried after an uncertain network result;
callers should inspect existing PRs before repeating a submission.

## Remote tracking and synchronization

`fetch()` updates remote-tracking refs without changing the worktree. `remoteTracking()` compares
the named local and fetched remote branches and returns both SHAs plus ahead/behind counts.
`sync()` requires the requested checked-out branch and a clean worktree, fetches, then merges only
with `--ff-only`. Divergence is surfaced as an error, never resolved through reset or force push.
Branch operations are explicit and do not create hidden background Git jobs.

`readCommit()` returns validated commit metadata. `readPullRequest()` exposes GitHub PR state,
including merge state when supplied by GitHub. `readCommitChecks()` retrieves up to 100 check runs
and preserves `total_count`; callers must handle truncation and pending/missing checks and must
not treat an empty or partial list as CI success. It does not implement an automatic CI gate.

## Verification

Temporary bare repositories exercise reviewed push, stale-HEAD rejection, feature-branch policy,
fetch, ahead/behind tracking, actual fast-forward updates, and dirty-tree refusal. Loopback HTTP
fixtures exercise the remote-SHA check before PR creation, request payload and authentication,
errors, timeouts, draft defaults, protected branch rejection, and check-run inspection. No tests
require live credentials or create public PRs.

REST behavior follows [GitHub's pull request API](https://docs.github.com/en/rest/pulls/pulls)
and [commit API](https://docs.github.com/en/rest/commits/commits).
