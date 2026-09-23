// Offload Failover Fabric - minimal in-repository test harness.
// No third-party dependency, deterministic ordering, no timeouts.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace offtest {

struct Failure {
  std::string file;
  int line{0};
  std::string message;
};

struct Case {
  std::string suite;
  std::string name;
  std::function<void()> body;
};

[[nodiscard]] std::vector<Case>& registry();
void record_failure(const char* file, int line, const std::string& message);
void clear_failure();
[[nodiscard]] const Failure* current_failure();

struct Registrar {
  Registrar(const char* suite, const char* name, std::function<void()> body);
};

/// Runs every registered case whose "suite.name" contains the filter (an empty
/// filter runs everything) in registration order. Returns the number of
/// failing cases.
int run_all(const std::string& filter);

}  // namespace offtest

#define OFF_TEST(suite, name)                                                     \
  static void suite##_##name##_body();                                            \
  static const ::offtest::Registrar suite##_##name##_registrar(                   \
      #suite, #name, suite##_##name##_body);                                      \
  static void suite##_##name##_body()

#define OFF_CHECK(condition)                                                      \
  do {                                                                            \
    if (!(condition)) {                                                           \
      ::offtest::record_failure(__FILE__, __LINE__, "check failed: " #condition); \
    }                                                                             \
  } while (0)

#define OFF_CHECK_EQ(lhs, rhs)                                                            \
  do {                                                                                    \
    if (!((lhs) == (rhs))) {                                                              \
      ::offtest::record_failure(__FILE__, __LINE__, "equality failed: " #lhs " == " #rhs); \
    }                                                                                     \
  } while (0)

#define OFF_CHECK_NE(lhs, rhs)                                                            \
  do {                                                                                    \
    if (!((lhs) != (rhs))) {                                                              \
      ::offtest::record_failure(__FILE__, __LINE__, "inequality failed: " #lhs " != " #rhs); \
    }                                                                                     \
  } while (0)

/// Checks a reason code result and reports the produced code on mismatch.
#define OFF_CHECK_REASON(expression, expected)                                            \
  do {                                                                                    \
    const ::off::Reason off_actual_ = (expression);                                       \
    if (off_actual_ != (expected)) {                                                      \
      ::offtest::record_failure(__FILE__, __LINE__,                                       \
                                std::string("reason mismatch: " #expression " produced ") + \
                                    std::string(::off::reason_code_text(off_actual_)) +   \
                                    " expected " #expected);                              \
    }                                                                                     \
  } while (0)

/// Requires a value-producing expression to be present.
#define OFF_REQUIRE(condition)                                                    \
  do {                                                                            \
    if (!(condition)) {                                                           \
      ::offtest::record_failure(__FILE__, __LINE__,                               \
                                "requirement failed: " #condition);               \
      return;                                                                     \
    }                                                                             \
  } while (0)
