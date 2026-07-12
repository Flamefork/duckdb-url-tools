# DuckDB URL Tools Extension

[![License](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Main Extension Distribution Pipeline](https://github.com/Flamefork/duckdb-url-tools/actions/workflows/MainDistributionPipeline.yml/badge.svg)](https://github.com/Flamefork/duckdb-url-tools/actions/workflows/MainDistributionPipeline.yml)

A DuckDB extension for parsing URLs and query strings.

Built on [ada](https://github.com/ada-url/ada), the WHATWG-compliant URL parser used by Node.js and ClickHouse. All functions are total over arbitrary input: junk values yield `NULL` or an empty object instead of an error, so one malformed value cannot fail a whole scan.

**WARNING: This extension is maintained on a best-effort basis by a developer who doesn’t write C++ professionally, so expect rough edges. It hasn’t been hardened for production, and you should validate it in your own environment before relying on it. Feedback and contributions are welcome via GitHub.**

## Functions

- **`url_components(text)`**: Parses a URL into `STRUCT(scheme, hostname, path, query_params JSON, fragment)`. Absolute URLs of any scheme yield all fields; relative paths (`/path?q=1`) yield `NULL` scheme/hostname; unparseable input yields `NULL`.
- **`query_params(text)`**: Extracts decoded query parameters from a URL (same inputs as `url_components`) as a JSON object. Input without a parseable query yields `{}`.
- **`query_params_from_string(text[, separator])`**: Parses a bare query string (`utm_source=x&utm_medium=y`, no URL around it) into a JSON object. A leading `?` is tolerated. The optional pair separator (default `&`) covers formats like `key=v1|key2=v2`.

Query parameters decode per WHATWG form semantics: percent-escapes and `+` as space; repeated keys resolve last-wins; percent-decoded bytes that are not valid UTF-8 are sanitized with U+FFFD.

## Quick Start

```sql
LOAD './build/release/extension/url_tools/url_tools.duckdb_extension';

SELECT url_components('https://example.com/path?utm_source=duckdb#top');
-- {'scheme': https, 'hostname': example.com, 'path': /path, 'query_params': '{"utm_source":"duckdb"}', 'fragment': top}

SELECT url_components('/search?q=%D0%BB&tab=products');
-- {'scheme': NULL, 'hostname': NULL, 'path': /search, 'query_params': '{"q":"л","tab":"products"}', 'fragment': ''}

SELECT query_params('myapp://open?screen=cart&promo=x');
-- {"screen":"cart","promo":"x"}

SELECT query_params_from_string('utm_source=yandex&plus=a+b');
-- {"utm_source":"yandex","plus":"a b"}

SELECT query_params_from_string('wp1=fb_smm|wp2=post+15%2F06', '|');
-- {"wp1":"fb_smm","wp2":"post 15/06"}
```

## Development

Build and tooling commands run through [uv](https://docs.astral.sh/uv/): invoke every `make` target as `uv run make ...` so the pinned formatter and Python scripts are on PATH.

### Building from Source

```shell
uv run make release
```

This creates the following binaries in the `./build/release` directory:
- `duckdb`: A shell with the extension pre-loaded.
- `test/unittest`: The test runner.
- `extension/url_tools/url_tools.duckdb_extension`: The distributable extension binary.

### Testing

```shell
uv run make verify
```

`verify` chains `release` → `test` → `format-check` in the right order and is the pre-PR gate. Note: `make test` alone only runs the already-built test binary — without a preceding `release` it exercises a stale build.

The totality contract ("junk yields `NULL` or `{}`, never an error") is additionally fuzz-tested by a hypothesis property harness — see [test/README.md](test/README.md):

```shell
uv run --frozen test/property/url_tools_property.py
```

Benchmarks (regression gate and comparisons against netquack / stock SQL) live in [bench/](bench/README.md).
