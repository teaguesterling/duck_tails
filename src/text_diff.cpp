#include "text_diff.hpp"
#include "duckdb_compat.hpp"
#include "git_filesystem.hpp"
#include "git_utils.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/file_opener.hpp"
#include <algorithm>
#include <cstring>
#include <sstream>

namespace duckdb {

//===--------------------------------------------------------------------===//
// TextDiff Implementation
//===--------------------------------------------------------------------===//

TextDiff::TextDiff(vector<DiffLine> lines) : diff_lines_(std::move(lines)) {
}

TextDiff TextDiff::CreateDiff(const string &old_text, const string &new_text) {
	if (old_text == new_text) {
		// Identical texts = empty diff
		return TextDiff();
	}

	auto old_lines = SplitLines(old_text);
	auto new_lines = SplitLines(new_text);
	auto diff_lines = ComputeDiff(old_lines, new_lines);

	return TextDiff(std::move(diff_lines));
}

TextDiff::Stats TextDiff::GetStats() const {
	Stats stats;

	for (const auto &line : diff_lines_) {
		switch (line.type) {
		case LineType::ADDED:
			stats.lines_added++;
			break;
		case LineType::REMOVED:
			stats.lines_removed++;
			break;
		case LineType::MODIFIED:
			stats.lines_modified++;
			break;
		case LineType::CONTEXT:
			stats.lines_context++;
			break;
		}
	}

	return stats;
}

string TextDiff::ToString() const {
	if (IsEmpty()) {
		return "No differences";
	}

	std::ostringstream oss;
	for (const auto &line : diff_lines_) {
		switch (line.type) {
		case LineType::CONTEXT:
			oss << " " << line.content << "\n";
			break;
		case LineType::ADDED:
			oss << "+" << line.content << "\n";
			break;
		case LineType::REMOVED:
			oss << "-" << line.content << "\n";
			break;
		case LineType::MODIFIED:
			oss << "~" << line.content << "\n";
			break;
		}
	}

	return oss.str();
}

bool TextDiff::operator==(const TextDiff &other) const {
	if (diff_lines_.size() != other.diff_lines_.size()) {
		return false;
	}

	for (size_t i = 0; i < diff_lines_.size(); i++) {
		const auto &a = diff_lines_[i];
		const auto &b = other.diff_lines_[i];

		if (a.type != b.type || a.content != b.content || a.old_line_number != b.old_line_number ||
		    a.new_line_number != b.new_line_number) {
			return false;
		}
	}

	return true;
}

//===--------------------------------------------------------------------===//
// Reading a diff back from its textual form
//===--------------------------------------------------------------------===//

// "@@ -10,3 +12,4 @@ optional heading" -- take the two start line numbers.
// Returns false for anything that is not a hunk header, in which case the
// counters are left alone and the line is skipped as an unrecognised header.
static bool ParseHunkHeader(const string &line, idx_t &old_line, idx_t &new_line) {
	if (line.size() < 4 || line.compare(0, 2, "@@") != 0) {
		return false;
	}
	size_t pos = 2;
	auto skip_spaces = [&]() {
		while (pos < line.size() && line[pos] == ' ') {
			pos++;
		}
	};
	auto read_start = [&](char sign, idx_t &out) {
		skip_spaces();
		if (pos >= line.size() || line[pos] != sign) {
			return false;
		}
		pos++;
		size_t digits_start = pos;
		uint64_t value = 0;
		while (pos < line.size() && line[pos] >= '0' && line[pos] <= '9') {
			value = value * 10 + static_cast<uint64_t>(line[pos] - '0');
			pos++;
		}
		if (pos == digits_start) {
			return false;
		}
		// A ",count" may follow the start; it tells us nothing we need.
		if (pos < line.size() && line[pos] == ',') {
			pos++;
			while (pos < line.size() && line[pos] >= '0' && line[pos] <= '9') {
				pos++;
			}
		}
		// git writes "@@ -1 +0,0 @@" for an emptied file: line 0 of a zero-line
		// range. Clamp to 1 so the numbers we hand back always name a real line.
		out = value == 0 ? 1 : static_cast<idx_t>(value);
		return true;
	};

	idx_t parsed_old = 0, parsed_new = 0;
	if (!read_start('-', parsed_old) || !read_start('+', parsed_new)) {
		return false;
	}
	old_line = parsed_old;
	new_line = parsed_new;
	return true;
}

// A line that describes the diff rather than the files it compares.
static bool IsDiffHeaderLine(const string &line) {
	if (line == "---" || line == "+++") {
		return true;
	}
	static const char *const prefixes[] = {"--- ",
	                                       "+++ ",
	                                       "diff ",
	                                       "index ",
	                                       "old mode ",
	                                       "new mode ",
	                                       "new file mode ",
	                                       "deleted file mode ",
	                                       "similarity index ",
	                                       "rename ",
	                                       "copy ",
	                                       "Binary files ",
	                                       "\\"};
	for (const char *prefix : prefixes) {
		const size_t len = strlen(prefix);
		if (line.size() >= len && line.compare(0, len, prefix) == 0) {
			return true;
		}
	}
	return false;
}

TextDiff TextDiff::Parse(const string &diff_text) {
	vector<DiffLine> lines;
	// Line numbers count from 1 when no hunk header says otherwise, which is what
	// ToString()'s output (a single implicit hunk starting at the top) needs.
	idx_t old_line = 1;
	idx_t new_line = 1;

	for (auto &raw : SplitLines(diff_text)) {
		string line = raw;
		// A CRLF diff leaves the '\r' on the end of every line; it belongs to the
		// line terminator, not to the content.
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}

		if (ParseHunkHeader(line, old_line, new_line)) {
			continue;
		}
		if (IsDiffHeaderLine(line)) {
			continue;
		}
		if (line.empty()) {
			// An unchanged empty line: git omits the leading space on it.
			lines.emplace_back(LineType::CONTEXT, string(), old_line, new_line);
			old_line++;
			new_line++;
			continue;
		}

		const string content = line.substr(1);
		switch (line[0]) {
		case ' ':
			lines.emplace_back(LineType::CONTEXT, content, old_line, new_line);
			old_line++;
			new_line++;
			break;
		case '+':
			lines.emplace_back(LineType::ADDED, content, 0, new_line);
			new_line++;
			break;
		case '-':
			lines.emplace_back(LineType::REMOVED, content, old_line, 0);
			old_line++;
			break;
		case '~':
			// ToString()'s spelling for MODIFIED; a unified diff never produces it.
			lines.emplace_back(LineType::MODIFIED, content, old_line, new_line);
			old_line++;
			new_line++;
			break;
		default:
			// Outside any hunk. Skipped rather than guessed at.
			break;
		}
	}

	return TextDiff(std::move(lines));
}

// Using string representation for simplicity

vector<string> TextDiff::SplitLines(const string &text) {
	vector<string> lines;
	if (text.empty()) {
		return lines;
	}

	std::stringstream ss(text);
	string line;

	// Split by newlines, preserving empty lines
	while (std::getline(ss, line)) {
		lines.push_back(line);
	}

	// Handle case where text doesn't end with newline
	if (!text.empty() && text.back() != '\n') {
		// Last line was already added by getline
	}

	return lines;
}

vector<TextDiff::DiffLine> TextDiff::ComputeDiff(const vector<string> &old_lines, const vector<string> &new_lines) {
	vector<DiffLine> result;

	// Simple diff algorithm - Myers algorithm would be better but this is sufficient
	size_t old_idx = 0, new_idx = 0;

	while (old_idx < old_lines.size() || new_idx < new_lines.size()) {
		if (old_idx >= old_lines.size()) {
			// Only new lines remaining - all additions
			result.emplace_back(LineType::ADDED, new_lines[new_idx], 0, new_idx + 1);
			new_idx++;
		} else if (new_idx >= new_lines.size()) {
			// Only old lines remaining - all removals
			result.emplace_back(LineType::REMOVED, old_lines[old_idx], old_idx + 1, 0);
			old_idx++;
		} else if (old_lines[old_idx] == new_lines[new_idx]) {
			// Lines match - context
			result.emplace_back(LineType::CONTEXT, old_lines[old_idx], old_idx + 1, new_idx + 1);
			old_idx++;
			new_idx++;
		} else {
			// Lines differ - mark as modified (simplified)
			result.emplace_back(LineType::REMOVED, old_lines[old_idx], old_idx + 1, 0);
			result.emplace_back(LineType::ADDED, new_lines[new_idx], 0, new_idx + 1);
			old_idx++;
			new_idx++;
		}
	}

	return result;
}

//===--------------------------------------------------------------------===//
// DuckDB Type Integration
//===--------------------------------------------------------------------===//

// Custom TextDiff type implementation
struct TextDiffTypeInfo {
	static constexpr const LogicalTypeId TYPE_ID = LogicalTypeId::INVALID;
	static constexpr const char *NAME = "TEXTDIFF";
};

LogicalType TextDiffType() {
	// Create custom logical type for TextDiff
	return LogicalType(LogicalTypeId::BLOB); // Use BLOB as base type for now
}

// TextDiff creation function
static void TextDiffFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &old_vector = args.data[0];
	auto &new_vector = args.data[1];

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = CompatFlatDataMutable<string_t>(result);
	auto &result_validity = CompatFlatValidityMutable(result);

	for (idx_t i = 0; i < args.size(); i++) {
		if (old_vector.GetValue(i).IsNull() || new_vector.GetValue(i).IsNull()) {
			result_validity.SetInvalid(i);
			continue;
		}

		string old_text = old_vector.GetValue(i).ToString();
		string new_text = new_vector.GetValue(i).ToString();

		auto diff = TextDiff::CreateDiff(old_text, new_text);
		string diff_str = diff.ToString();

		result_data[i] = StringVector::AddString(result, diff_str);
	}
}

// diff_text function - pure text diffing (no file I/O)
static void DiffTextFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &old_vector = args.data[0];
	auto &new_vector = args.data[1];

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = CompatFlatDataMutable<string_t>(result);
	auto &result_validity = CompatFlatValidityMutable(result);

	for (idx_t i = 0; i < args.size(); i++) {
		if (old_vector.GetValue(i).IsNull() || new_vector.GetValue(i).IsNull()) {
			result_validity.SetInvalid(i);
			continue;
		}

		string old_text = old_vector.GetValue(i).ToString();
		string new_text = new_vector.GetValue(i).ToString();

		try {
			// Pure text diffing - no file I/O
			auto diff = TextDiff::CreateDiff(old_text, new_text);

			if (diff.IsEmpty()) {
				// Return NULL for identical content
				result_validity.SetInvalid(i);
			} else {
				string diff_str = diff.ToString();
				result_data[i] = StringVector::AddString(result, diff_str);
			}

		} catch (const std::exception &e) {
			// Return error as string for now - full implementation would throw proper exceptions
			string error_str = "Error: " + string(GitExceptionMessage(e));
			result_data[i] = StringVector::AddString(result, error_str);
		}
	}
}

//===--------------------------------------------------------------------===//
// text_diff_stats
//===--------------------------------------------------------------------===//

// The statistics a diff carries, as a struct rather than a sentence: the caller
// asked for numbers and wants to compute with them.
static LogicalType TextDiffStatsType() {
	child_list_t<LogicalType> children;
	children.emplace_back("lines_added", LogicalType::BIGINT);
	children.emplace_back("lines_removed", LogicalType::BIGINT);
	children.emplace_back("lines_modified", LogicalType::BIGINT);
	children.emplace_back("lines_context", LogicalType::BIGINT);
	return LogicalType::STRUCT(std::move(children));
}

static Value TextDiffStatsValue(const TextDiff::Stats &stats) {
	child_list_t<Value> children;
	children.emplace_back("lines_added", Value::BIGINT(static_cast<int64_t>(stats.lines_added)));
	children.emplace_back("lines_removed", Value::BIGINT(static_cast<int64_t>(stats.lines_removed)));
	children.emplace_back("lines_modified", Value::BIGINT(static_cast<int64_t>(stats.lines_modified)));
	children.emplace_back("lines_context", Value::BIGINT(static_cast<int64_t>(stats.lines_context)));
	return Value::STRUCT(std::move(children));
}

// Count the lines of a diff the caller already has.
//
// This function used to return the constant string "lines_added: 1,
// lines_removed: 1, lines_modified: 1" for every input -- an empty string and a
// nine-line diff produced byte-identical output, because the argument was bound
// and then never read (#54). Nothing raised and nothing was empty, so the number
// looked like an answer.
static void TextDiffStatsFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &diff_vector = args.data[0];

	result.SetVectorType(VectorType::FLAT_VECTOR);
	for (idx_t i = 0; i < args.size(); i++) {
		auto diff_value = diff_vector.GetValue(i);
		if (diff_value.IsNull()) {
			result.SetValue(i, Value(TextDiffStatsType()));
			continue;
		}
		auto diff = TextDiff::Parse(diff_value.ToString());
		result.SetValue(i, TextDiffStatsValue(diff.GetStats()));
	}
	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

// The two-argument form the docs use: diff the pair, then count.
static void TextDiffStatsPairFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &old_vector = args.data[0];
	auto &new_vector = args.data[1];

	result.SetVectorType(VectorType::FLAT_VECTOR);
	for (idx_t i = 0; i < args.size(); i++) {
		auto old_value = old_vector.GetValue(i);
		auto new_value = new_vector.GetValue(i);
		if (old_value.IsNull() || new_value.IsNull()) {
			result.SetValue(i, Value(TextDiffStatsType()));
			continue;
		}
		auto diff = TextDiff::CreateDiff(old_value.ToString(), new_value.ToString());
		result.SetValue(i, TextDiffStatsValue(diff.GetStats()));
	}
	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

//===--------------------------------------------------------------------===//
// text_diff_lines
//===--------------------------------------------------------------------===//

// Split a diff the caller already has into one row per line.
//
// Like text_diff_stats above, this used to answer with fabricated data: three
// fixed demo rows (CONTEXT/Hello, REMOVED/World, ADDED/DuckDB) for every input,
// including the empty string, because the argument reached the bind and stopped
// there (#54).
struct TextDiffLinesBindData : public FunctionData {
	vector<TextDiff::DiffLine> lines;

	explicit TextDiffLinesBindData(vector<TextDiff::DiffLine> lines_p) : lines(std::move(lines_p)) {
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<TextDiffLinesBindData>(lines);
	}

	bool Equals(const FunctionData &other) const override {
		auto &other_data = other.Cast<TextDiffLinesBindData>();
		if (lines.size() != other_data.lines.size()) {
			return false;
		}
		for (idx_t i = 0; i < lines.size(); i++) {
			const auto &a = lines[i];
			const auto &b = other_data.lines[i];
			if (a.type != b.type || a.content != b.content || a.old_line_number != b.old_line_number ||
			    a.new_line_number != b.new_line_number) {
				return false;
			}
		}
		return true;
	}
};

struct TextDiffLinesData : public GlobalTableFunctionState {
	idx_t position = 0;
};

static unique_ptr<FunctionData> TextDiffLinesBind(ClientContext &context, TableFunctionBindInput &input,
                                                  vector<LogicalType> &return_types, vector<CompatName> &names) {
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BIGINT};
	names = {"line_type", "content", "line_number"};

	vector<TextDiff::DiffLine> lines;
	if (!input.inputs.empty() && !input.inputs[0].IsNull()) {
		lines = TextDiff::Parse(input.inputs[0].ToString()).GetLines();
	}
	return make_uniq<TextDiffLinesBindData>(std::move(lines));
}

static unique_ptr<GlobalTableFunctionState> TextDiffLinesInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<TextDiffLinesData>();
}

static void TextDiffLinesFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<TextDiffLinesData>();
	auto &bind_data = data_p.bind_data->Cast<TextDiffLinesBindData>();

	idx_t output_idx = 0;
	while (data.position < bind_data.lines.size() && output_idx < STANDARD_VECTOR_SIZE) {
		const auto &line = bind_data.lines[data.position];

		// Set line_type
		string line_type_str;
		switch (line.type) {
		case TextDiff::LineType::CONTEXT:
			line_type_str = "CONTEXT";
			break;
		case TextDiff::LineType::ADDED:
			line_type_str = "ADDED";
			break;
		case TextDiff::LineType::REMOVED:
			line_type_str = "REMOVED";
			break;
		case TextDiff::LineType::MODIFIED:
			line_type_str = "MODIFIED";
			break;
		}

		// line_number is the line's number in the file it exists in: the new file
		// for a context or added line, the old file for a removed one. Reporting
		// the row's position in the diff instead would be a number that looks like
		// a line number and is not one.
		const idx_t line_number =
		    line.type == TextDiff::LineType::REMOVED ? line.old_line_number : line.new_line_number;

		output.SetValue(0, output_idx, Value(line_type_str));
		output.SetValue(1, output_idx, Value(line.content));
		output.SetValue(2, output_idx, Value::BIGINT(static_cast<int64_t>(line_number)));

		data.position++;
		output_idx++;
	}

	CompatSetOutputCardinality(output, output_idx);
}

//===--------------------------------------------------------------------===//
// read_git_diff table function
//===--------------------------------------------------------------------===//

// Bind data to store function arguments
struct ReadGitDiffBindData : public FunctionData {
	string path1;
	string path2;
	bool include_metadata;

	ReadGitDiffBindData(string p1, string p2, bool metadata)
	    : path1(std::move(p1)), path2(std::move(p2)), include_metadata(metadata) {
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<ReadGitDiffBindData>(path1, path2, include_metadata);
	}

	bool Equals(const FunctionData &other) const override {
		auto &other_data = other.Cast<ReadGitDiffBindData>();
		return path1 == other_data.path1 && path2 == other_data.path2 &&
		       include_metadata == other_data.include_metadata;
	}
};

// Global state for execution
struct ReadGitDiffData : public GlobalTableFunctionState {
	string diff_text;
	string path1;
	string path2;
	bool include_metadata;
	bool returned_row = false;

	ReadGitDiffData(string diff, string p1, string p2, bool metadata)
	    : diff_text(std::move(diff)), path1(std::move(p1)), path2(std::move(p2)), include_metadata(metadata) {
	}
};

// The HEAD version of whatever the caller named, as a git:// URI the extension's
// own filesystem can open.
//
// The single-argument form used to build this by appending "@HEAD" to the string
// it was given, producing "README.md@HEAD" -- a filename with an '@' in it, which
// the local filesystem cannot open, so the one-argument form could never succeed
// (#55.2). A git:// URI is what names a file at a revision here.
static string HeadUriFor(const string &path) {
	auto git_path = GitPath::Parse(StringUtil::StartsWith(path, "git://") ? path : "git://" + path);
	string uri = "git://" + git_path.repository_path;
	if (!git_path.file_path.empty()) {
		uri += "/" + git_path.file_path;
	}
	return uri + "@HEAD";
}

static unique_ptr<FunctionData> ReadGitDiffBind(ClientContext &context, TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types, vector<CompatName> &names) {
	// Parse arguments from input.inputs
	string path1 = input.inputs[0].ToString();
	string path2;

	if (input.inputs.size() > 1) {
		// Two-argument version: diff between two paths, old first and new second.
		path2 = input.inputs[1].ToString();
	} else {
		// Single-argument version: the file as it is now, against its HEAD version.
		// The columns keep the two-argument form's meaning -- path1 is the old side
		// and path2 the new one -- so the argument is reported as path2 and the
		// HEAD side it was compared against as path1.
		path2 = path1;
		try {
			path1 = HeadUriFor(path2);
		} catch (const std::exception &e) {
			throw BinderException("read_git_diff: cannot resolve '%s' to a file at HEAD: %s", path2,
			                      GitExceptionMessage(e));
		}
	}

	// Basic return columns
	return_types = {LogicalType::VARCHAR};
	names = {"diff_text"};

	// TODO: Add metadata columns when include_metadata parameter is implemented
	// For now, also include basic path info
	return_types.push_back(LogicalType::VARCHAR);
	return_types.push_back(LogicalType::VARCHAR);
	names.push_back("path1");
	names.push_back("path2");

	// Store arguments in bind data
	return make_uniq<ReadGitDiffBindData>(path1, path2, true);
}

static unique_ptr<GlobalTableFunctionState> ReadGitDiffInit(ClientContext &context, TableFunctionInitInput &input) {
	// Get arguments from bind data
	auto &bind_data = input.bind_data->Cast<ReadGitDiffBindData>();
	string path1 = bind_data.path1;
	string path2 = bind_data.path2;

	// A failure here is raised, not returned. This function used to catch every
	// exception and hand the message back in diff_text, so a query against a file
	// that could not be read got a one-row result set describing the failure --
	// callers checking row counts saw success, and the message was data (#55.2).
	auto &fs = FileSystem::GetFileSystem(context);

	auto read_whole_file = [&fs](const string &path) {
		try {
			auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
			auto file_size = handle->GetFileSize();
			string content;
			content.resize(static_cast<size_t>(file_size));
			if (file_size > 0) {
				// The positional Read: it loops until the request is satisfied and
				// raises if it cannot be. The sequential overload is a single read(2),
				// which returns at most 2 GiB - 4 KiB on Linux, so a larger file came
				// back quietly truncated with the tail left as zero bytes (#55.5).
				handle->Read(const_cast<char *>(content.data()), static_cast<idx_t>(file_size), 0);
			}
			handle->Close();
			return content;
		} catch (const std::exception &e) {
			throw IOException("read_git_diff: failed to read '%s': %s", path, GitExceptionMessage(e));
		}
	};

	string content1 = read_whole_file(path1);
	string content2 = read_whole_file(path2);

	// Create diff using our TextDiff implementation
	auto diff = TextDiff::CreateDiff(content1, content2);
	string diff_text = diff.ToString();

	return make_uniq<ReadGitDiffData>(std::move(diff_text), path1, path2, bind_data.include_metadata);
}

static void ReadGitDiffFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<ReadGitDiffData>();

	if (data.returned_row) {
		// We only return one row
		CompatSetOutputCardinality(output, 0);
		return;
	}

	// Return the diff data
	output.SetValue(0, 0, Value(data.diff_text));
	output.SetValue(1, 0, Value(data.path1));
	output.SetValue(2, 0, Value(data.path2));

	data.returned_row = true;
	CompatSetOutputCardinality(output, 1);
}

void RegisterTextDiffType(ExtensionLoader &loader) {
	// Register text_diff function
	auto text_diff_func = ScalarFunction("text_diff", {LogicalType::VARCHAR, LogicalType::VARCHAR},
	                                     LogicalType::VARCHAR, TextDiffFunction);
	loader.RegisterFunction(text_diff_func);

	// Register diff_text function (Phase 2 main function)
	auto diff_text_func = ScalarFunction("diff_text", {LogicalType::VARCHAR, LogicalType::VARCHAR},
	                                     LogicalType::VARCHAR, DiffTextFunction);
	loader.RegisterFunction(diff_text_func);

	// Register text_diff_stats: over a diff the caller already has, and over the
	// pair of texts to diff first.
	ScalarFunctionSet stats_set("text_diff_stats");
	stats_set.AddFunction(ScalarFunction({LogicalType::VARCHAR}, TextDiffStatsType(), TextDiffStatsFunction));
	stats_set.AddFunction(
	    ScalarFunction({LogicalType::VARCHAR, LogicalType::VARCHAR}, TextDiffStatsType(), TextDiffStatsPairFunction));
	loader.RegisterFunction(stats_set);

	// Register text_diff_lines table function
	TableFunction lines_func("text_diff_lines", {LogicalType::VARCHAR}, TextDiffLinesFunction, TextDiffLinesBind,
	                         TextDiffLinesInit);
	loader.RegisterFunction(lines_func);

	// Register read_git_diff table function (Phase 2 main function)
	// Single-argument version
	TableFunction read_git_diff_func_1("read_git_diff", {LogicalType::VARCHAR}, ReadGitDiffFunction, ReadGitDiffBind,
	                                   ReadGitDiffInit);
	loader.RegisterFunction(read_git_diff_func_1);

	// Two-argument version
	TableFunction read_git_diff_func_2("read_git_diff", {LogicalType::VARCHAR, LogicalType::VARCHAR},
	                                   ReadGitDiffFunction, ReadGitDiffBind, ReadGitDiffInit);
	loader.RegisterFunction(read_git_diff_func_2);
}

} // namespace duckdb
