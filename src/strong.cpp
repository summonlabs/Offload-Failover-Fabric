// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "off/strong.hpp"

namespace off {

bool is_valid_name_char(char c) noexcept {
  const unsigned char value = static_cast<unsigned char>(c);
  if (value >= 'a' && value <= 'z') {
    return true;
  }
  if (value >= 'A' && value <= 'Z') {
    return true;
  }
  if (value >= '0' && value <= '9') {
    return true;
  }
  switch (c) {
    case '.':
    case '_':
    case '-':
    case ':':
    case '@':
      return true;
    default:
      return false;
  }
}

}  // namespace off
