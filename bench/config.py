from pathlib import Path

BENCH_DIR = Path(__file__).parent
DATA_DIR = BENCH_DIR / "data"
RESULTS_DIR = BENCH_DIR / "results"
PROJECT_ROOT = BENCH_DIR.parent

URL_TOOLS_EXTENSION_PATH = PROJECT_ROOT / "build" / "release" / "extension" / "url_tools" / "url_tools.duckdb_extension"

SCHEMA_VERSION = 1
BENCH_SEED = 42
DEFAULT_RUNS = 5
DEFAULT_THREADS = [1, 8]
DEFAULT_TOLERANCE_PCT = 10.0
DEFAULT_MIN_EFFECT_MS = 1.0

SIZES = {
    "10k": 10_000,
    "100k": 100_000,
    "1m": 1_000_000,
}
DEFAULT_SIZES = ["100k", "1m"]

# Targets are separate connections: each loads only what its stack needs, so one
# target's extensions cannot help or hinder another's plan.
TARGETS = {
    "url_tools": {"extensions": ["json"], "load_url_tools": True},
    "netquack": {"extensions": ["netquack"], "load_url_tools": False},
    "native": {"extensions": [], "load_url_tools": False},
}

# The loose extraction hand-rolled without the function, the way the sole consumer's `utm_params_json`
# macro had to write it: find the fragment, shape its pseudo-query, then take the query tail — the
# same string walked three times, where query_params_loose walks it once. Spelled in stock SQL (the
# `native` target loads nothing), and written to mirror the loose CONTRACT, the '=' rule included, so
# the two sides compute one answer and the comparison stays a comparison.
LOOSE_FRAGMENT = "regexp_extract(url, '#(.*)$', 1)"
LOOSE_FRAGMENT_QUERY = (
    f"CASE WHEN strpos({LOOSE_FRAGMENT}, '?') > 0 THEN regexp_extract({LOOSE_FRAGMENT}, '[^?]*$') "
    f"WHEN strpos({LOOSE_FRAGMENT}, '=') > 0 THEN {LOOSE_FRAGMENT} ELSE '' END"
)
LOOSE_QUERY = (
    "CASE WHEN strpos(url, '?') > 0 THEN regexp_extract(url, '\\?([^#]*)', 1) "
    "WHEN strpos(url, '=') > 0 AND NOT regexp_matches(url, '^([a-zA-Z][a-zA-Z0-9+.-]*:|/)') THEN url "
    "ELSE '' END"
)


def loose_entries(source: str) -> str:
    return (
        "map_from_entries([(split_part(p, '=', 1), split_part(p, '=', 2)) "
        f"for p in str_split({source}, '&') if p <> ''])"
    )


# One entry per operation; `targets` maps target label to the timed expression
# over the `_bench_in` temp table (column named by `column`). Only targets with a
# comparable practical form are present — the absence of a target IS the
# statement that the stack has no comparable spelling of the task.
#
# Contract differences that stay in (documented, gate rows avoid them):
# - `native` extracts raw (undecoded) values; url_tools decodes per WHATWG and
#   netquack's url_decode matches it on %XX and '+'.
# - On junk input url_tools yields NULL, netquack/native yield ''.
# - The map operations time the 'last' mode: MAP(VARCHAR, VARCHAR) is the shape
#   the netquack/native emulations produce, so the comparison stays a comparison.
OPERATIONS = {
    "host": {
        "input": "urls",
        "column": "url",
        "targets": {
            "url_tools": "url_host(url)",
            "netquack": "extract_host(url)",
            "native": "regexp_extract(url, '^[a-zA-Z][a-zA-Z0-9+.-]*://(?:[^/?#@]*@)?([^/?#:]+)', 1)",
        },
    },
    # Both sides consult a Public Suffix List, which is the only way to answer this at all — stock SQL
    # has no comparable spelling, and its absence here is that statement.
    "domain": {
        "input": "urls",
        "column": "url",
        "targets": {
            "url_tools": "url_domain(url)",
            "netquack": "extract_domain(url)",
        },
    },
    "path": {
        "input": "urls",
        "column": "url",
        "targets": {
            "url_tools": "url_path(url)",
            "netquack": "extract_path(url)",
            "native": "regexp_extract(url, '^[a-zA-Z][a-zA-Z0-9+.-]*://[^/?#]*([^?#]*)', 1)",
        },
    },
    "query_param": {
        "input": "urls",
        "column": "url",
        "targets": {
            "url_tools": "query_param(url, 'utm_source')",
            "netquack": "url_decode(regexp_extract(extract_query_string(url), '(?:^|&)utm_source=([^&]*)', 1))",
            "native": "regexp_extract(url, '[?&]utm_source=([^&#]*)', 1)",
        },
    },
    "query_params_all": {
        "input": "urls",
        "column": "url",
        "targets": {
            "url_tools": "query_params(url, 'last')",
            "netquack": "(SELECT map(list(key), list(value)) FROM extract_query_parameters(_bench_in.url))",
            "native": (
                "map_from_entries([(split_part(p, '=', 1), split_part(p, '=', 2)) "
                "for p in str_split(nullif(regexp_extract(url, '\\?([^#]*)', 1), ''), '&')])"
            ),
        },
    },
    "components": {
        "input": "urls",
        "column": "url",
        "targets": {
            "url_tools": "url_components(url)",
        },
    },
    "utm_loose": {
        "input": "urls",
        "column": "url",
        "targets": {
            "url_tools": "query_params_loose(url, 'last')",
            "native": f"map_concat({loose_entries(LOOSE_FRAGMENT_QUERY)}, {loose_entries(LOOSE_QUERY)})",
        },
    },
    "params_from_string": {
        "input": "query_strings",
        "column": "qs",
        "targets": {
            "url_tools": "query_params_from_string(qs, '&', 'last')",
            "native": (
                "map_from_entries([(split_part(p, '=', 1), split_part(p, '=', 2)) "
                "for p in str_split(nullif(qs, ''), '&')])"
            ),
        },
    },
}

# Curated rows where every target's contract overlaps exactly (absolute http(s),
# non-empty path, utm_source present, no percent-encoding, no duplicate keys):
# the correctness gate asserts all targets agree here before anything is timed.
GATE_URLS = [
    "https://example.com/path/to?utm_source=news&q=duck",
    "https://sub.shop.co.uk:8443/catalog/item?utm_source=email&id=42#frag",
    "http://example.org/a/b/c?utm_source=cpc&ref=x",
]
GATE_QUERY_STRINGS = [
    "utm_source=news&q=duck",
    "utm_source=email&id=42",
]
