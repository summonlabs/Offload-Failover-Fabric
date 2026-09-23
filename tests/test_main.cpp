// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <cstring>
#include <string>

#include "testing.hpp"

int main(int argc, char** argv) {
  std::string filter;
  for (int index = 1; index < argc; ++index) {
    if (std::strncmp(argv[index], "--filter=", 9) == 0) {
      filter = argv[index] + 9;
    } else {
      filter = argv[index];
    }
  }
  return offtest::run_all(filter) == 0 ? 0 : 1;
}
