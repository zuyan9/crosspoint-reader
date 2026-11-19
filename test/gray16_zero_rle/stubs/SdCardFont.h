#pragma once

#include <EpdFont.h>

#include <cstddef>
#include <cstdint>

class SdCardFont {
 public:
  using TextGetter = const char* (*)(const void* ctx, uint32_t index);

  explicit SdCardFont(const EpdFont* epd = nullptr) : epd_(epd) {}

  void clearCache() {}
  void beginPrewarmScope() {}
  void releaseResidentCaches() {}
  void noteScannedStyle(uint8_t) {}
  uint8_t scannedStyleMask() const { return 0; }
  void compactForRender(uint8_t, bool) {}
  bool areStylesRenderResident(uint8_t) const { return true; }
  uint32_t residentGeneration() const { return 1; }
  uint8_t resolveStyle(uint8_t style) const { return style & 0x03; }
  bool hasAdvanceTable() const { return false; }
  uint16_t getAdvance(uint32_t, uint8_t) const { return 0; }
  const EpdFont* getEpdFont(uint8_t = 0) const { return epd_; }
  int buildAdvanceTable(const char*, uint8_t, const char* = nullptr) { return 0; }
  int buildAdvanceTablePacked(const char* const*, const size_t*, size_t, bool, bool, uint8_t, const char* = nullptr,
                              bool = false) {
    return 0;
  }
  int prewarm(const char*, uint8_t, bool = false, bool = true, bool = true) {
    prewarmCalls++;
    return 0;
  }
  int prewarm(TextGetter, const void*, uint32_t, uint8_t, bool = false, bool = true, bool = true) {
    prewarmCalls++;
    return 0;
  }
  void logStats(const char*) {}
  void resetStats() {}

  int prewarmCalls = 0;

 private:
  const EpdFont* epd_ = nullptr;
};
