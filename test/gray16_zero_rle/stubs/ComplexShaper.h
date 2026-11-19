#pragma once

#include <cstddef>

class ComplexShaper {
 public:
  static void beginMemo() {}
  static void endMemo() {}
  static size_t releaseAll() { return 0; }
  static bool containsComplexScript(const char*) { return false; }
};
