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

## WPT URL corpus (non-SQLLogic)

`test/wpt/urltestdata.json` is the WHATWG conformance corpus from
[web-platform-tests](https://github.com/web-platform-tests/wpt), pinned at commit
`181476aa16e8b28a07698bef3a0275fa53dd22e5`. It checks `url_components` against
the same adversarial cases browser vendors hold their URL parsers to, so the glue
between ada and the struct — scheme/fragment stripping, NULL semantics, field
mapping — is pinned by the spec rather than by examples this repo thought of.
Refreshing the corpus is a deliberate act, not an automatic one; see
`docs/UPDATING.md`.

```bash
uv run --frozen test/wpt/run_wpt_corpus.py
```

`URL_TOOLS_DUCKDB_BIN` / `URL_TOOLS_EXTENSION` override the binary and extension
paths (both default to the release build), as in the property harness. The runner
also runs under sanitizers in CI.

**Case selection** — a corpus entry is compared only if it is a case (not a
section-comment string), has no `base` (`url_components` resolves no base URL),
and has a non-empty `input` that does not start with `/` (this repo deliberately
parses `/`-prefixed input as a relative path instead of failing). Failure cases
must yield a NULL row; the rest are compared on `scheme`, `hostname`, `path` and
`fragment`. WPT's `search` is not compared: `url_components` exposes decoded query
params as an object, not the raw search string — a representational difference,
not a defect. Kept and skipped counts are printed per reason; nothing is dropped
silently.

**Coverage gap, recorded honestly**: the `/`-prefix skip rule never actually
fires — every relative input in the corpus carries a `base`, so the base-less
filter already excludes it. The corpus therefore pins absolute-URL parsing and
failure semantics only, and does not exercise the `relative:` placeholder-scheme
path in `UrlToolsParseInput` at all. That path stays covered by SQLLogic cases.

**Known ada deviations** — `KNOWN_ADA_DEVIATIONS` in the runner allowlists 8
cases, counted and printed in the summary (never silently suppressed). WPT commit
`b63305b743` (2026-06-25) applied a WHATWG change — IDNA cannot fail ASCII
domains, even ones starting with `xn--` — so an ASCII host carrying an invalid
punycode label (`http://a.b.c.xn--pokxncvks`, `https://xn--/`, …) must now parse.
The vendored ada v3.4.4 predates the change, still rejects such a host, and
`url_components` returns NULL. The allowlist is keyed on the exact inputs and on
the exact deviation, so a new mismatch — or one of these inputs deviating
*differently* — still fails the run. It is a tripwire, not an exemption: if an
allowlisted case starts matching WPT (an ada bump caught up) the runner fails and
tells you to delete the entry.
