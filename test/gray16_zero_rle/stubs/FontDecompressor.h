#pragma once

#include <EpdFontData.h>

#include <cstdint>

class FontDecompressor {
 public:
  const uint8_t* getBitmap(const EpdFontData*, const EpdGlyph*, uint32_t) { return nullptr; }
  void clearCache() {}
  int prewarmCache(const EpdFontData*, const char*) { return 0; }
  void logStats(const char*) {}
  void resetStats() {}
};
