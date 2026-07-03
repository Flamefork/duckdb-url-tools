from json import loads as json_loads
from os import environ
from pathlib import Path
from select import select
from subprocess import PIPE
from subprocess import Popen
from sys import stderr
from tempfile import TemporaryFile
from typing import Any
from urllib.parse import urlencode

from hypothesis import HealthCheck
from hypothesis import example
from hypothesis import given
from hypothesis import settings
from hypothesis import strategies as st

REPO_ROOT = Path(__file__).resolve().parents[2]
BUILD_DIR = REPO_ROOT / "build" / "release"
DEFAULT_BINARY = BUILD_DIR / "duckdb"
DEFAULT_EXTENSION = BUILD_DIR / "extension" / "url_tools" / "url_tools.duckdb_extension"

# Statement boundaries are marked by a sentinel SELECT, not a `.print`: the CLI's
# dot-command output is not ordered against the result renderer, so a `.print`
# can surface before the preceding query's rows. Routing the marker through the
# same renderer keeps it strictly after the rows. A missing marker (the pipe
# closed mid-query) is the crash signal the fuzz tests rely on.
SENTINEL_TOKEN = "URL_TOOLS_PROPERTY_SENTINEL_7c1d"
SENTINEL_LINE = f'[{{"url_tools_sentinel":"{SENTINEL_TOKEN}"}}]'

MAX_EXAMPLES = int(environ.get("URL_TOOLS_PROPERTY_MAX_EXAMPLES", "300"))

# Ceiling on how long one statement may go without producing output. A property query runs in
# milliseconds, so this only fires on a genuine stall: a CLI that treats the sent SQL as an
# incomplete statement (it blocks reading stdin for a continuation it never gets while we block
# reading stdout) deadlocks the pipe round-trip forever. The timeout converts that into a bounded
# UrlToolsCrash with the stuck statement and stderr tail, which restarts the process — the same
# recovery path as an outright process death.
DRAIN_TIMEOUT_SECONDS = float(environ.get("URL_TOOLS_PROPERTY_DRAIN_TIMEOUT", "30"))


class UrlToolsCrash(Exception):
    pass


class UrlToolsSession:
    # One long-lived CLI process: each query is a pipe round-trip rather than a
    # fresh process, so hypothesis can drive thousands of examples (and shrink
    # failures) without paying spawn cost per case. A crash restarts the process
    # so the next shrink attempt runs against a live session. The pipe is bytes,
    # decoded with replacement, so a stray non-UTF-8 byte in output never raises
    # on the Python side and masks a real C++ defect.
    def __init__(self, binary: Path, extension: Path) -> None:
        self.binary = binary
        self.extension = extension
        self.proc: Popen[bytes] | None = None
        self.stderr_file: Any = None
        self._start()

    def _start(self) -> None:
        # stderr goes to a temp file, not DEVNULL: SQL errors are still dropped from the
        # result stream (only stdout is parsed), but when the process dies the file holds
        # its last words — an ASan/UBSan stack trace under the sanitizer build — which the
        # crash message then surfaces. Detaching it entirely turns every sanitizer crash
        # into an undiagnosable "exited before sentinel" with no trace.
        self.stderr_file = TemporaryFile()
        self.proc = Popen(
            [str(self.binary), "-unsigned", "-batch", "-json"],
            stdin=PIPE,
            stdout=PIPE,
            stderr=self.stderr_file,
            bufsize=0,
        )
        self._send(".mode json")
        self._send(f"LOAD '{self.extension}';")
        self._drain()

    def _stderr_tail(self) -> str:
        if self.stderr_file is None:
            return ""
        self.stderr_file.seek(0)
        text = self.stderr_file.read().decode("utf-8", "replace")
        # The CLI's stderr also carries the pre-crash SQL errors of a long-lived session;
        # the actionable part is the final sanitizer report. A report begins with its header
        # and prints the top access stack *before* the long shadow-byte dump and SUMMARY, so
        # a fixed tail can drop the most useful frames. Anchor on the last report header and
        # return everything from there — the report is self-bounded, the noise before it is not.
        markers = ("ERROR: AddressSanitizer", "ERROR: LeakSanitizer", "runtime error:")
        start = max(text.rfind(marker) for marker in markers)
        if start < 0:
            return text[-8000:]
        return text[text.rfind("\n", 0, start) + 1 :]

    def _send(self, line: str) -> None:
        assert self.proc is not None and self.proc.stdin is not None
        self.proc.stdin.write(line.encode("utf-8") + b"\n")
        self.proc.stdin.flush()

    def _drain(self, context: str = "") -> list[str]:
        assert self.proc is not None and self.proc.stdout is not None
        self._send(f"SELECT '{SENTINEL_TOKEN}' AS url_tools_sentinel;")
        lines: list[str] = []
        while True:
            ready, _, _ = select([self.proc.stdout], [], [], DRAIN_TIMEOUT_SECONDS)
            if not ready:
                tail = self._stderr_tail()
                detail = f"\n--- CLI stderr tail ---\n{tail}" if tail.strip() else ""
                stmt = f"\n--- stuck statement ---\n{context}" if context else ""
                raise UrlToolsCrash(
                    f"CLI produced no output for {DRAIN_TIMEOUT_SECONDS:.0f}s "
                    f"(hang or incomplete statement){stmt}{detail}"
                )
            raw = self.proc.stdout.readline()
            if raw == b"":
                tail = self._stderr_tail()
                detail = f"\n--- CLI stderr tail ---\n{tail}" if tail.strip() else ""
                raise UrlToolsCrash(f"CLI process exited before sentinel{detail}")
            line = raw.decode("utf-8", "replace").rstrip("\n")
            if line == SENTINEL_LINE:
                return lines
            lines.append(line)

    def restart(self) -> None:
        if self.proc is not None:
            self.proc.kill()
            self.proc.wait()
        self._start()

    # A SQL error raised by the evaluated expression. `value()` collapses this to None (its
    # contract is "the value, or None for NULL/error"), but the 3-state `read()` returns it so a
    # fails-loud property can assert a LOUD ERROR specifically — not merely "not a value". `message`
    # is the CLI's stderr text for this one statement (the error goes to stderr, never the result
    # stream), so a property can pin the expected error to its real substring.
    class Errored:
        def __init__(self, message: str) -> None:
            self.message = message

    # Evaluate one scalar expression and return one of three states: an Errored (the SQL raised),
    # None (the result was SQL NULL or empty), or the value's text. The expression is cast to VARCHAR
    # so a JSON-typed result comes back as a single JSON string (the renderer would otherwise inline a
    # JSON column as a nested object, which is not round-trippable through json_loads). A UrlToolsCrash
    # here is a genuine process death, distinct from a SQL error (which leaves the process alive and
    # surfaces as Errored). An errored statement emits no result row, so its payload is empty; a NULL
    # result emits a row whose cell is null — the two are told apart by the payload, not stderr.
    def read(self, expr: str) -> "UrlToolsSession.Errored | str | None":
        if self.proc is None or self.proc.poll() is not None:
            self.restart()
        assert self.stderr_file is not None
        self.stderr_file.seek(0, 2)
        stderr_before = self.stderr_file.tell()
        try:
            stmt = f"SELECT CAST(({expr}) AS VARCHAR) AS v;"
            self._send(stmt)
            rows = self._drain(stmt)
        except UrlToolsCrash:
            self.restart()
            raise
        payload = "".join(rows).strip()
        parsed = json_loads(payload) if payload and payload != "[]" else None
        if not parsed:
            self.stderr_file.seek(stderr_before)
            return UrlToolsSession.Errored(self.stderr_file.read().decode("utf-8", "replace"))
        (cell,) = parsed[0].values()
        return None if cell is None else str(cell)


def sql_literal(text: str) -> str:
    # A SQL expression evaluating to exactly `text`. The persistent CLI reads stdin as
    # newline-delimited C-strings (local_getline), so a raw NUL byte on the wire terminates the
    # line early and the statement is never seen as complete — the pipe round-trip then deadlocks
    # (a bare newline is harmless: the shell reassembles the statement across lines). Splice NUL
    # back in with chr(0) so the wire carries no raw NUL while the value is preserved.
    if "\x00" not in text:
        return "'" + text.replace("'", "''") + "'"
    runs = ["'" + run.replace("'", "''") + "'" for run in text.split("\x00")]
    return "(" + " || chr(0) || ".join(runs) + ")"


SESSION = UrlToolsSession(
    Path(environ.get("URL_TOOLS_DUCKDB_BIN", str(DEFAULT_BINARY))),
    Path(environ.get("URL_TOOLS_EXTENSION", str(DEFAULT_EXTENSION))),
)

PROPERTY_SETTINGS = settings(
    max_examples=MAX_EXAMPLES,
    deadline=None,
    suppress_health_check=[HealthCheck.too_slow],
)

# Bias inputs toward URL shapes without giving up raw junk: a sampled prefix puts
# examples on the absolute / relative / protocol-relative / bare-query parse paths,
# and the URL-flavored alphabet keeps the suffix dense in delimiters, percent
# escapes, and non-ASCII rather than diluting them across all of Unicode.
url_prefixes = st.sampled_from(
    [
        "",
        "/",
        "//",
        "?",
        "https://example.com",
        "https://example.com/path?",
        "https://user:pass@example.com:8443/p;x",
        "relative:/hit",
        "x-custom://host",
    ]
)
url_flavored_text = st.text(
    alphabet="%&=+?#/:@[].-_~!$'()*,;\\ ^`{}|<>\"\x00\tл微😀aZ09",
    max_size=120,
)
url_inputs = st.builds(
    lambda prefix, rest: prefix + rest,
    url_prefixes,
    st.one_of(url_flavored_text, st.text(max_size=120)),
)


# Totality: url_components never raises, whatever the input — junk parses to a
# NULL row, never to a scan-killing error.
@PROPERTY_SETTINGS
@example(url="https://example.com/path?utm_source=duckdb#top")
@example(url="/search?q=%D0%BB&tab=products")
@example(url="//host/protocol-relative")
@example(url="http://[::1]:80/?k=%FF%FE")
@given(url=url_inputs)
def test_url_components_total(url: str) -> None:
    result = SESSION.read(f"url_components({sql_literal(url)})")
    assert not isinstance(result, UrlToolsSession.Errored), f"url_components errored: {result.message}"


# query_params is total, always a JSON object, and agrees with the query_params
# field of url_components: same parse, same object — and unparseable input means
# a NULL components row with query_params collapsing to {}.
@PROPERTY_SETTINGS
@example(url="https://example.com/path?utm_source=duckdb#top")
@example(url="/search?q=%D0%BB&tab=products")
@example(url="https://example.com/?a=1&a=2&b=%2B")
@given(url=url_inputs)
def test_query_params_agrees_with_components(url: str) -> None:
    literal = sql_literal(url)
    params_text = SESSION.read(f"query_params({literal})")
    assert not isinstance(params_text, UrlToolsSession.Errored), f"query_params errored: {params_text.message}"
    assert params_text is not None, "query_params returned NULL for a non-NULL input"
    params = json_loads(params_text)
    assert isinstance(params, dict), f"query_params is not a JSON object: {params_text!r}"
    components_params_text = SESSION.read(f"(url_components({literal})).query_params")
    assert not isinstance(components_params_text, UrlToolsSession.Errored)
    if components_params_text is None:
        assert params == {}, f"components NULL but query_params = {params_text!r}"
    else:
        assert json_loads(components_params_text) == params


# The bare-query-string variant is total and always yields a JSON object, even on
# junk that never came near a URL (raw percent noise, invalid UTF-8 escapes, NULs).
@PROPERTY_SETTINGS
@example(query="a=%FF")
@example(query="?a=1")
@example(query="a=1+2")
@example(query="=&=&a")
@given(query=st.one_of(url_flavored_text, st.text(max_size=120)))
def test_query_params_from_string_total(query: str) -> None:
    result = SESSION.read(f"query_params_from_string({sql_literal(query)})")
    assert not isinstance(result, UrlToolsSession.Errored), f"query_params_from_string errored: {result.message}"
    assert result is not None
    assert isinstance(json_loads(result), dict)


# The custom-separator overload accepts any non-empty separator and stays total.
@PROPERTY_SETTINGS
@example(query="k=v1|key2=v2", separator="|")
@example(query="a=1;;b=2", separator=";;")
@given(
    query=st.one_of(url_flavored_text, st.text(max_size=120)),
    separator=st.text(min_size=1, max_size=3),
)
def test_query_params_from_string_separator_total(query: str, separator: str) -> None:
    result = SESSION.read(f"query_params_from_string({sql_literal(query)}, {sql_literal(separator)})")
    assert not isinstance(result, UrlToolsSession.Errored), f"custom separator errored: {result.message}"
    assert result is not None
    assert isinstance(json_loads(result), dict)


# The one documented loud failure: an empty separator is a caller bug and must
# raise, not degrade into some implicit default.
@PROPERTY_SETTINGS
@given(query=st.text(max_size=40))
def test_empty_separator_fails_loud(query: str) -> None:
    result = SESSION.read(f"query_params_from_string({sql_literal(query)}, '')")
    assert isinstance(result, UrlToolsSession.Errored), f"empty separator did not raise: {result!r}"
    assert "separator must not be empty" in result.message


# Differential oracle: Python's urlencode emits WHATWG-compatible form encoding
# ('+' for space, %XX for reserved bytes, UTF-8), so decoding its output must
# reproduce the source dict exactly — including empty keys and empty values.
@PROPERTY_SETTINGS
@example(params={"": "", "a": ""})
@example(params={"q": "л 微", "emoji": "😀", "plus": "1+2=3", "amp": "a&b"})
@given(params=st.dictionaries(st.text(max_size=20), st.text(max_size=20), max_size=8))
def test_urlencode_round_trip(params: dict[str, str]) -> None:
    encoded = urlencode(params)
    result = SESSION.read(f"query_params_from_string({sql_literal(encoded)})")
    assert not isinstance(result, UrlToolsSession.Errored), f"round trip errored: {result.message}"
    assert result is not None
    assert json_loads(result) == params, f"urlencode({params!r}) = {encoded!r} decoded to {result!r}"


PROPERTIES = [
    test_url_components_total,
    test_query_params_agrees_with_components,
    test_query_params_from_string_total,
    test_query_params_from_string_separator_total,
    test_empty_separator_fails_loud,
    test_urlencode_round_trip,
]


def main() -> int:
    failures = 0
    for prop in PROPERTIES:
        name = prop.__name__
        try:
            prop()
        except UrlToolsCrash as crash:
            failures += 1
            print(f"CRASH  {name}: {crash}", file=stderr)
        except AssertionError as failure:
            failures += 1
            print(f"FAIL   {name}: {failure}", file=stderr)
        else:
            print(f"ok     {name}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
