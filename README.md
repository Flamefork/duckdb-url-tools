# DuckDB URL Tools Extension

[![License](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Main Extension Distribution Pipeline](https://github.com/Flamefork/duckdb-url-tools/actions/workflows/MainDistributionPipeline.yml/badge.svg)](https://github.com/Flamefork/duckdb-url-tools/actions/workflows/MainDistributionPipeline.yml)

A DuckDB extension for parsing URLs and query strings.

Built on [ada](https://github.com/ada-url/ada), the WHATWG-compliant URL parser used by Node.js and ClickHouse. All functions are total over arbitrary input: junk values yield `NULL` or an empty object instead of an error, so one malformed value cannot fail a whole scan.

**WARNING: This extension is maintained on a best-effort basis by a developer who doesn’t write C++ professionally, so expect rough edges. It hasn’t been hardened for production, and you should validate it in your own environment before relying on it. Feedback and contributions are welcome via GitHub.**

## Functions

- **`url_components(text[, query_values])`**: Parses a URL into a `STRUCT` of its WHATWG components. Absolute URLs of any scheme yield all fields; relative paths (`/path?q=1`) yield `NULL` scheme/host/port; unparseable input yields `NULL`. The optional `query_values` argument picks how the query is reported, may be passed by name (`url_components(url, query_values := 'all')`), and must be a constant:
  - `'raw'` (default) — `STRUCT(scheme, host, port USMALLINT, path, query, fragment)`, with `query` the undecoded query string. Nothing is decoded, so this is the cheapest form.
  - `'first'` / `'last'` — `query` is replaced by `query_params MAP(VARCHAR, VARCHAR)`, holding the first / last value of every key.
  - `'all'` — `query_params MAP(VARCHAR, VARCHAR[])`, holding every value of every key in occurrence order.

  `port` is `NULL` when the URL carries no port or the port is the scheme's default (`https://x.com:443/` → `NULL`, `https://x.com:8443/` → `8443`).
- **`query_params(text[, query_values])`**: Extracts decoded query parameters from a URL (same inputs as `url_components`) as a `MAP`. Input without a parseable query yields an empty map. `query_values` is the same axis as above, minus `'raw'`:
  - `'all'` (default) — `MAP(VARCHAR, VARCHAR[])`, every value of every key in occurrence order (a single-valued key is a one-element list).
  - `'first'` / `'last'` — `MAP(VARCHAR, VARCHAR)`, the first / last value of every key.
- **`query_params_from_string(text[, sep[, query_values]])`**: Parses a bare query string (`utm_source=x&utm_medium=y`, no URL around it) into the same `MAP`. A leading `?` is tolerated. The optional pair separator `sep` (default `&`) covers formats like `key=v1|key2=v2`.
- **`query_params_loose(text[, query_values])`**: Extracts parameters from a string that *carries* them without having to be a well-formed URL — a single-page-app fragment (`https://shop.ru/#/cart?utm_source=push`), a page title with a query tail (`Заголовок?utm_source=qr`), a bare query string (`utm_source=x&utm_medium=y`). Same `MAP` and the same `query_values` axis as `query_params`. The rules, in order:
  - the input parses as a URL → the fragment supplies the base parameters, and the query overrides them: a key the query carries takes only the query's values, and keys the query alone carries come last. What the fragment contributes is decided by the text before its first `?`: a `=` there means the fragment already *is* a query string, so the whole of it is parsed and a `?` inside a value is not a separator (`#access_token=t&next=/page?x=1` → `{access_token: t, next: '/page?x=1'}`); otherwise the fragment's **first** `?` opens the parameters and everything after it is the query (`#/cart?utm_source=push&next=/a?b=1` → `{utm_source: push, next: '/a?b=1'}` — a query starts at the first `?`, and a later one is a character inside a value); otherwise the fragment contributes nothing;
  - it does not parse → everything after its first `?` is the query string, or the whole input is one when it has no `?` but does have a `=`;
  - neither → an empty map. The `=` requirement is what keeps plain anchors (`#top`) and prose out of the result.

  The pair separator is `&`. On a URL whose fragment carries neither `?` nor `=`, `query_params_loose` is exactly `query_params`.
- **`query_param(text, key[, query_values])`**: The decoded value of one key, `VARCHAR`, without building a map. `query_values` is `'last'` (default) or `'first'`; `'all'` has no scalar result — use `query_params(url, 'all')`. An absent key yields `NULL`, a key present with an empty value yields `''`.
- **`url_scheme(text)`**, **`url_host(text)`**, **`url_path(text)`**, **`url_query(text)`**, **`url_fragment(text)`** → `VARCHAR`, and **`url_port(text)`** → `USMALLINT`: one component of a URL, without building the struct or touching the query parameters — reach for these when you want a single field. Each is exactly the same-named field of `url_components(url)`, NULLs included: `url_scheme` / `url_host` / `url_port` are `NULL` for a relative path, `url_query` / `url_fragment` are `''` on a parseable URL that carries none, and every accessor is `NULL` for unparseable input.
- **`url_domain(text)`** → `VARCHAR`: the registrable domain (eTLD+1) of the URL's host — what "one site" means when you group by it. `https://m.ozon.ru/p` and `https://ozon.ru/` both yield `ozon.ru`; `shop.example.co.uk` yields `example.co.uk`; `alice.github.io` yields `alice.github.io` (`github.io` is a public suffix). The answer is `NULL` wherever no registrable domain exists: an IP-literal host (`192.168.0.1`, `[::1]`), a host that *is* a public suffix (`co.uk`), a single label (`localhost`), a host carrying an empty label — one the parser accepts as it stands but no name registers under (`https://foo..example.com/`, `http://x.com../`) — and relative or unparseable input. The host is the parser's serialization, so an internationalized domain answers in punycode (`https://кто.рф/` → `xn--j1ail.xn--p1ai`).

  The suffixes come from a [Public Suffix List](https://publicsuffix.org/list/) snapshot compiled into the extension (wildcard and exception rules included, so `a.foo.ck` → `a.foo.ck` and `x.www.ck` → `www.ck`). Nothing is fetched at run time, and a given binary always answers the same; refreshing the snapshot is a deliberate act (see [docs/UPDATING.md](docs/UPDATING.md)).

The key set and its order (first occurrence) are the same in every mode — the mode changes values only. Every key appears exactly once.

Query parameters decode per WHATWG form semantics: percent-escapes and `+` as space; percent-decoded bytes that are not valid UTF-8 are sanitized with U+FFFD.

`query_values` selects the result type, so it must be a constant; an unknown mode, a `NULL`, or a column reference is a bind-time error. Any optional argument may be passed by name (`query_params_from_string(qs, query_values := 'last')` leaves `sep` at its default).

## Quick Start

```sql
LOAD './build/release/extension/url_tools/url_tools.duckdb_extension';

SELECT url_components('https://example.com:8443/path?utm_source=duckdb&id=1&id=2#top');
-- {'scheme': https, 'host': example.com, 'port': 8443, 'path': /path, 'query': 'utm_source=duckdb&id=1&id=2', 'fragment': top}

SELECT url_components('/search?q=%D0%BB&tab=products', 'last');
-- {'scheme': NULL, 'host': NULL, 'port': NULL, 'path': /search, 'query_params': {q=л, tab=products}, 'fragment': ''}

SELECT (url_components('https://example.com/?id=1&id=2', query_values := 'all')).query_params;
-- {id=[1, 2]}

SELECT (url_components('https://example.com/?utm_source=duckdb', 'last')).query_params['utm_source'];
-- duckdb

SELECT query_params('myapp://open?screen=cart&promo=x&promo=y');
-- {screen=[cart], promo=[x, y]}

SELECT query_params('myapp://open?screen=cart&promo=x&promo=y', 'last');
-- {screen=cart, promo=y}

SELECT query_param('https://example.com/?utm_source=duckdb&id=1', 'utm_source');
-- duckdb

SELECT url_host('https://example.com:8443/path?a=1#top'), url_port('https://example.com:8443/path?a=1#top');
-- example.com, 8443

SELECT url_path('/search?q=1'), url_scheme('/search?q=1'), url_query('https://example.com/p');
-- /search, NULL, ''

SELECT url_domain('https://m.ozon.ru/p/1'), url_domain('https://shop.example.co.uk/'), url_domain('http://localhost/');
-- ozon.ru, example.co.uk, NULL

SELECT query_params_loose('https://shop.ru/#/cart?utm_source=push', 'last');
-- {utm_source=push}

SELECT query_params_loose('https://shop.ru/?utm_source=url#/cart?utm_source=frag&promo=x', 'last');
-- {utm_source=url, promo=x}   -- the query overrides the fragment

SELECT query_params_loose('https://shop.ru/cb#access_token=t&next=/page?x=1', 'last');
-- {access_token=t, next='/page?x=1'}   -- the fragment IS the query string

SELECT query_params_loose('Заголовок страницы?utm_source=qr', 'last'), query_params_loose('https://shop.ru/p#top');
-- {utm_source=qr}, {}

SELECT query_params_from_string('utm_source=yandex&plus=a+b', query_values := 'last');
-- {utm_source=yandex, plus=a b}

SELECT query_params_from_string('wp1=fb_smm|wp2=post+15%2F06', '|', 'last');
-- {wp1=fb_smm, wp2=post 15/06}
```

A `MAP` result carries the same object a JSON string would, without the serialize/parse round trip — and `CAST(m AS JSON)` still gives you the JSON spelling when you want it:

```sql
SELECT CAST(query_params_from_string('utm_source=yandex&plus=a+b', query_values := 'last') AS JSON);
-- {"utm_source":"yandex","plus":"a b"}
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
