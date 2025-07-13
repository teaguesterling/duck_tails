#define DUCKDB_EXTENSION_MAIN

#include "duck_tails_extension.hpp"
#include "git_filesystem.hpp"
#include "git_functions.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/extension_util.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>

// OpenSSL linked through vcpkg
#include <openssl/opensslv.h>

// libgit2 linked through vcpkg
#include <git2.h>

namespace duckdb {

inline void DuckTailsScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &name_vector = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(name_vector, result, args.size(), [&](string_t name) {
		return StringVector::AddString(result, "DuckTails " + name.GetString() + " 🐥");
	});
}

inline void DuckTailsOpenSSLVersionScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &name_vector = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(name_vector, result, args.size(), [&](string_t name) {
		return StringVector::AddString(result, "DuckTails " + name.GetString() + ", my linked OpenSSL version is " +
		                                           OPENSSL_VERSION_TEXT);
	});
}

static void LoadInternal(DatabaseInstance &instance) {
	// Register git filesystem
	RegisterGitFileSystem(instance);
	
	// Register git table functions
	RegisterGitFunctions(instance);
	
	// Register a scalar function
	auto duck_tails_scalar_function = ScalarFunction("duck_tails", {LogicalType::VARCHAR}, LogicalType::VARCHAR, DuckTailsScalarFun);
	ExtensionUtil::RegisterFunction(instance, duck_tails_scalar_function);

	// Register another scalar function
	auto duck_tails_openssl_version_scalar_function = ScalarFunction("duck_tails_openssl_version", {LogicalType::VARCHAR},
	                                                            LogicalType::VARCHAR, DuckTailsOpenSSLVersionScalarFun);
	ExtensionUtil::RegisterFunction(instance, duck_tails_openssl_version_scalar_function);
}

void DuckTailsExtension::Load(DuckDB &db) {
	LoadInternal(*db.instance);
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

DUCKDB_EXTENSION_API void duck_tails_init(duckdb::DatabaseInstance &db) {
	duckdb::DuckDB db_wrapper(db);
	db_wrapper.LoadExtension<duckdb::DuckTailsExtension>();
}

DUCKDB_EXTENSION_API const char *duck_tails_version() {
	return duckdb::DuckDB::LibraryVersion();
}
}

#ifndef DUCKDB_EXTENSION_MAIN
#error DUCKDB_EXTENSION_MAIN not defined
#endif
