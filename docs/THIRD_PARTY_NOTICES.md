# Third-Party Notices

This project is licensed under MIT. The following third-party components are
included in the source tree or in release binaries.

Source distributions should include this file and the referenced or embedded
license notices. Binary distributions should include this file together with
the referenced license files from the build tree.

## Source Tree

### DuckDB Extension Template

- Upstream: https://github.com/duckdb/extension-template
- Files: repository build and extension scaffolding
- License: MIT

Copyright 2018-2025 Stichting DuckDB Foundation.

### Ada URL Parser

- Upstream: https://github.com/ada-url/ada
- Version: v3.4.4 (vendored amalgamated build)
- Files: `third_party/ada/ada.h`, `third_party/ada/ada.cpp`
- License: dual Apache-2.0 / MIT
- License texts: `LICENSE-APACHE` and `LICENSE-MIT` in the upstream repository

Copyright 2023 Yagiz Nizipli and Daniel Lemire.

### ankerl::unordered_dense

- Upstream: https://github.com/martinus/unordered_dense
- Version: v4.8.1
- Files: `third_party/ankerl/unordered_dense.h`, `third_party/ankerl/stl.h`
- License: MIT
- License text: embedded in the header

Copyright 2022 Martin Leitner-Ankerl.

## Release Binaries

DuckDB loadable extension binaries are linked with DuckDB build artifacts and
may include DuckDB third-party components. Notices for DuckDB itself are in the
DuckDB submodule.

### DuckDB

- Upstream: https://github.com/duckdb/duckdb
- Version used by this repository: submodule `duckdb`
- License: MIT
- License file: `duckdb/LICENSE`

Copyright 2018-2025 Stichting DuckDB Foundation.

### yyjson

- Upstream: https://github.com/ibireme/yyjson
- Source in DuckDB: `duckdb/third_party/yyjson` (not vendored here; the extension
  links yyjson symbols exported by DuckDB)
- License: MIT
- License file: `duckdb/third_party/yyjson/LICENSE`

Copyright 2020 YaoYuan.

### utf8proc

- Upstream: https://github.com/JuliaStrings/utf8proc
- Source in DuckDB: `duckdb/third_party/utf8proc` (the extension calls the
  wrapper DuckDB exports)
- License: MIT-style (utf8proc license)
- License file: `duckdb/third_party/utf8proc/LICENSE.md`

Copyright 2014-2021 Steven G. Johnson, Jiahao Chen, Peter Colberg, Tony Kelman,
Scott P. Jones, and other contributors.
