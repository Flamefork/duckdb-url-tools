# DuckDB URL Tools Extension

[![License](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Main Extension Distribution Pipeline](https://github.com/Flamefork/duckdb-url-tools/actions/workflows/MainDistributionPipeline.yml/badge.svg)](https://github.com/Flamefork/duckdb-url-tools/actions/workflows/MainDistributionPipeline.yml)

A DuckDB extension that parses URLs and query strings.

- **A WHATWG parser.** The extension uses [ada](https://github.com/ada-url/ada), the WHATWG-compliant URL parser that Node.js and ClickHouse use.
- **Total over each input.** A junk value gives `NULL` or an empty map, never an error. So one bad value cannot fail a full scan.
- **A typed toolset.** The WHATWG components as a `STRUCT`, the decoded query parameters as a `MAP`, and the registrable domain (eTLD+1) from a compiled-in Public Suffix List — see the [Function reference](#function-reference).

> [!WARNING]
> The maintainer does not write C++ professionally and maintains the extension on a best-effort basis. Rough edges are possible. The extension is not hardened for production. Do tests in your own environment before you use the extension. Give feedback and contributions through GitHub.

## Installation

> **Note:** This extension is not in DuckDB's extension repository. Build the extension from source and load the local extension file.

### Prerequisites

- [`uv`](https://docs.astral.sh/uv/) — controls the build and the Python tools.
- A C++17 toolchain and CMake.
- [Ninja](https://ninja-build.org/) — the build uses the Ninja generator by default. Use `GEN=make` to use Make.
- `ccache` (optional) — makes incremental builds faster.

### Build and load (CLI)

1. Build the release extension:

   ```bash
   uv run make release
   ```

2. Start the bundled shell — the extension is already loaded:

   ```bash
   ./build/release/duckdb
   ```

To use a different DuckDB shell, start it with `-unsigned` and load the extension file:

```sql
LOAD './build/release/extension/url_tools/url_tools.duckdb_extension';
```

### Python

```python
import duckdb

con = duckdb.connect(":memory:", config={"allow_unsigned_extensions": "true"})
con.execute("LOAD './build/release/extension/url_tools/url_tools.duckdb_extension'")
```

## Quick Start

```sql
SELECT url_components('https://example.com:8443/path?utm_source=duckdb&id=1&id=2#top');
-- {'scheme': https, 'host': example.com, 'port': 8443, 'path': /path, 'query': 'utm_source=duckdb&id=1&id=2', 'fragment': top}

SELECT query_params('myapp://open?screen=cart&promo=x&promo=y', 'last');
-- {screen=cart, promo=y}

SELECT url_domain('https://m.ozon.ru/p/1'), url_domain('https://shop.example.co.uk/');
-- ozon.ru, example.co.uk

SELECT query_params_loose('https://shop.ru/#/cart?utm_source=push', 'last');
-- {utm_source=push}
```

## Query parameter semantics

`url_components`, `query_params`, `query_params_from_string`, `query_params_loose`, and `query_param` read the query through the same `query_values` axis. The axis selects **how the values are reported**. The key set and the key order (first occurrence) are the same in each mode, and each key appears exactly one time.

| `query_values` | result | values |
|---|---|---|
| `'raw'` | `VARCHAR` (the `query` field) | the undecoded query string — no parse and no decode, so this form is the cheapest (`url_components` only) |
| `'all'` | `MAP(VARCHAR, VARCHAR[])` | each value of each key, in occurrence order (a key with one value gives a one-element list) |
| `'first'` / `'last'` | `MAP(VARCHAR, VARCHAR)` | the first / last value of each key |

The default of each function is the mode with the lowest cost:

| function | default | accepts |
|---|---|---|
| `url_components` | `'raw'` | `'raw'`, `'first'`, `'last'`, `'all'` |
| `query_params`, `query_params_from_string`, `query_params_loose` | `'all'` | `'first'`, `'last'`, `'all'` |
| `query_param` | `'last'` | `'first'`, `'last'` — a scalar result has no `'all'` form |

**`query_values` selects the result type, so it must be a constant.** An unknown mode, a `NULL`, or a column reference causes a bind-time error. You can also give each optional argument by name, the same as in core DuckDB:

```sql
SELECT query_params_from_string('a=1&b=2', query_values := 'last');   -- leaves `sep` at its default
SELECT url_components('https://example.com/?id=1&id=2', query_values := 'all');
```

The values decode with WHATWG form semantics: percent-escapes, and `+` as a space. A pair without `=` is a key with no value. So `?flag` and `?flag=` are the same pair, and both values are `NULL` — an empty value never becomes an empty string. The one empty string in the output is a map key (`?=1` → `{'': '1'}`): that key is present, and DuckDB does not permit a `NULL` map key. Percent-decoded bytes that are not valid UTF-8 become U+FFFD, so the output is always valid UTF-8.

```sql
SELECT query_params('https://example.com/?flag&empty=&a=1', 'last');
-- {flag=NULL, empty=NULL, a=1}
```

A `MAP` result holds the same object that a JSON string would hold, with no serialize/parse round trip. `CAST(m AS JSON)` gives the JSON text when you need it:

```sql
SELECT CAST(query_params_from_string('utm_source=yandex&plus=a+b', query_values := 'last') AS JSON);
-- {"utm_source":"yandex","plus":"a b"}
```

## Function reference

Parse a URL:

- [`url_components(text[, query_values])`](#url_components) — the WHATWG components of a URL as a `STRUCT`, with the query raw or parsed into a `MAP`.
- [`url_scheme`, `url_host`, `url_port`, `url_path`, `url_query`, `url_fragment`](#component-accessors) — one component, without the struct.
- [`url_domain(text)`](#url_domain) — the registrable domain (eTLD+1) of the URL's host, from a compiled-in Public Suffix List.

Extract query parameters:

- [`query_params(text[, query_values])`](#query_params) — the decoded parameters of a URL as a `MAP`.
- [`query_param(text, key[, query_values])`](#query_param) — the decoded value of one key as `VARCHAR`.
- [`query_params_from_string(text[, sep[, query_values]])`](#query_params_from_string) — the parameters of a bare query string, with an optional custom pair separator.
- [`query_params_loose(text[, query_values])`](#query_params_loose) — the parameters of a string that has them but is not a well-formed URL (an SPA fragment, a page title, a bare query string).

The four functions that make parameters share the [`query_values` axis and the decoding rules](#query-parameter-semantics) above.

### `url_components`

`url_components` parses a URL into a `STRUCT` of its WHATWG components. An absolute URL of each scheme gives all fields. A relative path (`/path?q=1`) gives `NULL` scheme, host, and port. A component that the URL does not have, and a component that is empty, are `NULL` — the extension never returns an empty string. Input that does not parse gives `NULL`.

```sql
SELECT url_components('https://example.com:8443/path?utm_source=duckdb&id=1&id=2#top');
-- {'scheme': https, 'host': example.com, 'port': 8443, 'path': /path, 'query': 'utm_source=duckdb&id=1&id=2', 'fragment': top}
```

In the default `'raw'` mode, the struct is `STRUCT(scheme, host, port USMALLINT, path, query, fragment)`, and `query` is the undecoded query string. In the `'first'` / `'last'` / `'all'` modes, a `query_params` `MAP` takes the place of the `query` field — see [Query parameter semantics](#query-parameter-semantics):

```sql
SELECT url_components('/search?q=%D0%BB&tab=products', 'last');
-- {'scheme': NULL, 'host': NULL, 'port': NULL, 'path': /search, 'query_params': {q=л, tab=products}, 'fragment': NULL}

SELECT (url_components('https://example.com/?id=1&id=2', query_values := 'all')).query_params;
-- {id=[1, 2]}

SELECT (url_components('https://example.com/?utm_source=duckdb', 'last')).query_params['utm_source'];
-- duckdb
```

`port` is `NULL` when the URL has no port and when the port is the default port of the scheme: `https://x.com:443/` → `NULL`, `https://x.com:8443/` → `8443`.

### Component accessors

`url_scheme(text)`, `url_host(text)`, `url_path(text)`, `url_query(text)`, `url_fragment(text)` → `VARCHAR`, and `url_port(text)` → `USMALLINT`.

Each accessor returns one component of a URL. It does not build the struct and does not touch the query parameters. Use these functions when you read a single field.

```sql
SELECT url_host('https://example.com:8443/path?a=1#top'), url_port('https://example.com:8443/path?a=1#top');
-- example.com, 8443

SELECT url_path('/search?q=1'), url_scheme('/search?q=1'), url_query('https://example.com/p');
-- /search, NULL, NULL
```

Each accessor is exactly the field of `url_components(url)` that has the same name, `NULL`s included. `url_scheme`, `url_host`, and `url_port` are `NULL` for a relative path. Each component that the URL does not have is `NULL`. Each accessor is `NULL` for input that does not parse.

### `url_domain`

`url_domain` returns the registrable domain (eTLD+1) of the URL's host — the meaning of "one site" in a `GROUP BY`.

```sql
SELECT url_domain('https://m.ozon.ru/p/1'), url_domain('https://shop.example.co.uk/'), url_domain('http://localhost/');
-- ozon.ru, example.co.uk, NULL
```

`https://m.ozon.ru/p` and `https://ozon.ru/` both give `ozon.ru`. `shop.example.co.uk` gives `example.co.uk`. `alice.github.io` gives `alice.github.io`, because `github.io` is a public suffix.

The answer is `NULL` where no registrable domain exists:

- an IP-literal host (`192.168.0.1`, `[::1]`);
- a host that *is* a public suffix (`co.uk`);
- a single label (`localhost`);
- a host with an empty label (`https://foo..example.com/`, `http://x.com../`) — the parser accepts such a host, but no name registers under it;
- relative input, and input that does not parse.

The host is the parser's serialization. So an internationalized domain answers in punycode (`https://кто.рф/` → `xn--j1ail.xn--p1ai`).

The suffixes come from a [Public Suffix List](https://publicsuffix.org/list/) snapshot that is compiled into the extension. The wildcard and exception rules are included, so `a.foo.ck` → `a.foo.ck` and `x.www.ck` → `www.ck`. The extension gets nothing at run time, and a given binary always gives the same answers. A snapshot refresh is a deliberate step (see [docs/UPDATING.md](docs/UPDATING.md)).

### `query_params`

`query_params` extracts the decoded query parameters of a URL as a `MAP`. It accepts the same inputs as `url_components`. Input without a query that parses gives an empty map.

```sql
SELECT query_params('myapp://open?screen=cart&promo=x&promo=y');
-- {screen=[cart], promo=[x, y]}

SELECT query_params('myapp://open?screen=cart&promo=x&promo=y', 'last');
-- {screen=cart, promo=y}
```

The default `query_values` is `'all'` — see [Query parameter semantics](#query-parameter-semantics).

### `query_param`

`query_param` returns the decoded value of one key as `VARCHAR`. It does not build a map.

```sql
SELECT query_param('https://example.com/?utm_source=duckdb&id=1', 'utm_source');
-- duckdb
```

`query_values` is `'last'` (the default) or `'first'`. A scalar result has no `'all'` form — use `query_params(url, 'all')` for that. An absent key, a key with no value (`?flag`), and a key with an empty value (`?a=`) each give `NULL`. To see that a key is present, use `map_contains` or `map_keys` on the `query_params` map.

### `query_params_from_string`

`query_params_from_string` parses a bare query string (`utm_source=x&utm_medium=y`, with no URL around it) into the same `MAP`. A leading `?` is permitted.

```sql
SELECT query_params_from_string('utm_source=yandex&plus=a+b', query_values := 'last');
-- {utm_source=yandex, plus=a b}
```

The optional pair separator `sep` (default `&`) is for formats such as `key=v1|key2=v2`:

```sql
SELECT query_params_from_string('wp1=fb_smm|wp2=post+15%2F06', '|', 'last');
-- {wp1=fb_smm, wp2=post 15/06}
```

A custom separator changes nothing else — the same WHATWG form decoding applies.

### `query_params_loose`

`query_params_loose` extracts the parameters of a string that has them but is not a well-formed URL: a single-page-app fragment (`https://shop.ru/#/cart?utm_source=push`), a page title with a query tail (`Заголовок?utm_source=qr`), a bare query string (`utm_source=x&utm_medium=y`). The result is the same `MAP` with the same `query_values` axis as `query_params`. The pair separator is `&`.

```sql
SELECT query_params_loose('https://shop.ru/#/cart?utm_source=push', 'last');
-- {utm_source=push}

SELECT query_params_loose('Заголовок страницы?utm_source=qr', 'last'), query_params_loose('https://shop.ru/p#top');
-- {utm_source=qr}, {}
```

The rules, in order:

1. **The input parses as a URL.** The fragment gives the base parameters, and the query overrides them: a key that the query has takes only the query's values, and the keys that only the query has come last.

   ```sql
   SELECT query_params_loose('https://shop.ru/?utm_source=url#/cart?utm_source=frag&promo=x', 'last');
   -- {utm_source=url, promo=x}   -- the query overrides the fragment
   ```

   The text before the fragment's first `?` decides what the fragment contributes:
   - if that text has a `=`, the fragment *is* a query string: the full fragment is parsed, and a `?` inside a value is not a separator — `#access_token=t&next=/page?x=1` → `{access_token: t, next: '/page?x=1'}`;
   - if not, the fragment's **first** `?` starts the parameters, and all of the text after it is the query — `#/cart?utm_source=push&next=/a?b=1` → `{utm_source: push, next: '/a?b=1'}` (the query starts at the first `?`; a later `?` is a character inside a value);
   - with no `=` and no `?`, the fragment contributes nothing.

   ```sql
   SELECT query_params_loose('https://shop.ru/cb#access_token=t&next=/page?x=1', 'last');
   -- {access_token=t, next='/page?x=1'}   -- the fragment IS the query string
   ```

2. **The input does not parse.** The text after its first `?` is the query string. If the input has no `?` but has a `=`, the full input is the query string.

3. **Neither.** The result is an empty map. The `=` requirement keeps plain anchors (`#top`) and prose out of the result.

For a URL whose fragment has no `?` and no `=`, `query_params_loose` is exactly `query_params`.

## Development

All build and tool commands run through [uv](https://docs.astral.sh/uv/): run each `make` target as `uv run make ...`, so that the pinned formatter and the Python scripts are on PATH.

### Build from source

```bash
uv run make release
```

The build makes these binaries in `./build/release`:

- `duckdb` — a shell with the extension already loaded.
- `test/unittest` — the test runner.
- `extension/url_tools/url_tools.duckdb_extension` — the extension binary for distribution.

### Tests

```bash
uv run make verify
```

`verify` is the pre-PR gate. It builds and runs the SQLLogic suite two times — one time against `release`, and one time against `relassert` (the same optimized build, with DuckDB's assertions compiled in) — and then does the format check. The second run is not redundant: `release` removes `D_ASSERT`, so only an assert build examines DuckDB's internal contracts (vector types, validity, `string_t` lifetimes). A warm run takes seconds; the first run pays for the extra build.

> [!NOTE]
> `make test` alone only runs the test binary that is already built. Without a `release` build before it, it tests a stale build.

Three harnesses are outside the SQLLogic suite (see [test/README.md](test/README.md)):

```bash
uv run --frozen test/property/url_tools_property.py   # hypothesis fuzzing of the totality contract
uv run --frozen test/wpt/run_wpt_corpus.py            # the WHATWG conformance corpus
uv run --frozen test/plan/run_plan_roundtrip.py       # bound-plan serialization round-trip
```

The property harness fuzzes the totality contract ("junk gives `NULL` or an empty map, never an error"). The WPT corpus pins `url_components` to the browser-vendor conformance suite. The plan round trip protects the named-argument spelling, which requires that aliases survive a re-bind.

### Benchmarks

Performance is important for this extension. The harness gates `url_tools` against a stored baseline, and compares it with [netquack](https://github.com/hatamiarash7/duckdb-netquack) and stock DuckDB SQL.

```bash
uv run --frozen python bench/run_benchmarks.py       # full run -> bench/results/latest.json
uv run --frozen python bench/compare_results.py      # latest vs bench/results/baseline.json
```

[bench/README.md](bench/README.md) documents the operation set, the comparison contracts, and the result-reading rules.

## Contributing

Contributions and feedback are welcome. Please:

1. Open an issue first to discuss the changes.
2. Add or update SQLLogic tests in `test/sql/` for new behavior. Cover junk, `NULL`, and empty-string input together with the happy path.
3. Run `uv run make verify` before you send a pull request. CI also runs `uv run make tidy-check`, which requires a local clang-tidy and compile-database setup.

See [GitHub Issues](https://github.com/Flamefork/duckdb-url-tools/issues) for current tasks and feature requests.

## License

MIT. See [LICENSE](LICENSE).

For third-party components and their licenses, see [THIRD_PARTY_NOTICES.md](docs/THIRD_PARTY_NOTICES.md).

---
*This extension is based on the [DuckDB Extension Template](https://github.com/duckdb/extension-template).*
