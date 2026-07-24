#include "psl_index.hpp"

#include "ada.h"
#include "psl_rules.h"
#include "ankerl/unordered_dense.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <string>

namespace duckdb {

namespace {

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

const PublicSuffixIndex &GetPublicSuffixIndex() {
	static const PublicSuffixIndex index;
	return index;
}

} // namespace

// The registrable domain (eTLD+1) of a host, per the PSL algorithm: the longest matching rule names
// the public suffix, and the answer is that suffix plus one more label.
//
// Candidates run longest-first (label by label from the left), so the first ordinary match is
// already the longest one — but an exception rule prevails over every other match regardless of its
// length, which is why finding a match does not end the walk. Only the two labels left of the
// current candidate are ever needed (the wildcard's public suffix reaches one label further left
// than the normal rule's), so the walk carries them instead of an offset table.
std::optional<std::string_view> UrlToolsRegistrableDomain(std::string_view host) {
	auto &index = GetPublicSuffixIndex();
	// The root label of an FQDN ('example.com.') is not a label and matches nothing; it takes no part
	// in the algorithm but stays in the answer, which is a suffix of the host as the caller sees it.
	auto labeled = host;
	if (!labeled.empty() && labeled.back() == '.') {
		labeled.remove_suffix(1);
	}
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

} // namespace duckdb
