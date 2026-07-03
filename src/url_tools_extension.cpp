#define DUCKDB_EXTENSION_MAIN

#include "url_tools_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/allocator.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/execution/expression_executor_state.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "ada.h"
#include "utf8proc_wrapper.hpp"
#include "../duckdb/extension/json/include/json_common.hpp"
#include "ankerl/unordered_dense.h"
#include "yyjson.hpp"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace duckdb {

namespace {

using duckdb_yyjson::yyjson_mut_doc;
using duckdb_yyjson::yyjson_mut_doc_new;
using duckdb_yyjson::yyjson_mut_doc_set_root;
using duckdb_yyjson::yyjson_mut_obj;
using duckdb_yyjson::yyjson_mut_obj_add;
using duckdb_yyjson::yyjson_mut_val;
using duckdb_yyjson::yyjson_mut_write_opts;

struct YyjsonMutDocDeleter {
	void operator()(yyjson_mut_doc *doc) const {
		duckdb_yyjson::yyjson_mut_doc_free(doc);
	}
};
using yyjson_mut_doc_ptr = std::unique_ptr<yyjson_mut_doc, YyjsonMutDocDeleter>;

struct UrlToolsQueryParam {
	std::string_view key;
	std::string_view value;
	uint64_t key_hash;
};

struct UrlToolsLocalState : public FunctionLocalState {
	explicit UrlToolsLocalState(Allocator &allocator) : json_allocator(std::make_shared<JSONAllocator>(allocator)) {
	}

	shared_ptr<JSONAllocator> json_allocator;
	vector<UrlToolsQueryParam> query_params;
	vector<std::pair<std::string, std::string>> decoded_pairs;
	std::string url_buffer;
};

static unique_ptr<FunctionLocalState> UrlToolsInitLocalState(ExpressionState &state, const BoundFunctionExpression &,
                                                             FunctionData *) {
	auto &context = state.GetContext();
	return make_uniq<UrlToolsLocalState>(BufferAllocator::Get(context));
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

// Query keys/values are percent-decoded, so they can carry bytes that are not
// valid UTF-8 (e.g. percent-decoding yields a truncated multi-byte sequence).
// yyjson refuses to serialize invalid UTF-8 and would fail the whole call;
// sanitize first so the functions stay total over arbitrary URL input.
static yyjson_mut_val *UrlToolsCopiedString(yyjson_mut_doc *doc, std::string_view value) {
	auto data = value.empty() ? "" : value.data();
	if (!Utf8Proc::IsValid(data, value.size())) {
		auto sanitized = UrlToolsSanitizeUtf8(value);
		auto string_value = yyjson_mut_strncpy(doc, sanitized.c_str(), sanitized.size());
		if (!string_value) {
			throw InternalException("url_tools: failed to allocate string value");
		}
		return string_value;
	}
	auto string_value = yyjson_mut_strncpy(doc, data, value.size());
	if (!string_value) {
		throw InternalException("url_tools: failed to allocate string value");
	}
	return string_value;
}

static void UrlToolsPutQueryParam(UrlToolsLocalState &local_state, std::string_view key, std::string_view value) {
	auto key_hash = ankerl::unordered_dense::hash<std::string_view> {}(key);
	for (auto &query_param : local_state.query_params) {
		if (query_param.key_hash == key_hash && query_param.key == key) {
			query_param.value = value;
			return;
		}
	}
	local_state.query_params.push_back({key, value, key_hash});
}

// Splits a query string on a custom pair separator, mirroring the WHATWG form
// parsing that ada::url_search_params hardcodes for '&': empty segments are
// skipped, the key ends at the first '=', '+' decodes to space, then
// percent-escapes decode. Decoded pairs are collected fully before taking views,
// so vector growth cannot invalidate them.
static void UrlToolsCollectCustomSeparated(std::string_view query, std::string_view separator,
                                           UrlToolsLocalState &local_state) {
	auto &pairs = local_state.decoded_pairs;
	pairs.clear();
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
		UrlToolsPutQueryParam(local_state, pair.first, pair.second);
	}
}

// Builds the query_params JSON object from a raw (undecoded, no leading '?') query
// string. Decoding follows WHATWG form semantics: percent-escapes and '+' as space;
// repeated keys resolve last-wins.
static yyjson_mut_val *UrlToolsBuildQueryParams(yyjson_mut_doc *doc, std::string_view query, std::string_view separator,
                                                UrlToolsLocalState &local_state) {
	auto query_params = yyjson_mut_obj(doc);
	if (!query_params) {
		throw InternalException("url_tools: failed to allocate query_params object");
	}
	if (!query.empty()) {
		ada::url_search_params params;
		if (separator == "&") {
			params.reset(query);
			local_state.query_params.reserve(params.size());
			for (const auto &entry : params) {
				UrlToolsPutQueryParam(local_state, entry.first, entry.second);
			}
		} else {
			UrlToolsCollectCustomSeparated(query, separator, local_state);
		}
		for (const auto &entry : local_state.query_params) {
			auto key_value = UrlToolsCopiedString(doc, entry.key);
			auto string_value = UrlToolsCopiedString(doc, entry.value);
			if (!yyjson_mut_obj_add(query_params, key_value, string_value)) {
				throw InternalException("url_tools: failed to add query parameter");
			}
		}
		// The views above point into params/decoded_pairs, so local state must not
		// retain them across rows.
		local_state.query_params.clear();
		local_state.decoded_pairs.clear();
	}
	return query_params;
}

static string_t UrlToolsWriteQueryParams(Vector &result, std::string_view query, std::string_view separator,
                                         UrlToolsLocalState &local_state) {
	local_state.json_allocator->Reset();
	auto alc = local_state.json_allocator->GetYYAlc();
	auto out_doc = yyjson_mut_doc_ptr(yyjson_mut_doc_new(alc));
	if (!out_doc) {
		throw InternalException("url_tools: failed to allocate output document");
	}
	yyjson_mut_doc_set_root(out_doc.get(), UrlToolsBuildQueryParams(out_doc.get(), query, separator, local_state));
	size_t output_length = 0;
	auto output_cstr = yyjson_mut_write_opts(out_doc.get(), JSONCommon::WRITE_FLAG, nullptr, &output_length, nullptr);
	if (!output_cstr) {
		throw InternalException("url_tools: failed to serialize query_params");
	}
	std::unique_ptr<char, decltype(&free)> output_handle(output_cstr, free);
	return StringVector::AddString(result, output_cstr, output_length);
}

// Shared URL-input handling: absolute URLs of any scheme parse as-is; a single
// leading slash is a relative path (SPA hit tracking sends `ym(id, 'hit', '/path')`
// as-is), parsed via a placeholder scheme; a double slash is a protocol-relative
// URL, which is ambiguous without a base and treated as unparseable.
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

// url_components(varchar) -> STRUCT(scheme, hostname, path, query_params JSON, fragment).
// Total over raw analytics input: unparseable values yield a NULL row instead of an
// error, so one junk value cannot fail a whole scan.
inline void UrlComponentsScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto state_ptr = ExecuteFunctionState::GetFunctionState(state);
	D_ASSERT(state_ptr);
	auto &local_state = state_ptr->Cast<UrlToolsLocalState>();

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto &children = StructVector::GetEntries(result);
	D_ASSERT(children.size() == 5);
	for (auto &child : children) {
		child->SetVectorType(VectorType::FLAT_VECTOR);
	}
	auto &scheme_vec = *children[0];
	auto &hostname_vec = *children[1];
	auto &path_vec = *children[2];
	auto &query_params_vec = *children[3];
	auto &fragment_vec = *children[4];

	auto set_null_row = [&](idx_t row) {
		FlatVector::SetNull(result, row, true);
		for (auto &child : children) {
			FlatVector::SetNull(*child, row, true);
		}
	};
	auto set_string_field = [](Vector &vec, idx_t row, std::string_view value) {
		auto data = value.empty() ? "" : value.data();
		FlatVector::GetData<string_t>(vec)[row] = StringVector::AddString(vec, data, value.size());
	};

	UnifiedVectorFormat input_data;
	args.data[0].ToUnifiedFormat(args.size(), input_data);
	auto inputs = UnifiedVectorFormat::GetData<string_t>(input_data);

	for (idx_t row = 0; row < args.size(); row++) {
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

		if (parsed.relative) {
			FlatVector::SetNull(scheme_vec, row, true);
			FlatVector::SetNull(hostname_vec, row, true);
		} else {
			set_string_field(scheme_vec, row, StripTrailingChar(std::string_view(url.get_protocol()), ':'));
			set_string_field(hostname_vec, row, std::string_view(url.get_hostname()));
		}
		set_string_field(path_vec, row, std::string_view(url.get_pathname()));
		set_string_field(fragment_vec, row, StripLeadingChar(std::string_view(url.get_hash()), '#'));

		auto query = StripLeadingChar(std::string_view(url.get_search()), '?');
		FlatVector::GetData<string_t>(query_params_vec)[row] =
		    UrlToolsWriteQueryParams(query_params_vec, query, "&", local_state);
	}
}

// query_params(varchar) -> JSON object of decoded query parameters. Accepts the
// same inputs as url_components; anything without a parseable query (junk, no query
// part) yields '{}' so downstream JSON access stays uniform.
inline void QueryParamsScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto state_ptr = ExecuteFunctionState::GetFunctionState(state);
	D_ASSERT(state_ptr);
	auto &local_state = state_ptr->Cast<UrlToolsLocalState>();
	auto &input = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(input, result, args.size(), [&](const string_t &url_input) {
		std::string_view raw(url_input.GetDataUnsafe(), url_input.GetSize());
		auto parsed = UrlToolsParseInput(raw, local_state);
		if (!parsed.url) {
			return UrlToolsWriteQueryParams(result, std::string_view(), "&", local_state);
		}
		auto query = StripLeadingChar(std::string_view(parsed.url->get_search()), '?');
		return UrlToolsWriteQueryParams(result, query, "&", local_state);
	});
}

// query_params_from_string(varchar) -> JSON object from a bare query string
// (no URL around it); a leading '?' is tolerated.
inline void QueryParamsFromStringScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto state_ptr = ExecuteFunctionState::GetFunctionState(state);
	D_ASSERT(state_ptr);
	auto &local_state = state_ptr->Cast<UrlToolsLocalState>();
	auto &input = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(input, result, args.size(), [&](const string_t &query_input) {
		auto query = StripLeadingChar(std::string_view(query_input.GetDataUnsafe(), query_input.GetSize()), '?');
		return UrlToolsWriteQueryParams(result, query, "&", local_state);
	});
}

// query_params_from_string(varchar, varchar) -> same, with a custom pair separator
// (e.g. '|' for Adjust deeplink labels).
inline void QueryParamsFromStringSeparatorScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto state_ptr = ExecuteFunctionState::GetFunctionState(state);
	D_ASSERT(state_ptr);
	auto &local_state = state_ptr->Cast<UrlToolsLocalState>();
	BinaryExecutor::Execute<string_t, string_t, string_t>(
	    args.data[0], args.data[1], result, args.size(),
	    [&](const string_t &query_input, const string_t &separator_input) {
		    std::string_view separator(separator_input.GetDataUnsafe(), separator_input.GetSize());
		    if (separator.empty()) {
			    throw InvalidInputException("query_params_from_string: separator must not be empty");
		    }
		    auto query = StripLeadingChar(std::string_view(query_input.GetDataUnsafe(), query_input.GetSize()), '?');
		    return UrlToolsWriteQueryParams(result, query, separator, local_state);
	    });
}

static void LoadInternal(ExtensionLoader &loader) {
	child_list_t<LogicalType> url_components_children {{"scheme", LogicalType::VARCHAR},
	                                                   {"hostname", LogicalType::VARCHAR},
	                                                   {"path", LogicalType::VARCHAR},
	                                                   {"query_params", LogicalType::JSON()},
	                                                   {"fragment", LogicalType::VARCHAR}};
	auto url_components_function =
	    ScalarFunction("url_components", {LogicalType::VARCHAR}, LogicalType::STRUCT(url_components_children),
	                   UrlComponentsScalarFun, nullptr, nullptr, nullptr, UrlToolsInitLocalState);
	url_components_function.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
	loader.RegisterFunction(url_components_function);

	auto query_params_function =
	    ScalarFunction("query_params", {LogicalType::VARCHAR}, LogicalType::JSON(), QueryParamsScalarFun, nullptr,
	                   nullptr, nullptr, UrlToolsInitLocalState);
	loader.RegisterFunction(query_params_function);

	ScalarFunctionSet query_params_from_string_set("query_params_from_string");
	query_params_from_string_set.AddFunction(ScalarFunction({LogicalType::VARCHAR}, LogicalType::JSON(),
	                                                        QueryParamsFromStringScalarFun, nullptr, nullptr, nullptr,
	                                                        UrlToolsInitLocalState));
	query_params_from_string_set.AddFunction(
	    ScalarFunction({LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::JSON(),
	                   QueryParamsFromStringSeparatorScalarFun, nullptr, nullptr, nullptr, UrlToolsInitLocalState));
	loader.RegisterFunction(query_params_from_string_set);
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
