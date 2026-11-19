#pragma once

#include <cstddef>
#include <cstdint>

class HalFile {
 public:
  int read() { return -1; }
  int read(void*, size_t) { return 0; }
  int available() const { return 0; }
  size_t position() const { return 0; }
  bool seek(size_t) { return false; }
  bool seekSet(size_t) { return false; }
  bool seekCur(ptrdiff_t) { return false; }
  explicit operator bool() const { return false; }
};
