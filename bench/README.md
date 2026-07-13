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

`compare_results.py` exits non-zero when a `url_tools` case is slower than the
baseline beyond `--tolerance-pct` (default 10%) and `--min-effect-ms` (default
1 ms). netquack/native rows are shown for context and never gate. Stored
baselines drift with machine state — treat a flagged regression as a hint and
confirm by re-running both builds in one session. To refresh the baseline after
an intentional change, copy `latest.json` over `baseline.json` and commit it.

## Operation set

Only targets with a comparable practical spelling of a task are present; a
missing cell means that stack has no comparable form.

| operation | url_tools | netquack | native (stock SQL) |
|---|---|---|---|
| `host` | `(url_components(url)).host` | `extract_host(url)` | `regexp_extract(...)` |
| `path` | `(url_components(url)).path` | `extract_path(url)` | `regexp_extract(...)` |
| `query_param` | `query_param(url, 'utm_source')` | `url_decode(regexp_extract(extract_query_string(url), ...))` | `regexp_extract(...)` |
| `query_params_all` | `query_params(url, 'last')` | `map(...)` over the `extract_query_parameters` table function | `map_from_entries(...)` over `str_split` |
| `components` | `url_components(url)` | — | — |
| `params_from_string` | `query_params_from_string(qs, '&', 'last')` | — | `map_from_entries(...)` over `str_split` |

Contract differences that stay in (the correctness gate pins agreement on
curated rows where the contracts overlap exactly):

- `native` extracts **raw** (undecoded) values; `url_tools` decodes per WHATWG
  form semantics, and netquack's `url_decode` matches that on `%XX` and `+`.
- On junk input `url_tools` yields `NULL` for component-shaped results and an
  empty MAP for parameter results; netquack/native yield `''`.
- The map operations time the `'last'` mode: `MAP(VARCHAR, VARCHAR)` is what the
  netquack/native emulations build, so the two sides stay comparable. `'all'`
  (the default) has no counterpart in either stack.
- The generated data has no duplicate query keys: the MAP-based emulations
  error on duplicates, and last-wins correctness is owned by the property
  harness, not the bench.

Read results relative within one run (same machine state); cross-session
absolute numbers are not comparable.
