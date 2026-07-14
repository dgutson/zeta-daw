# Agent Working Agreement

Read `CONTRIBUTING.md` completely before proposing or making changes. Its
product, architecture, testing, and real-time constraints remain authoritative.

## Requirements and simplicity

- Prefer the simplest design that satisfies the explicitly agreed requirements.
- Do not invent, infer, or silently expand product requirements. When behavior
  or a tradeoff has not been specified, ask the project owner before
  implementing it.
- Do not add defensive machinery for scenarios outside the agreed usage
  contract without discussing it first.
- Keep tickets focused. Put unrelated changes in a separate issue and branch
  unless the project owner explicitly requests otherwise.
- Explain unavoidable complexity before introducing it so the project owner can
  approve the tradeoff.

## Review collaboration

- Treat the project owner as an advanced code reviewer. Present design reasoning
  and meaningful tradeoffs directly; do not optimize explanations for a novice.
- When a ticket requests multiple separately verified commits, stop after each
  verified commit, present the implementation and verification evidence, and
  wait for the project owner's review before starting the next change.
- Treat wording such as "consider refactoring" as a request for engineering
  judgment, not as an already approved implementation. Explain whether the
  refactor is worthwhile, surface ownership or architecture alternatives, and
  obtain agreement before changing code.
