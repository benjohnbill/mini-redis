# Issue tracker: Local Markdown

Specs and tickets for this repo live as Markdown files in `docs/milestones/`. They do not live in GitHub Issues.

## Conventions

- One milestone per folder: `docs/milestones/<name>/`. `<name>` is the milestone name: `store`, `api`, `concurrency`, `expire`, `cache`, or `persist`.
- The spec is `docs/milestones/<name>/spec.md`.
- Tickets are one file each: `docs/milestones/<name>/issues/<NN>-<slug>.md`, numbered from `01`. Never put all tickets in one file.
- The triage state is a `Status:` line near the top of each file. See `triage-labels.md`.
- Comments and discussion go at the bottom of the file under a `## Comments` heading.

## Files that skills read but do not write

- `docs/milestones/<name>/overview.md`: the team writes it at stage 6 of the previous milestone. It lists the problems to explore. It never holds Redis's real answers, so do not add them.

## Milestone folders are history

- A milestone folder records what the team thought at that time. Do not edit a folder of a finished milestone.
- When a later milestone changes an earlier decision, say so in the current milestone's `spec.md`, and supersede the old ADR (see `domain.md`).
- The current state of a feature is in `src/`, `tests/`, `README.md`, `docs/api.md`, and `demo/README.md`, not in old milestone folders. `README.md` holds only summaries and links; the HTTP contract goes in `docs/api.md`.

## The spec

`spec.md` has a section `## Redis와 대조` in this order: our idea, how Redis does it, what we chose. `README.md` links to these sections.

## When a skill says "publish to the issue tracker"

Create a new file under `docs/milestones/<name>/`. Create the folder if it does not exist.

## When a skill says "fetch the relevant ticket"

Read the file at the given path. The user normally gives the path or the ticket number.
