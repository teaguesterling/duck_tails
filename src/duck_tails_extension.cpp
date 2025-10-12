#define DUCKDB_EXTENSION_MAIN

#include "duck_tails_extension.hpp"
#include "git_filesystem.hpp"
#include "git_functions.hpp"
#include "text_diff.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/scalar_function.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>

// OpenSSL linked through vcpkg
#include <openssl/opensslv.h>

// libgit2 linked through vcpkg
#include <git2.h>

namespace duckdb {


static void LoadInternal(ExtensionLoader &loader) {
	// Register git filesystem
	RegisterGitFileSystem(loader);

	// Register git table functions
	RegisterGitFunctions(loader);

	// Register TextDiff type and functions (Phase 2)
	RegisterTextDiffType(loader);
}

void DuckTailsExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string DuckTailsExtension::Name() {
	return "duck_tails";
}

std::string DuckTailsExtension::Version() const {
#ifdef EXT_VERSION_DUCK_TAILS
	return EXT_VERSION_DUCK_TAILS;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(duck_tails, loader) {
	duckdb::LoadInternal(loader);
}

DUCKDB_EXTENSION_API const char *duck_tails_version() {
	return duckdb::DuckDB::LibraryVersion();
}
}

#ifndef DUCKDB_EXTENSION_MAIN
#error DUCKDB_EXTENSION_MAIN not defined
#endif
