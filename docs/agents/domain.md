# Domain Docs

How the engineering skills use this repo's domain documentation when they explore the code.

## Before exploring, read these

- **`GLOSSARY.md`** at the repo root.
- **`docs/adr/`**: read the ADRs that touch the area you will work in. The `milestone:` field in each ADR's frontmatter names its milestone (`store`, `api`, `concurrency`, `expire`, `cache`, or `persist`).

If any of these files do not exist, **proceed silently**. Do not flag their absence, and do not suggest creating them upfront. The `/domain-modeling` skill (reached through `/grill-with-docs`) creates them when terms or decisions get resolved.

## File structure

Single-context repo:

```
/
├── GLOSSARY.md
├── docs/adr/
│   ├── reuse-pintos-hash-table.md
│   └── blocking-server-until-concurrency.md
└── src/
```

ADR file names have no number. ADRs written on different branches would get the same number at merge time.

## Use the glossary's vocabulary

When your output names a domain concept (in a ticket title, a proposal, a hypothesis, a test name), use the term as `GLOSSARY.md` defines it. Do not drift to synonyms that the glossary avoids.

If the concept you need is not in the glossary yet, either you are inventing language the project does not use (reconsider), or there is a real gap (note it for `/domain-modeling`).

## Flag ADR conflicts

If your output contradicts an existing ADR, say so explicitly. Do not override it silently:

> _Contradicts `reuse-pintos-hash-table`, but worth reopening because…_

To change a decision, write a new ADR and set the old ADR's `status` to `superseded by <new ADR file name>`.
