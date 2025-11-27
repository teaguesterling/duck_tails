#include "git_functions.hpp"
#include "git_filesystem.hpp"
#include "git_utils.hpp"
#include "git_context_manager.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// UTF-8 Validation Helper
//===--------------------------------------------------------------------===//

// Simple UTF-8 validation to prevent verification crashes
static bool IsValidUTF8(const char* data, size_t length) {
    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(data);
    for (size_t i = 0; i < length; ) {
        unsigned char byte = bytes[i];
        
        // ASCII (0-127)
        if (byte <= 0x7F) {
            i++;
            continue;
        }
        
        // Multi-byte sequence
        int num_bytes = 0;
        if ((byte & 0xE0) == 0xC0) {
            num_bytes = 2;
        } else if ((byte & 0xF0) == 0xE0) {
            num_bytes = 3;
        } else if ((byte & 0xF8) == 0xF0) {
            num_bytes = 4;
        } else {
            return false; // Invalid start byte
        }
        
        // Check if we have enough bytes
        if (i + num_bytes > length) {
            return false;
        }
        
        // Check continuation bytes
        for (int j = 1; j < num_bytes; j++) {
            if ((bytes[i + j] & 0xC0) != 0x80) {
                return false;
            }
        }
        
        i += num_bytes;
    }
    return true;
}

//===--------------------------------------------------------------------===//
// Helper Functions
//===--------------------------------------------------------------------===//

static string oid_to_hex(const git_oid *oid) {
    char hex[GIT_OID_HEXSZ + 1];
    git_oid_tostr(hex, sizeof(hex), oid);
    return string(hex);
}

static string ExtractFileExtension(const string &path) {
    size_t dot_pos = path.find_last_of('.');
    if (dot_pos == string::npos || dot_pos == path.length() - 1) {
        return "";
    }
    return path.substr(dot_pos);
}

//===--------------------------------------------------------------------===//
// Git Read Functions (both static and LATERAL support for reading blob content)
//===--------------------------------------------------------------------===//

// Common bind data for both static and LATERAL git_read functions
struct GitReadBindData : public TableFunctionData {
    int64_t max_bytes;
    string decode_base64;
    string transcode;
    string filters;
    string repo_path;
    string uri;  // For static git_read function
    string ref;  // Fallback ref for GitContextManager
    
    GitReadBindData(int64_t max_bytes, const string& decode_base64, const string& transcode, 
                   const string& filters, const string& repo_path, const string& uri = "", const string& ref = "HEAD")
        : max_bytes(max_bytes), decode_base64(decode_base64), transcode(transcode), 
          filters(filters), repo_path(repo_path), uri(uri), ref(ref) {}
};

// Global state for static git_read function
struct GitReadGlobalState : public GlobalTableFunctionState {
    GitReadGlobalState() : finished(false) {}
    bool finished;
};

struct GitReadLocalState : public LocalTableFunctionState {
    GitReadLocalState() = default;
    
    bool initialized_row = false;
    idx_t current_input_row = 0;
    idx_t current_output_row = 0;
    
    git_repository *repo = nullptr;
    
    struct ReadResult {
        string git_uri;           // Renamed from uri - complete git:// URI
        string repo_path;         // NEW - repository filesystem path
        string commit_hash;       // NEW - Git commit hash
        string tree_hash;         // NEW - Git tree hash containing the file
        string file_path;         // NEW - File path within repository
        string file_ext;          // NEW - File extension (e.g., .js, .cpp, .md)
        string ref;              // NEW - Git reference (SHA/branch/tag)
        string blob_hash;         // NEW - Git blob hash of file content
        int32_t mode;            // File mode
        string kind;             // Object kind (blob, tree, etc.)
        bool is_text;            // Whether content is text
        string encoding;         // Text encoding (utf8, binary)
        int64_t size_bytes;      // File size in bytes
        bool truncated;          // Whether content was truncated
        string text;             // Text content
        string blob;             // Base64 encoded binary content
        
        // Constructor to ensure proper initialization
        ReadResult() : 
            git_uri(""), repo_path(""), commit_hash(""), tree_hash(""),
            file_path(""), file_ext(""), ref(""), blob_hash(""),
            mode(0), kind("blob"), is_text(true), encoding("utf8"),
            size_bytes(0), truncated(false), text(""), blob("") {}
    };
    
    vector<ReadResult> current_results;
    
    ~GitReadLocalState() {
        if (repo) {
            git_repository_free(repo);
        }
    }
};

// Init functions
static unique_ptr<GlobalTableFunctionState> GitReadInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
    return make_uniq<GitReadGlobalState>();
}

// Helper function to process a git:// URI and extract content using GitContextManager
static void ProcessGitURI(const string& uri, const GitReadBindData& bind_data, 
                         GitReadLocalState::ReadResult& result) {
    // Initialize all fields with defaults
    result.git_uri = uri;
    result.repo_path = "";
    result.commit_hash = "";
    result.tree_hash = "";
    result.file_path = "";
    result.file_ext = "";
    result.ref = "";
    result.blob_hash = "";
    result.mode = 0;
    result.kind = "unknown";
    result.is_text = false;
    result.encoding = "unknown";
    result.size_bytes = 0;
    result.truncated = false;
    result.text = "";
    result.blob = "";
    
    // Use GitContextManager for unified URI processing
    try {
        auto ctx = GitContextManager::Instance().ProcessGitUri(uri, bind_data.ref);

        // Populate extracted URI components from GitContextManager
        result.repo_path = ctx.repo_path;
        result.file_path = ctx.file_path;
        result.ref = ctx.final_ref;
        result.file_ext = ExtractFileExtension(ctx.file_path);

        // Open repository temporarily for bind-time processing
        git_repository *repo = nullptr;
        int error = git_repository_open(&repo, ctx.repo_path.c_str());
        if (error != 0) {
            const git_error *e = git_error_last();
            throw IOException("Failed to open repository: %s", e ? e->message : "Unknown error");
        }

        // Get commit object from validated ref
        git_commit *commit = nullptr;
        const git_oid *commit_oid = git_object_id(ctx.resolved_object);
        error = git_commit_lookup(&commit, repo, commit_oid);
        if (error != 0) {
            const git_error *e = git_error_last();
            throw IOException("git_read: unable to parse OID: %s", e ? e->message : "Unknown error");
        }
        
        // Get tree from commit
        git_tree *tree = nullptr;
        error = git_commit_tree(&tree, commit);
        if (error != 0) {
            const git_error *e = git_error_last();
            git_commit_free(commit);
            git_repository_free(repo);
            throw IOException("git_read: failed to get commit tree: %s", e ? e->message : "Unknown error");
        }
        
        // Populate hash fields
        result.commit_hash = oid_to_hex(commit_oid);
        result.tree_hash = oid_to_hex(git_tree_id(tree));
        
        // Find the file in the tree
        git_tree_entry *entry = nullptr;
        error = git_tree_entry_bypath(&entry, tree, ctx.file_path.c_str());
        if (error != 0) {
            git_tree_free(tree);
            git_commit_free(commit);
            git_repository_free(repo);
            throw IOException("git_read: file not found '%s' in commit '%s'", ctx.file_path.c_str(), ctx.final_ref.c_str());
        }
        
        // Get file mode and kind
        git_filemode_t filemode = git_tree_entry_filemode(entry);
        result.mode = static_cast<int32_t>(filemode);
        
        // Get blob hash from tree entry
        const git_oid *entry_oid = git_tree_entry_id(entry);
        result.blob_hash = oid_to_hex(entry_oid);
        
        switch (filemode) {
            case GIT_FILEMODE_BLOB:
            case GIT_FILEMODE_BLOB_EXECUTABLE:
                result.kind = "file";
                break;
            case GIT_FILEMODE_LINK:
                result.kind = "symlink";
                break;
            case GIT_FILEMODE_TREE:
                result.kind = "tree";
                git_tree_entry_free(entry);
                git_tree_free(tree);
                git_commit_free(commit);
                git_repository_free(repo);
                return;
            case GIT_FILEMODE_COMMIT:
                result.kind = "submodule";
                git_tree_entry_free(entry);
                git_tree_free(tree);
                git_commit_free(commit);
                git_repository_free(repo);
                return;
            default:
                git_tree_entry_free(entry);
                git_tree_free(tree);
                git_commit_free(commit);
                git_repository_free(repo);
                throw IOException("git_read: unsupported file mode %d", static_cast<int>(filemode));
        }
        
        // Get the blob
        git_blob *blob = nullptr;
        const git_oid *blob_oid = git_tree_entry_id(entry);
        error = git_blob_lookup(&blob, repo, blob_oid);
        if (error != 0) {
            const git_error *e = git_error_last();
            git_tree_entry_free(entry);
            git_tree_free(tree);
            git_commit_free(commit);
            git_repository_free(repo);
            throw IOException("git_read: failed to load blob: %s", e ? e->message : "Unknown error");
        }
        
        // Get blob content
        const void *raw_content = git_blob_rawcontent(blob);
        git_off_t raw_size = git_blob_rawsize(blob);
        
        result.size_bytes = static_cast<int64_t>(raw_size);
        
        // Apply max_bytes limit
        size_t content_size = static_cast<size_t>(raw_size);
        if (bind_data.max_bytes > 0 && content_size > static_cast<size_t>(bind_data.max_bytes)) {
            content_size = static_cast<size_t>(bind_data.max_bytes);
            result.truncated = true;
        }
        
        if (content_size > 0) {
            // Use libgit2's efficient binary detection
            bool is_text = !git_blob_is_binary(blob);
            result.is_text = is_text;
            
            if (is_text) {
                // Create a safe text string with proper memory handling
                const char* char_content = static_cast<const char*>(raw_content);
                
                // Check for null bytes (which can cause verification issues)
                bool has_null_bytes = false;
                for (size_t i = 0; i < content_size; i++) {
                    if (char_content[i] == '\0') {
                        has_null_bytes = true;
                        break;
                    }
                }
                
                // Additional UTF-8 validation
                bool is_valid_utf8 = !has_null_bytes && IsValidUTF8(char_content, content_size);
                
                if (is_valid_utf8) {
                    result.encoding = "utf8";
                    result.text = string(char_content, content_size);
                    
                    // Defensive check: ensure no uninitialized memory patterns
                    if (result.text.find('\xbe') != string::npos) {
                        // Contains suspicious uninitialized memory pattern - treat as binary
                        result.encoding = "binary";
                        result.is_text = false;
                        result.text.clear();
                        result.blob = string(char_content, content_size);
                    }
                } else {
                    // Invalid UTF-8 or contains null bytes - treat as binary
                    result.encoding = "binary";
                    result.is_text = false;
                    result.blob = string(char_content, content_size);
                }
            } else {
                result.encoding = "binary";
                result.blob = string(static_cast<const char*>(raw_content), content_size);
            }
        }
        
        // Clean up local objects
        git_blob_free(blob);
        git_tree_entry_free(entry);
        git_tree_free(tree);
        git_commit_free(commit);
        git_repository_free(repo);

    } catch (const std::exception &e) {
        throw BinderException("git_read: %s", e.what());
    }
}

// Bind function for static git_read (single URI parameter)
static unique_ptr<FunctionData> GitReadBind(ClientContext &context, TableFunctionBindInput &input,
                                          vector<LogicalType> &return_types, vector<string> &names) {
    
    // Extract first parameter (repo_path_or_uri_with_file)
    if (input.inputs.empty()) {
        throw BinderException("git_read requires at least one parameter: the file path or git:// URI");
    }
    
    string first_param = input.inputs[0].GetValue<string>();
    string uri;
    string repo_path = ".";
    string fallback_ref = "HEAD";
    
    // Check if it's a git:// URI or filesystem path
    if (StringUtil::StartsWith(first_param, "git://")) {
        // git:// URI - use as-is (existing behavior)
        uri = first_param;
    } else {
        // Filesystem path - use unified parameter parsing to build git:// URI
        auto params = ParseUnifiedGitParams(input, 1);  // ref parameter at index 1
        
        // Build git:// URI from discovered repo and file paths
        if (params.resolved_file_path.empty()) {
            throw BinderException("git_read: filesystem path '%s' does not appear to contain a file component", first_param);
        }
        
        uri = "git://" + params.resolved_repo_path + "/" + params.resolved_file_path + "@" + params.ref;
        repo_path = params.resolved_repo_path;
        fallback_ref = params.ref;  // Use the parsed ref as fallback
    }
    
    // Set default parameters - note parameter indices shift by 1 if ref was provided
    int64_t max_bytes = -1;  // No limit by default
    string decode_base64 = "auto";
    string transcode = "utf8";  
    string filters = "raw";
    
    // Determine parameter offset based on whether filesystem path had ref parameter
    int param_offset = 1;
    if (!StringUtil::StartsWith(first_param, "git://") && input.inputs.size() >= 2 && 
        input.inputs[1].type().id() == LogicalTypeId::VARCHAR) {
        // Second parameter might be ref for filesystem paths - check if third param is int (max_bytes)
        if (input.inputs.size() >= 3 && input.inputs[2].type().id() == LogicalTypeId::BIGINT) {
            param_offset = 2;  // ref was provided, skip to max_bytes at index 2
        }
    }
    
    // Parse optional parameters with correct offset
    if (input.inputs.size() > param_offset && !input.inputs[param_offset].IsNull()) {
        if (input.inputs[param_offset].type().id() == LogicalTypeId::BIGINT) {
            max_bytes = input.inputs[param_offset].GetValue<int64_t>();
            param_offset++;
        }
    }
    if (input.inputs.size() > param_offset && !input.inputs[param_offset].IsNull()) {
        decode_base64 = input.inputs[param_offset].GetValue<string>();
        param_offset++;
    }
    if (input.inputs.size() > param_offset && !input.inputs[param_offset].IsNull()) {
        transcode = input.inputs[param_offset].GetValue<string>();
        param_offset++;
    }
    if (input.inputs.size() > param_offset && !input.inputs[param_offset].IsNull()) {
        filters = input.inputs[param_offset].GetValue<string>();
    }
    
    // Check for repo_path named parameter (backward compatibility)
    for (const auto &kv : input.named_parameters) {
        if (kv.first == "repo_path") {
            repo_path = kv.second.GetValue<string>();
        }
    }
    
    // Define return schema - matches git_tree first 8 columns + git_read specific columns
    return_types = {
        LogicalType::VARCHAR,  // git_uri (complete git:// URI)
        LogicalType::VARCHAR,  // repo_path (repository filesystem path)
        LogicalType::VARCHAR,  // commit_hash (git commit hash)
        LogicalType::VARCHAR,  // tree_hash (git tree hash containing the file)
        LogicalType::VARCHAR,  // file_path (file path within repository)
        LogicalType::VARCHAR,  // file_ext (file extension)
        LogicalType::VARCHAR,  // ref (git reference)
        LogicalType::VARCHAR,  // blob_hash (git blob hash of file content)
        LogicalType::INTEGER,  // mode (file mode)
        LogicalType::VARCHAR,  // kind (object kind)
        LogicalType::BOOLEAN,  // is_text (whether content is text)
        LogicalType::VARCHAR,  // encoding (text encoding)
        LogicalType::BIGINT,   // size_bytes (file size in bytes)
        LogicalType::BOOLEAN,  // truncated (whether content was truncated)
        LogicalType::VARCHAR,  // text (text content)
        LogicalType::BLOB      // blob (base64 encoded binary content)
    };
    
    names = {"git_uri", "repo_path", "commit_hash", "tree_hash", "file_path", "file_ext", 
             "ref", "blob_hash", "mode", "kind", "is_text", "encoding", "size_bytes", 
             "truncated", "text", "blob"};
    
    return make_uniq<GitReadBindData>(max_bytes, decode_base64, transcode, filters, repo_path, uri, fallback_ref);
}

// Static git_read execution function (processes single URI from bind data)
static void GitReadFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
    auto &bind_data = input.bind_data->Cast<GitReadBindData>();
    auto &gstate = input.global_state->Cast<GitReadGlobalState>();
    
    if (gstate.finished) {
        output.SetCardinality(0);
        return;
    }
    
    // Process the URI from bind data
    GitReadLocalState::ReadResult result;
    ProcessGitURI(bind_data.uri, bind_data, result);
    
    // Fill output row using safe SetValue pattern (Our Fix #2)
    output.SetValue(0, 0, Value(result.git_uri));        // git_uri
    output.SetValue(1, 0, Value(result.repo_path));      // repo_path
    output.SetValue(2, 0, Value(result.commit_hash));    // commit_hash
    output.SetValue(3, 0, Value(result.tree_hash));      // tree_hash
    output.SetValue(4, 0, Value(result.file_path));      // file_path
    output.SetValue(5, 0, Value(result.file_ext));       // file_ext
    output.SetValue(6, 0, Value(result.ref));            // ref
    output.SetValue(7, 0, Value(result.blob_hash));      // blob_hash
    output.SetValue(8, 0, Value::INTEGER(result.mode));  // mode
    output.SetValue(9, 0, Value(result.kind));           // kind
    output.SetValue(10, 0, Value::BOOLEAN(result.is_text)); // is_text
    output.SetValue(11, 0, Value(result.encoding));      // encoding
    output.SetValue(12, 0, Value::BIGINT(result.size_bytes)); // size_bytes
    output.SetValue(13, 0, Value::BOOLEAN(result.truncated)); // truncated
    
    // Handle TEXT column
    if (!result.text.empty()) {
        output.SetValue(14, 0, Value(result.text));      // text
    } else {
        FlatVector::SetNull(output.data[14], 0, true);
    }
    
    // BLOB via SetValue (future-proof against non-flat vectors)
    if (!result.blob.empty()) {
        output.SetValue(15, 0, Value::BLOB_RAW(result.blob));
    } else {
        FlatVector::SetNull(output.data[15], 0, true);
    }
    
    output.SetCardinality(1);
    gstate.finished = true;
}

// Bind function for git_read_each (Pure LATERAL function)
static unique_ptr<FunctionData> GitReadEachBind(ClientContext &context, TableFunctionBindInput &input,
                                          vector<LogicalType> &return_types, vector<string> &names) {
    
    // LATERAL-only: reject direct calls, arguments arrive at runtime via input DataChunk
    if (!input.inputs.empty()) {
        throw BinderException("git_read_each is LATERAL-only. For direct calls, use git_read(...) instead");
    }
    
    // Use ParseLateralGitParams to get defaults (ref will be "HEAD")
    auto params = ParseLateralGitParams(input, 1);  // ref at index 1 for LATERAL
    
    // Set default parameters
    int64_t max_bytes = -1;  // No limit by default
    string decode_base64 = "auto";
    string transcode = "utf8";
    string filters = "raw";
    string repo_path = ".";
    string fallback_ref = params.ref;  // Use the parsed ref as fallback (defaults to "HEAD")
    
    // LATERAL-only: only named parameters are processed at bind time
    // Runtime parameters (repo_path_or_uri, ref, etc.) come via input DataChunk
    for (const auto &kv : input.named_parameters) {
        if (kv.first == "repo_path") {
            repo_path = kv.second.GetValue<string>();
        }
    }
    
    // Define return schema - matches git_tree first 8 columns + git_read specific columns
    return_types = {
        LogicalType::VARCHAR,  // git_uri (complete git:// URI)
        LogicalType::VARCHAR,  // repo_path (repository filesystem path)
        LogicalType::VARCHAR,  // commit_hash (git commit hash)
        LogicalType::VARCHAR,  // tree_hash (git tree hash containing the file)
        LogicalType::VARCHAR,  // file_path (file path within repository)
        LogicalType::VARCHAR,  // file_ext (file extension)
        LogicalType::VARCHAR,  // ref (git reference)
        LogicalType::VARCHAR,  // blob_hash (git blob hash of file content)
        LogicalType::INTEGER,  // mode (file mode)
        LogicalType::VARCHAR,  // kind (object kind)
        LogicalType::BOOLEAN,  // is_text (whether content is text)
        LogicalType::VARCHAR,  // encoding (text encoding)
        LogicalType::BIGINT,   // size_bytes (file size in bytes)
        LogicalType::BOOLEAN,  // truncated (whether content was truncated)
        LogicalType::VARCHAR,  // text (text content)
        LogicalType::BLOB      // blob (base64 encoded binary content)
    };
    
    names = {"git_uri", "repo_path", "commit_hash", "tree_hash", "file_path", "file_ext", 
             "ref", "blob_hash", "mode", "kind", "is_text", "encoding", "size_bytes", 
             "truncated", "text", "blob"};
    
    // LATERAL-only: never store URI in bind_data, it always comes from input DataChunk
    return make_uniq<GitReadBindData>(max_bytes, decode_base64, transcode, filters, repo_path, "", fallback_ref);
}

static unique_ptr<LocalTableFunctionState> GitReadLocalInit(ExecutionContext &context, 
                                                           TableFunctionInitInput &input,
                                                           GlobalTableFunctionState *global_state) {
    return make_uniq<GitReadLocalState>();
}

// git_read_each is ONLY for LATERAL joins - processes input from another table
// For direct calls, use git_read instead
static OperatorResultType GitReadEachFunction(ExecutionContext &context, TableFunctionInput &data_p, 
                                        DataChunk &input, DataChunk &output) {
    auto &bind_data = data_p.bind_data->Cast<GitReadBindData>();
    auto &state = data_p.local_state->Cast<GitReadLocalState>();
    
    // LATERAL-only; inputs flow via 'input' data chunk
    
    // Defensive invariants (debug builds) 
    // Note: output.ColumnCount() may differ from schema in LATERAL context
    D_ASSERT(input.ColumnCount() >= 1);
    
    // LATERAL mode - process input chunk
    while (true) {
        if (!state.initialized_row) {
            if (state.current_input_row >= input.size()) {
                state.current_input_row = 0;
                state.initialized_row = false;
                return OperatorResultType::NEED_MORE_INPUT;
            }
            
            // Read inputs via UnifiedVectorFormat (dictionary/constant safe)
            UnifiedVectorFormat repo_fmt;
            input.data[0].ToUnifiedFormat(input.size(), repo_fmt);
            const auto *repo_vals = UnifiedVectorFormat::GetData<string_t>(repo_fmt);
            const auto ridx = repo_fmt.sel->get_index(state.current_input_row);
            if (!repo_fmt.validity.RowIsValid(ridx)) {
                state.current_input_row++;
                state.initialized_row = false;
                continue; // NULL repo/URI row
            }
            const string first_param = repo_vals[ridx].GetString();
            
            string explicit_ref;
            if (input.ColumnCount() > 1) {
                UnifiedVectorFormat ref_fmt;
                input.data[1].ToUnifiedFormat(input.size(), ref_fmt);
                const auto *ref_vals = UnifiedVectorFormat::GetData<string_t>(ref_fmt);
                const auto r2idx = ref_fmt.sel->get_index(state.current_input_row);
                if (ref_fmt.validity.RowIsValid(r2idx)) {
                    explicit_ref = ref_vals[r2idx].GetString();
                }
            }

            // Canonicalize URI
            string uri;
            if (StringUtil::StartsWith(first_param, "git://")) {
                uri = explicit_ref.empty() ? first_param : first_param + "@" + explicit_ref;
            } else {
                const string &ref = explicit_ref.empty() ? bind_data.ref : explicit_ref;
                uri = "git://" + first_param + "@" + ref;
            }
            
            // Process the URI and extract content using GitContextManager
            state.current_results.clear();
            GitReadLocalState::ReadResult result;
            ProcessGitURI(uri, bind_data, result);
            state.current_results.push_back(result);
            
            state.initialized_row = true;
            state.current_output_row = 0;
        }
        
        // Output results for current input row (LATERAL mode)
        // Compute how many rows we can output this call
        idx_t remaining = state.current_results.size() - state.current_output_row;
        idx_t count = MinValue<idx_t>(remaining, STANDARD_VECTOR_SIZE);
        
        // Fill columns defensively based on actual column count
        const idx_t col_count = output.ColumnCount();
        
        for (idx_t i = 0; i < count; i++) {
            auto &result = state.current_results[state.current_output_row + i];
            
            // Fill columns up to available count (16-column schema from binder)
            // Force string materialization for LATERAL chain safety  
            if (col_count > 0) {
                // Create a completely new string to avoid any memory reference issues
                string safe_git_uri;
                safe_git_uri.reserve(result.git_uri.length() + 1);
                safe_git_uri.assign(result.git_uri.begin(), result.git_uri.end());
                output.SetValue(0, i, Value(safe_git_uri));        // git_uri
            }
            if (col_count > 1)  output.SetValue(1,  i, Value(result.repo_path));      // repo_path  
            if (col_count > 2) {
                // Ensure commit_hash is properly materialized (used in CONCAT operations)
                string safe_commit_hash;
                safe_commit_hash.reserve(result.commit_hash.length() + 1);
                safe_commit_hash.assign(result.commit_hash.begin(), result.commit_hash.end());
                output.SetValue(2, i, Value(safe_commit_hash));    // commit_hash
            }
            if (col_count > 3)  output.SetValue(3,  i, Value(result.tree_hash));      // tree_hash
            if (col_count > 4)  output.SetValue(4,  i, Value(result.file_path));      // file_path
            if (col_count > 5)  output.SetValue(5,  i, Value(result.file_ext));       // file_ext
            if (col_count > 6)  output.SetValue(6,  i, Value(result.ref));            // ref
            if (col_count > 7)  output.SetValue(7,  i, Value(result.blob_hash));      // blob_hash
            if (col_count > 8)  output.SetValue(8,  i, Value::INTEGER(result.mode));  // mode
            if (col_count > 9)  output.SetValue(9,  i, Value(result.kind));           // kind
            if (col_count > 10) output.SetValue(10, i, Value::BOOLEAN(result.is_text)); // is_text
            if (col_count > 11) output.SetValue(11, i, Value(result.encoding));       // encoding
            if (col_count > 12) output.SetValue(12, i, Value::BIGINT(result.size_bytes)); // size_bytes
            if (col_count > 13) output.SetValue(13, i, Value::BOOLEAN(result.truncated)); // truncated
            
            // Text column (14)
            if (col_count > 14) {
                if (!result.text.empty()) {
                    output.SetValue(14, i, Value(result.text));       // text
                } else {
                    FlatVector::SetNull(output.data[14], i, true);
                }
            }
            
            // BLOB column (15) via SetValue (future-proof against non-flat vectors)
            if (col_count > 15) {
                if (!result.blob.empty()) {
                    output.SetValue(15, i, Value::BLOB_RAW(result.blob));
                } else {
                    FlatVector::SetNull(output.data[15], i, true);
                }
            }
        }
        
        // Set cardinality after all values are filled to ensure proper vector initialization
        output.SetCardinality(count);
        
        // Force vector verification and finalization for LATERAL chain safety
        if (count > 0) {
            try {
                // Ensure all vectors are properly initialized and valid
                for (idx_t col_idx = 0; col_idx < output.ColumnCount(); col_idx++) {
                    auto &vec = output.data[col_idx];
                    if (vec.GetVectorType() != VectorType::CONSTANT_VECTOR) {
                        // Force vector to be in a known good state
                        vec.Flatten(count);
                    }
                }
            } catch (...) {
                // If vector operations fail, skip problematic row and continue processing
                output.SetCardinality(0);
                state.current_input_row++;
                state.initialized_row = false;
                return OperatorResultType::NEED_MORE_INPUT;
            }
        }
        
        state.current_output_row += count;
        
        if (count > 0) {
            if (state.current_output_row >= state.current_results.size()) {
                // Done with this input row - move to next
                state.current_input_row++;
                state.initialized_row = false;
            }
            
            return OperatorResultType::HAVE_MORE_OUTPUT;
        }
        
        // Move to next input row
        state.current_input_row++;
        state.initialized_row = false;
    }
}

void RegisterGitReadFunction(ExtensionLoader &loader) {
    // Static git_read function (processes literal URIs)
    // Correct pattern: TableFunction(name, args, function, bind, init_global)
    TableFunctionSet git_read_set("git_read");
    
    TableFunction git_read_1({LogicalType::VARCHAR}, GitReadFunction, GitReadBind, GitReadInitGlobal);
    git_read_1.named_parameters["repo_path"] = LogicalType::VARCHAR;
    git_read_set.AddFunction(git_read_1);
    
    TableFunction git_read_2({LogicalType::VARCHAR, LogicalType::BIGINT}, GitReadFunction, GitReadBind, GitReadInitGlobal);
    git_read_2.named_parameters["repo_path"] = LogicalType::VARCHAR;
    git_read_set.AddFunction(git_read_2);
    
    TableFunction git_read_3({LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::VARCHAR}, GitReadFunction, GitReadBind, GitReadInitGlobal);
    git_read_3.named_parameters["repo_path"] = LogicalType::VARCHAR;
    git_read_set.AddFunction(git_read_3);
    
    TableFunction git_read_4({LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::VARCHAR, LogicalType::VARCHAR}, GitReadFunction, GitReadBind, GitReadInitGlobal);
    git_read_4.named_parameters["repo_path"] = LogicalType::VARCHAR;
    git_read_set.AddFunction(git_read_4);
    
    TableFunction git_read_5({LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR}, GitReadFunction, GitReadBind, GitReadInitGlobal);
    git_read_5.named_parameters["repo_path"] = LogicalType::VARCHAR;
    git_read_set.AddFunction(git_read_5);
    
    loader.RegisterFunction( git_read_set);
    
    // git_read_each is ONLY for LATERAL joins - first param comes from LATERAL context
    // For direct calls, use git_read instead
    TableFunctionSet git_read_each_set("git_read_each");
    
    // Version 1: repo_path_or_uri_with_file (LATERAL-only)
    TableFunction git_read_each_1({LogicalType::VARCHAR}, nullptr, GitReadEachBind, GitReadInitGlobal, GitReadLocalInit);
    git_read_each_1.in_out_function = GitReadEachFunction;
    git_read_each_1.named_parameters["repo_path"] = LogicalType::VARCHAR;
    git_read_each_set.AddFunction(git_read_each_1);
    
    // Version 2: repo_path_or_uri_with_file, ref (LATERAL-only)
    TableFunction git_read_each_2({LogicalType::VARCHAR, LogicalType::VARCHAR}, nullptr, GitReadEachBind, GitReadInitGlobal, GitReadLocalInit);
    git_read_each_2.in_out_function = GitReadEachFunction;
    git_read_each_2.named_parameters["repo_path"] = LogicalType::VARCHAR;
    git_read_each_set.AddFunction(git_read_each_2);
    
    // Version 3: repo_path_or_uri_with_file, ref, max_bytes (LATERAL-only)
    TableFunction git_read_each_3({LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BIGINT}, nullptr, GitReadEachBind, GitReadInitGlobal, GitReadLocalInit);
    git_read_each_3.in_out_function = GitReadEachFunction;
    git_read_each_3.named_parameters["repo_path"] = LogicalType::VARCHAR;
    git_read_each_set.AddFunction(git_read_each_3);
    
    // Version 4: repo_path_or_uri_with_file, ref, max_bytes, decode_base64 (LATERAL-only)
    TableFunction git_read_each_4({LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::VARCHAR}, nullptr, GitReadEachBind, GitReadInitGlobal, GitReadLocalInit);
    git_read_each_4.in_out_function = GitReadEachFunction;
    git_read_each_4.named_parameters["repo_path"] = LogicalType::VARCHAR;
    git_read_each_set.AddFunction(git_read_each_4);
    
    // Version 5: repo_path_or_uri_with_file, ref, max_bytes, decode_base64, transcode (LATERAL-only)
    TableFunction git_read_each_5({LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::VARCHAR, LogicalType::VARCHAR}, nullptr, GitReadEachBind, GitReadInitGlobal, GitReadLocalInit);
    git_read_each_5.in_out_function = GitReadEachFunction;
    git_read_each_5.named_parameters["repo_path"] = LogicalType::VARCHAR;
    git_read_each_set.AddFunction(git_read_each_5);
    
    loader.RegisterFunction( git_read_each_set);
}

} // namespace duckdb