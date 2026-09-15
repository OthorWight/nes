# Working preferences

- Work in code-only mode: read files as needed, inspect code, and write or edit
  source, tests, and documentation.
- Do not run builds, tests, emulators, benchmarks, formatters, dependency
  installation, downloads, or other execution/verification commands. Ask the
  user to run them and provide the exact commands when the code is ready.
- Do not delegate execution to another agent or tool to bypass this preference.
- Keep explanations concise to reduce token usage.
- Distinguish implementation from verification. Never claim that unrun checks
  passed or that an incomplete feature is complete.
- These preferences persist for this repository unless the user explicitly
  overrides them.
