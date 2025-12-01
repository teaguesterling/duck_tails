#pragma once

#include "duckdb.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include <string>
#include <vector>

namespace duckdb {

// Forward declarations
struct TextDiffLine;
struct TextDiffStats;

//===--------------------------------------------------------------------===//
// TextDiff Data Type
//===--------------------------------------------------------------------===//

struct TextDiff {
public:
	// Diff line types
	enum class LineType : uint8_t {
		CONTEXT = 0, // Unchanged line
		ADDED = 1,   // Line added in new version
		REMOVED = 2, // Line removed from old version
		MODIFIED = 3 // Line modified between versions
	};

	// Individual diff line
	struct DiffLine {
		LineType type;
		string content;
		idx_t old_line_number; // Line number in old file (0 if added)
		idx_t new_line_number; // Line number in new file (0 if removed)

		DiffLine(LineType t, string c, idx_t old_num = 0, idx_t new_num = 0)
		    : type(t), content(std::move(c)), old_line_number(old_num), new_line_number(new_num) {
		}
	};

	// Diff statistics
	struct Stats {
		idx_t lines_added = 0;
		idx_t lines_removed = 0;
		idx_t lines_modified = 0;
		idx_t lines_context = 0;

		Stats() = default;
		Stats(idx_t added, idx_t removed, idx_t modified, idx_t context)
		    : lines_added(added), lines_removed(removed), lines_modified(modified), lines_context(context) {
		}
	};

public:
	TextDiff() = default;
	explicit TextDiff(vector<DiffLine> lines);

	// Create diff from two strings
	static TextDiff CreateDiff(const string &old_text, const string &new_text);

	// Accessors
	const vector<DiffLine> &GetLines() const {
		return diff_lines_;
	}
	Stats GetStats() const;
	bool IsEmpty() const {
		return diff_lines_.empty();
	}

	// String representation
	string ToString() const;

	// Comparison operators
	bool operator==(const TextDiff &other) const;
	bool operator!=(const TextDiff &other) const {
		return !(*this == other);
	}

private:
	vector<DiffLine> diff_lines_;

	// Internal diff computation
	static vector<DiffLine> ComputeDiff(const vector<string> &old_lines, const vector<string> &new_lines);
	static vector<string> SplitLines(const string &text);
};

//===--------------------------------------------------------------------===//
// DuckDB Type Integration
//===--------------------------------------------------------------------===//

// TextDiff LogicalType
extern LogicalType TextDiffType();

// Type functions for DuckDB integration
void RegisterTextDiffType(ExtensionLoader &loader);

} // namespace duckdb
