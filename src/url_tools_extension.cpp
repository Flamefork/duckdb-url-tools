#define DUCKDB_EXTENSION_MAIN

#include "url_tools_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/execution/expression_executor_state.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "ada.h"
#include "psl_rules.h"
#include "utf8proc_wrapper.hpp"
#include "ankerl/unordered_dense.h"

#include <algorithm>
#include <charconv>
#include <deque>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace duckdb {

namespace {

// How the values of a repeated query key are reported. The axis selects the result type, so it
// is resolved at bind time, never per row.
enum class QueryValuesMode : uint8_t { RAW, FIRST, LAST, ALL };

struct QueryValuesModeName {
	const char *name;
	QueryValuesMode mode;
};

// The modes each axis accepts, in the order its error messages list them. 'raw' is url_components'
// alone (hand back the query string and parse nothing); 'all' is not available where the result is
// a scalar.
constexpr QueryValuesModeName COMPONENT_MODES[] = {{"raw", QueryValuesMode::RAW},
                                                   {"first", QueryValuesMode::FIRST},
                                                   {"last", QueryValuesMode::LAST},
                                                   {"all", QueryValuesMode::ALL}};
constexpr QueryValuesModeName MAP_MODES[] = {
    {"all", QueryValuesMode::ALL}, {"first", QueryValuesMode::FIRST}, {"last", QueryValuesMode::LAST}};
constexpr QueryValuesModeName SCALAR_MODES[] = {{"first", QueryValuesMode::FIRST}, {"last", QueryValuesMode::LAST}};

struct ValuesAxis {
	const char *function_name;
	const char *parameter_name;
	const QueryValuesModeName *modes;
	idx_t mode_count;
};

constexpr ValuesAxis URL_COMPONENTS_AXIS {"url_components", "query_values", COMPONENT_MODES,
                                          std::size(COMPONENT_MODES)};
constexpr ValuesAxis QUERY_PARAMS_AXIS {"query_params", "query_values", MAP_MODES, std::size(MAP_MODES)};
constexpr ValuesAxis QUERY_PARAMS_FROM_STRING_AXIS {"query_params_from_string", "query_values", MAP_MODES,
                                                    std::size(MAP_MODES)};
constexpr ValuesAxis QUERY_PARAMS_LOOSE_AXIS {"query_params_loose", "query_values", MAP_MODES, std::size(MAP_MODES)};
constexpr ValuesAxis QUERY_PARAM_AXIS {"query_param", "query_values", SCALAR_MODES, std::size(SCALAR_MODES)};

// Every occurrence of every key, in arrival order; `next` chains the occurrences that share a
// key. One collection pass therefore serves all three value modes: 'first' and 'last' read the
// ends of a chain, 'all' walks it.
struct UrlToolsQueryValue {
	std::string_view value;
	idx_t next;
};

struct UrlToolsQueryParam {
	std::string_view key;
	idx_t first_value;
	idx_t last_value;
	idx_t count;
};

struct UrlToolsLocalState : public FunctionLocalState {
	vector<UrlToolsQueryParam> query_params;
	ankerl::unordered_dense::map<std::string_view, idx_t> query_param_index;
	vector<UrlToolsQueryValue> query_values;
	vector<std::pair<std::string, std::string>> decoded_pairs;
	// Sanitized copies of the keys/values that needed one. A deque, not a vector: the collection
	// holds views into these strings while more are still being appended.
	std::deque<std::string> sanitized;
	std::string url_buffer;
};

static unique_ptr<FunctionLocalState> UrlToolsInitLocalState(ExpressionState &, const BoundFunctionExpression &,
                                                             FunctionData *) {
	return make_uniq<UrlToolsLocalState>();
}

static std::string_view StripLeadingChar(std::string_view input, char prefix) {
	if (!input.empty() && input.front() == prefix) {
		return input.substr(1);
	}
	return input;
}

static std::string_view StripTrailingChar(std::string_view input, char suffix) {
	if (!input.empty() && input.back() == suffix) {
		return input.substr(0, input.size() - 1);
	}
	return input;
}

// Replace each ill-formed UTF-8 byte with U+FFFD so the result is always valid
// UTF-8 (per-byte, not WHATWG maximal-subpart). Utf8Proc::Analyze reports the first
// invalid position; for a bad continuation byte that position lands inside the
// sequence, so back off to the start of the offending sequence before emitting the
// replacement.
static string UrlToolsSanitizeUtf8(std::string_view value) {
	auto data = value.data();
	auto len = value.size();
	string out;
	out.reserve(len);
	size_t pos = 0;
	while (pos < len) {
		size_t invalid_pos = 0;
		if (Utf8Proc::Analyze(data + pos, len - pos, nullptr, &invalid_pos) != UnicodeType::INVALID) {
			out.append(data + pos, len - pos);
			break;
		}
		size_t valid_len = invalid_pos;
		while (valid_len > 0) {
			if (Utf8Proc::Analyze(data + pos, valid_len, nullptr, nullptr) != UnicodeType::INVALID) {
				break;
			}
			valid_len--;
		}
		out.append(data + pos, valid_len);
		out.append("\xEF\xBF\xBD");
		pos += valid_len + 1;
	}
	return out;
}

// Percent-decoding yields arbitrary bytes, and a DuckDB VARCHAR may not carry ill-formed UTF-8, so
// every key and value is repaired on the way into the collection — not on the way out. Sanitizing
// this early is what keeps map keys unique: distinct raw keys can collapse onto the same repaired
// string (every ill-formed byte becomes U+FFFD), and it is the repaired string that lands in the
// result, so it must be the one the dedup index sees.
static std::string_view UrlToolsWellFormed(UrlToolsLocalState &local_state, std::string_view value) {
	auto data = value.empty() ? "" : value.data();
	if (Utf8Proc::IsValid(data, value.size())) {
		return value;
	}
	return local_state.sanitized.emplace_back(UrlToolsSanitizeUtf8(value));
}

static string_t UrlToolsAddString(Vector &vec, std::string_view value) {
	return StringVector::AddString(vec, value.empty() ? "" : value.data(), value.size());
}

// Repeated keys keep first-occurrence order: the index map is only ever written on insert
// (try_emplace, never operator[]), so a repeat extends the key's value chain instead of moving
// the key to the back. One entry per key is what makes the written MAP valid — DuckDB does not
// check key uniqueness of a hand-written map vector in release builds.
//
// `override_from` is the first value index of an overriding pass: a key whose whole chain predates
// it was collected by a weaker source (query_params_loose collects the fragment's pseudo-query
// first, then lets the query override it), so its first occurrence here REPLACES that chain instead
// of extending it — while a repeat within the overriding pass appends as usual. A collection with
// nothing to override passes 0, which no chain can predate.
static void UrlToolsPutQueryParam(UrlToolsLocalState &local_state, std::string_view key, std::string_view value,
                                  idx_t override_from) {
	auto well_formed_key = UrlToolsWellFormed(local_state, key);
	auto value_index = local_state.query_values.size();
	local_state.query_values.push_back({UrlToolsWellFormed(local_state, value), DConstants::INVALID_INDEX});

	auto [entry, inserted] =
	    local_state.query_param_index.try_emplace(well_formed_key, local_state.query_params.size());
	if (inserted) {
		local_state.query_params.push_back({well_formed_key, value_index, value_index, 1});
		return;
	}
	auto &param = local_state.query_params[entry->second];
	if (param.last_value < override_from) {
		param.first_value = value_index;
		param.last_value = value_index;
		param.count = 1;
		return;
	}
	local_state.query_values[param.last_value].next = value_index;
	param.last_value = value_index;
	param.count++;
}

static void UrlToolsClearQueryParams(UrlToolsLocalState &local_state) {
	local_state.query_params.clear();
	local_state.query_param_index.clear();
	local_state.query_values.clear();
	local_state.decoded_pairs.clear();
	local_state.sanitized.clear();
}

// Splits a query string on a custom pair separator, mirroring the WHATWG form
// parsing that ada::url_search_params hardcodes for '&': empty segments are
// skipped, the key ends at the first '=', '+' decodes to space, then
// percent-escapes decode. Decoded pairs are collected fully before taking views,
// so vector growth cannot invalidate them.
static void UrlToolsCollectCustomSeparated(std::string_view query, std::string_view separator,
                                           UrlToolsLocalState &local_state) {
	auto &pairs = local_state.decoded_pairs;
	auto input = query;
	while (!input.empty()) {
		auto separator_index = input.find(separator);
		auto current = separator_index == std::string_view::npos ? input : input.substr(0, separator_index);
		if (!current.empty()) {
			auto equal = current.find('=');
			std::string name(current.substr(0, equal == std::string_view::npos ? current.size() : equal));
			std::string value(equal == std::string_view::npos ? std::string_view() : current.substr(equal + 1));
			std::replace(name.begin(), name.end(), '+', ' ');
			std::replace(value.begin(), value.end(), '+', ' ');
			pairs.emplace_back(ada::unicode::percent_decode(name, name.find('%')),
			                   ada::unicode::percent_decode(value, value.find('%')));
		}
		if (separator_index == std::string_view::npos) {
			break;
		}
		input.remove_prefix(separator_index + separator.size());
	}
	for (const auto &pair : pairs) {
		UrlToolsPutQueryParam(local_state, pair.first, pair.second, 0);
	}
}

// Collects the decoded pairs of a raw (undecoded, no leading '?') query string into local_state.
// The collected views point into `params` or local_state.decoded_pairs, so both must outlive
// every read of the collection — hence `params` is owned by the caller, not by this function.
static void UrlToolsCollectQueryParams(std::string_view query, std::string_view separator,
                                       ada::url_search_params &params, UrlToolsLocalState &local_state) {
	if (query.empty()) {
		return;
	}
	if (separator == "&") {
		params.reset(query);
		local_state.query_params.reserve(params.size());
		for (const auto &entry : params) {
			UrlToolsPutQueryParam(local_state, entry.first, entry.second, 0);
		}
	} else {
		UrlToolsCollectCustomSeparated(query, separator, local_state);
	}
}

// A MAP vector is physically LIST(STRUCT(key, value)): entries are appended to the list child and
// each row stamps its own (offset, length). ListVector::Reserve grows the child geometrically, so
// a chunk pays a handful of reallocations rather than one per row — but a grow moves the buffer,
// so the child data pointers are re-read after every reserve.
struct UrlToolsMapWriter {
	UrlToolsMapWriter(Vector &map_vec, QueryValuesMode mode)
	    : map_vec(map_vec), keys_vec(MapVector::GetKeys(map_vec)), values_vec(MapVector::GetValues(map_vec)),
	      mode(mode) {
	}

	void WriteRow(idx_t row, UrlToolsLocalState &local_state) {
		auto &params = local_state.query_params;
		ListVector::Reserve(map_vec, entry_count + params.size());
		if (mode == QueryValuesMode::ALL) {
			ListVector::Reserve(values_vec, value_count + local_state.query_values.size());
		}
		auto offset = entry_count;
		auto keys = FlatVector::GetData<string_t>(keys_vec);
		if (mode == QueryValuesMode::ALL) {
			auto value_lists = FlatVector::GetData<list_entry_t>(values_vec);
			auto &value_child = ListVector::GetEntry(values_vec);
			auto value_strings = FlatVector::GetData<string_t>(value_child);
			for (const auto &param : params) {
				keys[entry_count] = UrlToolsAddString(keys_vec, param.key);
				value_lists[entry_count] = list_entry_t(value_count, param.count);
				for (auto value = param.first_value; value != DConstants::INVALID_INDEX;) {
					value_strings[value_count] = UrlToolsAddString(value_child, local_state.query_values[value].value);
					value_count++;
					value = local_state.query_values[value].next;
				}
				entry_count++;
			}
		} else {
			auto values = FlatVector::GetData<string_t>(values_vec);
			for (const auto &param : params) {
				keys[entry_count] = UrlToolsAddString(keys_vec, param.key);
				auto value = mode == QueryValuesMode::FIRST ? param.first_value : param.last_value;
				values[entry_count] = UrlToolsAddString(values_vec, local_state.query_values[value].value);
				entry_count++;
			}
		}
		FlatVector::GetData<list_entry_t>(map_vec)[row] = list_entry_t(offset, params.size());
	}

	// A NULL map still carries a list entry; point it at an empty range so nothing reads past the
	// child's size.
	void WriteNullRow(idx_t row) {
		FlatVector::SetNull(map_vec, row, true);
		FlatVector::GetData<list_entry_t>(map_vec)[row] = list_entry_t(entry_count, 0);
	}

	void Finish() {
		ListVector::SetListSize(map_vec, entry_count);
		if (mode == QueryValuesMode::ALL) {
			ListVector::SetListSize(values_vec, value_count);
		}
	}

	Vector &map_vec;
	Vector &keys_vec;
	Vector &values_vec;
	QueryValuesMode mode;
	idx_t entry_count = 0;
	idx_t value_count = 0;
};

// Collects `query` and writes it as one map row. `params` owns the decoded strings the collected
// views point at, so the collect and the write must share one scope.
static void UrlToolsWriteQueryParamsMap(UrlToolsMapWriter &writer, idx_t row, std::string_view query,
                                        std::string_view separator, UrlToolsLocalState &local_state) {
	ada::url_search_params params;
	UrlToolsCollectQueryParams(query, separator, params, local_state);
	writer.WriteRow(row, local_state);
	UrlToolsClearQueryParams(local_state);
}

// Shared URL-input handling: absolute URLs of any scheme parse as-is; a single
// leading slash is a relative path (raw URL logs often carry bare paths), parsed
// via a placeholder scheme; a double slash is a protocol-relative URL, which is
// ambiguous without a base and treated as unparseable.
struct UrlToolsParsedInput {
	ada::result<ada::url_aggregator> url;
	bool relative;
};

static UrlToolsParsedInput UrlToolsParseInput(std::string_view raw, UrlToolsLocalState &local_state) {
	auto relative = !raw.empty() && raw.front() == '/' && !(raw.size() >= 2 && raw[1] == '/');
	if (relative) {
		local_state.url_buffer.assign("relative:");
		local_state.url_buffer.append(raw);
		return {ada::parse<ada::url_aggregator>(local_state.url_buffer), true};
	}
	return {ada::parse<ada::url_aggregator>(raw), false};
}

// A fragment carries parameters only when it is SHAPED like a query, and what it is shaped like is
// decided by the text in FRONT of its first '?'. A '=' there means the fragment already IS a query
// string ('#access_token=t&next=/page?x=1', the OAuth implicit flow), so the whole of it is parsed
// and a '?' inside a value is just a character. Without that '=' the front is a route and its first
// '?' opens the parameters ('#/cart?utm_source=push', the single-page-app shape) — first, not last:
// a query starts where the first '?' is, and a later '?' sits inside a value, exactly as it does in a
// real URL. A fragment with neither is a plain anchor ('#top') and stays out.
static std::string_view UrlToolsFragmentQuery(std::string_view fragment) {
	auto question = fragment.find('?');
	if (fragment.substr(0, question).find('=') != std::string_view::npos) {
		return fragment;
	}
	if (question != std::string_view::npos) {
		return fragment.substr(question + 1);
	}
	return {};
}

// The same question for a string that is not a URL at all (a page title, a half-expanded macro
// template): parameters start at its FIRST '?' — everything before it is prose — or the whole string
// is one query when it has no '?' but does have a '='.
static std::string_view UrlToolsLooseQuery(std::string_view raw) {
	auto question = raw.find('?');
	if (question != std::string_view::npos) {
		return raw.substr(question + 1);
	}
	return raw.find('=') == std::string_view::npos ? std::string_view() : raw;
}

// One URL parse decides which of the two readings the input gets. A parseable one has both a
// fragment and a query, and they are not peers: the fragment's pseudo-query is the base, the query
// overrides it key by key (a key the query carries drops the fragment's values for it entirely).
// Anything that does not parse falls back on the string's own shape.
//
// Both collections own the decoded strings the collected views point at, so the caller owns them —
// they must outlive the map write, the fragment's as much as the query's.
static void UrlToolsCollectLooseParams(std::string_view raw, ada::url_search_params &fragment_params,
                                       ada::url_search_params &query_params, UrlToolsLocalState &local_state) {
	auto parsed = UrlToolsParseInput(raw, local_state);
	if (!parsed.url) {
		query_params.reset(UrlToolsLooseQuery(raw));
		for (const auto &entry : query_params) {
			UrlToolsPutQueryParam(local_state, entry.first, entry.second, 0);
		}
		return;
	}
	fragment_params.reset(UrlToolsFragmentQuery(StripLeadingChar(parsed.url->get_hash(), '#')));
	for (const auto &entry : fragment_params) {
		UrlToolsPutQueryParam(local_state, entry.first, entry.second, 0);
	}
	// Everything collected so far is fragment-side, so the current value count is the boundary the
	// overriding pass measures a key's chain against.
	auto override_from = local_state.query_values.size();
	query_params.reset(StripLeadingChar(parsed.url->get_search(), '?'));
	for (const auto &entry : query_params) {
		UrlToolsPutQueryParam(local_state, entry.first, entry.second, override_from);
	}
}

// One reader per component, shared by the accessors and by url_components' struct: the two spellings
// of "the scheme of this URL" are the same code, so they cannot drift apart. A relative input has no
// authority, so scheme, host and port are absent together; path, query and fragment are present for
// every URL that parses at all ('' when it carries none, never NULL).
using UrlStringField = std::optional<std::string_view> (*)(const ada::url_aggregator &, bool);

static std::optional<std::string_view> UrlSchemeField(const ada::url_aggregator &url, bool relative) {
	if (relative) {
		return std::nullopt;
	}
	return StripTrailingChar(url.get_protocol(), ':');
}

static std::optional<std::string_view> UrlHostField(const ada::url_aggregator &url, bool relative) {
	if (relative) {
		return std::nullopt;
	}
	return url.get_hostname();
}

static std::optional<std::string_view> UrlPathField(const ada::url_aggregator &url, bool) {
	return url.get_pathname();
}

static std::optional<std::string_view> UrlQueryField(const ada::url_aggregator &url, bool) {
	return StripLeadingChar(url.get_search(), '?');
}

static std::optional<std::string_view> UrlFragmentField(const ada::url_aggregator &url, bool) {
	return StripLeadingChar(url.get_hash(), '#');
}

// ada normalizes the port away when it equals the scheme's default (WHATWG), and reports what is
// left as an already-validated 16-bit decimal — so the conversion cannot fail.
static std::optional<uint16_t> UrlPortField(const ada::url_aggregator &url, bool relative) {
	if (relative || !url.has_port()) {
		return std::nullopt;
	}
	auto port = url.get_port();
	uint16_t value = 0;
	std::from_chars(port.data(), port.data() + port.size(), value);
	return value;
}

// What the Public Suffix List says about one suffix string. A suffix can be named by several rules
// at once ('*.ck' and '!www.ck' are different statements about strings ending in 'ck'), so the
// kinds are flags, not a value.
enum PublicSuffixRuleKind : uint8_t { PSL_RULE_NORMAL = 1, PSL_RULE_WILDCARD = 2, PSL_RULE_EXCEPTION = 4 };

// The list compiled into one lookup, keyed by the suffix a rule speaks about: 'com' for the rule
// 'com', 'ck' for '*.ck' (the wildcard's own labels are the KEY, the '*' is the flag), 'www.ck' for
// '!www.ck'. One hash lookup per candidate suffix then answers the whole PSL algorithm.
//
// Built on the first url_domain call rather than at load, so a session that never calls it pays
// nothing for carrying the list (the build itself is a few milliseconds). Immutable afterwards, so
// the threads of a scan share it without a lock — the local static's initialization is the only
// synchronization there is to do.
struct PublicSuffixIndex {
	PublicSuffixIndex() {
		rules.reserve(url_tools_psl::PUBLIC_SUFFIX_RULE_COUNT);
		for (size_t index = 0; index < url_tools_psl::PUBLIC_SUFFIX_RULE_COUNT; index++) {
			std::string_view rule(url_tools_psl::PUBLIC_SUFFIX_RULES[index]);
			uint8_t kind = PSL_RULE_NORMAL;
			if (rule.front() == '!') {
				kind = PSL_RULE_EXCEPTION;
				rule.remove_prefix(1);
			} else if (rule.compare(0, 2, "*.") == 0) {
				kind = PSL_RULE_WILDCARD;
				rule.remove_prefix(2);
			}
			rules[Ascii(rule)] |= kind;
		}
	}

	uint8_t Lookup(std::string_view suffix) const {
		auto entry = rules.find(suffix);
		return entry == rules.end() ? 0 : entry->second;
	}

	// An IDN rule is written in Unicode ('рф'), a parsed host is serialized in punycode
	// ('xn--p1ai'), and the two have to meet. They meet through ada's own IDNA rather than through a
	// second implementation in the generator: the host serialization matched against here comes out
	// of exactly this code, so there is nothing for the two to disagree about.
	std::string_view Ascii(std::string_view rule) {
		if (std::all_of(rule.begin(), rule.end(), [](unsigned char byte) { return byte < 0x80; })) {
			return rule;
		}
		return punycode_rules.emplace_back(ada::idna::to_ascii(rule));
	}

	// The keys of ASCII rules view into the static rule table; the keys of IDN rules view into these
	// strings, which therefore may never move — a deque, not a vector.
	std::deque<std::string> punycode_rules;
	ankerl::unordered_dense::map<std::string_view, uint8_t> rules;
};

static const PublicSuffixIndex &GetPublicSuffixIndex() {
	static const PublicSuffixIndex index;
	return index;
}

// The registrable domain (eTLD+1) of a host, per the PSL algorithm: the longest matching rule names
// the public suffix, and the answer is that suffix plus one more label.
//
// Candidates run longest-first (label by label from the left), so the first ordinary match is
// already the longest one — but an exception rule prevails over every other match regardless of its
// length, which is why finding a match does not end the walk. Only the two labels left of the
// current candidate are ever needed (the wildcard's public suffix reaches one label further left
// than the normal rule's), so the walk carries them instead of an offset table.
static std::optional<std::string_view> UrlToolsRegistrableDomain(std::string_view host) {
	auto &index = GetPublicSuffixIndex();
	// The root label of an FQDN ('example.com.') is not a label and matches nothing; it takes no part
	// in the algorithm but stays in the answer, which is a suffix of the host as the caller sees it.
	auto labeled = StripTrailingChar(host, '.');
	constexpr size_t NO_LABEL = std::string_view::npos;
	size_t before2 = NO_LABEL;
	size_t before = NO_LABEL;
	size_t candidate = 0;
	size_t domain_start = NO_LABEL;
	bool matched = false;
	for (;;) {
		// An empty label is not a label: ada parses 'foo..example.com' and hands the host over as it
		// stands, but no rule of the list is about such a string and no name registers under it. The
		// walk is where it shows — every candidate starts a label, so an empty one is a dot (or the end
		// of the host) sitting where a label should begin.
		if (candidate == labeled.size() || labeled[candidate] == '.') {
			return std::nullopt;
		}
		auto kinds = index.Lookup(labeled.substr(candidate));
		if (kinds & PSL_RULE_EXCEPTION) {
			// '!www.ck' says www.ck is registrable after all: the exception's own labels are the answer.
			return host.substr(candidate);
		}
		if (!matched) {
			if ((kinds & PSL_RULE_WILDCARD) && before != NO_LABEL) {
				// '*.ck' makes <label>.ck the public suffix, so the answer starts one label further left
				// than an ordinary rule's would.
				domain_start = before2;
				matched = true;
			} else if (kinds & PSL_RULE_NORMAL) {
				domain_start = before;
				matched = true;
			}
		}
		auto dot = labeled.find('.', candidate);
		if (dot == std::string_view::npos) {
			break;
		}
		before2 = before;
		before = candidate;
		candidate = dot + 1;
	}
	if (!matched) {
		// No rule matched at all: the algorithm's implicit '*' rule makes the last label the public
		// suffix, which is what gives an unlisted TLD the same shape as a listed one.
		domain_start = before;
	}
	if (domain_start == NO_LABEL) {
		// Nothing left of the public suffix: the host IS one ('co.uk'), or it is a single label
		// ('localhost'). Either way there is no registrable domain to hand back.
		return std::nullopt;
	}
	return host.substr(domain_start);
}

static LogicalType QueryParamsMapType(QueryValuesMode mode) {
	auto value_type = mode == QueryValuesMode::ALL ? LogicalType::LIST(LogicalType::VARCHAR) : LogicalType::VARCHAR;
	return LogicalType::MAP(LogicalType::VARCHAR, value_type);
}

// The query_values axis selects the shape of the query field: 'raw' hands back the undecoded query
// string and collects nothing, the parsed modes hand back a MAP whose value type is what the mode
// promises.
static LogicalType UrlComponentsType(QueryValuesMode mode) {
	child_list_t<LogicalType> children {{"scheme", LogicalType::VARCHAR},
	                                    {"host", LogicalType::VARCHAR},
	                                    {"port", LogicalType::USMALLINT},
	                                    {"path", LogicalType::VARCHAR}};
	if (mode == QueryValuesMode::RAW) {
		children.push_back({"query", LogicalType::VARCHAR});
	} else {
		children.push_back({"query_params", QueryParamsMapType(mode)});
	}
	children.push_back({"fragment", LogicalType::VARCHAR});
	return LogicalType::STRUCT(children);
}

static string ValuesAxisModeList(const ValuesAxis &axis) {
	vector<string> names;
	for (idx_t index = 0; index < axis.mode_count; index++) {
		names.push_back(StringUtil::Format("'%s'", axis.modes[index].name));
	}
	return StringUtil::Join(names, ", ");
}

// A named call arrives at bind as an ordinary positional argument carrying an alias, so the alias
// is the only thing that can tell two same-typed optionals apart:
// query_params_from_string(qs, values := 'all') names the second optional without supplying the
// first, while query_params_from_string(qs, '|') is still a separator. Returns, per parameter, the
// index of the argument filling it (INVALID_INDEX when the caller left it out).
//
// An alias does not by itself mean the caller wrote a name: a column reference reaches bind
// carrying its column name, and a column is a legitimate positional separator. Only a foldable
// argument could have been named, which is why an unrecognized alias is a caller bug exactly there.
// An unresolved placeholder is neither — it cannot be classified until it has a value, so the bind
// is deferred to execution time.
static vector<idx_t> BindNamedArguments(const char *function_name, const vector<unique_ptr<Expression>> &arguments,
                                        idx_t first_optional, const vector<string> &names) {
	vector<idx_t> filled_by(names.size(), DConstants::INVALID_INDEX);
	for (idx_t argument_index = first_optional; argument_index < arguments.size(); argument_index++) {
		auto &argument = *arguments[argument_index];
		auto &alias = argument.GetAlias();
		auto named = std::find(names.begin(), names.end(), alias);
		if (named == names.end() && !alias.empty()) {
			if (argument.HasParameter()) {
				throw ParameterNotResolvedException();
			}
			if (argument.IsFoldable()) {
				throw BinderException("%s: Unknown argument '%s'", function_name, alias);
			}
		}
		auto parameter =
		    named != names.end() ? static_cast<idx_t>(named - names.begin()) : argument_index - first_optional;
		if (filled_by[parameter] != DConstants::INVALID_INDEX) {
			throw BinderException("%s: argument '%s' specified more than once", function_name, names[parameter]);
		}
		filled_by[parameter] = argument_index;
	}
	return filled_by;
}

// The axis selects the result type, so it has to be resolved by the binder: a column reference, a
// NULL, or a name we do not know cannot yield a type and is a caller bug.
static string BindValuesAxisName(ClientContext &context, Expression &argument, const ValuesAxis &axis) {
	if (argument.HasParameter()) {
		throw ParameterNotResolvedException();
	}
	if (!argument.IsFoldable()) {
		throw BinderException("%s: %s must be a constant, one of %s", axis.function_name, axis.parameter_name,
		                      ValuesAxisModeList(axis));
	}
	auto mode_value = ExpressionExecutor::EvaluateScalar(context, argument);
	if (mode_value.IsNull()) {
		throw BinderException("%s: %s must not be NULL, expected one of %s", axis.function_name, axis.parameter_name,
		                      ValuesAxisModeList(axis));
	}
	return StringValue::Get(mode_value);
}

static QueryValuesMode LookupQueryValuesMode(const string &name, const ValuesAxis &axis) {
	for (idx_t index = 0; index < axis.mode_count; index++) {
		if (name == axis.modes[index].name) {
			return axis.modes[index].mode;
		}
	}
	throw BinderException("%s: unknown %s '%s', expected one of %s", axis.function_name, axis.parameter_name, name,
	                      ValuesAxisModeList(axis));
}

static QueryValuesMode BindQueryValuesMode(ClientContext &context, Expression &argument, const ValuesAxis &axis) {
	return LookupQueryValuesMode(BindValuesAxisName(context, argument, axis), axis);
}

struct QueryValuesBindData : public FunctionData {
	explicit QueryValuesBindData(QueryValuesMode mode) : mode(mode) {
	}

	QueryValuesMode mode;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<QueryValuesBindData>(mode);
	}
	bool Equals(const FunctionData &other) const override {
		return mode == other.Cast<QueryValuesBindData>().mode;
	}
};

static QueryValuesMode BindOptionalMode(ClientContext &context, const vector<unique_ptr<Expression>> &arguments,
                                        idx_t argument_index, const ValuesAxis &axis, QueryValuesMode fallback) {
	if (argument_index == DConstants::INVALID_INDEX) {
		return fallback;
	}
	return BindQueryValuesMode(context, *arguments[argument_index], axis);
}

// The one-argument spelling is the 'raw' mode, bound through the same path so the two are
// observationally identical. The mode is re-derived from the argument on every bind — including
// the re-bind of a deserialized plan — so the bind data needs no serialization callbacks of its own.
static unique_ptr<FunctionData> UrlComponentsBind(ClientContext &context, ScalarFunction &bound_function,
                                                  vector<unique_ptr<Expression>> &arguments) {
	auto filled_by = BindNamedArguments("url_components", arguments, 1, {"query_values"});
	auto mode = BindOptionalMode(context, arguments, filled_by[0], URL_COMPONENTS_AXIS, QueryValuesMode::RAW);
	bound_function.SetReturnType(UrlComponentsType(mode));
	return make_uniq<QueryValuesBindData>(mode);
}

// query_params and query_params_loose take the same axis over the same single optional argument and
// differ only in where a row's parameters come from, so they share their bind rather than keeping two
// copies of it in step.
static unique_ptr<FunctionData> BindQueryParamsMapMode(ClientContext &context, ScalarFunction &bound_function,
                                                       vector<unique_ptr<Expression>> &arguments,
                                                       const ValuesAxis &axis) {
	auto filled_by = BindNamedArguments(axis.function_name, arguments, 1, {"query_values"});
	auto mode = BindOptionalMode(context, arguments, filled_by[0], axis, QueryValuesMode::ALL);
	bound_function.SetReturnType(QueryParamsMapType(mode));
	return make_uniq<QueryValuesBindData>(mode);
}

static unique_ptr<FunctionData> QueryParamsBind(ClientContext &context, ScalarFunction &bound_function,
                                                vector<unique_ptr<Expression>> &arguments) {
	return BindQueryParamsMapMode(context, bound_function, arguments, QUERY_PARAMS_AXIS);
}

static unique_ptr<FunctionData> QueryParamsLooseBind(ClientContext &context, ScalarFunction &bound_function,
                                                     vector<unique_ptr<Expression>> &arguments) {
	return BindQueryParamsMapMode(context, bound_function, arguments, QUERY_PARAMS_LOOSE_AXIS);
}

// Which argument carries the separator is decided by the aliases, so the resolution is bind data:
// query_params_from_string(qs, query_values := 'all') and query_params_from_string(qs, '|') are the
// same two constants once the names are gone. The names are not gone, though — a bound plan
// serializes each child's alias with it, so a re-bind sees them again and re-derives this from the
// arguments (test/plan pins it). No Serialize/Deserialize callbacks needed.
struct QueryParamsFromStringBindData : public FunctionData {
	QueryParamsFromStringBindData(QueryValuesMode mode, idx_t separator_index)
	    : mode(mode), separator_index(separator_index) {
	}

	QueryValuesMode mode;
	idx_t separator_index;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<QueryParamsFromStringBindData>(mode, separator_index);
	}
	bool Equals(const FunctionData &other) const override {
		auto &other_data = other.Cast<QueryParamsFromStringBindData>();
		return mode == other_data.mode && separator_index == other_data.separator_index;
	}
};

static unique_ptr<FunctionData> QueryParamsFromStringBind(ClientContext &context, ScalarFunction &bound_function,
                                                          vector<unique_ptr<Expression>> &arguments) {
	auto filled_by = BindNamedArguments("query_params_from_string", arguments, 1, {"sep", "query_values"});
	auto mode = BindOptionalMode(context, arguments, filled_by[1], QUERY_PARAMS_FROM_STRING_AXIS, QueryValuesMode::ALL);
	bound_function.SetReturnType(QueryParamsMapType(mode));
	return make_uniq<QueryParamsFromStringBindData>(mode, filled_by[0]);
}

static unique_ptr<FunctionData> QueryParamBind(ClientContext &context, ScalarFunction &,
                                               vector<unique_ptr<Expression>> &arguments) {
	auto filled_by = BindNamedArguments("query_param", arguments, 2, {"query_values"});
	if (filled_by[0] == DConstants::INVALID_INDEX) {
		return make_uniq<QueryValuesBindData>(QueryValuesMode::LAST);
	}
	auto name = BindValuesAxisName(context, *arguments[filled_by[0]], QUERY_PARAM_AXIS);
	// 'all' is a legal mode on every map-returning function; a scalar result cannot carry a list, so
	// name the function that hands one back rather than only listing what is allowed here.
	if (name == "all") {
		throw BinderException("query_param: query_values 'all' has no scalar result, use query_params(url, 'all')");
	}
	return make_uniq<QueryValuesBindData>(LookupQueryValuesMode(name, QUERY_PARAM_AXIS));
}

static QueryValuesMode BoundQueryValuesMode(ExpressionState &state) {
	return state.expr.Cast<BoundFunctionExpression>().bind_info->Cast<QueryValuesBindData>().mode;
}

static UrlToolsLocalState &GetUrlToolsLocalState(ExpressionState &state) {
	auto state_ptr = ExecuteFunctionState::GetFunctionState(state);
	D_ASSERT(state_ptr);
	return state_ptr->Cast<UrlToolsLocalState>();
}

// url_components(varchar [, query_values]) -> STRUCT(scheme, host, port, path,
// query VARCHAR | query_params MAP, fragment). Total over arbitrary input: unparseable values
// yield a NULL row instead of an error, so one junk value cannot fail a whole scan.
inline void UrlComponentsScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &local_state = GetUrlToolsLocalState(state);
	auto mode = BoundQueryValuesMode(state);

	// An all-constant call is folded by evaluating a single row and reading row 0 of the result,
	// which the executor requires to be a CONSTANT_VECTOR — a flat answer there is a contract
	// violation, not a shape it adapts to. UnaryExecutor carried this for us; a hand-written vector
	// carries it itself: write row 0, then flip the type once the writes are done.
	auto constant_result = args.AllConstant();
	auto row_count = constant_result ? 1 : args.size();

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto &children = StructVector::GetEntries(result);
	D_ASSERT(children.size() == 6);
	for (auto &child : children) {
		child->SetVectorType(VectorType::FLAT_VECTOR);
	}
	auto &scheme_vec = *children[0];
	auto &host_vec = *children[1];
	auto &port_vec = *children[2];
	auto &path_vec = *children[3];
	auto &query_vec = *children[4];
	auto &fragment_vec = *children[5];

	std::optional<UrlToolsMapWriter> map_writer;
	if (mode != QueryValuesMode::RAW) {
		map_writer.emplace(query_vec, mode);
	}

	auto set_null_row = [&](idx_t row) {
		FlatVector::SetNull(result, row, true);
		for (auto &child : children) {
			FlatVector::SetNull(*child, row, true);
		}
		if (map_writer) {
			map_writer->WriteNullRow(row);
		}
	};
	auto set_string_field = [](Vector &vec, idx_t row, std::optional<std::string_view> value) {
		if (!value) {
			FlatVector::SetNull(vec, row, true);
			return;
		}
		FlatVector::GetData<string_t>(vec)[row] = UrlToolsAddString(vec, *value);
	};

	UnifiedVectorFormat input_data;
	args.data[0].ToUnifiedFormat(args.size(), input_data);
	auto inputs = UnifiedVectorFormat::GetData<string_t>(input_data);

	for (idx_t row = 0; row < row_count; row++) {
		auto input_idx = input_data.sel->get_index(row);
		if (!input_data.validity.RowIsValid(input_idx)) {
			set_null_row(row);
			continue;
		}
		auto &input = inputs[input_idx];
		std::string_view raw(input.GetDataUnsafe(), input.GetSize());
		if (raw.empty()) {
			set_null_row(row);
			continue;
		}
		auto parsed = UrlToolsParseInput(raw, local_state);
		if (!parsed.url) {
			set_null_row(row);
			continue;
		}
		auto &url = *parsed.url;

		set_string_field(scheme_vec, row, UrlSchemeField(url, parsed.relative));
		set_string_field(host_vec, row, UrlHostField(url, parsed.relative));
		auto port = UrlPortField(url, parsed.relative);
		if (port) {
			FlatVector::GetData<uint16_t>(port_vec)[row] = *port;
		} else {
			FlatVector::SetNull(port_vec, row, true);
		}
		set_string_field(path_vec, row, UrlPathField(url, parsed.relative));
		set_string_field(fragment_vec, row, UrlFragmentField(url, parsed.relative));

		auto query = UrlQueryField(url, parsed.relative);
		if (map_writer) {
			UrlToolsWriteQueryParamsMap(*map_writer, row, *query, "&", local_state);
		} else {
			set_string_field(query_vec, row, query);
		}
	}

	if (map_writer) {
		map_writer->Finish();
	}
	if (constant_result) {
		// A constant STRUCT carries the vector type down to its children; DuckDB verifies it.
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
		for (auto &child : children) {
			child->SetVectorType(VectorType::CONSTANT_VECTOR);
		}
	}
}

// Every map-returning function shares one result shape: a flat map vector written row by row, and
// the constant-vector contract an all-constant call carries (see UrlComponentsScalarFun — the
// executor evaluates one row and reads row 0 back, which it requires to be CONSTANT). Only where a
// row's parameters come from differs, so that is all a caller writes — and the contract is held in
// one place rather than in a copy per function.
template <class WriteRows>
static void UrlToolsWriteMapResult(DataChunk &args, Vector &result, QueryValuesMode mode, WriteRows write_rows) {
	auto constant_result = args.AllConstant();
	result.SetVectorType(VectorType::FLAT_VECTOR);
	UrlToolsMapWriter writer(result, mode);
	write_rows(writer, constant_result ? 1 : args.size());
	writer.Finish();
	if (constant_result) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

// query_params(varchar [, query_values]) -> MAP of decoded query parameters. Accepts the same inputs
// as url_components; anything without a parseable query yields an empty map, so downstream key access
// stays uniform. NULL input is the one NULL map: it is the absence of a URL, not of parameters.
inline void QueryParamsScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &local_state = GetUrlToolsLocalState(state);

	UnifiedVectorFormat input_data;
	args.data[0].ToUnifiedFormat(args.size(), input_data);
	auto inputs = UnifiedVectorFormat::GetData<string_t>(input_data);

	UrlToolsWriteMapResult(args, result, BoundQueryValuesMode(state), [&](UrlToolsMapWriter &writer, idx_t row_count) {
		for (idx_t row = 0; row < row_count; row++) {
			auto input_idx = input_data.sel->get_index(row);
			if (!input_data.validity.RowIsValid(input_idx)) {
				writer.WriteNullRow(row);
				continue;
			}
			auto &input = inputs[input_idx];
			auto parsed = UrlToolsParseInput(std::string_view(input.GetDataUnsafe(), input.GetSize()), local_state);
			auto query =
			    parsed.url ? StripLeadingChar(std::string_view(parsed.url->get_search()), '?') : std::string_view();
			UrlToolsWriteQueryParamsMap(writer, row, query, "&", local_state);
		}
	});
}

// query_params_loose(varchar [, query_values]) -> MAP from a string that CARRIES parameters without
// having to be a well-formed URL: a single-page-app fragment ('#/cart?utm_source=push'), a page title
// with a query tail, a bare query string. What it does NOT collect is the point: a plain anchor
// ('#top') and prose are not parameters, so a fragment needs a '?' or a '=' before it counts.
inline void QueryParamsLooseScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &local_state = GetUrlToolsLocalState(state);

	UnifiedVectorFormat input_data;
	args.data[0].ToUnifiedFormat(args.size(), input_data);
	auto inputs = UnifiedVectorFormat::GetData<string_t>(input_data);

	UrlToolsWriteMapResult(args, result, BoundQueryValuesMode(state), [&](UrlToolsMapWriter &writer, idx_t row_count) {
		for (idx_t row = 0; row < row_count; row++) {
			auto input_idx = input_data.sel->get_index(row);
			if (!input_data.validity.RowIsValid(input_idx)) {
				writer.WriteNullRow(row);
				continue;
			}
			auto &input = inputs[input_idx];
			ada::url_search_params fragment_params;
			ada::url_search_params query_params;
			UrlToolsCollectLooseParams(std::string_view(input.GetDataUnsafe(), input.GetSize()), fragment_params,
			                           query_params, local_state);
			writer.WriteRow(row, local_state);
			UrlToolsClearQueryParams(local_state);
		}
	});
}

// query_params_from_string(varchar [, sep [, values]]) -> MAP from a bare query string (no URL
// around it); a leading '?' is tolerated. The separator is an ordinary per-row argument — it does
// not select the result type — and an empty one is a caller bug, loud at runtime.
inline void QueryParamsFromStringScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &local_state = GetUrlToolsLocalState(state);
	auto &bind_data = state.expr.Cast<BoundFunctionExpression>().bind_info->Cast<QueryParamsFromStringBindData>();

	UnifiedVectorFormat input_data;
	args.data[0].ToUnifiedFormat(args.size(), input_data);
	auto inputs = UnifiedVectorFormat::GetData<string_t>(input_data);

	UnifiedVectorFormat separator_data;
	if (bind_data.separator_index != DConstants::INVALID_INDEX) {
		args.data[bind_data.separator_index].ToUnifiedFormat(args.size(), separator_data);
	}

	UrlToolsWriteMapResult(args, result, bind_data.mode, [&](UrlToolsMapWriter &writer, idx_t row_count) {
		for (idx_t row = 0; row < row_count; row++) {
			auto input_idx = input_data.sel->get_index(row);
			if (!input_data.validity.RowIsValid(input_idx)) {
				writer.WriteNullRow(row);
				continue;
			}
			std::string_view separator("&");
			if (bind_data.separator_index != DConstants::INVALID_INDEX) {
				auto separator_idx = separator_data.sel->get_index(row);
				if (!separator_data.validity.RowIsValid(separator_idx)) {
					writer.WriteNullRow(row);
					continue;
				}
				auto &separator_input = UnifiedVectorFormat::GetData<string_t>(separator_data)[separator_idx];
				separator = std::string_view(separator_input.GetDataUnsafe(), separator_input.GetSize());
				if (separator.empty()) {
					throw InvalidInputException("query_params_from_string: separator must not be empty");
				}
			}
			auto &input = inputs[input_idx];
			auto query = StripLeadingChar(std::string_view(input.GetDataUnsafe(), input.GetSize()), '?');
			UrlToolsWriteQueryParamsMap(writer, row, query, separator, local_state);
		}
	});
}

// query_param(varchar, varchar [, values]) -> the decoded value of one key. It stops at that value:
// no map is built and no per-key collection happens, which is the whole reason to reach for it
// instead of query_params(url)[key]. An absent key is NULL; a present key with an empty value is ''.
inline void QueryParamScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &local_state = GetUrlToolsLocalState(state);
	auto mode = BoundQueryValuesMode(state);

	BinaryExecutor::ExecuteWithNulls<string_t, string_t, string_t>(
	    args.data[0], args.data[1], result, args.size(),
	    [&](const string_t &url_input, const string_t &key_input, ValidityMask &mask, idx_t row) {
		    auto parsed =
		        UrlToolsParseInput(std::string_view(url_input.GetDataUnsafe(), url_input.GetSize()), local_state);
		    if (!parsed.url) {
			    mask.SetInvalid(row);
			    return string_t();
		    }
		    ada::url_search_params params;
		    params.reset(StripLeadingChar(std::string_view(parsed.url->get_search()), '?'));

		    // The map's keys are the sanitized ones, so the comparison has to run against those too
		    // — otherwise a key with ill-formed bytes would be unreachable through this function.
		    local_state.sanitized.clear();
		    std::string_view key(key_input.GetDataUnsafe(), key_input.GetSize());
		    std::optional<std::string_view> found;
		    for (const auto &entry : params) {
			    if (UrlToolsWellFormed(local_state, entry.first) != key) {
				    continue;
			    }
			    found = entry.second;
			    if (mode == QueryValuesMode::FIRST) {
				    break;
			    }
		    }
		    if (!found) {
			    mask.SetInvalid(row);
			    return string_t();
		    }
		    return UrlToolsAddString(result, UrlToolsWellFormed(local_state, *found));
	    });
}

// url_scheme / url_host / url_path / url_query / url_fragment (varchar) -> one component of the URL.
// What they do NOT do is why they exist: one parse, one field read, no query parsing, no map, no
// struct — the cost of url_components(url, 'raw') without the five fields the caller did not ask for.
// Each equals that struct's field on every input because both read it through the same function.
template <UrlStringField GetField>
inline void UrlStringAccessorFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &local_state = GetUrlToolsLocalState(state);

	UnaryExecutor::ExecuteWithNulls<string_t, string_t>(
	    args.data[0], result, args.size(), [&](const string_t &input, ValidityMask &mask, idx_t row) {
		    auto parsed = UrlToolsParseInput(std::string_view(input.GetDataUnsafe(), input.GetSize()), local_state);
		    if (!parsed.url) {
			    mask.SetInvalid(row);
			    return string_t();
		    }
		    auto field = GetField(*parsed.url, parsed.relative);
		    if (!field) {
			    mask.SetInvalid(row);
			    return string_t();
		    }
		    return UrlToolsAddString(result, *field);
	    });
}

// url_port(varchar) -> USMALLINT. The one accessor whose component is not text; the same NULLs
// (unparseable input, relative input, no port, the scheme's default port) reach it as one nullopt.
inline void UrlPortScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &local_state = GetUrlToolsLocalState(state);

	UnaryExecutor::ExecuteWithNulls<string_t, uint16_t>(
	    args.data[0], result, args.size(), [&](const string_t &input, ValidityMask &mask, idx_t row) -> uint16_t {
		    auto parsed = UrlToolsParseInput(std::string_view(input.GetDataUnsafe(), input.GetSize()), local_state);
		    if (!parsed.url) {
			    mask.SetInvalid(row);
			    return 0;
		    }
		    auto port = UrlPortField(*parsed.url, parsed.relative);
		    if (!port) {
			    mask.SetInvalid(row);
			    return 0;
		    }
		    return *port;
	    });
}

// url_domain(varchar) -> the registrable domain of the URL's host (eTLD+1), the unit "one site" is
// counted in: every host under m.ozon.ru and ozon.ru answers ozon.ru. NULL where no such unit
// exists — an IP literal, a host that IS a public suffix, a single label, a URL that does not parse.
// The host is the parser's serialization, so an IDN host answers in punycode, the form it stores in.
inline void UrlDomainScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &local_state = GetUrlToolsLocalState(state);

	UnaryExecutor::ExecuteWithNulls<string_t, string_t>(
	    args.data[0], result, args.size(), [&](const string_t &input, ValidityMask &mask, idx_t row) {
		    auto parsed = UrlToolsParseInput(std::string_view(input.GetDataUnsafe(), input.GetSize()), local_state);
		    // An address is not a name: there is no suffix to look up and no site to group by. The
		    // parser has already classified the host, so nothing is re-derived from its text here.
		    if (!parsed.url || parsed.url->host_type != ada::url_host_type::DEFAULT) {
			    mask.SetInvalid(row);
			    return string_t();
		    }
		    auto host = UrlHostField(*parsed.url, parsed.relative);
		    auto domain = host ? UrlToolsRegistrableDomain(*host) : std::nullopt;
		    if (!domain) {
			    mask.SetInvalid(row);
			    return string_t();
		    }
		    return UrlToolsAddString(result, *domain);
	    });
}

static void LoadInternal(ExtensionLoader &loader) {
	// The result type comes from the bind function, so ANY is the registration-time placeholder
	// (the struct_extract precedent), and the NULL rows are written by the function itself. Every
	// arity shares one bind and one execute: the shorter spellings are the defaults of the longer
	// ones, not separate behavior.
	auto bind_typed_function = [](vector<LogicalType> arguments, scalar_function_t function,
	                              bind_scalar_function_t bind) {
		ScalarFunction typed_function(std::move(arguments), LogicalType::ANY, std::move(function), bind, nullptr,
		                              nullptr, UrlToolsInitLocalState);
		typed_function.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
		return typed_function;
	};

	ScalarFunctionSet url_components_set("url_components");
	url_components_set.AddFunction(
	    bind_typed_function({LogicalType::VARCHAR}, UrlComponentsScalarFun, UrlComponentsBind));
	url_components_set.AddFunction(
	    bind_typed_function({LogicalType::VARCHAR, LogicalType::VARCHAR}, UrlComponentsScalarFun, UrlComponentsBind));
	loader.RegisterFunction(url_components_set);

	ScalarFunctionSet query_params_set("query_params");
	query_params_set.AddFunction(bind_typed_function({LogicalType::VARCHAR}, QueryParamsScalarFun, QueryParamsBind));
	query_params_set.AddFunction(
	    bind_typed_function({LogicalType::VARCHAR, LogicalType::VARCHAR}, QueryParamsScalarFun, QueryParamsBind));
	loader.RegisterFunction(query_params_set);

	ScalarFunctionSet query_params_from_string_set("query_params_from_string");
	query_params_from_string_set.AddFunction(
	    bind_typed_function({LogicalType::VARCHAR}, QueryParamsFromStringScalarFun, QueryParamsFromStringBind));
	query_params_from_string_set.AddFunction(bind_typed_function(
	    {LogicalType::VARCHAR, LogicalType::VARCHAR}, QueryParamsFromStringScalarFun, QueryParamsFromStringBind));
	query_params_from_string_set.AddFunction(
	    bind_typed_function({LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
	                        QueryParamsFromStringScalarFun, QueryParamsFromStringBind));
	loader.RegisterFunction(query_params_from_string_set);

	ScalarFunctionSet query_params_loose_set("query_params_loose");
	query_params_loose_set.AddFunction(
	    bind_typed_function({LogicalType::VARCHAR}, QueryParamsLooseScalarFun, QueryParamsLooseBind));
	query_params_loose_set.AddFunction(bind_typed_function({LogicalType::VARCHAR, LogicalType::VARCHAR},
	                                                       QueryParamsLooseScalarFun, QueryParamsLooseBind));
	loader.RegisterFunction(query_params_loose_set);

	// The scalar result type never varies, but the axis still has to be resolved at bind time: 'all'
	// has no scalar answer, and that is a caller bug worth reporting before the scan. It also has to
	// see a NULL axis, which is why NULL handling is the function's own: under DEFAULT handling the
	// binder folds a call with any constant-NULL argument straight to NULL, and query_values := NULL
	// would quietly answer NULL instead of failing. NULL url/key rows still yield NULL — the
	// executor's own NULL handling, not the binder's.
	auto query_param_function = [](vector<LogicalType> arguments) {
		ScalarFunction function(std::move(arguments), LogicalType::VARCHAR, QueryParamScalarFun, QueryParamBind,
		                        nullptr, nullptr, UrlToolsInitLocalState);
		function.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
		return function;
	};
	ScalarFunctionSet query_param_set("query_param");
	query_param_set.AddFunction(query_param_function({LogicalType::VARCHAR, LogicalType::VARCHAR}));
	query_param_set.AddFunction(
	    query_param_function({LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR}));
	loader.RegisterFunction(query_param_set);

	// The accessors have no axis and no bind: their result type is fixed, and NULL input yielding NULL
	// is exactly the default null handling, so the executor carries it (and the constant-vector
	// contract) for them.
	auto accessor_function = [&loader](const char *name, LogicalType return_type, scalar_function_t function) {
		loader.RegisterFunction(ScalarFunction(name, {LogicalType::VARCHAR}, std::move(return_type),
		                                       std::move(function), nullptr, nullptr, nullptr, UrlToolsInitLocalState));
	};
	accessor_function("url_scheme", LogicalType::VARCHAR, UrlStringAccessorFun<UrlSchemeField>);
	accessor_function("url_host", LogicalType::VARCHAR, UrlStringAccessorFun<UrlHostField>);
	accessor_function("url_port", LogicalType::USMALLINT, UrlPortScalarFun);
	accessor_function("url_path", LogicalType::VARCHAR, UrlStringAccessorFun<UrlPathField>);
	accessor_function("url_query", LogicalType::VARCHAR, UrlStringAccessorFun<UrlQueryField>);
	accessor_function("url_fragment", LogicalType::VARCHAR, UrlStringAccessorFun<UrlFragmentField>);
	accessor_function("url_domain", LogicalType::VARCHAR, UrlDomainScalarFun);
}

} // namespace

void UrlToolsExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string UrlToolsExtension::Name() {
	return "url_tools";
}

std::string UrlToolsExtension::Version() const {
#ifdef EXT_VERSION_URL_TOOLS
	return EXT_VERSION_URL_TOOLS;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(url_tools, loader) {
	duckdb::LoadInternal(loader);
}
}
