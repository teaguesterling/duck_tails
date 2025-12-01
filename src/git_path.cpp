// git_path.cpp
#include "git_path.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

static void ValidateNoDotDot(const string &path) {
	// Split without allocating too much: simple scan
	idx_t i = 0, n = path.size();
	while (i < n) {
		idx_t j = i;
		while (j < n && path[j] != '/')
			j++;
		if (j - i == 2 && path[i] == '.' && path[i + 1] == '.') {
			throw InvalidInputException("git_tree: path must not contain '..'");
		}
		i = (j == n) ? n : j + 1;
	}
}

string NormalizeRepoPathSpec(const string &in) {
	if (in.empty())
		return in;

	// 1) Strip leading "./" repeatedly (ergonomics)
	string s = in;
	while (StringUtil::StartsWith(s, "./")) {
		s.erase(0, 2);
	}

	// 2) Strip leading/trailing '/'
	while (!s.empty() && s.front() == '/')
		s.erase(s.begin());
	while (!s.empty() && s.back() == '/')
		s.pop_back();

	if (s.empty())
		return s;

	// 3) Collapse consecutive '/'
	string out;
	out.reserve(s.size());
	bool last_slash = false;
	for (char c : s) {
		if (c == '/') {
			if (!last_slash)
				out.push_back('/');
			last_slash = true;
		} else {
			out.push_back(c);
			last_slash = false;
		}
	}

	// 4) Validate: forbid any '..' segment; do NOT alter mid-path './'
	ValidateNoDotDot(out);

	// 5) Return as-is (no case-folding, no '\' → '/' translation)
	return out;
}

} // namespace duckdb
