# DuckDB URL Tools Extension

[![License](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Main Extension Distribution Pipeline](https://github.com/Flamefork/duckdb-url-tools/actions/workflows/MainDistributionPipeline.yml/badge.svg)](https://github.com/Flamefork/duckdb-url-tools/actions/workflows/MainDistributionPipeline.yml)

A DuckDB extension for parsing URLs and query strings from raw analytics data.

Built on [ada](https://github.com/ada-url/ada), the WHATWG-compliant URL parser used by Node.js and ClickHouse. All functions are total over arbitrary input: junk values yield `NULL` or an empty object instead of an error, so one malformed value cannot fail a whole scan.

**WARNING: This extension is maintained on a best-effort basis by a developer who doesn’t write C++ professionally, so expect rough edges. It hasn’t been hardened for production, and you should validate it in your own environment before relying on it. Feedback and contributions are welcome via GitHub.**

## Functions

- **`url_components(text)`**: Parses a URL into `STRUCT(scheme, hostname, path, query_params JSON, fragment)`. Absolute URLs of any scheme yield all fields; relative paths (`/path?q=1`, as sent by SPA hit tracking) yield `NULL` scheme/hostname; unparseable input yields `NULL`.
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

### Building from Source

1.  **Build the extension:**
    ```shell
    make
    ```

This will create the following binaries in the `./build/release` directory:
- `duckdb`: A shell with the extension pre-loaded.
- `test/unittest`: The test runner.
- `extension/url_tools/url_tools.duckdb_extension`: The distributable extension binary.

### Running Tests

To run the SQL tests:
```shell
make test
```
