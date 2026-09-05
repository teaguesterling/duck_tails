#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"
#include <type_traits>
#include <utility>

// duckdb_compat.hpp — fleet-standard cross-version shim for DuckDB extensions.
//
// Pattern established by @bendrucker in teaguesterling/duckdb_webbed#76 (May 2026):
// detect the new API via __has_include of headers that moved in the same DuckDB
// refactor ([duckdb/duckdb#22377](https://github.com/duckdb/duckdb/pull/22377) —
// "mandatory per-vector size tracking" landed alongside the vector-buffer header
// reshuffle), then dispatch via a single #ifdef block.
//
// Cross-version coverage:
//   - duckdb v1.4.x / v1.5.x: old API everywhere
//   - duckdb main / v1.6.x:   new API everywhere
//
// See teaguesterling/duckdb_markdown's docs/DUCKDB_API_MIGRATION.md for the
// long-form rationale + upgrade checklist for other extensions.

#if __has_include("duckdb/common/vector/list_vector.hpp")
#define DUCKDB_HAS_NEW_VECTOR_HEADERS 1
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#endif

// --- v2.0 (v2.0-cyanoptera) additions ----------------------------------------
// FEATURE DETECTION, NOT VERSION NUMBERS, and each change PROBED SEPARATELY. A
// version macro says when a thing changed; a probe says whether it changed here,
// and keeps working when a change is backported, reverted, or lands on a branch
// nobody expected. Tying several changes to one macro silently picks the wrong
// branch the moment they land in different releases.
//
// duckdb::Identifier replaced std::string as the name type in table-function and
// COPY bind signatures. Identifier compares case-insensitively, and construction
// from a RUNTIME string is explicit by design -- promoting a string to an
// identifier is meant to be a deliberate act at the call site -- so a boundary
// helper is needed rather than an implicit conversion. String LITERALS convert
// implicitly (Identifier(const char *)), which is why `names = {"a", "b"}` needs
// no change at all.
//
// __has_include ONLY gates whether an Identifier overload of CompatNameStr can
// exist. It deliberately does NOT decide what CompatName is -- see below.
#if __has_include("duckdb/common/identifier.hpp")
#define DUCKDB_HAS_IDENTIFIER 1
#include "duckdb/common/identifier.hpp"
#endif

namespace duckdb {

// --- bind-signature name type -------------------------------------------------
// Used wherever a bind callback receives or fills a vector of column names.
//
// DERIVED FROM DUCKDB, NOT PROBED FOR. This is the one place where the
// header-presence idiom is actively WRONG, and it fails in the dangerous
// direction -- on the version we ship, not the one we are porting to:
//
//   v1.5-variegata @ our pin      no identifier.hpp    bind takes vector<string>
//   v1.5-variegata @ branch tip   HAS identifier.hpp   bind takes vector<string>
//   main (v2.0)                   HAS identifier.hpp   bind takes vector<Identifier>
//
// identifier.hpp was BACKPORTED to the stable branch WITHOUT changing
// table_function_bind_t. So `#if __has_include(identifier.hpp)` is correct today
// only by the accident that our pin predates the backport; the next routine
// submodule bump would flip CompatName to Identifier on a DuckDB that still
// wants strings, and every bind signature in this repo would stop compiling at
// once. A probe for a header answers "did this header land", which is simply not
// the question -- the question is "what does the bind callback take".
//
// So ask DuckDB directly. TableFunctionBindInput::input_table_names has the same
// element type as the bind out-parameter on both lines (table_function.hpp:110
// and :289 on the pin; :123 and :319 on main), and it is a member we can name in
// a decltype. Deriving from it cannot drift, because if the two ever disagreed
// DuckDB itself would be inconsistent.
using CompatName = typename std::remove_reference<decltype(
    std::declval<TableFunctionBindInput &>().input_table_names)>::type::value_type;

// Returned BY VALUE, not as a reference into the argument: callers routinely
// write CompatNameStr(CompatMakeName(s)) on a temporary.
inline string CompatNameStr(const string &name) {
	return name;
}
#ifdef DUCKDB_HAS_IDENTIFIER
inline string CompatNameStr(const Identifier &id) {
	return id.GetIdentifierName();
}
#endif
inline CompatName CompatMakeName(string name) {
	return CompatName(std::move(name));
}

// Every shim below dispatches on a TAG rather than with `if constexpr`, so this
// header also compiles at C++11. Several extensions in this ecosystem build
// their TUs at C++11 deliberately (forcing C++17 on the extension but not on
// libduckdb makes static-const members in duckdb's headers acquire implicit
// inline linkage in one and not the other, which produces multiple-definition
// link errors), and `if constexpr` is C++17-only. Tag dispatch has the property
// that matters here: only the selected overload is instantiated, so the branch
// naming the absent member is never compiled.

// --- LogicalType alias ---------------------------------------------------------
// v1.5: void SetAlias(string)               -- mutates in place
// v2.0: LogicalType WithAlias(string) const -- returns a copy, never mutating a
//       type whose type-info may be shared. SetAlias is REMOVED, not deprecated.
// (Note that only LogicalType::SetAlias went away. BaseExpression::SetAlias is
// alive and well -- it just takes an Identifier now -- so a bare grep for
// SetAlias finds sites that must NOT be touched.)
template <class T, class = void>
struct CompatHasWithAlias : std::false_type {};
template <class T>
struct CompatHasWithAlias<T, decltype(void(std::declval<const T &>().WithAlias(string())))> : std::true_type {};

template <class TYPE>
inline LogicalType CompatWithAliasImpl(TYPE type, string alias, std::true_type) {
	return type.WithAlias(std::move(alias));
}
template <class TYPE>
inline LogicalType CompatWithAliasImpl(TYPE type, string alias, std::false_type) {
	type.SetAlias(std::move(alias));
	return type;
}
// The ENTRY POINT is deliberately NOT a template. A `template <class TYPE =
// LogicalType>` form looks equivalent but is not: the default template argument
// is inert because deduction wins, so the very common call
//
//     CompatWithAlias(LogicalType::VARCHAR, "md")
//
// deduces TYPE = LogicalTypeId -- `LogicalType::VARCHAR` is a static constexpr
// LogicalTypeId (types.hpp), not a LogicalType -- and then hard-errors inside
// the shim with "request for member 'SetAlias' in 'type', which is of non-class
// type 'duckdb::LogicalTypeId'". A concrete parameter restores the implicit
// LogicalTypeId -> LogicalType conversion at the call site.
inline LogicalType CompatWithAlias(LogicalType type, string alias) {
	return CompatWithAliasImpl(std::move(type), std::move(alias), CompatHasWithAlias<LogicalType>());
}

// --- Vector::ToUnifiedFormat ---------------------------------------------------
// v1.5: ToUnifiedFormat(count, data)  -- the only overload
// v2.0: ToUnifiedFormat(data)         -- plus the count form kept as [[deprecated]]
//
// PROBE FOR THE COUNT-FREE OVERLOAD, not the count-taking one. v2.0 did not
// remove the count form, it deprecated it, so a probe for the count form is true
// on BOTH versions and the shim would always take the deprecated path -- silently
// never calling the new API it exists to reach, and compiling clean the whole
// time. The count-free form is the one that exists only on v2.0, so it is the
// one that discriminates.
template <class T, class = void>
struct CompatToUnifiedWithoutCount : std::false_type {};
template <class T>
struct CompatToUnifiedWithoutCount<T, decltype(void(std::declval<T &>().ToUnifiedFormat(
                                          std::declval<UnifiedVectorFormat &>())))> : std::true_type {};

template <class VEC>
inline void CompatToUnifiedFormatImpl(VEC &vec, idx_t, UnifiedVectorFormat &data, std::true_type) {
	vec.ToUnifiedFormat(data);
}
template <class VEC>
inline void CompatToUnifiedFormatImpl(VEC &vec, idx_t count, UnifiedVectorFormat &data, std::false_type) {
	vec.ToUnifiedFormat(count, data);
}
template <class VEC = Vector>
inline void CompatToUnifiedFormat(VEC &vec, idx_t count, UnifiedVectorFormat &data) {
	CompatToUnifiedFormatImpl(vec, count, data, CompatToUnifiedWithoutCount<VEC>());
}

// --- Flat/Constant vector mutable data ------------------------------------------
// v1.5: FlatVector::GetData<T>(vec)         returns T*
// v2.0: FlatVector::GetData<T>(vec)         returns const T*
//       FlatVector::GetDataMutable<T>(vec)  returns T*
// Writing through the v2.0 GetData is a compile error, which is the point of the
// split -- so the WRITE path must ask for mutability explicitly. Parameterised on
// the accessor class so it serves ConstantVector as well as FlatVector:
//   CompatFlatDataMutable<string_t>(result)                    // FlatVector
//   CompatFlatDataMutable<string_t, ConstantVector>(result)    // ConstantVector
template <class T, class = void>
struct CompatHasFlatGetDataMutable : std::false_type {};
template <class T>
struct CompatHasFlatGetDataMutable<T, decltype(void(T::template GetDataMutable<bool>(std::declval<Vector &>())))>
    : std::true_type {};

// NOT interchangeable with const_cast<VALUE *>(FV::GetData<VALUE>(vec)): the
// mutable accessor goes through BufferMutable(), which un-shares a copy-on-write
// buffer first. Casting the constness off the read accessor writes into a buffer
// that may still be shared -- it compiles, and it corrupts silently.
template <class VALUE, class FV>
inline VALUE *CompatFlatDataMutableImpl(Vector &vec, std::true_type) {
	return FV::template GetDataMutable<VALUE>(vec);
}
template <class VALUE, class FV>
inline VALUE *CompatFlatDataMutableImpl(Vector &vec, std::false_type) {
	return FV::template GetData<VALUE>(vec);
}
template <class VALUE, class FV = FlatVector>
inline VALUE *CompatFlatDataMutable(Vector &vec) {
	return CompatFlatDataMutableImpl<VALUE, FV>(vec, CompatHasFlatGetDataMutable<FV>());
}

// --- Flat vector mutable validity mask -------------------------------------------
// The same const-split applied to the validity mask, and it is a SEPARATE change
// from the data-pointer one, so it gets its own probe:
//   v1.5: FlatVector::Validity(Vector &)         returns ValidityMask&
//   v2.0: FlatVector::Validity(const Vector &)   returns const ValidityMask&
//         FlatVector::ValidityMutable(Vector &)  returns ValidityMask&
// On v2.0 the old spelling still COMPILES at the call site and only fails later,
// where a mutating method is invoked on the returned reference:
//   error: passing 'const duckdb::ValidityMask' as 'this' argument discards
//          qualifiers
// so the reported error line is the SetInvalid/SetValid call, not the accessor.
template <class T, class = void>
struct CompatHasFlatValidityMutable : std::false_type {};
template <class T>
struct CompatHasFlatValidityMutable<T, decltype(void(T::ValidityMutable(std::declval<Vector &>())))> : std::true_type {
};

template <class FV>
inline ValidityMask &CompatFlatValidityMutableImpl(Vector &vec, std::true_type) {
	return FV::ValidityMutable(vec);
}
template <class FV>
inline ValidityMask &CompatFlatValidityMutableImpl(Vector &vec, std::false_type) {
	return FV::Validity(vec);
}
template <class FV = FlatVector>
inline ValidityMask &CompatFlatValidityMutable(Vector &vec) {
	return CompatFlatValidityMutableImpl<FV>(vec, CompatHasFlatValidityMutable<FV>());
}

// --- Output chunk finalization ---------------------------------------------------
// DuckDB main mandates per-vector Size() tracking. DataChunk::SetCardinality only
// updates chunk.count (and is [[deprecated]] there); SetChildCardinality
// additionally calls FlatVector::SetSize on every column, so operators reading
// vec.Size() see the right value. Without it, VariadicExecutor and friends report:
//   "Mismatch in input vector sizes ... expected 0 rows but got N"
//
// PROBED FOR THE MEMBER, not for a header. This dispatch used to hang off
// __has_include("duckdb/common/vector/list_vector.hpp"), which is the same shape
// of bug as the identifier.hpp probe above: it asks whether a header landed when
// the question is whether a method exists. Those headers could be backported to
// the stable branch without SetChildCardinality coming with them, and the PINNED
// build would break. The #include gate stays header-based, because that one
// genuinely is a question about headers.
//
// ORDER MATTERS AT THE CALL SITE, and upstream's own comment on the deprecated
// SetCardinality warns that forwarding it to SetChildCardinality "would
// resize/overwrite their data" for callers that mutate child vectors first. That
// warning does NOT apply to this extension, and the distinction is worth stating
// because it is easy to over-apply:
//
//   - Every one of duck_tails' ~25 call sites is WRITE-AT-INDEX-then-set: fill
//     with output.SetValue(col, row, ...) / FlatVector::SetNull(vec, row, ...),
//     then set the cardinality once at the end.
//   - Writing at an index does not update the vector's tracked size, so these
//     sites NEED SetChildCardinality; SetCardinalityUnsafe would leave every
//     child at size 0 and reproduce the mismatch above.
//   - Doing it after the writes is safe: SetChildCardinality only calls
//     FlatVector::SetSize, which bottoms out in VectorBuffer::SetVectorSize,
//     which bounds-checks and then assigns v_size. It moves no data and zeroes
//     nothing (verified in duckdb main, src/common/types/vector_buffer.cpp).
//
// The callers upstream is warning about are Vector::Append-style ones, whose
// vectors already carry a size. If such a call site is ever added here, it needs
// SetCardinalityUnsafe instead -- and it will compile either way, so the only
// symptom would be wrong column data on v2.0.
template <class T, class = void>
struct CompatHasSetChildCardinality : std::false_type {};
template <class T>
struct CompatHasSetChildCardinality<T, decltype(void(std::declval<T &>().SetChildCardinality(idx_t(0))))>
    : std::true_type {};

// The Impl overloads MUST be templates. Tag dispatch only avoids compiling the
// untaken branch when that branch is a template that is never instantiated -- as
// plain functions both bodies are compiled, and the one naming the absent member
// fails on whichever version lacks it.
template <class CHUNK>
inline void CompatSetOutputCardinalityImpl(CHUNK &chunk, idx_t count, std::true_type) {
	chunk.SetChildCardinality(count);
}
template <class CHUNK>
inline void CompatSetOutputCardinalityImpl(CHUNK &chunk, idx_t count, std::false_type) {
	chunk.SetCardinality(count);
}
template <class CHUNK = DataChunk>
inline void CompatSetOutputCardinality(CHUNK &chunk, idx_t count) {
	CompatSetOutputCardinalityImpl(chunk, count, CompatHasSetChildCardinality<CHUNK>());
}

} // namespace duckdb
