from os import environ
from pathlib import Path
from subprocess import run as subprocess_run
from sys import stderr
from tempfile import TemporaryDirectory

REPO_ROOT = Path(__file__).resolve().parents[2]
BUILD_DIR = REPO_ROOT / "build" / "release"
DEFAULT_SERIALIZER = BUILD_DIR / "tools" / "plan_serializer"

# Serializing a BOUND plan is the one thing that re-binds a function from arguments the caller never
# wrote: DuckDB's deserializer re-runs the bind callback on the deserialized children (see
# FunctionSerializer::Deserialize). Every optional argument in this surface is resolved by its alias,
# so a re-bind that could not see aliases would read query_params_from_string(qs, query_values :=
# 'last') as a separator of 'last' — a silently wrong answer with no error anywhere. Aliases DO
# survive (Expression::Serialize carries one per child), which is why no bind-data Serialize callback
# is needed; this harness is what holds that up. If DuckDB ever stops carrying them, these cases fail
# instead of the extension quietly misreading its own arguments.
#
# No SQL statement serializes a bound plan — PRAGMA enable_verification round-trips the PARSED
# statement and PREPARE keeps the bound plan in memory — so SQLLogic cannot reach this at all.
# duckdb/tools/plan_serializer can: it plans the last statement, writes the bound plan out, reads it
# back, executes the deserialized plan and compares it against executing the statement directly.
#
# A case is (name, setup statements, the statement whose plan is round-tripped). A column input is
# the load-bearing shape: a constant one folds away in any pipeline that optimizes before it
# serializes, taking the function expression out of the plan with it.
SETUP = [
    "CREATE TABLE t AS SELECT 'a=1|a=2|b=3' AS qs, 'https://e.com/p?a=1&a=2&b=3#f' AS u;",
]

CASES = [
    # The alias is the only thing separating the two same-typed optionals of
    # query_params_from_string. Lose it and the axis argument becomes the separator.
    ("named axis, column input", "SELECT query_params_from_string(qs, query_values := 'last') AS m FROM t;"),
    ("named axis, constant input", "SELECT query_params_from_string('a=1&a=2&b=3', query_values := 'first') AS m;"),
    ("both optionals named", "SELECT query_params_from_string(qs, sep := '|', query_values := 'first') AS m FROM t;"),
    # The mirror image: an unnamed second argument must stay a separator across the round-trip.
    ("positional separator", "SELECT query_params_from_string(qs, '|') AS m FROM t;"),
    ("positional separator and axis", "SELECT query_params_from_string(qs, '|', 'last') AS m FROM t;"),
    # The axis selects the return type on these too, so a misbind changes the plan's types.
    ("url_components named axis", "SELECT url_components(u, query_values := 'all') AS c FROM t;"),
    ("url_components default", "SELECT url_components(u) AS c FROM t;"),
    ("query_params named axis", "SELECT query_params(u, query_values := 'last') AS m FROM t;"),
    ("query_param named axis", "SELECT query_param(u, 'a', query_values := 'first') AS v FROM t;"),
]


def roundtrip(serializer: Path, work_dir: Path, name: str, statement: str) -> str | None:
    # plan_serializer reads one statement per line and serializes the plan of the last one.
    sql_file = work_dir / "case.sql"
    sql_file.write_text("\n".join(SETUP + [statement]) + "\n")
    plan_file = work_dir / "case.plan"
    for mode in ("serialize", "deserialize"):
        result = subprocess_run(
            [str(serializer), mode, str(sql_file), str(plan_file)],
            capture_output=True,
        )
        if result.returncode != 0:
            detail = (result.stderr or result.stdout).decode("utf-8", "replace").strip()
            return f"{name}: {mode} failed\n{detail}"
    return None


def main() -> int:
    serializer = Path(environ.get("URL_TOOLS_PLAN_SERIALIZER", DEFAULT_SERIALIZER))
    if not serializer.exists():
        raise SystemExit(f"plan_serializer not found at {serializer} (build it with `uv run make release`)")

    with TemporaryDirectory() as temp_dir:
        failures = [
            failure
            for name, statement in CASES
            if (failure := roundtrip(serializer, Path(temp_dir), name, statement)) is not None
        ]

    for failure in failures:
        print(failure, file=stderr)
    print(f"{len(CASES)} bound-plan round-trips, {len(failures)} mismatches")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
