import argparse
import csv
import random
from pathlib import Path
from urllib.parse import urlencode

import duckdb

from config import BENCH_SEED
from config import DATA_DIR
from config import DEFAULT_SIZES
from config import SIZES

HOSTS = [
    "example.com",
    "shop.example.com",
    "news.example.org",
    "sub.domain.co.uk",
    "api.service.io",
    "cdn.assets.net",
    "m.store.example",
    "landing.example.dev",
]
SCHEMES = ["https", "https", "https", "http"]
PATH_SEGMENTS = [
    "catalog",
    "item",
    "search",
    "blog",
    "2024",
    "p",
    "user",
    "cart",
    "checkout",
    "docs",
    "api",
    "v2",
]
PARAM_KEYS = [
    "utm_medium",
    "utm_campaign",
    "q",
    "page",
    "id",
    "ref",
    "sort",
    "lang",
    "filter",
    "session",
]
PLAIN_VALUES = [
    "news",
    "email",
    "cpc",
    "organic",
    "duck",
    "42",
    "true",
    "main",
    "ru",
    "b",
]
# Values that exercise the decode path: spaces become '+', unicode and reserved
# characters become %XX once urlencode has escaped them.
PHRASE_VALUES = ["a b c", "спб зима", "50% off", "a/b test", "path=like", "emoji 😀"]
JUNK = [
    "",
    "not a url",
    "???",
    "javascript:void(0)",
    "12345",
    "   ",
    "词 测试",
    "http:/broken",
    "mailto:",
]


# No duplicate keys within one query string: the MAP-based native/netquack
# emulations error on duplicates, and the bench compares throughput, not the
# last-wins rule (the property harness owns that).
def make_query(rnd: random.Random, with_utm: bool) -> str:
    keys = rnd.sample(PARAM_KEYS, k=rnd.randint(1, 6))
    if with_utm:
        keys = ["utm_source"] + keys
    pairs = []
    for key in keys:
        value = rnd.choice(PHRASE_VALUES) if rnd.random() < 0.25 else rnd.choice(PLAIN_VALUES)
        pairs.append((key, value))
    return urlencode(pairs)


def make_path(rnd: random.Random) -> str:
    return "/" + "/".join(rnd.sample(PATH_SEGMENTS, k=rnd.randint(1, 4)))


def make_url(rnd: random.Random) -> str:
    shape = rnd.random()
    if shape < 0.55:
        query = f"?{make_query(rnd, with_utm=rnd.random() < 0.7)}" if rnd.random() < 0.85 else ""
        fragment = "#frag" if rnd.random() < 0.2 else ""
        return f"{rnd.choice(SCHEMES)}://{rnd.choice(HOSTS)}{make_path(rnd)}{query}{fragment}"
    if shape < 0.63:
        userinfo = "user:pass@" if rnd.random() < 0.3 else ""
        return f"https://{userinfo}{rnd.choice(HOSTS)}:8443{make_path(rnd)}?{make_query(rnd, with_utm=True)}"
    if shape < 0.75:
        return f"{make_path(rnd)}?{make_query(rnd, with_utm=rnd.random() < 0.5)}"
    if shape < 0.81:
        return f"myapp://open{make_path(rnd)}?{make_query(rnd, with_utm=False)}"
    if shape < 0.85:
        return f"//{rnd.choice(HOSTS)}{make_path(rnd)}"
    if shape < 0.92:
        return f"{rnd.choice(SCHEMES)}://{rnd.choice(HOSTS)}{make_path(rnd)}"
    return rnd.choice(JUNK)


def make_query_string(rnd: random.Random) -> str:
    if rnd.random() < 0.05:
        return ""
    return make_query(rnd, with_utm=rnd.random() < 0.6)


def write_parquet(values: list[str], column: str, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    csv_path = path.with_suffix(".csv.tmp")
    with open(csv_path, "w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow([column])
        writer.writerows([value] for value in values)
    con = duckdb.connect()
    con.execute(
        f"COPY (SELECT {column} FROM read_csv(?, header = true, columns = {{'{column}': 'VARCHAR'}})) "
        f"TO '{path}' (FORMAT parquet)",
        [str(csv_path)],
    )
    (actual,) = con.execute(f"SELECT count(*) FROM read_parquet('{path}')").fetchone()
    if actual != len(values):
        raise RuntimeError(f"{path}: wrote {actual} rows, expected {len(values)}")
    csv_path.unlink()


def data_path(dataset: str, size: str) -> Path:
    return DATA_DIR / f"{dataset}_{size}.parquet"


def generate_dataset(dataset: str, size: str) -> Path:
    # Seed per (dataset, size) so each file's content is stable regardless of
    # which other files are generated in the same invocation.
    rnd = random.Random(f"{BENCH_SEED}:{dataset}:{size}")
    row_count = SIZES[size]
    path = data_path(dataset, size)
    if dataset == "urls":
        write_parquet([make_url(rnd) for _ in range(row_count)], "url", path)
    elif dataset == "query_strings":
        write_parquet([make_query_string(rnd) for _ in range(row_count)], "qs", path)
    else:
        raise ValueError(f"unknown dataset: {dataset}")
    return path


def ensure_data(datasets: set[str], sizes: list[str], force: bool = False) -> None:
    for dataset in sorted(datasets):
        for size in sizes:
            path = data_path(dataset, size)
            if force or not path.exists():
                print(f"generating {path} ({SIZES[size]} rows)")
                generate_dataset(dataset, size)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sizes", default=",".join(DEFAULT_SIZES))
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args()
    ensure_data({"urls", "query_strings"}, args.sizes.split(","), force=args.force)


if __name__ == "__main__":
    main()
