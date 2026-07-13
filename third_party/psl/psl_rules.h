// The Public Suffix List, compiled into a C++ rule table by scripts/generate_psl.py. Rules keep
// their PSL spelling: a leading '!' marks an exception rule, a leading '*.' a wildcard rule, and
// an IDN rule stays in Unicode (the extension converts it through ada's IDNA at load).

#ifndef URL_TOOLS_PSL_RULES_H
#define URL_TOOLS_PSL_RULES_H

#include <cstddef>

namespace url_tools_psl {

extern const char *const PUBLIC_SUFFIX_RULES[];
extern const size_t PUBLIC_SUFFIX_RULE_COUNT;

} // namespace url_tools_psl

#endif // URL_TOOLS_PSL_RULES_H
