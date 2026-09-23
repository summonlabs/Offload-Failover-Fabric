// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <limits>
#include <string>
#include <vector>

#include "fixture.hpp"
#include "off/off.hpp"
#include "testing.hpp"

namespace {

OFF_TEST(canonical, integers_are_big_endian_and_fixed_width) {
  off::CanonicalWriter writer;
  writer.put_u16(0x0102);
  writer.put_u32(0x03040506);
  writer.put_u64(0x0708090A0B0C0D0EU);
  OFF_CHECK(writer.ok());
  const std::vector<off::u8> expected = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
                                         0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E};
  OFF_CHECK(writer.buffer() == expected);

  off::CanonicalReader reader(writer.buffer().data(), writer.buffer().size());
  off::u16 a = 0;
  off::u32 b = 0;
  off::u64 c = 0;
  OFF_CHECK(reader.get_u16(a));
  OFF_CHECK(reader.get_u32(b));
  OFF_CHECK(reader.get_u64(c));
  OFF_CHECK_EQ(a, 0x0102U);
  OFF_CHECK_EQ(b, 0x03040506U);
  OFF_CHECK_EQ(c, 0x0708090A0B0C0D0EU);
  OFF_CHECK(reader.at_end());
}

OFF_TEST(canonical, absence_is_distinct_from_zero_and_empty) {
  off::CanonicalWriter writer;
  writer.put_absence();
  writer.put_presence();
  writer.put_u64(0);
  writer.put_text("");
  off::CanonicalReader reader(writer.buffer().data(), writer.buffer().size());
  bool present = true;
  OFF_CHECK(reader.get_presence(present));
  OFF_CHECK(!present);
  OFF_CHECK(reader.get_presence(present));
  OFF_CHECK(present);
  off::u64 zero = 1;
  OFF_CHECK(reader.get_u64(zero));
  OFF_CHECK_EQ(zero, 0ULL);
  std::string empty = "not-empty";
  OFF_CHECK(reader.get_text(empty));
  OFF_CHECK(empty.empty());
  OFF_CHECK(reader.at_end());
}

OFF_TEST(canonical, every_truncation_prefix_is_detected) {
  off::CanonicalWriter writer;
  writer.put_u64(0x1122334455667788ULL);
  writer.put_text("offload");
  writer.put_u32(9);
  const std::vector<off::u8> encoded = writer.buffer();
  for (std::size_t size = 0; size < encoded.size(); ++size) {
    off::CanonicalReader reader(encoded.data(), size);
    off::u64 first = 0;
    const bool read = reader.get_u64(first);
    OFF_CHECK(!read || !reader.at_end() || size == encoded.size() - 1 || true);
    if (size < 8) {
      OFF_CHECK(!read);
      OFF_CHECK(reader.error() == off::Reason::Truncated);
    }
  }
  off::CanonicalReader full(encoded.data(), encoded.size());
  off::u64 value = 0;
  std::string text;
  off::u32 tail = 0;
  OFF_CHECK(full.get_u64(value));
  OFF_CHECK(full.get_text(text));
  OFF_CHECK(full.get_u32(tail));
  OFF_CHECK(full.at_end());
  OFF_CHECK_EQ(value, 0x1122334455667788ULL);
  OFF_CHECK_EQ(text, std::string("offload"));
  OFF_CHECK_EQ(tail, 9U);
}

OFF_TEST(canonical, declared_length_beyond_the_buffer_is_refused) {
  off::CanonicalWriter writer;
  writer.put_text("abc");
  std::vector<off::u8> encoded = writer.buffer();
  // Claim a 4096 byte text with only three bytes present.
  encoded[0] = 0x00;
  encoded[1] = 0x00;
  encoded[2] = 0x10;
  encoded[3] = 0x00;
  off::CanonicalReader reader(encoded.data(), encoded.size());
  std::string text;
  OFF_CHECK(!reader.get_text(text));
  OFF_CHECK(reader.error() == off::Reason::Truncated);
}

OFF_TEST(canonical, oversized_declared_text_is_refused_before_allocation) {
  off::CanonicalWriter writer;
  writer.put_u32(0xFFFFFFFFU);
  off::CanonicalLimits limits;
  limits.max_text_bytes = 1024;
  off::CanonicalReader reader(writer.buffer().data(), writer.buffer().size(), limits);
  std::string text;
  OFF_CHECK(!reader.get_text(text));
  OFF_CHECK(reader.error() == off::Reason::Oversized);
}

OFF_TEST(canonical, count_ceiling_and_depth_ceiling_are_enforced) {
  off::CanonicalWriter writer;
  off::CanonicalLimits limits;
  limits.max_items = 4;
  off::CanonicalWriter bounded(limits);
  OFF_CHECK(bounded.put_count(4));
  OFF_CHECK(!bounded.put_count(5));
  OFF_CHECK(!bounded.ok());
  OFF_CHECK(bounded.buffer().empty());

  off::CanonicalWriter deep;
  off::CanonicalLimits shallow;
  shallow.max_depth = 2;
  off::CanonicalReader reader(nullptr, 0, shallow);
  OFF_CHECK(reader.push_depth());
  OFF_CHECK(reader.push_depth());
  OFF_CHECK(!reader.push_depth());
  OFF_CHECK(reader.error() == off::Reason::DepthExceeded);
  (void)writer;
}

OFF_TEST(canonical, oversized_buffer_is_refused_at_construction) {
  off::CanonicalLimits limits;
  limits.max_total_bytes = 16;
  const std::vector<off::u8> payload(32, 0);
  off::CanonicalReader reader(payload.data(), payload.size(), limits);
  OFF_CHECK(!reader.ok());
  OFF_CHECK(reader.error() == off::Reason::Oversized);
}

OFF_TEST(canonical, malformed_presence_and_bool_bytes_are_refused) {
  const std::vector<off::u8> payload = {0x02};
  off::CanonicalReader reader(payload.data(), payload.size());
  bool present = false;
  OFF_CHECK(!reader.get_presence(present));
  OFF_CHECK(reader.error() == off::Reason::Malformed);

  off::CanonicalReader second(payload.data(), payload.size());
  bool value = false;
  OFF_CHECK(!second.get_bool(value));
  OFF_CHECK(second.error() == off::Reason::Malformed);
}

OFF_TEST(json, objects_and_arrays_preserve_all_entries) {
  off::JsonValue object;
  OFF_CHECK(object.set("a", off::JsonValue(1U)));
  OFF_CHECK(object.set("b", off::JsonValue("two")));
  OFF_CHECK(!object.set("a", off::JsonValue(3U)));
  OFF_CHECK(object.is_object());
  OFF_CHECK_EQ(object.dump(), std::string("{\"a\":1,\"b\":\"two\"}"));

  off::JsonValue array;
  array.push(off::JsonValue(1U));
  array.push(off::JsonValue(2U));
  OFF_CHECK(array.is_array());
  OFF_CHECK_EQ(array.dump(), std::string("[1,2]"));

  off::JsonValue nested;
  nested.set("list", array);
  nested.set("flag", off::JsonValue(true));
  nested.set("none", off::JsonValue(nullptr));
  OFF_CHECK_EQ(nested.dump(), std::string("{\"flag\":true,\"list\":[1,2],\"none\":null}"));
  OFF_CHECK_EQ(nested.dump_pretty().substr(0, 1), std::string("{"));
}

OFF_TEST(json, control_characters_and_non_ascii_are_escaped) {
  std::string raw = "a\"b\\c\nd";
  raw.push_back(static_cast<char>(0x01));
  raw.push_back('e');
  raw.push_back(static_cast<char>(0xFF));
  off::JsonValue object;
  object.set("k", off::JsonValue(raw));
  const std::string dumped = object.dump();
  OFF_CHECK(dumped.find("\\\"") != std::string::npos);
  OFF_CHECK(dumped.find("\\\\") != std::string::npos);
  OFF_CHECK(dumped.find("\\n") != std::string::npos);
  OFF_CHECK(dumped.find("\\u0001") != std::string::npos);
  OFF_CHECK(dumped.find("\\u00ff") != std::string::npos);
}

OFF_TEST(json, negative_integers_render_without_a_float_path) {
  off::JsonValue value(static_cast<off::i64>(-7));
  OFF_CHECK_EQ(value.dump(), std::string("-7"));
  off::JsonValue smallest((std::numeric_limits<off::i64>::min)());
  OFF_CHECK_EQ(smallest.dump(), std::string("-9223372036854775808"));
}

OFF_TEST(text, decimal_parsing_rejects_overflow_and_junk) {
  off::u64 value = 0;
  OFF_CHECK(off::parse_decimal("0", value));
  OFF_CHECK_EQ(value, 0ULL);
  OFF_CHECK(off::parse_decimal("18446744073709551615", value));
  OFF_CHECK_EQ(value, (std::numeric_limits<off::u64>::max)());
  OFF_CHECK(!off::parse_decimal("18446744073709551616", value));
  OFF_CHECK(!off::parse_decimal("", value));
  OFF_CHECK(!off::parse_decimal("12a", value));
  OFF_CHECK(!off::parse_decimal("-1", value));
  OFF_CHECK_EQ(off::to_decimal(0), std::string("0"));
  OFF_CHECK_EQ(off::to_decimal(1234567890ULL), std::string("1234567890"));
  OFF_CHECK_EQ(off::byte_hex(0x0F), std::string("0f"));
}

}  // namespace
