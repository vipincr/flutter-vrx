# AGENTS.md

Instructions for agents working in this repository.

- **A UI change states whether it reached the design system** (vrx plan 0025 D14, G086): a commit touching a path in `.design-ui-paths` carries `Design-system: promoted <key>` or `Design-system: local: <why it is not general>`; the hooks (`bash scripts/install-hooks.sh`) refuse it without, and `docs/brand/DESIGN_SYSTEM.md` says how to decide.
