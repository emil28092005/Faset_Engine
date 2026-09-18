# Keep the manual executable

Document user tasks and actual APIs in English. Explain what each argument means,
which callback it belongs in, and what happens when an object or resource is missing.

Prioritize complete small gameplay examples over isolated declarations. Link examples
to their source files and include them in build or integration checks. When an API
changes, update its examples in the same change.

Mark planned capabilities explicitly. Do not describe a prototype image as a running
editor, a Linux test as Windows validation, or a planned feature as implemented.

Build with strict documentation validation before publishing:

```sh
.cache/docs-venv/bin/python -m mkdocs build --strict
```

Generated output goes to `build/manual`; source Markdown and configuration are tracked
in Git. Research and architecture documents remain separate from this user manual.
