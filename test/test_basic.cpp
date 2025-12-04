#include "duckdb/main/appender.hpp"
#include "duckdb/main/db_instance_cache.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

TEST_CASE("Test duck_tails extension basic functionality", "[duck_tails]") {
	DuckDB db(nullptr);
	Connection con(db);

	// Load the duck_tails extension
	auto result = con.Query("LOAD 'duck_tails';");
	REQUIRE(!result->HasError());

	// Test basic extension function
	result = con.Query("SELECT duck_tails('test');");
	REQUIRE(!result->HasError());
	auto data = result->GetValue(0, 0);
	REQUIRE(data.ToString() == "DuckTails test 🐥");
}

TEST_CASE("Test git filesystem registration", "[duck_tails]") {
	DuckDB db(nullptr);
	Connection con(db);

	// Load the duck_tails extension
	auto result = con.Query("LOAD 'duck_tails';");
	REQUIRE(!result->HasError());

	// Test git URL parsing (this will fail if no git repo, but shouldn't crash)
	// We just want to verify the filesystem is registered
	result = con.Query("SELECT 'git://README.md@HEAD' AS git_path;");
	REQUIRE(!result->HasError());
	auto data = result->GetValue(0, 0);
	REQUIRE(data.ToString() == "git://README.md@HEAD");
}
