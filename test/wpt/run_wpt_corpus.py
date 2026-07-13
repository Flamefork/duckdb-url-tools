from collections import Counter
from json import loads as json_loads
from os import environ
from pathlib import Path
from re import compile as re_compile
from subprocess import run as subprocess_run
from sys import stderr

REPO_ROOT = Path(__file__).resolve().parents[2]
BUILD_DIR = REPO_ROOT / "build" / "release"
DEFAULT_BINARY = BUILD_DIR / "duckdb"
DEFAULT_EXTENSION = BUILD_DIR / "extension" / "url_tools" / "url_tools.duckdb_extension"
CORPUS = Path(__file__).resolve().parent / "urltestdata.json"

# Split on (and keep) the bytes the CLI's line reader may swallow — see sql_literal.
CONTROL_BYTE = re_compile(r"([\x00-\x1f\x7f])")

# The single place that maps url_components struct fields onto WPT case keys:
# (struct field, WPT key, how the WPT value is normalized to ours). WPT reports
# `protocol` with its trailing ':', `search` with its leading '?' and `hash` with its
# leading '#'; url_components strips all three. WPT spells an absent port as '' and a
# default port is already normalized away by the parser, so both map to our NULL.
FIELD_MAPPING = (
    ("scheme", "protocol", lambda value: value.removesuffix(":")),
    ("host", "hostname", lambda value: value),
    ("port", "port", lambda value: int(value) if value else None),
    ("path", "pathname", lambda value: value),
    ("query", "search", lambda value: value.removeprefix("?")),
    ("fragment", "hash", lambda value: value.removeprefix("#")),
)

# A deviation is (field, expected, got); "<row>" means the whole struct, not one field.
ROW_NULL_INSTEAD_OF_STRUCT = [("<row>", "<struct>", None)]

# Known deviations of the vendored ada from the pinned corpus — counted and printed, never
# silently suppressed. WPT commit b63305b743 (2026-06-25) applied a WHATWG change ("IDNA
# cannot fail ASCII domains, even if they start with xn--"), so an ASCII host carrying an
# invalid punycode label must now parse. Vendored ada v3.4.4 (released 2026-03) predates the
# change and still rejects such a host, so url_components yields a NULL row.
#
# Keyed on exact inputs and on the exact deviation: a new mismatch, or a different deviation
# on these inputs (e.g. the host parses but a field is wrong), still fails the runner. When
# an ada bump makes one of these parse, the runner fails and says to delete the entry — an
# allowlist entry that stopped being needed is signal, not noise.
KNOWN_ADA_DEVIATIONS = {
    "http://a.b.c.xn--pokxncvks": ROW_NULL_INSTEAD_OF_STRUCT,
    "http://a.b.c.XN--pokxncvks": ROW_NULL_INSTEAD_OF_STRUCT,
    "http://a.b.c.Xn--pokxncvks": ROW_NULL_INSTEAD_OF_STRUCT,
    "http://10.0.0.xn--pokxncvks": ROW_NULL_INSTEAD_OF_STRUCT,
    "http://10.0.0.XN--pokxncvks": ROW_NULL_INSTEAD_OF_STRUCT,
    "http://10.0.0.xN--pokxncvks": ROW_NULL_INSTEAD_OF_STRUCT,
    "https://xn--/": ROW_NULL_INSTEAD_OF_STRUCT,
    "file://xn--/p": ROW_NULL_INSTEAD_OF_STRUCT,
}


def sql_literal(text: str) -> str:
    # A SQL expression evaluating to exactly `text`. The CLI reads stdin as newline-delimited
    # C-strings, so a control byte travelling raw on the wire is at the mercy of the line reader
    # before the statement is assembled — a NUL ends the line early, a 0x03 opening a continuation
    # line drops the statement outright. Splice control bytes back in with chr() so the wire only
    # ever carries printable text. (Copied from test/property/url_tools_property.py.)
    pieces = [
        f"chr({ord(chunk)})" if index % 2 else "'" + chunk.replace("'", "''") + "'"
        for index, chunk in enumerate(CONTROL_BYTE.split(text))
        if chunk
    ]
    return "(" + " || ".join(pieces) + ")" if pieces else "''"


# Case selection. url_components resolves no base URL and deliberately deviates from WHATWG
# for '/'-prefixed input (parsed as a relative path rather than failing), so those cases are
# not comparable and are skipped by reason — never silently dropped.
def select_cases(corpus: list) -> tuple[list[dict], Counter]:
    kept: list[dict] = []
    skipped: Counter = Counter()
    for entry in corpus:
        if not isinstance(entry, dict):
            skipped["section comment"] += 1
        elif entry.get("base") is not None:
            skipped["needs base URL"] += 1
        elif not entry["input"]:
            skipped["empty input"] += 1
        elif entry["input"].startswith("/"):
            skipped["'/'-prefixed (documented relative-parse deviation)"] += 1
        else:
            kept.append(entry)
    return kept, skipped


def run_url_components(inputs: list[str], binary: Path, extension: Path) -> list:
    values = ",\n".join(f"({index}, {sql_literal(text)})" for index, text in enumerate(inputs))
    script = "\n".join(
        [
            ".mode json",
            f"LOAD '{extension}';",
            f"WITH cases(i, u) AS (VALUES\n{values}\n)\nSELECT i, url_components(u) AS c FROM cases ORDER BY i;",
        ]
    )
    result = subprocess_run(
        [str(binary), "-unsigned", "-batch", "-json"],
        input=script.encode("utf-8"),
        capture_output=True,
        check=True,
    )
    rows = json_loads(result.stdout.decode("utf-8"))
    # The corpus is the contract: a row per kept case, or the transport lost cases and every
    # comparison below is suspect.
    if [row["i"] for row in rows] != list(range(len(inputs))):
        raise SystemExit(f"CLI returned {len(rows)} rows for {len(inputs)} cases (transport dropped cases)")
    return [row["c"] for row in rows]


def deviations_for(case: dict, components: dict | None) -> list[tuple]:
    if case.get("failure"):
        return [] if components is None else [("<row>", None, components)]
    if components is None:
        return ROW_NULL_INSTEAD_OF_STRUCT
    return [
        (field, expected, components[field])
        for field, wpt_key, normalize in FIELD_MAPPING
        for expected in [normalize(case[wpt_key])]
        if components[field] != expected
    ]


def main() -> int:
    kept, skipped = select_cases(json_loads(CORPUS.read_text(encoding="utf-8")))
    binary = Path(environ.get("URL_TOOLS_DUCKDB_BIN", str(DEFAULT_BINARY)))
    extension = Path(environ.get("URL_TOOLS_EXTENSION", str(DEFAULT_EXTENSION)))

    results = run_url_components([case["input"] for case in kept], binary, extension)

    failures: list[str] = []
    known = 0
    for case, components in zip(kept, results, strict=True):
        deviations = deviations_for(case, components)
        prefix = f"input={case['input']!r}"
        if case["input"] not in KNOWN_ADA_DEVIATIONS:
            failures += [f"{prefix} field={f} expected={e!r} got={g!r}" for f, e, g in deviations]
        elif not deviations:
            failures.append(f"{prefix} now matches WPT: delete it from KNOWN_ADA_DEVIATIONS")
        elif deviations != KNOWN_ADA_DEVIATIONS[case["input"]]:
            failures += [
                f"{prefix} deviates differently than KNOWN_ADA_DEVIATIONS records: "
                f"field={f} expected={e!r} got={g!r}"
                for f, e, g in deviations
            ]
        else:
            known += 1

    failures += [
        f"input={stale!r} is in KNOWN_ADA_DEVIATIONS but no longer a kept corpus case: delete it"
        for stale in sorted(set(KNOWN_ADA_DEVIATIONS) - {case["input"] for case in kept})
    ]

    for line in failures:
        print(line, file=stderr)
    reasons = ", ".join(f"{count} {reason}" for reason, count in sorted(skipped.items()))
    print(
        f"{len(kept)} cases, {len(failures)} mismatches, {known} known ada deviations "
        f"({sum(skipped.values())} skipped: {reasons})"
    )
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
