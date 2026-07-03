# Tests

`test/sql/` holds [SQLLogicTests](https://duckdb.org/dev/sqllogictest/intro.html) —
the primary test format for this extension. One `.test` file per function or
related cluster of behavior.

Run them from the repo root:

```bash
uv run make test          # release build
uv run make test_debug    # debug build
```

Each SQLLogic file must start with `# group: [sql]` and `require url_tools`
(add `require json` for cases that inspect the JSON results with json_*
functions). Keep output deterministic — sort results or use fixed inputs.

All url_tools functions are total over arbitrary input: cover junk input,
NULL input, and empty-string behavior alongside the happy path. For bug fixes,
add or update a focused SQLLogic case before changing the implementation.

## Property harness (non-SQLLogic)

The totality contract ("junk yields NULL or `{}`, never an error") is enforced
by a hypothesis property/fuzz harness that drives the built CLI
(`URL_TOOLS_DUCKDB_BIN` / `URL_TOOLS_EXTENSION` override the binary and
extension paths; both default to the release build):

```bash
uv run --frozen test/property/url_tools_property.py
```

`URL_TOOLS_PROPERTY_MAX_EXAMPLES` scales the example budget (default 300). The
harness runs under sanitizers in CI (`.github/workflows/Sanitizer.yml`): a
small budget on every push/PR, a deep sweep weekly.
