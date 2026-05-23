---
name: ai-tmp-cleanup
description: "Use when working with temporary files or evaluating scratchpads. Instructs AI agents on where to write test code, scratchpads, or temporary debug logs."
---

# Temporary Files Created by AI

When generating temporary test files, debug scripts (such as `run_g4.gdb`), scratchpads, or one-off code blocks that are not intended to be committed to the repository, you **must** place them inside the `AI-tmp/` directory at the root of the workspace.

- The `AI-tmp/` directory is untracked by Git.
- Its contents are automatically deleted when the user runs `make clean`.
- Do not create temporary files directly in the workspace root or inside `src/` or `tests/` directories unless explicitly modifying the project code.
