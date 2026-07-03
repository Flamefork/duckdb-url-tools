import argparse
import json
import time
from pathlib import Path
from statistics import median

import duckdb

from config import DEFAULT_RUNS
from config import DEFAULT_SIZES
from config import DEFAULT_THREADS
from config import GATE_QUERY_STRINGS
from config import GATE_URLS
from config import OPERATIONS
from config import RESULTS_DIR
from config import SCHEMA_VERSION
from config import SIZES
from config import TARGETS
from config import URL_TOOLS_EXTENSION_PATH
from environment import collect_environment
from generate_data import data_path
from generate_data import ensure_data


def bootstrap_extensions(target_labels: set[str]) -> None:
    # One INSTALL pass up front: the per-case connections then only LOAD, which
    # is offline and cheap. netquack comes from the community repository, so the
    # very first run needs network access.
    for label in sorted(target_labels):
        con = duckdb.connect()
        for extension in TARGETS[label]["extensions"]:
            try:
                if extension == "netquack":
                    con.execute("INSTALL netquack FROM community")
                else:
                    con.execute(f"INSTALL {extension}")
            except duckdb.Error as error:
                raise RuntimeError(
                    f"failed to install extension {extension!r} for target {label!r} "
                    f"(first netquack install needs network; use --skip-netquack to bench without it): {error}"
                ) from error
        con.close()


def connect(target_label: str) -> duckdb.DuckDBPyConnection:
    target = TARGETS[target_label]
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    for extension in target["extensions"]:
        con.execute(f"LOAD {extension}")
    if target["load_url_tools"]:
        if not URL_TOOLS_EXTENSION_PATH.exists():
            raise RuntimeError(
                f"{URL_TOOLS_EXTENSION_PATH} not found; run `uv run make release` first"
            )
        con.execute(f"LOAD '{URL_TOOLS_EXTENSION_PATH}'")
    return con


def normalize_gate_value(value: object) -> object:
    # url_tools returns query params as JSON text while the MAP emulations
    # return dicts; fold both into plain dicts so equality means the same thing.
    if isinstance(value, str) and value.startswith("{"):
        return json.loads(value)
    if isinstance(value, dict):
        return {str(key): str(item) for key, item in value.items()}
    return value


def gate_rows_sql(column: str, values: list[str]) -> str:
    rows = ", ".join("('" + value.replace("'", "''") + "')" for value in values)
    return f"CREATE OR REPLACE TEMP TABLE _bench_in AS SELECT * FROM (VALUES {rows}) v({column})"


# Correctness gate: before anything is timed, every target of every operation
# must agree on the curated rows where the contracts overlap exactly. A silent
# semantic divergence would otherwise turn the throughput comparison into a
# comparison of different work.
def run_gate(target_labels: set[str]) -> None:
    for operation, spec in OPERATIONS.items():
        targets = {
            label: expr
            for label, expr in spec["targets"].items()
            if label in target_labels
        }
        if len(targets) < 2:
            continue
        gate_values = GATE_URLS if spec["input"] == "urls" else GATE_QUERY_STRINGS
        outputs = {}
        for label, expr in targets.items():
            con = connect(label)
            con.execute(gate_rows_sql(spec["column"], gate_values))
            rows = con.execute(f"SELECT {expr} FROM _bench_in").fetchall()
            outputs[label] = [normalize_gate_value(value) for (value,) in rows]
            con.close()
        reference_label = next(iter(outputs))
        for label, values in outputs.items():
            if values != outputs[reference_label]:
                raise RuntimeError(
                    f"correctness gate failed for {operation}: "
                    f"{reference_label}={outputs[reference_label]!r} vs {label}={values!r}"
                )
    print(f"correctness gate passed ({', '.join(sorted(target_labels))})")


def run_case(
    target_label: str, expr: str, spec: dict, size: str, threads: int, runs: int
) -> dict:
    con = connect(target_label)
    con.execute(f"SET threads = {threads}")
    con.execute(
        f"CREATE OR REPLACE TEMP TABLE _bench_in AS "
        f"SELECT {spec['column']} FROM read_parquet('{data_path(spec['input'], size)}')"
    )
    timed_sql = (
        f"CREATE OR REPLACE TEMP TABLE _bench_out AS SELECT {expr} AS r FROM _bench_in"
    )
    con.execute(timed_sql)  # warmup, not recorded
    times_ms = []
    for _ in range(runs):
        start = time.perf_counter()
        con.execute(timed_sql)
        times_ms.append((time.perf_counter() - start) * 1000)
    con.close()
    return {
        "min_ms": round(min(times_ms), 3),
        "median_ms": round(median(times_ms), 3),
        "runs": runs,
        "row_count": SIZES[size],
    }


def print_summary(results: list[dict]) -> None:
    by_case: dict[tuple, dict[str, float]] = {}
    for row in results:
        by_case.setdefault((row["operation"], row["size"], row["threads"]), {})[
            row["target"]
        ] = row["min_ms"]
    header = f"{'operation':<20} {'size':>5} {'thr':>3} {'url_tools':>10} {'netquack':>10} {'nq/ut':>6} {'native':>10} {'nat/ut':>7}"
    print()
    print(header)
    print("-" * len(header))
    for (operation, size, threads), timings in by_case.items():
        url_tools_ms = timings.get("url_tools")

        def cell(label: str) -> tuple[str, str]:
            ms = timings.get(label)
            if ms is None:
                return "-", "-"
            ratio = f"{ms / url_tools_ms:.2f}" if url_tools_ms else "-"
            return f"{ms:.1f}", ratio

        netquack_ms, netquack_ratio = cell("netquack")
        native_ms, native_ratio = cell("native")
        url_tools_cell = f"{url_tools_ms:.1f}" if url_tools_ms is not None else "-"
        print(
            f"{operation:<20} {size:>5} {threads:>3} {url_tools_cell:>10} "
            f"{netquack_ms:>10} {netquack_ratio:>6} {native_ms:>10} {native_ratio:>7}"
        )
    print("\nratios are target_ms / url_tools_ms: above 1.00 means url_tools is faster")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--filter",
        default="",
        help="only run operations whose name contains this substring",
    )
    parser.add_argument("--runs", type=int, default=DEFAULT_RUNS)
    parser.add_argument("--sizes", default=",".join(DEFAULT_SIZES))
    parser.add_argument("--threads", default=",".join(str(t) for t in DEFAULT_THREADS))
    parser.add_argument("--skip-netquack", action="store_true")
    parser.add_argument("--output", type=Path, default=RESULTS_DIR / "latest.json")
    args = parser.parse_args()

    sizes = args.sizes.split(",")
    for size in sizes:
        if size not in SIZES:
            raise SystemExit(f"unknown size {size!r}; known: {', '.join(SIZES)}")
    thread_modes = [int(t) for t in args.threads.split(",")]
    operations = {
        name: spec for name, spec in OPERATIONS.items() if args.filter in name
    }
    if not operations:
        raise SystemExit(f"no operations match filter {args.filter!r}")

    target_labels = {label for spec in operations.values() for label in spec["targets"]}
    if args.skip_netquack:
        target_labels.discard("netquack")

    ensure_data({spec["input"] for spec in operations.values()}, sizes)
    bootstrap_extensions(target_labels)
    run_gate(target_labels)

    results = []
    for operation, spec in operations.items():
        for target_label, expr in spec["targets"].items():
            if target_label not in target_labels:
                continue
            for size in sizes:
                for threads in thread_modes:
                    case = run_case(target_label, expr, spec, size, threads, args.runs)
                    case.update(
                        {
                            "operation": operation,
                            "target": target_label,
                            "size": size,
                            "threads": threads,
                        }
                    )
                    results.append(case)
                    print(
                        f"{operation}/{target_label}/{size}/t{threads}: "
                        f"min {case['min_ms']} ms, median {case['median_ms']} ms"
                    )

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(
            {
                "schema_version": SCHEMA_VERSION,
                "environment": collect_environment(),
                "results": results,
            },
            indent=2,
            ensure_ascii=False,
        )
        + "\n"
    )
    print(f"\nwrote {args.output}")
    print_summary(results)


if __name__ == "__main__":
    main()
