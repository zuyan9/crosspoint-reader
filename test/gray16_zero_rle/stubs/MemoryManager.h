#pragma once

#include <cstddef>
#include <functional>

namespace freeink {

struct CacheSink {
  const char* name = nullptr;
  int priority = 0;
  std::function<size_t(size_t)> evict;
};

class MemoryManager {
 public:
  static MemoryManager& instance() {
    static MemoryManager inst;
    return inst;
  }
  void registerSink(CacheSink) {}
  size_t freeBytes() const { return 256 * 1024; }
};

}  // namespace freeink
