module;

// StitchKey (#504, split out of storm_orm_utilities in #671, which re-exports
// this module so existing importers are unchanged).

#include <cassert>

export module storm_orm_stitch_key;

import std;

export namespace storm::orm::utilities {

    // ============================================================================
    // Stitch Key (#504)
    // ============================================================================

    // The m2m/reverse-FK two-query eager-load stitches Q2 rows to their Q1 owner
    // through a hash map keyed on the owner's primary key. A single-column PK key
    // was a bare std::int64_t; a composite PK needs a multi-value key that still
    // crosses the M2MRelation type-erased vtable (ErasedStatementPtr — no T in
    // scope there), so it can't be a template parameter. StitchKey is a
    // fixed-size inline byte buffer built by appending one part at a time:
    // append_int64 for integral PK parts (8 bytes, matching today's exact
    // single-PK layout so that case is byte-identical), append_string for text
    // parts (hashed to 8 bytes — the stitch only needs equality for map lookup,
    // never the original text back, so storing a hash instead of the bytes
    // avoids capping composite key width by string length). CAPACITY=32 covers
    // every composite PK in the codebase today (max 3 parts before #504 ships);
    // a 4th part (int64 or string-hash) still fits exactly (4 x 8 = 32) but is a
    // hard ceiling with zero slack — a 5th part overflows.
    //
    // That ceiling is enforced at COMPILE TIME, at the model (#537). The only
    // guard used to be the assert in append_word, which NDEBUG compiles out —
    // i.e. it was absent from exactly the Release configuration where the
    // overrun would matter. ModelPrimaryKeyPartLimit<T> (model_concepts.cppm) now rejects a
    // model with more parts than MAX_PARTS, so append_word cannot be reached with
    // a full buffer and the assert is a redundant backstop rather than the guard.
    //
    // CAPACITY was NOT simply grown to make room: it is load-bearing for
    // performance. #504 measured a wider key costing ~4% on the stitch hot path,
    // which is why single-PK models bypass this class entirely for a bare
    // std::uint64_t. Folding parts beyond the 4th was also rejected in #504 —
    // two distinct composite keys colliding in the fold is a MIS-STITCH (rows
    // attached to the wrong owner), not a recoverable map collision.
    class StitchKey {
      public:
        static constexpr std::size_t CAPACITY = 32;

        // How many parts fit. Every appender writes exactly one 8-byte word (see
        // append_word), so this is the capacity in words. DERIVED rather than
        // written as a literal 4 so the compile-time model gate that reads it
        // tracks CAPACITY automatically: changing one cannot leave the other
        // stale, which is the failure mode that would reintroduce the overrun.
        static constexpr std::size_t MAX_PARTS = CAPACITY / sizeof(std::uint64_t);

        void append_int64(std::int64_t v) noexcept {
            append_word(static_cast<std::uint64_t>(v));
        }

        // Both loops below read whole 8-byte words, so every appender must write
        // exactly 8. append_int64 does by construction; append_string does only
        // while size_t is 64-bit, so assert it rather than leave it to a comment.
        static_assert(sizeof(std::size_t) == sizeof(std::uint64_t), "StitchKey assumes a 64-bit std::size_t");

        void append_string(std::string_view v) noexcept {
            append_word(std::hash<std::string_view>{}(v));
        }

        // Word-wise compare, inline. This runs on EVERY hash-map probe of the m2m
        // stitch, where the bare std::int64_t key it replaced compared in one
        // inlined instruction, so the two obvious spellings both cost real time:
        // std::ranges::equal over a span lowers to a byte-at-a-time loop
        // (movzbl/cmp/jne per byte), and std::memcmp lowers to a PLT call into
        // libc's bcmp — perf attributed 11.7% of the fanout200 profile to that
        // call alone. Comparing whole words keeps it branch-light and callable-free.
        // Safe because len_ is always a multiple of 8 (see hash()).
        friend auto operator==(const StitchKey& lhs, const StitchKey& rhs) noexcept -> bool {
            if (lhs.len_ != rhs.len_) {
                return false;
            }
            for (std::size_t off = 0; off + sizeof(std::uint64_t) <= lhs.len_; off += sizeof(std::uint64_t)) {
                std::uint64_t lhs_word = 0;
                std::uint64_t rhs_word = 0;
                std::memcpy(&lhs_word, lhs.bytes_.data() + off, sizeof(lhs_word));
                std::memcpy(&rhs_word, rhs.bytes_.data() + off, sizeof(rhs_word));
                if (lhs_word != rhs_word) {
                    return false;
                }
            }
            return true;
        }

        // Word-wise FNV-1a. NOT std::hash<string_view>: libc++ routes that to
        // murmur2/cityhash, which perf showed running once per Q1 owner AND once
        // per Q2 related row, where the bare std::int64_t key this replaced used
        // the identity hash.
        //
        // Reads whole words, which is safe because len_ is always a multiple of 8:
        // the only writer is append_word, which is private and takes a whole
        // std::uint64_t, so a part of any other width is not expressible. Were one
        // ever added, its tail would fall outside this loop and contribute nothing
        // to the hash — not a correctness bug (operator== still separates such
        // keys, so the map resolves it) but a silent collision cliff.
        [[nodiscard]] auto hash() const noexcept -> std::size_t {
            constexpr std::uint64_t FNV_OFFSET = 1469598103934665603ULL;
            constexpr std::uint64_t FNV_PRIME  = 1099511628211ULL;
            std::uint64_t           h          = FNV_OFFSET;
            for (std::size_t off = 0; off + sizeof(std::uint64_t) <= len_; off += sizeof(std::uint64_t)) {
                std::uint64_t word = 0;
                std::memcpy(&word, bytes_.data() + off, sizeof(word));
                h = (h ^ word) * FNV_PRIME;
            }
            return static_cast<std::size_t>(h);
        }

        // The single word of a ONE-part key (#504 perf). A single-column PK is the
        // overwhelmingly common shape, and for it this whole class is a 33-byte
        // wrapper around one std::uint64_t: the map can be keyed on that word
        // directly, recovering libc++'s identity std::hash<integral> and a
        // one-instruction inlined operator== in place of hash<StitchKey>'s FNV loop
        // and this class's out-of-line word compare. SelectStatement narrows to this
        // when Base::primary_key_column_count_ == 1; composite models keep the full
        // key. Only meaningful for len_ == sizeof(std::uint64_t) — asserted, since a
        // composite key read through here would silently stitch on its first part
        // alone (a real mis-stitch, not just a collision).
        [[nodiscard]] auto first_word() const noexcept -> std::uint64_t {
            assert(len_ == sizeof(std::uint64_t) && "StitchKey::first_word on a non-single-part key");
            std::uint64_t word = 0;
            std::memcpy(&word, bytes_.data(), sizeof(word));
            return word;
        }

      private:
        // Takes a whole word rather than a (pointer, size) pair: the "every part is
        // exactly 8 bytes" invariant that hash() and operator== both read whole words
        // on is then enforced by the signature instead of resting on each caller
        // passing sizeof correctly.
        //
        // The assert is a BACKSTOP, not the guard (#537). It is compiled out under
        // NDEBUG — precisely the Release build — so it never protected the case it
        // names. ModelPrimaryKeyPartLimit<T> rejects an over-wide model at compile
        // time, before any statement class instantiates, so this is now unreachable
        // for every model that exists; it is retained to catch a direct caller
        // (the unit tests append parts by hand, bypassing the model gate).
        void append_word(std::uint64_t word) noexcept {
            assert(len_ + sizeof(word) <= CAPACITY && "StitchKey: append exceeds fixed CAPACITY");
            std::memcpy(bytes_.data() + len_, &word, sizeof(word));
            len_ += sizeof(word);
        }

        std::array<std::byte, CAPACITY> bytes_{};
        std::size_t                     len_ = 0;
    };

} // namespace storm::orm::utilities

// std::hash<StitchKey> must live in namespace std (import std; provides no macro
// or ADL escape hatch for specializing it inside storm::). Delegates to the
// member hash(), which covers the occupied byte range only (mirroring
// operator=='s len_-bounded comparison).
export template <> struct std::hash<storm::orm::utilities::StitchKey> {
    auto operator()(const storm::orm::utilities::StitchKey& k) const noexcept -> std::size_t {
        return k.hash();
    }
};
