---
name: Split README into docs
overview: Simplify the main README to a concise quick-start guide with a production disclaimer, and move detailed reference material into a new `doc/` directory with one file per topic.
todos:
  - id: create-doc-dir
    content: Create the `doc/` directory
    status: pending
  - id: doc-configuration
    content: Create `doc/configuration.md` with Configuration Reference content (lines 111-190)
    status: pending
  - id: doc-output-format
    content: Create `doc/output-format.md` with Output Format content (lines 192-305)
    status: pending
  - id: doc-pt-query-digest
    content: Create `doc/pt-query-digest.md` with pt-query-digest usage content (lines 306-357)
    status: pending
  - id: doc-architecture
    content: Create `doc/architecture.md` with Architecture content (lines 358-431)
    status: pending
  - id: doc-sql-functions
    content: Create `doc/sql-functions.md` with SQL Functions content (lines 433-461)
    status: pending
  - id: doc-testing
    content: Create `doc/testing.md` with Testing content (lines 463-660)
    status: pending
  - id: doc-compatibility
    content: Create `doc/compatibility.md` with Compatibility Notes content (lines 662-670)
    status: pending
  - id: doc-contributing
    content: Create `doc/contributing.md` with Contributing + Code Style content (lines 672-690)
    status: pending
  - id: simplify-readme
    content: "Rewrite README.md: keep intro, add disclaimer, keep features/requirements/installation/quick-start, add Documentation links section, keep license/acknowledgments"
    status: pending
isProject: false
---

# Split README into Focused Documentation

## Current state

The [README.md](README.md) is ~700 lines covering everything from features to architecture to testing. No `doc/` or `docs/` directory exists yet.

## New structure

```
README.md                        (simplified, ~120-150 lines)
doc/
  configuration.md               (all GUC tables + runtime examples)
  output-format.md               (verbosity examples + field reference + key naming)
  pt-query-digest.md             (usage examples, filters, comparisons, sampled data)
  architecture.md                (hook chain diagram, rate limiting, file I/O, plan tree)
  sql-functions.md               (reset + stats functions)
  testing.md                     (prerequisites, regression, TAP, meson, CI, manual)
  compatibility.md               (compatibility notes)
  contributing.md                (contributing guide + code style)
```

## Simplified README.md

Keep these sections in the main README:

1. **Title and one-paragraph description** -- what the extension does, the pt-query-digest angle.
2. **Disclaimer** -- a prominent warning that the extension is not production-ready yet.
3. **Key features** -- the existing bullet list (trimmed if needed, but it is already concise).
4. **Requirements** -- PostgreSQL version, compiler, optional pt-query-digest (keep the install snippet).
5. **Installation** -- building from source + loading the extension (unchanged).
6. **Quick Start** -- the existing quick-start block (postgresql.conf snippet + pt-query-digest one-liner).
7. **Documentation** -- a new section listing each `doc/*.md` with a one-line description and link.
8. **License** and **Acknowledgments** -- keep as-is (short).

Everything else moves to `doc/`.

## doc/ files

Each doc file will:

- Start with a level-1 heading matching the topic.
- Include a "back to README" link at the top.
- Contain the content verbatim from the current README (preserving tables, code blocks, diagrams).

### Mapping of current README sections to doc files

- **Configuration Reference** (lines 111-190) -> `doc/configuration.md`
- **Output Format** (lines 192-305) -> `doc/output-format.md`
- **Using with pt-query-digest** (lines 306-357) -> `doc/pt-query-digest.md`
- **Architecture** (lines 358-431) -> `doc/architecture.md`
- **SQL Functions** (lines 433-461) -> `doc/sql-functions.md`
- **Testing** (lines 463-660) -> `doc/testing.md`
- **Compatibility Notes** (lines 662-670) -> `doc/compatibility.md`
- **Contributing** + **Code Style** (lines 672-690) -> `doc/contributing.md`

## Disclaimer

A boxed warning will be added right after the title/description:

```markdown
> **Warning**
> This extension is under active development and has **not** been validated for production use.
> Use it in development and testing environments only. APIs, configuration parameters, and
> log format may change in future releases without notice.
```

## Key decisions

- Directory name: `doc/` (singular, common in C/PostgreSQL projects).
- Each doc file gets a nav link back to the main README.
- The README "Documentation" section will be a simple bullet list linking to each file.
- No content is removed -- everything is preserved, just relocated.

