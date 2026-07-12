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
3. Rebuild and run the full gate: `uv run make verify`.
4. Run the property harness against the fresh build:
   `uv run --frozen test/property/url_tools_property.py`.
5. If the build breaks: extensions link against DuckDB's internal C++ API,
   which is not stable across releases. To find what changed, use DuckDB's
   [release notes](https://github.com/duckdb/duckdb/releases), the history of
   [core extension patches](https://github.com/duckdb/duckdb/commits/main/.github/patches/extensions),
   and the git history of the relevant header in `duckdb/src/include/`.
6. Commit the submodule bumps and the workflow edits together, then push and
   confirm the distribution pipeline is green.

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

### ankerl unordered_dense

1. Take `include/ankerl/unordered_dense.h` (and `stl.h`, if still present) from
   the target release at https://github.com/martinus/unordered_dense/releases.
2. Replace the files under `third_party/ankerl/`.
3. Update the version in `docs/THIRD_PARTY_NOTICES.md`.
4. Run `uv run make verify`.
