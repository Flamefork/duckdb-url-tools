# Updating dependencies

## DuckDB version bump

When a new DuckDB stable release comes out:

1. Bump both submodules:
   - `duckdb` → the new release tag:
     `git -C duckdb fetch --tags && git -C duckdb checkout vX.Y.Z`
   - `extension-ci-tools` → the branch matching that release:
     `git -C extension-ci-tools fetch && git -C extension-ci-tools checkout vX.Y.Z`
2. Update every version pin in `.github/workflows/MainDistributionPipeline.yml`.
   Both jobs (`duckdb-stable-build` and `code-quality-check`) pin the release
   tag in three places each: the `uses:` ref of the reusable workflow, and the
   `duckdb_version` and `ci_tools_version` inputs. All six carry the same tag,
   so a global search-and-replace of the old tag is the reliable way.
   (`Sanitizer.yml` pins no version — it builds DuckDB from the submodule.)
3. Pin the Python `duckdb` package to the same version in `pyproject.toml` and
   relock (`uv lock`). A built extension loads only into the exact version it
   was built for, so a bumped submodule with a stale package leaves the bench
   harness — the one tool that drives DuckDB from Python — unable to load
   `url_tools` at all.
4. Rebuild and run the full gate: `uv run make verify`.
5. Run the property harness against the fresh build:
   `uv run --frozen test/property/url_tools_property.py`.
6. Run the bench harness once (`uv run --frozen python bench/run_benchmarks.py
   --sizes 100k --runs 1 --filter host`): its correctness gate is what proves
   the new package and the new binary actually agree.
7. If the build breaks: extensions link against DuckDB's internal C++ API,
   which is not stable across releases. To find what changed, use DuckDB's
   [release notes](https://github.com/duckdb/duckdb/releases), the history of
   [core extension patches](https://github.com/duckdb/duckdb/commits/main/.github/patches/extensions),
   and the git history of the relevant header in `duckdb/src/include/`.
8. Commit the submodule bumps, the workflow edits and the `pyproject.toml` /
   `uv.lock` pin together, then push and confirm the distribution pipeline is
   green.

## Vendored dependencies (third_party/)

`third_party/` is never edited by hand. Updates replace whole files with a
fresh upstream release and sync `docs/THIRD_PARTY_NOTICES.md`.

### ada (URL parser)

1. Download the amalgamated build (`singleheader.zip`) attached to the target
   release at https://github.com/ada-url/ada/releases.
2. Replace `third_party/ada/ada.h` and `third_party/ada/ada.cpp` with the new
   files. The amalgamation also ships `ada_c.h`, which this repo does not use.
3. Update the ada version in `docs/THIRD_PARTY_NOTICES.md`.
4. Run `uv run make verify`, then the property harness (see above). ada parses
   arbitrary untrusted input, so also confirm the Sanitizer CI run on the push
   is green — it exercises the parser under ASan + UBSan.
5. Refresh the pinned WPT URL corpus in the same commit (re-fetch
   `url/resources/urltestdata.json`, update the commit in
   `docs/THIRD_PARTY_NOTICES.md`) and run `test/wpt/run_wpt_corpus.py`: a new ada
   may legitimately turn allowlisted cases green, and the runner fails until the
   stale `KNOWN_ADA_DEVIATIONS` entries are deleted (see `test/README.md`).

### Public Suffix List (url_domain)

The list is a data dependency, not code: it is pinned to an upstream commit and
compiled into the binary, so `url_domain` answers the same for a given build no
matter when or where it runs. It is never fetched at build or at run time.

1. Pick the commit to pin (the list changes most days; a refresh is a deliberate
   act, like the WPT corpus). Download that exact revision — from the
   `publicsuffix/list` repository, or from
   https://publicsuffix.org/list/public_suffix_list.dat, which serves the tip:

   ```shell
   curl -o third_party/psl/public_suffix_list.dat \
     https://raw.githubusercontent.com/publicsuffix/list/<commit>/public_suffix_list.dat
   ```
2. Regenerate the compiled rule table (the only file in `third_party/` that is
   produced rather than downloaded; the extension links it, never the `.dat`):

   ```shell
   uv run scripts/generate_psl.py
   ```
3. Update the pinned commit and its date in `docs/THIRD_PARTY_NOTICES.md`.
4. Run `uv run make verify` and the property harness (see above). Registrable
   domains move with the list: a suffix added upstream (a new eTLD, a new entry
   in the PRIVATE section) legitimately changes `url_domain` for hosts under it,
   so a SQLLogic case failing after a refresh is a decision to make, not
   automatically a bug — check the rule in the new `.dat` before touching the
   test.
5. Commit the `.dat`, the regenerated table and the notices together: they are
   one snapshot, and a build from a mixed pair is not reproducible.

### ankerl unordered_dense

1. Take `include/ankerl/unordered_dense.h` (and `stl.h`, if still present) from
   the target release at https://github.com/martinus/unordered_dense/releases.
2. Replace the files under `third_party/ankerl/`.
3. Update the version in `docs/THIRD_PARTY_NOTICES.md`.
4. Run `uv run make verify`.
