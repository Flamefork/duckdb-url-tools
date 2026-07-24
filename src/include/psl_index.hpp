#pragma once

#include <optional>
#include <string_view>

namespace duckdb {

// The registrable domain (eTLD+1) of a host, per the Public Suffix List algorithm. The answer is a
// suffix of `host`; nullopt when no registrable domain exists (the host IS a public suffix, is a
// single label, or carries an empty label).
std::optional<std::string_view> UrlToolsRegistrableDomain(std::string_view host);

} // namespace duckdb
