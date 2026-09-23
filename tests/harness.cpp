// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "testing.hpp"

#include <cstdio>
#include <string>

namespace offtest {

std::vector<Case>& registry() {
  static std::vector<Case> cases;
  return cases;
}

namespace {
Failure g_failure{};
bool g_has_failure = false;
}  // namespace

void record_failure(const char* file, int line, const std::string& message) {
  if (g_has_failure) {
    return;
  }
  g_has_failure = true;
  g_failure.file = file;
  g_failure.line = line;
  g_failure.message = message;
}

void clear_failure() {
  g_has_failure = false;
  g_failure = Failure{};
}

const Failure* current_failure() { return g_has_failure ? &g_failure : nullptr; }

Registrar::Registrar(const char* suite, const char* name, std::function<void()> body) {
  registry().push_back(Case{suite, name, std::move(body)});
}

int run_all(const std::string& filter) {
  int failures = 0;
  int executed = 0;
  for (const Case& test : registry()) {
    const std::string full = test.suite + "." + test.name;
    if (!filter.empty() && full.find(filter) == std::string::npos) {
      continue;
    }
    ++executed;
    clear_failure();
    std::printf("[run ] %s\n", full.c_str());
    std::fflush(stdout);
    test.body();
    const Failure* failure = current_failure();
    if (failure == nullptr) {
      std::printf("[ ok ] %s\n", full.c_str());
      std::fflush(stdout);
      continue;
    }
    ++failures;
    std::printf("[FAIL] %s\n        %s:%d: %s\n", full.c_str(), failure->file.c_str(),
                failure->line, failure->message.c_str());
    std::fflush(stdout);
  }
  std::printf("\n%d case(s) executed, %d failure(s)\n", executed, failures);
  std::fflush(stdout);
  return failures;
}

}  // namespace offtest
