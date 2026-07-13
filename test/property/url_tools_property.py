from json import loads as json_loads
from os import environ
from pathlib import Path
from re import compile as re_compile
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

# Split on (and keep) the bytes the CLI's line reader may swallow — see sql_literal.
CONTROL_BYTE = re_compile(r"([\x00-\x1f\x7f])")

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
    # newline-delimited C-strings (local_getline), so a control byte travelling raw on the wire is
    # at the mercy of the line reader before the statement is ever assembled: a NUL terminates the
    # line early (the statement never completes and the pipe round-trip deadlocks), and a 0x03
    # opening a continuation line makes the shell drop the whole statement (it answers with no row
    # and no error, which reads exactly like a SQL failure). Neither is url_tools behavior. So keep
    # control bytes off the wire entirely and splice them back with chr(), which the parser applies
    # to the value while the transport only ever sees printable text.
    pieces = [
        f"chr({ord(chunk)})" if index % 2 else "'" + chunk.replace("'", "''") + "'"
        for index, chunk in enumerate(CONTROL_BYTE.split(text))
        if chunk
    ]
    return "(" + " || ".join(pieces) + ")" if pieces else "''"


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


QUERY_VALUES_MODES = ("raw", "first", "last", "all")
PARSED_QUERY_VALUES_MODES = ("first", "last", "all")


# Totality: url_components never raises in any mode, whatever the input — junk parses
# to a NULL row, never to a scan-killing error.
@PROPERTY_SETTINGS
@example(url="https://example.com/path?utm_source=duckdb#top", mode="raw")
@example(url="/search?q=%D0%BB&tab=products", mode="all")
@example(url="//host/protocol-relative", mode="first")
@example(url="http://[::1]:80/?k=%FF%FE", mode="last")
@given(url=url_inputs, mode=st.sampled_from(QUERY_VALUES_MODES))
def test_url_components_total(url: str, mode: str) -> None:
    result = SESSION.read(f"url_components({sql_literal(url)}, '{mode}')")
    assert not isinstance(result, UrlToolsSession.Errored), f"url_components errored: {result.message}"


# Read one param map as a Python object. The MAP is cast to JSON because that is the one spelling
# the harness can parse back losslessly; the cast itself is pinned by law 7 below.
def read_params(expr: str) -> Any:
    text = SESSION.read(f"CAST(({expr}) AS JSON)")
    assert not isinstance(text, UrlToolsSession.Errored), f"{expr} errored: {text.message}"
    return None if text is None else json_loads(text)


# Mode key-invariance (law 2): the values axis changes values only. The key set and its
# first-occurrence order are the same in every parsed mode, every key carries at least one
# value under 'all', and 'first'/'last' are the ends of that list. Checked on both spellings of
# the same collection — the standalone function and the struct field — which also pins that the
# two never drift apart.
@PROPERTY_SETTINGS
@example(url="https://example.com/?a=1&a=2&b=%2B")
@example(url="https://example.com/?a=&a=&a=")
@example(url="/search?q=%D0%BB&tab=products")
@given(url=url_inputs)
def test_query_values_modes_agree(url: str) -> None:
    literal = sql_literal(url)
    maps: dict[str, Any] = {}
    for mode in PARSED_QUERY_VALUES_MODES:
        maps[mode] = read_params(f"query_params({literal}, '{mode}')")
        assert maps[mode] is not None, f"query_params({mode!r}) returned NULL for a non-NULL input"
        field = read_params(f"(url_components({literal}, '{mode}')).query_params")
        # An unparseable URL has no component row at all; it still has an (empty) parameter map.
        agrees = field == maps[mode] if field is not None else maps[mode] == {}
        assert agrees, f"query_params({mode!r}) is {maps[mode]!r}, the struct field is {field!r}"
    assert list(maps["first"]) == list(maps["all"]), f"'first' keys differ from 'all': {maps!r}"
    assert list(maps["last"]) == list(maps["all"]), f"'last' keys differ from 'all': {maps!r}"
    for key, values in maps["all"].items():
        assert values, f"key {key!r} carries no values: {maps!r}"
        assert maps["first"][key] == values[0], f"'first' is not the head of 'all' for {key!r}: {maps!r}"
        assert maps["last"][key] == values[-1], f"'last' is not the tail of 'all' for {key!r}: {maps!r}"


# Point/object agreement (law 3): query_param(u, k, m) is exactly query_params(u, m)[k] — for every
# key the URL carries (empty values included) and for a key it does not carry, where both are NULL.
@PROPERTY_SETTINGS
@example(url="https://example.com/?a=1&a=2&b=")
@example(url="https://example.com/?%FF=1")
@example(url="/p?flag&x=1")
@given(url=url_inputs)
def test_query_param_agrees_with_the_map(url: str) -> None:
    literal = sql_literal(url)
    for mode in ("first", "last"):
        entries = read_params(f"query_params({literal}, '{mode}')")
        for key in list(entries) + ["url_tools_absent_key"]:
            point = SESSION.read(f"query_param({literal}, {sql_literal(key)}, '{mode}')")
            assert not isinstance(point, UrlToolsSession.Errored), f"query_param errored: {point.message}"
            expected = entries[key] if key in entries else None
            assert point == expected, f"query_param({key!r}, {mode!r}) is {point!r}, the map says {expected!r}"


# Key uniqueness is our invariant, not DuckDB's: a hand-written map vector is not validated in
# release builds, so only the collector's dedup stands behind it. Checked on map_keys, not on the
# JSON form — a JSON parser would silently collapse a duplicate key and hide the defect.
@PROPERTY_SETTINGS
@example(url="https://example.com/?a=1&a=2&a=3", mode="all")
@example(url="https://example.com/?%FF=1&%FE=2", mode="last")
@given(url=url_inputs, mode=st.sampled_from(PARSED_QUERY_VALUES_MODES))
def test_map_keys_are_unique(url: str, mode: str) -> None:
    keys_text = SESSION.read(f"CAST(map_keys((url_components({sql_literal(url)}, '{mode}')).query_params) AS JSON)")
    assert not isinstance(keys_text, UrlToolsSession.Errored), f"map_keys errored: {keys_text.message}"
    if keys_text is None:
        return
    keys = json_loads(keys_text)
    assert len(keys) == len(set(keys)), f"duplicate keys in the map: {keys!r}"


# v1's JSON, spelled out: compact separators, keys in first-occurrence order, non-ASCII raw, '"' and
# backslash escaped, and the C0 controls escaped with their short form where they have one and
# \uXXXX — UPPERCASE hex — otherwise. That hex case is why the oracle is written out instead of
# borrowed from Python's json, which spells the very same escape in lowercase and would fail a byte
# comparison for a reason of its own.
JSON_SHORT_ESCAPES = {'"': '\\"', "\\": "\\\\", "\b": "\\b", "\f": "\\f", "\n": "\\n", "\r": "\\r", "\t": "\\t"}


def v1_json_string(text: str) -> str:
    escaped = []
    for char in text:
        if char in JSON_SHORT_ESCAPES:
            escaped.append(JSON_SHORT_ESCAPES[char])
        elif char < " ":
            escaped.append(f"\\u{ord(char):04X}")
        else:
            escaped.append(char)
    return '"' + "".join(escaped) + '"'


def v1_json_object(entries: dict[str, str]) -> str:
    return "{" + ",".join(f"{v1_json_string(key)}:{v1_json_string(value)}" for key, value in entries.items()) + "}"


# v1 compatibility anchor (law 7): CAST(query_params(u, 'last') AS JSON) is byte-for-byte what v1's
# JSON-returning query_params wrote. v1's writer left the extension with the JSON return type, so the
# anchor is held against the independent oracle above. The consumer's migration off the JSON spelling
# is only verifiable while this holds.
@PROPERTY_SETTINGS
@example(url="https://example.com/path?utm_source=duckdb#top")
@example(url="https://example.com/?a=1&a=2&b=%2B")
@example(url="https://example.com/?bad=%FF&%FF=x")
@example(url="https://example.com/?q=%22quoted%22%5C&nl=%0A&tab=%09")
@given(url=url_inputs)
def test_last_map_matches_v1_json(url: str) -> None:
    literal = sql_literal(url)
    map_json = SESSION.read(f"CAST(query_params({literal}, 'last') AS JSON)")
    assert not isinstance(map_json, UrlToolsSession.Errored), f"query_params errored: {map_json.message}"
    assert map_json is not None, "query_params returned NULL for a non-NULL input"
    every_value = read_params(f"query_params({literal}, 'all')")
    v1_json = v1_json_object({key: values[-1] for key, values in every_value.items()})
    assert map_json == v1_json, f"the MAP as JSON is {map_json!r}, v1 wrote {v1_json!r}"


# Raw/parsed agreement (law 5): the raw query the accessor hands back is exactly the string the param
# functions parse — the two spellings of "the query of this URL" cannot drift apart.
@PROPERTY_SETTINGS
@example(url="https://example.com/?a=1&a=2&b=%2B")
@example(url="/search?q=%D0%BB&tab=products")
@given(url=url_inputs)
def test_raw_query_reparses_to_the_same_params(url: str) -> None:
    literal = sql_literal(url)
    from_raw = read_params(f"query_params_from_string(url_query({literal}))")
    direct = read_params(f"query_params({literal})")
    if from_raw is None:
        assert direct == {}, f"no query (unparseable URL) but query_params = {direct!r}"
    else:
        assert from_raw == direct, f"the raw query parses to {from_raw!r}, query_params says {direct!r}"


# The struct field of url_components(u, 'raw'), by accessor.
URL_ACCESSORS = {
    "url_scheme": "scheme",
    "url_host": "host",
    "url_port": "port",
    "url_path": "path",
    "url_query": "query",
    "url_fragment": "fragment",
}


# Accessor/struct agreement (law 4): each accessor is the corresponding url_components(u, 'raw')
# field on every input — the relative-input NULLs (no scheme/host/port without an authority) and the
# ''-for-absent convention included. The accessors exist to skip the other five fields, so this
# property is what keeps the cheap spelling honest against the complete one.
@PROPERTY_SETTINGS
@example(url="https://sub.shop.co.uk:8443/p/x?a=1&b=%20&a=2#f")
@example(url="https://x.com:443/")
@example(url="/p?a=1#f")
@example(url="//host/protocol-relative")
@given(url=url_inputs)
def test_accessors_agree_with_components(url: str) -> None:
    literal = sql_literal(url)
    for accessor, field in URL_ACCESSORS.items():
        point = SESSION.read(f"{accessor}({literal})")
        assert not isinstance(point, UrlToolsSession.Errored), f"{accessor} errored: {point.message}"
        component = SESSION.read(f"(url_components({literal})).{field}")
        assert not isinstance(component, UrlToolsSession.Errored), f"url_components errored: {component.message}"
        assert point == component, f"{accessor} is {point!r}, the struct field {field} is {component!r}"


# Domain containment (law 8): a non-NULL url_domain is a suffix of url_host and carries at least one
# dot. It is a public suffix plus one label, so it can be neither absent from the host nor a single
# label. url_domain is the one function whose answer comes out of a vendored data file, and this is
# what keeps that answer tied to the host it was derived from — whatever the list says.
@PROPERTY_SETTINGS
@example(url="https://m.ozon.ru/p/1")
@example(url="https://shop.example.co.uk/")
@example(url="https://кто.рф/")
@example(url="https://a.foo.ck/")
@example(url="https://192.168.0.1/")
@given(url=url_inputs)
def test_domain_is_a_suffix_of_the_host(url: str) -> None:
    literal = sql_literal(url)
    domain = SESSION.read(f"url_domain({literal})")
    assert not isinstance(domain, UrlToolsSession.Errored), f"url_domain errored: {domain.message}"
    if domain is None:
        return
    host = SESSION.read(f"url_host({literal})")
    assert not isinstance(host, UrlToolsSession.Errored), f"url_host errored: {host.message}"
    assert host is not None, f"url_domain is {domain!r} for a URL with no host at all"
    assert host.endswith(domain), f"url_domain {domain!r} is not a suffix of the host {host!r}"
    assert "." in domain, f"url_domain {domain!r} carries no dot, so it is not a suffix plus a label"


# The loose variant is total on anything at all — it is the one function whose input is not even
# expected to be a URL — and it always yields a map, never NULL for a non-NULL input.
@PROPERTY_SETTINGS
@example(url="https://s.ru/#/cart?utm_source=push", mode="last")
@example(url="Заголовок страницы?utm_source=qr", mode="all")
@example(url="utm_source=x&utm_medium=y", mode="first")
@example(url="just some text", mode="all")
@given(url=url_inputs, mode=st.sampled_from(PARSED_QUERY_VALUES_MODES))
def test_query_params_loose_total(url: str, mode: str) -> None:
    result = read_params(f"query_params_loose({sql_literal(url)}, '{mode}')")
    assert isinstance(result, dict), f"query_params_loose yielded {result!r}"


# Loose conservativity (law 6): query_params_loose only ever ADDS the parameters a fragment shaped
# like a query carries. Where there is no such fragment — no fragment at all, a plain anchor — it is
# query_params exactly, so a consumer can swap one for the other without auditing its URLs. A NULL
# fragment is an unparseable input, which is where the two are allowed to differ (loose falls back on
# the string's own shape).
@PROPERTY_SETTINGS
@example(url="https://example.com/?a=1&a=2#top")
@example(url="https://example.com/?a=1&a=2")
@example(url="/search?q=%D0%BB&tab=products#anchor")
@given(url=url_inputs)
def test_loose_is_conservative(url: str) -> None:
    literal = sql_literal(url)
    fragment = SESSION.read(f"url_fragment({literal})")
    assert not isinstance(fragment, UrlToolsSession.Errored), f"url_fragment errored: {fragment.message}"
    if fragment is None or "?" in fragment or "=" in fragment:
        return
    for mode in PARSED_QUERY_VALUES_MODES:
        loose = read_params(f"query_params_loose({literal}, '{mode}')")
        strict = read_params(f"query_params({literal}, '{mode}')")
        assert loose == strict, f"loose is {loose!r}, query_params({mode!r}) is {strict!r}"


# The bare-query-string variant is total and always yields a map, even on junk that never came near
# a URL (raw percent noise, invalid UTF-8 escapes, NULs). Its default mode is 'all', so every value
# is a list — a single-valued key included.
@PROPERTY_SETTINGS
@example(query="a=%FF")
@example(query="?a=1")
@example(query="a=1+2")
@example(query="=&=&a")
@given(query=st.one_of(url_flavored_text, st.text(max_size=120)))
def test_query_params_from_string_total(query: str) -> None:
    result = read_params(f"query_params_from_string({sql_literal(query)})")
    assert isinstance(result, dict), f"query_params_from_string yielded {result!r}"
    assert all(isinstance(values, list) and values for values in result.values()), f"'all' is not lists: {result!r}"


# The custom-separator overload accepts any non-empty separator and stays total.
@PROPERTY_SETTINGS
@example(query="k=v1|key2=v2", separator="|")
@example(query="a=1;;b=2", separator=";;")
@given(
    query=st.one_of(url_flavored_text, st.text(max_size=120)),
    separator=st.text(min_size=1, max_size=3),
)
def test_query_params_from_string_separator_total(query: str, separator: str) -> None:
    result = read_params(f"query_params_from_string({sql_literal(query)}, {sql_literal(separator)})")
    assert isinstance(result, dict), f"the custom separator yielded {result!r}"


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
    result = read_params(f"query_params_from_string({sql_literal(encoded)}, '&', 'last')")
    assert result == params, f"urlencode({params!r}) = {encoded!r} decoded to {result!r}"


PROPERTIES = [
    test_url_components_total,
    test_query_values_modes_agree,
    test_query_param_agrees_with_the_map,
    test_map_keys_are_unique,
    test_last_map_matches_v1_json,
    test_raw_query_reparses_to_the_same_params,
    test_accessors_agree_with_components,
    test_domain_is_a_suffix_of_the_host,
    test_query_params_loose_total,
    test_loose_is_conservative,
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
