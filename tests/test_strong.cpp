// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <limits>
#include <string>

#include "fixture.hpp"
#include "off/off.hpp"
#include "testing.hpp"

namespace {

using ByteTag = off::Gen<off::ServiceTag>;

OFF_TEST(strong, checked_arithmetic_reports_overflow) {
  off::u64 out = 0;
  OFF_CHECK(!off::add_overflow(1ULL, 2ULL, out));
  OFF_CHECK_EQ(out, 3ULL);
  OFF_CHECK(off::add_overflow((std::numeric_limits<off::u64>::max)(), 1ULL, out));
  OFF_CHECK(!off::mul_overflow(6ULL, 7ULL, out));
  OFF_CHECK_EQ(out, 42ULL);
  OFF_CHECK(off::mul_overflow((std::numeric_limits<off::u64>::max)(), 2ULL, out));
  OFF_CHECK(!off::mul_overflow(0ULL, (std::numeric_limits<off::u64>::max)(), out));
  OFF_CHECK_EQ(out, 0ULL);

  off::u32 small = 0;
  OFF_CHECK(off::narrow(static_cast<off::u64>((std::numeric_limits<off::u32>::max)()) + 1ULL, small));
  OFF_CHECK(!off::narrow(42ULL, small));
  OFF_CHECK_EQ(small, 42U);
}

OFF_TEST(strong, generation_successor_is_checked) {
  const off::Gen<off::ServiceTag> initial = off::Gen<off::ServiceTag>::initial();
  OFF_CHECK(initial.is_initial());
  const std::optional<off::Gen<off::ServiceTag>> next = initial.successor();
  OFF_REQUIRE(next.has_value());
  OFF_CHECK_EQ(next->value(), 1ULL);
  const auto last = off::Gen<off::ServiceTag>::from_value((std::numeric_limits<off::u64>::max)());
  OFF_CHECK(!last.successor().has_value());
}

OFF_TEST(strong, name_parsing_rejects_non_canonical_input) {
  OFF_CHECK(!off::ServiceName::parse("").has_value());
  OFF_CHECK(!off::ServiceName::parse(std::string(off::kMaxNameLength + 1, 'a')).has_value());
  OFF_CHECK(!off::ServiceName::parse("bad name").has_value());
  OFF_CHECK(!off::ServiceName::parse("bad\tname").has_value());
  const std::optional<off::ServiceName> good = off::ServiceName::parse("svc-1.edge:a@b");
  OFF_REQUIRE(good.has_value());
  OFF_CHECK_EQ(good->view(), std::string_view("svc-1.edge:a@b"));
  OFF_CHECK(off::ServiceName::parse(std::string(off::kMaxNameLength, 'a')).has_value());
}

OFF_TEST(strong, canonical_name_parsing_permits_absence) {
  const std::optional<off::TargetName> absent = off::TargetName::parse_canonical("");
  OFF_REQUIRE(absent.has_value());
  OFF_CHECK(absent->empty());
  OFF_CHECK(!off::TargetName::parse_canonical("no/slash").has_value());
}

OFF_TEST(strong, identity_nil_is_never_a_real_identity) {
  const off::ServiceName empty;
  OFF_CHECK(empty.empty());
  const off::EvidenceId nil;
  OFF_CHECK(nil.is_nil());
  OFF_CHECK(!off::EvidenceId::from_value(7).is_nil());
}

OFF_TEST(reason, codes_have_stable_text_and_families) {
  OFF_CHECK_EQ(off::reason_code_text(off::Reason::Ok), std::string_view("OK"));
  OFF_CHECK_EQ(off::reason_code_text(off::Reason::FenceStale), std::string_view("FENCE_STALE"));
  OFF_CHECK(off::reason_is_accept(off::Reason::Ok));
  OFF_CHECK(off::reason_is_accept(off::Reason::AcceptIntentEmitted));
  OFF_CHECK(!off::reason_is_accept(off::Reason::FenceStale));
  OFF_CHECK_EQ(off::reason_family(off::Reason::FenceStale), std::string_view("fence"));
  OFF_CHECK_EQ(off::reason_family(off::Reason::AcceptIntentEmitted), std::string_view("success"));
  OFF_CHECK_EQ(off::reason_summary(off::Reason::FenceStale), std::string("fence stale"));
}

OFF_TEST(reason, every_code_round_trips_through_its_text) {
  int checked = 0;
  for (int raw = 0; raw <= 0x12FF; ++raw) {
    const auto code = static_cast<off::Reason>(raw);
    const std::string_view text = off::reason_code_text(code);
    if (text == "UNRECOGNIZED_REASON") {
      continue;
    }
    off::Reason parsed{};
    OFF_CHECK(off::reason_from_text(text, parsed));
    OFF_CHECK(parsed == code);
    OFF_CHECK(!off::reason_summary(code).empty());
    ++checked;
  }
  OFF_CHECK(checked > 100);
}

OFF_TEST(digest, hex_round_trips_and_detects_change) {
  off::Digest value;
  value.hi = 0x0123456789abcdefULL;
  value.lo = 0xfedcba9876543210ULL;
  const std::string text = value.to_hex();
  OFF_CHECK_EQ(text, std::string("0123456789abcdeffedcba9876543210"));
  off::Digest parsed;
  OFF_CHECK(off::Digest::from_hex(text, parsed));
  OFF_CHECK(parsed == value);
  OFF_CHECK(!off::Digest::from_hex("0123", parsed));
  OFF_CHECK(!off::Digest::from_hex("0123456789abcdeffedcba987654321g", parsed));

  off::DigestBuilder first;
  const std::string a = "alpha";
  first.update(reinterpret_cast<const off::u8*>(a.data()), a.size());
  off::DigestBuilder second;
  const std::string b = "alphb";
  second.update(reinterpret_cast<const off::u8*>(b.data()), b.size());
  OFF_CHECK(first.value() != second.value());
}

OFF_TEST(digest, crc32c_matches_published_vector) {
  const std::string text = "123456789";
  OFF_CHECK_EQ(off::crc32c(reinterpret_cast<const off::u8*>(text.data()), text.size()),
               0xE3069283U);
  off::Crc32c incremental;
  incremental.update(reinterpret_cast<const off::u8*>(text.data()), 4);
  incremental.update(reinterpret_cast<const off::u8*>(text.data()) + 4, text.size() - 4);
  OFF_CHECK_EQ(incremental.value(), 0xE3069283U);
}

}  // namespace
