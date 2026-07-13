import argparse
import json
from pathlib import Path

from config import DEFAULT_MIN_EFFECT_MS
from config import DEFAULT_TOLERANCE_PCT
from config import RESULTS_DIR

CaseKey = tuple[str, str, str, int]


def load_results(path: Path) -> dict[CaseKey, dict]:
    payload = json.loads(path.read_text())
    return {(row["operation"], row["target"], row["size"], row["threads"]): row for row in payload["results"]}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline", type=Path, default=RESULTS_DIR / "baseline.json")
    parser.add_argument("--latest", type=Path, default=RESULTS_DIR / "latest.json")
    parser.add_argument("--tolerance-pct", type=float, default=DEFAULT_TOLERANCE_PCT)
    parser.add_argument("--min-effect-ms", type=float, default=DEFAULT_MIN_EFFECT_MS)
    args = parser.parse_args()

    baseline = load_results(args.baseline)
    latest = load_results(args.latest)

    shared = sorted(set(baseline) & set(latest))
    if not shared:
        raise SystemExit("no shared cases between baseline and latest")
    for key in sorted(set(baseline) - set(latest)):
        print(f"note: {'/'.join(str(part) for part in key)} is in baseline only")
    for key in sorted(set(latest) - set(baseline)):
        print(f"note: {'/'.join(str(part) for part in key)} is in latest only (no baseline)")

    regressions = []
    header = f"{'case':<48} {'base_ms':>9} {'latest_ms':>9} {'delta':>8}"
    print()
    print(header)
    print("-" * len(header))
    for key in shared:
        base_ms = baseline[key]["min_ms"]
        latest_ms = latest[key]["min_ms"]
        delta_pct = (latest_ms - base_ms) / base_ms * 100 if base_ms else 0.0
        case_label = "/".join(str(part) for part in key)
        # Only single-threaded url_tools rows gate. netquack/native drift is machine noise, and
        # multi-threaded timings swing 18-36% run to run for the same binary on a machine that is
        # not idle (measured over five back-to-back runs) — the stock-SQL rows drift with them, so
        # it is CPU contention, not this code. Gating on them makes the gate a coin flip; they stay
        # visible for context.
        gates = key[1] == "url_tools" and key[3] == 1
        regressed = delta_pct > args.tolerance_pct and (latest_ms - base_ms) > args.min_effect_ms
        marker = ""
        if regressed:
            marker = " REGRESSION" if gates else " (context)"
            if gates:
                regressions.append(case_label)
        elif delta_pct < -args.tolerance_pct and (base_ms - latest_ms) > args.min_effect_ms:
            marker = " improved"
        print(f"{case_label:<48} {base_ms:>9.1f} {latest_ms:>9.1f} {delta_pct:>+7.1f}%{marker}")

    if regressions:
        print(f"\n{len(regressions)} single-threaded url_tools regression(s) beyond {args.tolerance_pct}%:")
        for case_label in regressions:
            print(f"  {case_label}")
        print("stored baselines drift with machine state: treat as a hint, confirm by re-running both builds")
        return 1
    print(f"\nno single-threaded url_tools regressions beyond {args.tolerance_pct}%")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
