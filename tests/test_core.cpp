#include "buffer_fabric/accounting.hpp"
#include "buffer_fabric/codec.hpp"
#include "buffer_fabric/hash.hpp"
#include "buffer_fabric/json.hpp"
#include "buffer_fabric/policy.hpp"
#include "buffer_fabric/reclaim.hpp"
#include "test_framework.hpp"

using namespace buffer_fabric;

BF_TEST(types, checked_arithmetic_reports_overflow) {
    u64 out = 0;
    BF_CHECK(!add_overflow(1, 2, &out));
    BF_CHECK_EQ(out, u64{3});
    BF_CHECK(add_overflow((std::numeric_limits<u64>::max)(), 1, &out));
    BF_CHECK(!sub_underflow(10, 4, &out));
    BF_CHECK_EQ(out, u64{6});
    BF_CHECK(sub_underflow(4, 10, &out));
    BF_CHECK(!mul_overflow(1ull << 32, 1ull << 31, &out));
    BF_CHECK_EQ(out, 1ull << 63);
    BF_CHECK(mul_overflow(1ull << 33, 1ull << 31, &out));
}

BF_TEST(types, mul_compare_is_exact) {
    BF_CHECK_EQ(mul_compare(3, 4, 12, 1), 0);
    BF_CHECK_EQ(mul_compare(3, 4, 11, 1), 1);
    BF_CHECK_EQ(mul_compare(3, 4, 13, 1), -1);
    // Values whose product overflows 64 bits must still compare correctly.
    const u64 huge = (std::numeric_limits<u64>::max)();
    BF_CHECK_EQ(mul_compare(huge, 2, huge, 2), 0);
    BF_CHECK_EQ(mul_compare(huge, 2, huge, 3), -1);
    BF_CHECK_EQ(mul_compare(huge, huge, huge, huge - 1), 1);
    BF_CHECK_EQ(mul_compare(0, huge, 0, 1), 0);
}

BF_TEST(types, strong_ids_are_distinct_types) {
    const PoolId pool = PoolId::from_raw(7);
    BF_CHECK(pool.valid());
    BF_CHECK(!PoolId{}.valid());
    BF_CHECK_EQ(pool.raw(), u64{7});
    const PoolId other = PoolId::from_raw(7);
    BF_CHECK(pool == other);
    BF_CHECK(PoolId::from_raw(1) < PoolId::from_raw(2));
}

BF_TEST(types, names_are_bounded) {
    BF_CHECK(is_valid_name("pool-1.a_b"));
    BF_CHECK(!is_valid_name(""));
    BF_CHECK(!is_valid_name("has space"));
    BF_CHECK(!is_valid_name("semi;colon"));
    BF_CHECK(!is_valid_name(std::string(kMaxNameBytes + 1, 'a')));
    BF_CHECK(is_valid_name(std::string(kMaxNameBytes, 'a')));
}

BF_TEST(types, digest_is_stable_and_ordered) {
    const Digest a = digest_bytes("abc", 3);
    const Digest b = digest_bytes("abc", 3);
    const Digest c = digest_bytes("abd", 3);
    BF_CHECK(a == b);
    BF_CHECK(a != c);
    BF_CHECK(a.valid());
    BF_CHECK_EQ(a.to_hex().size(), usize{32});
}

BF_TEST(codec, crc32c_matches_known_vector) {
    // CRC-32C of "123456789" is 0xE3069283 (Castagnoli, reflected).
    BF_CHECK_EQ(crc32c("123456789", 9), 0xE3069283u);
    BF_CHECK_EQ(crc32c("", 0), 0u);
}

BF_TEST(codec, frame_round_trip_and_rejection) {
    const char payload[] = "framed payload";
    const usize size = sizeof(payload) - 1;
    const std::vector<u8> frame = encode_frame(payload, size, 0x11223344u, 7u);
    Frame decoded;
    usize consumed = 0;
    BF_CHECK(decode_frame(frame.data(), frame.size(), 0x11223344u, 7u, 1024, &decoded, &consumed)
                 .ok());
    BF_CHECK_EQ(consumed, frame.size());
    BF_CHECK_EQ(decoded.payload.size(), size);

    // Wrong magic.
    BF_CHECK_CODE(decode_frame(frame.data(), frame.size(), 0xAABBCCDDu, 7u, 1024, &decoded, &consumed),
                  ErrorCode::CorruptRecord);
    // Wrong version.
    BF_CHECK_CODE(decode_frame(frame.data(), frame.size(), 0x11223344u, 8u, 1024, &decoded, &consumed),
                  ErrorCode::VersionMismatch);
    // Payload above the bound.
    BF_CHECK_CODE(decode_frame(frame.data(), frame.size(), 0x11223344u, 7u, 4, &decoded, &consumed),
                  ErrorCode::OversizedMessage);
    // Truncated payload.
    BF_CHECK_CODE(decode_frame(frame.data(), frame.size() - 3, 0x11223344u, 7u, 1024, &decoded,
                               &consumed),
                  ErrorCode::TruncatedMessage);
    // Truncated header.
    BF_CHECK_CODE(decode_frame(frame.data(), 4, 0x11223344u, 7u, 1024, &decoded, &consumed),
                  ErrorCode::TruncatedMessage);
    // Corrupted payload byte.
    std::vector<u8> corrupted = frame;
    corrupted[kFrameHeaderBytes] ^= 0xFFu;
    BF_CHECK_CODE(decode_frame(corrupted.data(), corrupted.size(), 0x11223344u, 7u, 1024, &decoded,
                               &consumed),
                  ErrorCode::IntegrityFailure);
}

BF_TEST(codec, reader_rejects_oversized_lengths) {
    ByteWriter writer;
    writer.put_u32(0xFFFFFFFFu);
    ByteReader reader(writer.data());
    std::string text;
    BF_CHECK(!reader.get_str(&text, 64));
    BF_CHECK(reader.failed());
}

BF_TEST(codec, writer_refuses_oversized_strings) {
    ByteWriter writer;
    writer.put_str(std::string(kMaxEncodedStringBytes + 1, 'x'), kMaxEncodedStringBytes, "test");
    BF_CHECK(writer.failed());
}

BF_TEST(codec, boolean_encoding_is_strict) {
    ByteWriter writer;
    writer.put_u8(2);
    ByteReader reader(writer.data());
    bool value = false;
    BF_CHECK(!reader.get_bool(&value));
    BF_CHECK(reader.failed());
}

BF_TEST(policy, thresholds_must_be_ordered) {
    Thresholds thresholds;
    BF_CHECK(thresholds.validate().ok());
    thresholds.elevated_bp = 0;
    BF_CHECK_CODE(thresholds.validate(), ErrorCode::InvalidArgument);
    thresholds.elevated_bp = 9000;
    thresholds.high_bp = 8000;
    BF_CHECK_CODE(thresholds.validate(), ErrorCode::InvalidArgument);
    thresholds = Thresholds{};
    thresholds.critical_bp = 10001;
    BF_CHECK_CODE(thresholds.validate(), ErrorCode::InvalidArgument);
    thresholds = Thresholds{};
    thresholds.elevated_bp = thresholds.high_bp;
    BF_CHECK_CODE(thresholds.validate(), ErrorCode::InvalidArgument);
}

BF_TEST(policy, pressure_classification_is_exact_at_boundaries) {
    Thresholds thresholds;
    thresholds.elevated_bp = 7000;
    thresholds.high_bp = 8500;
    thresholds.critical_bp = 9500;
    const u64 usable = 10000;

    BF_CHECK(classify_pressure(thresholds, 6999, usable, false).state == PressureState::Clear);
    BF_CHECK(classify_pressure(thresholds, 7000, usable, false).state == PressureState::Elevated);
    BF_CHECK(classify_pressure(thresholds, 8499, usable, false).state == PressureState::Elevated);
    BF_CHECK(classify_pressure(thresholds, 8500, usable, false).state == PressureState::High);
    BF_CHECK(classify_pressure(thresholds, 9499, usable, false).state == PressureState::High);
    BF_CHECK(classify_pressure(thresholds, 9500, usable, false).state == PressureState::Critical);
    BF_CHECK(classify_pressure(thresholds, 10000, usable, false).state == PressureState::Critical);
    BF_CHECK(classify_pressure(thresholds, 12000, usable, true).state == PressureState::Critical);

    // Non-decimal usable capacity must not introduce rounding error.
    BF_CHECK(classify_pressure(thresholds, 7, 10, false).state == PressureState::Elevated);
    BF_CHECK(classify_pressure(thresholds, 6, 10, false).state == PressureState::Clear);
    BF_CHECK(classify_pressure(thresholds, 85, 100, false).state == PressureState::High);

    // Zero usable capacity with no usage is clear; any usage is critical.
    BF_CHECK(classify_pressure(thresholds, 0, 0, false).state == PressureState::Clear);
    BF_CHECK(classify_pressure(thresholds, 1, 0, false).state == PressureState::Critical);
}

BF_TEST(policy, pressure_classification_survives_huge_capacities) {
    Thresholds thresholds;
    const u64 usable = kMaxUnitsPerPool;
    const u64 just_below = (usable / 10000ull) * 6999ull;
    BF_CHECK(classify_pressure(thresholds, just_below, usable, false).state == PressureState::Clear);
    BF_CHECK(classify_pressure(thresholds, usable, usable, false).state == PressureState::Critical);
}

BF_TEST(policy, pool_policy_validation_rejects_contradictions) {
    PoolPolicy policy;
    policy.id = PolicyId::from_raw(1);
    policy.name = "p";
    BF_CHECK(policy.validate().ok());

    PoolPolicy bad = policy;
    bad.overcommit.mode = OvercommitMode::Bounded;
    bad.overcommit.limit_units = 0;
    BF_CHECK_CODE(bad.validate(), ErrorCode::InvalidArgument);

    bad = policy;
    bad.overcommit.mode = OvercommitMode::Forbid;
    bad.overcommit.limit_units = 10;
    BF_CHECK_CODE(bad.validate(), ErrorCode::InvalidArgument);

    bad = policy;
    bad.borrow.mode = BorrowMode::FromAncestorFree;
    bad.borrow.limit_units = 0;
    BF_CHECK_CODE(bad.validate(), ErrorCode::InvalidArgument);

    bad = policy;
    bad.soft_limit_bp = 20000;
    BF_CHECK_CODE(bad.validate(), ErrorCode::InvalidArgument);

    bad = policy;
    bad.max_single_request_units = kMaxUnitsPerPool + 1;
    BF_CHECK_CODE(bad.validate(), ErrorCode::Overflow);
}

BF_TEST(policy, pool_policy_digest_changes_with_content) {
    PoolPolicy policy;
    policy.id = PolicyId::from_raw(1);
    policy.name = "p";
    const Digest base = policy.digest();
    PoolPolicy same = policy;
    BF_CHECK_EQ(same.digest(), base);
    same.pressure.thresholds.high_bp = 8000;
    BF_CHECK(same.digest() != base);
}

BF_TEST(accounting, closure_identity_holds_for_constructed_states) {
    PoolAccounting accounting;
    accounting.pool = PoolId::from_raw(1);
    accounting.raw = 1000;
    accounting.protected_units = 100;
    accounting.usable = 900;
    accounting.allocated = 400;
    accounting.reserved = 100;
    accounting.committed = 300;
    accounting.pinned = 150;
    accounting.reclaimable = 250;
    accounting.free = 500;
    BF_CHECK(check_pool_accounting(accounting, OvercommitMode::Forbid, 0).ok());

    PoolAccounting broken = accounting;
    broken.free = 501;
    BF_CHECK_CODE(check_pool_accounting(broken, OvercommitMode::Forbid, 0).status(),
                  ErrorCode::AccountingViolation);

    broken = accounting;
    broken.usable = 899;
    BF_CHECK(!check_pool_accounting(broken, OvercommitMode::Forbid, 0).ok());

    broken = accounting;
    broken.allocated = 401;
    BF_CHECK(!check_pool_accounting(broken, OvercommitMode::Forbid, 0).ok());
}

BF_TEST(accounting, overcommit_closure_and_policy) {
    PoolAccounting accounting;
    accounting.pool = PoolId::from_raw(2);
    accounting.raw = 1000;
    accounting.protected_units = 100;
    accounting.usable = 900;
    accounting.allocated = 1000;
    accounting.reserved = 0;
    accounting.committed = 1000;
    accounting.pinned = 0;
    accounting.reclaimable = 1000;
    accounting.overcommit_used = 100;
    accounting.free = 0;
    BF_CHECK(check_pool_accounting(accounting, OvercommitMode::Bounded, 100).ok());

    // The same state without an explicit bounded overcommit policy is a fault.
    BF_CHECK(!check_pool_accounting(accounting, OvercommitMode::Forbid, 0).ok());
    // Overcommit beyond the declared limit is a fault.
    BF_CHECK(!check_pool_accounting(accounting, OvercommitMode::Bounded, 99).ok());

    // Free capacity and overcommit cannot coexist.
    PoolAccounting contradiction = accounting;
    contradiction.free = 1;
    BF_CHECK(!check_pool_accounting(contradiction, OvercommitMode::Bounded, 100).ok());
}

BF_TEST(accounting, global_borrow_balance) {
    PoolAccounting parent;
    parent.pool = PoolId::from_raw(1);
    parent.raw = 1000;
    parent.usable = 1000;
    parent.lent_out = 100;
    parent.free = 500;
    parent.allocated = 400;
    parent.committed = 400;
    parent.reclaimable = 400;

    PoolAccounting child;
    child.pool = PoolId::from_raw(2);
    child.raw = 100;
    child.usable = 100;
    child.borrowed_in = 100;
    child.allocated = 200;
    child.committed = 200;
    child.reclaimable = 200;

    BF_CHECK(check_global_accounting({parent, child}).ok());

    PoolAccounting unbalanced = child;
    unbalanced.borrowed_in = 90;
    unbalanced.allocated = 190;
    unbalanced.committed = 190;
    BF_CHECK(!check_global_accounting({parent, unbalanced}).ok());

    PoolAccounting unbalanced_lender = parent;
    unbalanced_lender.lent_out = 90;
    unbalanced_lender.free = 510;
    BF_CHECK(!check_global_accounting({unbalanced_lender, child}).ok());
}

BF_TEST(json, writer_escapes_and_bounds) {
    JsonWriter writer(64);
    writer.begin_object();
    writer.key("a\"b");
    writer.value_string("line\nbreak\\slash");
    writer.end_object();
    const std::string text = writer.str();
    BF_CHECK(text.find("\\\"") != std::string::npos);
    BF_CHECK(text.find("\\n") != std::string::npos);

    JsonWriter small(16);
    small.begin_object();
    for (int i = 0; i < 100; ++i) {
        small.key("key");
        small.value_u64(123456789);
    }
    small.end_object();
    BF_CHECK(small.truncated());
    BF_CHECK(small.str().size() <= 80);
}

BF_TEST(reclaim, request_digest_is_order_independent_of_unset_fields) {
    ReclaimRequest a;
    a.attempt = AttemptId::from_raw(1);
    a.pool = PoolId::from_raw(2);
    a.target_units = 10;
    ReclaimRequest b = a;
    BF_CHECK_EQ(a.request_digest(), b.request_digest());
    b.target_units = 11;
    BF_CHECK(a.request_digest() != b.request_digest());
}
