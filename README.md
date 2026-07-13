# DuckDB URL Tools Extension

[![License](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Main Extension Distribution Pipeline](https://github.com/Flamefork/duckdb-url-tools/actions/workflows/MainDistributionPipeline.yml/badge.svg)](https://github.com/Flamefork/duckdb-url-tools/actions/workflows/MainDistributionPipeline.yml)

A DuckDB extension for parsing URLs and query strings.

Built on [ada](https://github.com/ada-url/ada), the WHATWG-compliant URL parser used by Node.js and ClickHouse. All functions are total over arbitrary input: junk values yield `NULL` or an empty map instead of an error, so one malformed value cannot fail a whole scan.

> [!WARNING]
> This extension is maintained on a best-effort basis by a developer who doesn't write C++ professionally, so expect rough edges. It hasn't been hardened for production, and you should validate it in your own environment before relying on it. Feedback and contributions are welcome via GitHub.

## Functions

Parse a URL:

- [`url_components(text[, query_values])`](#url_components) — a URL's WHATWG components as a `STRUCT`, with the query either raw or already parsed into a `MAP`.
- [`url_scheme`, `url_host`, `url_port`, `url_path`, `url_query`, `url_fragment`](#component-accessors) — one component, without building the struct.
- [`url_domain(text)`](#url_domain) — the registrable domain (eTLD+1) of the URL's host, from a compiled-in Public Suffix List.

Extract query parameters:

- [`query_params(text[, query_values])`](#query_params) — decoded parameters of a URL as a `MAP`.
- [`query_param(text, key[, query_values])`](#query_param) — the decoded value of one key as a `VARCHAR`, without building a map.
- [`query_params_from_string(text[, sep[, query_values]])`](#query_params_from_string) — parameters of a bare query string, with an optional custom pair separator.
- [`query_params_loose(text[, query_values])`](#query_params_loose) — parameters of a string that *carries* them without being a well-formed URL (SPA fragments, page titles, bare query strings).

The four parameter-producing functions share one [`query_values` axis and one set of decoding rules](#query-parameter-semantics).

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

## Installation

> [!NOTE]
> This extension is not published in DuckDB's extension repository. Build it from source and load the produced local binary.

### Prerequisites

- [`uv`](https://docs.astral.sh/uv/) — drives the build and the Python tooling.
- A C++17 toolchain and CMake.
- [Ninja](https://ninja-build.org/) — the build defaults to the Ninja generator; pass `GEN=make` to fall back to Make.
- `ccache` (optional) — speeds up incremental rebuilds.

### CLI

```bash
uv run make release
./build/release/duckdb          # a shell with the extension already loaded
```

To load the binary into another DuckDB shell, start it with `-unsigned`:

```sql
LOAD './build/release/extension/url_tools/url_tools.duckdb_extension';
```

### Python

```python
import duckdb

con = duckdb.connect(":memory:", config={"allow_unsigned_extensions": "true"})
con.execute("LOAD './build/release/extension/url_tools/url_tools.duckdb_extension'")
```

## Usage

### Query parameter semantics

`url_components`, `query_params`, `query_params_from_string`, `query_params_loose` and `query_param` all read the query through the same `query_values` axis. The axis selects **how values are reported**; the key set and its order (first occurrence) are the same in every mode, and every key appears exactly once.

| `query_values` | result | values |
|---|---|---|
| `'raw'` | `VARCHAR` (the `query` field) | the undecoded query string — nothing is parsed or decoded, so this is the cheapest form (`url_components` only) |
| `'all'` | `MAP(VARCHAR, VARCHAR[])` | every value of every key, in occurrence order (a single-valued key is a one-element list) |
| `'first'` / `'last'` | `MAP(VARCHAR, VARCHAR)` | the first / last value of every key |

Each function defaults to the mode that costs it least:

| function | default | accepts |
|---|---|---|
| `url_components` | `'raw'` | `'raw'`, `'first'`, `'last'`, `'all'` |
| `query_params`, `query_params_from_string`, `query_params_loose` | `'all'` | `'first'`, `'last'`, `'all'` |
| `query_param` | `'last'` | `'first'`, `'last'` — a scalar has no `'all'` result |

**`query_values` selects the result type, so it must be a constant.** An unknown mode, a `NULL`, or a column reference is a bind-time error. Any optional argument may also be passed by name, following the core DuckDB idiom:

```sql
SELECT query_params_from_string('a=1&b=2', query_values := 'last');   -- leaves `sep` at its default
SELECT url_components('https://example.com/?id=1&id=2', query_values := 'all');
```

Values decode per WHATWG form semantics: percent-escapes, and `+` as space. Percent-decoded bytes that are not valid UTF-8 are sanitized with U+FFFD, so the output is always valid UTF-8.

A `MAP` result carries the same object a JSON string would, without the serialize/parse round trip — and `CAST(m AS JSON)` still gives you the JSON spelling when you want it:

```sql
SELECT CAST(query_params_from_string('utm_source=yandex&plus=a+b', query_values := 'last') AS JSON);
-- {"utm_source":"yandex","plus":"a b"}
```

### `url_components`

Parses a URL into a `STRUCT` of its WHATWG components. Absolute URLs of any scheme yield all fields; relative paths (`/path?q=1`) yield `NULL` scheme/host/port; unparseable input yields `NULL`.

```sql
SELECT url_components('https://example.com:8443/path?utm_source=duckdb&id=1&id=2#top');
-- {'scheme': https, 'host': example.com, 'port': 8443, 'path': /path, 'query': 'utm_source=duckdb&id=1&id=2', 'fragment': top}
```

Under the default `'raw'` mode the struct is `STRUCT(scheme, host, port USMALLINT, path, query, fragment)`, with `query` the undecoded query string. Under `'first'` / `'last'` / `'all'` the `query` field is replaced by a `query_params` `MAP` — see [Query parameter semantics](#query-parameter-semantics):

```sql
SELECT url_components('/search?q=%D0%BB&tab=products', 'last');
-- {'scheme': NULL, 'host': NULL, 'port': NULL, 'path': /search, 'query_params': {q=л, tab=products}, 'fragment': ''}

SELECT (url_components('https://example.com/?id=1&id=2', query_values := 'all')).query_params;
-- {id=[1, 2]}

SELECT (url_components('https://example.com/?utm_source=duckdb', 'last')).query_params['utm_source'];
-- duckdb
```

`port` is `NULL` when the URL carries no port or the port is the scheme's default: `https://x.com:443/` → `NULL`, `https://x.com:8443/` → `8443`.

### Component accessors

`url_scheme(text)`, `url_host(text)`, `url_path(text)`, `url_query(text)`, `url_fragment(text)` → `VARCHAR`, and `url_port(text)` → `USMALLINT`.

Each returns one component of a URL without building the struct or touching the query parameters — reach for these when you want a single field.

```sql
SELECT url_host('https://example.com:8443/path?a=1#top'), url_port('https://example.com:8443/path?a=1#top');
-- example.com, 8443

SELECT url_path('/search?q=1'), url_scheme('/search?q=1'), url_query('https://example.com/p');
-- /search, NULL, ''
```

Each accessor is exactly the same-named field of `url_components(url)`, NULLs included: `url_scheme` / `url_host` / `url_port` are `NULL` for a relative path, `url_query` / `url_fragment` are `''` on a parseable URL that carries none, and every accessor is `NULL` for unparseable input.

### `url_domain`

Returns the registrable domain (eTLD+1) of the URL's host — what "one site" means when you group by it.

```sql
SELECT url_domain('https://m.ozon.ru/p/1'), url_domain('https://shop.example.co.uk/'), url_domain('http://localhost/');
-- ozon.ru, example.co.uk, NULL
```

`https://m.ozon.ru/p` and `https://ozon.ru/` both yield `ozon.ru`; `shop.example.co.uk` yields `example.co.uk`; `alice.github.io` yields `alice.github.io` (`github.io` is a public suffix).

The answer is `NULL` wherever no registrable domain exists:

- an IP-literal host (`192.168.0.1`, `[::1]`);
- a host that *is* a public suffix (`co.uk`);
- a single label (`localhost`);
- a host carrying an empty label — one the parser accepts as it stands but no name registers under (`https://foo..example.com/`, `http://x.com../`);
- relative or unparseable input.

The host is the parser's serialization, so an internationalized domain answers in punycode (`https://кто.рф/` → `xn--j1ail.xn--p1ai`).

The suffixes come from a [Public Suffix List](https://publicsuffix.org/list/) snapshot compiled into the extension (wildcard and exception rules included, so `a.foo.ck` → `a.foo.ck` and `x.www.ck` → `www.ck`). Nothing is fetched at run time, and a given binary always answers the same; refreshing the snapshot is a deliberate act (see [docs/UPDATING.md](docs/UPDATING.md)).

### `query_params`

Extracts the decoded query parameters of a URL (same inputs as `url_components`) as a `MAP`. Input without a parseable query yields an empty map.

```sql
SELECT query_params('myapp://open?screen=cart&promo=x&promo=y');
-- {screen=[cart], promo=[x, y]}

SELECT query_params('myapp://open?screen=cart&promo=x&promo=y', 'last');
-- {screen=cart, promo=y}
```

`query_values` defaults to `'all'`; see [Query parameter semantics](#query-parameter-semantics).

### `query_param`

The decoded value of one key as a `VARCHAR`, without building a map.

```sql
SELECT query_param('https://example.com/?utm_source=duckdb&id=1', 'utm_source');
-- duckdb
```

`query_values` is `'last'` (default) or `'first'`; `'all'` has no scalar result — use `query_params(url, 'all')` for that. An absent key yields `NULL`; a key present with an empty value yields `''`.

### `query_params_from_string`

Parses a bare query string (`utm_source=x&utm_medium=y`, no URL around it) into the same `MAP`. A leading `?` is tolerated.

```sql
SELECT query_params_from_string('utm_source=yandex&plus=a+b', query_values := 'last');
-- {utm_source=yandex, plus=a b}
```

The optional pair separator `sep` (default `&`) covers formats like `key=v1|key2=v2`:

```sql
SELECT query_params_from_string('wp1=fb_smm|wp2=post+15%2F06', '|', 'last');
-- {wp1=fb_smm, wp2=post 15/06}
```

A custom separator changes nothing else: the same WHATWG form decoding applies.

### `query_params_loose`

Extracts parameters from a string that *carries* them without having to be a well-formed URL — a single-page-app fragment (`https://shop.ru/#/cart?utm_source=push`), a page title with a query tail (`Заголовок?utm_source=qr`), a bare query string (`utm_source=x&utm_medium=y`). Same `MAP` and the same `query_values` axis as `query_params`; the pair separator is `&`.

```sql
SELECT query_params_loose('https://shop.ru/#/cart?utm_source=push', 'last');
-- {utm_source=push}

SELECT query_params_loose('Заголовок страницы?utm_source=qr', 'last'), query_params_loose('https://shop.ru/p#top');
-- {utm_source=qr}, {}
```

The rules, in order:

1. **The input parses as a URL** → the fragment supplies the base parameters, and the query overrides them: a key the query carries takes only the query's values, and keys the query alone carries come last.

   ```sql
   SELECT query_params_loose('https://shop.ru/?utm_source=url#/cart?utm_source=frag&promo=x', 'last');
   -- {utm_source=url, promo=x}   -- the query overrides the fragment
   ```

   What the fragment contributes is decided by the text before its first `?`:
   - a `=` there means the fragment already *is* a query string, so the whole of it is parsed and a `?` inside a value is not a separator — `#access_token=t&next=/page?x=1` → `{access_token: t, next: '/page?x=1'}`;
   - otherwise the fragment's **first** `?` opens the parameters and everything after it is the query — `#/cart?utm_source=push&next=/a?b=1` → `{utm_source: push, next: '/a?b=1'}` (a query starts at the first `?`, and a later one is a character inside a value);
   - otherwise the fragment contributes nothing.

   ```sql
   SELECT query_params_loose('https://shop.ru/cb#access_token=t&next=/page?x=1', 'last');
   -- {access_token=t, next='/page?x=1'}   -- the fragment IS the query string
   ```

2. **It does not parse** → everything after its first `?` is the query string, or the whole input is one when it has no `?` but does have a `=`.

3. **Neither** → an empty map. The `=` requirement is what keeps plain anchors (`#top`) and prose out of the result.

On a URL whose fragment carries neither `?` nor `=`, `query_params_loose` is exactly `query_params`.

## Development

Build and tooling commands run through [uv](https://docs.astral.sh/uv/): invoke every `make` target as `uv run make ...` so the pinned formatter and Python scripts are on PATH.

### Building from source

```bash
uv run make release
```

This creates the following binaries in `./build/release`:

- `duckdb` — a shell with the extension pre-loaded.
- `test/unittest` — the test runner.
- `extension/url_tools/url_tools.duckdb_extension` — the distributable extension binary.

### Testing

```bash
uv run make verify
```

`verify` is the pre-PR gate. It builds and runs the SQLLogic suite twice — once against `release`, once against `relassert` (the same optimized build with DuckDB's assertions compiled back in) — then checks formatting. The second run is not redundant: `D_ASSERT` is stripped from `release`, so DuckDB's internal contracts (vector types, validity, `string_t` lifetimes) are only enforced in an assert build. A warm run takes a few seconds; the first one pays for the extra build.

> [!NOTE]
> `make test` alone only runs the already-built test binary — without a preceding `release` it exercises a stale build.

Three harnesses live outside the SQLLogic suite (see [test/README.md](test/README.md)):

```bash
uv run --frozen test/property/url_tools_property.py   # hypothesis fuzzing of the totality contract
uv run --frozen test/wpt/run_wpt_corpus.py            # the WHATWG conformance corpus
uv run --frozen test/plan/run_plan_roundtrip.py       # bound-plan serialization round-trip
```

The totality contract ("junk yields `NULL` or an empty map, never an error") is what the property harness fuzzes; the WPT corpus pins `url_components` against the browser-vendor conformance suite; the plan round-trip guards the named-argument spelling, which relies on aliases surviving a re-bind.

### Benchmarks

Performance matters for this extension: the harness gates `url_tools` against a stored baseline and compares it with [netquack](https://github.com/hatamiarash7/duckdb-netquack) and stock DuckDB SQL.

```bash
uv run --frozen python bench/run_benchmarks.py       # full run -> bench/results/latest.json
uv run --frozen python bench/compare_results.py      # latest vs bench/results/baseline.json
```

The operation set, the comparison contracts, and the result-reading rules are documented in [bench/README.md](bench/README.md).

## Contributing

Contributions and feedback are welcome. Please:

1. Open an issue first to discuss proposed changes.
2. Add or update SQLLogic tests in `test/sql/` for new behavior — cover junk, `NULL` and empty-string input alongside the happy path.
3. Run `uv run make verify` before submitting a pull request. CI additionally runs `uv run make tidy-check`, which needs a local clang-tidy + compile-database setup.

See [GitHub Issues](https://github.com/Flamefork/duckdb-url-tools/issues) for current tasks and feature requests.

## License

MIT. See [LICENSE](LICENSE).

For third-party components and their licenses, see [THIRD_PARTY_NOTICES.md](docs/THIRD_PARTY_NOTICES.md).

---
*This extension is based on the [DuckDB Extension Template](https://github.com/duckdb/extension-template).*
