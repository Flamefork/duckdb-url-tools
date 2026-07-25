# Benchmark harness

This directory serves two purposes:

- catch performance regressions in `url_tools` against a stored baseline;
- compare `url_tools` operations against [netquack](https://github.com/hatamiarash7/duckdb-netquack)
  (the community URL extension) and stock DuckDB SQL on comparable tasks.

## Quick start

```bash
uv run make release                                  # the bench loads this build
uv run --frozen python bench/run_benchmarks.py       # full run -> bench/results/latest.json
uv run --frozen python bench/compare_results.py      # latest vs bench/results/baseline.json
```

`run_benchmarks.py` generates any missing parquet files in `bench/data/`, runs a
correctness gate, and times every case. The first run needs network access to
install `netquack` from the community repository (`--skip-netquack` benches
without it).

Useful flags:

```bash
uv run --frozen python bench/run_benchmarks.py --filter host --sizes 10k --runs 3 --threads 1
```

Every case runs once per DuckDB thread count (default `--threads 1,8`) so
single-thread cost and parallel scaling are visible side by side.

## Regression workflow

`bench/results/baseline.json` is committed on purpose; `latest.json` and
`bench/data/` are generated and git-ignored. After a performance-relevant change:

```bash
uv run --frozen python bench/run_benchmarks.py
uv run --frozen python bench/compare_results.py
```

`compare_results.py` exits non-zero when a **single-threaded** `url_tools` case
is slower than the baseline beyond `--tolerance-pct` (default 10%) and
`--min-effect-ms` (default 1 ms).

Multi-threaded rows are shown but never gate: on a machine that is not idle they
swing 18–36% run to run for the same binary (measured over five back-to-back
runs), and the stock-SQL rows drift with them — that is CPU contention, not this
extension's code. A gate that can be tripped by a background VM is a coin flip,
so parallel scaling stays informational. netquack/native rows never gate either.

Stored baselines drift with machine state — treat a flagged regression as a hint
and confirm by re-running both builds in one session. To refresh the baseline
after an intentional change, run the benchmarks **on an idle machine** (check
`uptime` first: a load average anywhere near the core count invalidates the run),
copy `latest.json` over `baseline.json`, and commit it saying why it was
refreshed.

## Operation set

Only targets with a comparable practical spelling of a task are present; a
missing cell means that stack has no comparable form.

| operation | url_tools | netquack | native (stock SQL) |
|---|---|---|---|
| `host` | `url_host(url)` | `extract_host(url)` | `regexp_extract(...)` |
| `path` | `url_path(url)` | `extract_path(url)` | `regexp_extract(...)` |
| `query_param` | `query_param(url, 'utm_source')` | `url_decode(regexp_extract(extract_query_string(url), ...))` | `regexp_extract(...)` |
| `query_params_all` | `query_params(url, 'last')` | `map(...)` over the `extract_query_parameters` table function | `map_from_entries(...)` over `str_split` |
| `components` | `url_components(url)` | — | — |
| `utm_loose` | `query_params_loose(url, 'last')` | — | `map_concat(...)` over two `regexp_extract`/`str_split` passes |
| `params_from_string` | `query_params_from_string(qs, '&', 'last')` | — | `map_from_entries(...)` over `str_split` |

`utm_loose` is the operation the sole consumer hand-rolled before `query_params_loose`
existed (rick/data's `utm_params_json` macro): find the fragment, shape its
pseudo-query, take the query tail — the same string walked three times. The
`native` spelling mirrors the loose *contract* — the fragment rule (a `=` before
the fragment's first `?` means the fragment *is* a query string, otherwise the
tail after that first `?`) and the key ending at the first `=` included — so both
sides compute one answer; only the number of passes differs.

Contract differences that stay in (the correctness gate pins agreement on
curated rows where the contracts overlap exactly):

- `native` extracts **raw** (undecoded) values; `url_tools` decodes per WHATWG
  form semantics, and netquack's `url_decode` matches that on `%XX` and `+`.
- `url_tools` returns no empty strings at all: junk input, a component the URL
  does not carry (`url_query` of a URL with no query) and a parameter with no
  value (`?flag`) are alike `NULL`, and a parameter result for junk is an empty
  MAP. netquack/native yield `''` throughout. The gate rows are curated so the
  two contracts overlap exactly, which is why the difference stays out of them.
- The map operations time the `'last'` mode: `MAP(VARCHAR, VARCHAR)` is what the
  netquack/native emulations build, so the two sides stay comparable. `'all'`
  (the default) has no counterpart in either stack.
- The generated data has no duplicate query keys: the MAP-based emulations
  error on duplicates, and last-wins correctness is owned by the property
  harness, not the bench.
- The **timed** `urls` corpus carries plain anchors (`#frag`), not single-page-app
  fragments, so `utm_loose` times the merge on realistic query-carrying URLs
  rather than on the fragment path. Whether the fragment is worth parsing is
  decided by two `string_view` scans either way. The **gate** rows do carry the
  fragment shapes (SPA route, OAuth-style fragment, a fragment key the query
  overrides): timing them is pointless, but agreeing on them is not — that branch
  is the reason `query_params_loose` exists.
- Every gate URL carries `utm_source` in its **query**: the regex emulations
  cannot tell a query from a fragment, so a URL carrying it only in the fragment
  would make `query_param` disagree about the NULL-vs-`''` difference above
  rather than about the operation.

Read results relative within one run (same machine state); cross-session
absolute numbers are not comparable.
