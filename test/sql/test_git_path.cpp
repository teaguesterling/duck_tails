// test_git_path.cpp
#include "git_path.hpp"
#include "duckdb/common/exception.hpp"
#include <catch2/catch.hpp>

using duckdb::NormalizeRepoPathSpec;
using duckdb::BinderException;

TEST_CASE("Normalize basic forms", "[git][path]") {
    CHECK(NormalizeRepoPathSpec("") == "");
    CHECK(NormalizeRepoPathSpec("/") == "");
    CHECK(NormalizeRepoPathSpec("src") == "src");
    CHECK(NormalizeRepoPathSpec("./src") == "src");
    CHECK(NormalizeRepoPathSpec("//src///lib//") == "src/lib");
}

TEST_CASE("Do not fold mid-path '.' segments", "[git][path]") {
    // We only drop *leading* "./"
    CHECK(NormalizeRepoPathSpec("src/./lib") == "src/./lib");
}

TEST_CASE("Forbid .. segments", "[git][path]") {
    CHECK_THROWS_AS(NormalizeRepoPathSpec(".."), BinderException);
    CHECK_THROWS_AS(NormalizeRepoPathSpec("../x"), BinderException);
    CHECK_THROWS_AS(NormalizeRepoPathSpec("src/../lib"), BinderException);
    // legit names that merely contain dots are allowed
    CHECK(NormalizeRepoPathSpec("src/.../lib") == "src/.../lib");
    CHECK(NormalizeRepoPathSpec("a/..b/c") == "a/..b/c");
}

TEST_CASE("No backslash translation", "[git][path]") {
    // Backslashes are literal; we don't touch them
    CHECK(NormalizeRepoPathSpec(R"(dir\name/file)") == R"(dir\name/file)");
}

TEST_CASE("Multiple leading ./ segments collapse", "[git][path]") {
    CHECK(NormalizeRepoPathSpec("./././a") == "a");
    CHECK(NormalizeRepoPathSpec("././") == "");
    CHECK(NormalizeRepoPathSpec("./") == "");
}

TEST_CASE("Heavy mixed normalization", "[git][path]") {
    // Leading slashes, leading ./, internal //, trailing //
    CHECK(NormalizeRepoPathSpec("////./src///") == "src");
    CHECK(NormalizeRepoPathSpec("a//b///c") == "a/b/c");
    CHECK(NormalizeRepoPathSpec("a//b/././c//") == "a/b/././c");
    CHECK(NormalizeRepoPathSpec("///") == "");
}

TEST_CASE("Mid-path '.' segments are preserved exactly", "[git][path]") {
    CHECK(NormalizeRepoPathSpec("src/.") == "src/.");              // trailing dot-segment stays
    CHECK(NormalizeRepoPathSpec("src/././lib") == "src/././lib");  // already covered, keep for redundancy
}

TEST_CASE("Dot-dot segments throw in all positions", "[git][path]") {
    CHECK_THROWS_AS(NormalizeRepoPathSpec("./../"), BinderException);  // becomes ".." after dropping leading "./"
    CHECK_THROWS_AS(NormalizeRepoPathSpec("a/../../b"), BinderException);
    CHECK_THROWS_AS(NormalizeRepoPathSpec("a/.../../c"), BinderException); // '..' as its own segment
}

TEST_CASE("Do not treat lookalikes as '..'", "[git][path]") {
    // Percent-encoded ".." isn't decoded here; allowed literally
    CHECK(NormalizeRepoPathSpec("src/%2E%2E/lib") == "src/%2E%2E/lib");
    // Segment that merely contains two dots is fine
    CHECK(NormalizeRepoPathSpec("a/..b/c") == "a/..b/c");
}

TEST_CASE("Unicode and mixed separators are preserved", "[git][path]") {
    CHECK(NormalizeRepoPathSpec("src/文件/データ") == "src/文件/データ");
    // Backslashes are literal; not translated or split
    CHECK(NormalizeRepoPathSpec(R"(dir\sub//file)") == R"(dir\sub/file)");
}

TEST_CASE("Idempotence of collapsing //", "[git][path]") {
    // After normalization, running normalization again should be a no-op
    auto once = NormalizeRepoPathSpec("a///b////c/././d//");
    auto twice = NormalizeRepoPathSpec(once);
    CHECK(once == twice);
}
