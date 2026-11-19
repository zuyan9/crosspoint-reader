#pragma once

#include <GrayscaleCapabilities.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#ifndef CROSSPOINT_SUPPORT_GRAY16
#if !defined(FREEINK_DRIVER_UC8253_X3) || FREEINK_DRIVER_UC8253_X3 != 0
#define CROSSPOINT_SUPPORT_GRAY16 1
#else
#define CROSSPOINT_SUPPORT_GRAY16 0
#endif
#endif

struct GfxTestEsp {
  uint32_t getFreeHeap() const { return 256 * 1024; }
  uint32_t getMaxAllocHeap() const { return 256 * 1024; }
  void restart() {}
};
inline GfxTestEsp ESP;
inline unsigned long millis() { return 0; }

class HalDisplay {
 public:
  enum RefreshMode { FULL_REFRESH, HALF_REFRESH, FAST_REFRESH };
  using GrayscaleMode = freeink::GrayscaleMode;
  using GrayscaleCapabilities = freeink::GrayscaleCapabilities;
  using GrayscaleBase = freeink::GrayscaleBase;
  using GrayscaleEncoding = freeink::GrayscaleEncoding;

  static constexpr uint16_t DISPLAY_WIDTH = 792;
  static constexpr uint16_t DISPLAY_HEIGHT = 528;
  static constexpr uint16_t DISPLAY_WIDTH_BYTES = DISPLAY_WIDTH / 8;
  static constexpr uint32_t BUFFER_SIZE = DISPLAY_WIDTH_BYTES * DISPLAY_HEIGHT;

  explicit HalDisplay(uint16_t w = DISPLAY_WIDTH, uint16_t h = DISPLAY_HEIGHT)
      : width_(w), height_(h), widthBytes_(w / 8), buffer_(static_cast<size_t>(widthBytes_) * height_, 0xFFu) {}

  void clearScreen(uint8_t color = 0xFF) const { std::fill(buffer_.begin(), buffer_.end(), color); }
  void drawImage(const uint8_t*, uint16_t, uint16_t, uint16_t, uint16_t, bool = false) const {}

  void displayBuffer(RefreshMode mode = FAST_REFRESH, bool turnOffScreen = false) {
    lastRefreshMode = mode;
    lastTurnOffScreen = turnOffScreen;
    displayBufferCalls++;
  }
  void displayWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool = false) {
    lastWindowX = x;
    lastWindowY = y;
    lastWindowW = w;
    lastWindowH = h;
    displayWindowCalls++;
  }
  void displayBufferAsync(RefreshMode mode = FAST_REFRESH) {
    lastRefreshMode = mode;
    displayBufferAsyncCalls++;
  }
  void waitRefreshComplete() {}
  bool supportsAsyncRefresh() const { return asyncRefreshSupported; }
  GrayscaleCapabilities grayscaleCapabilities(GrayscaleMode mode = GrayscaleMode::Overlay) const {
    if (mode == GrayscaleMode::Absolute) {
      return {GrayscaleEncoding::Unsupported, GrayscaleBase::Separate, false, false, false};
    }
    return {GrayscaleEncoding::OverlayMasks, GrayscaleBase::Separate, true, false, false};
  }
  bool displayRetainedGrayscaleOverlay(uint16_t, uint16_t, uint16_t, uint16_t, bool) { return false; }
  bool isInverted() const { return inverted; }
  bool toggleInverted() {
    inverted = !inverted;
    return inverted;
  }

#if CROSSPOINT_SUPPORT_GRAY16
  bool prewarmPowerOn() {
    prewarmPowerOnCalls++;
    return true;
  }
  bool serviceIdlePower() { return false; }
#else
  bool prewarmPowerOn() { return false; }
  bool serviceIdlePower() { return false; }
#endif

  uint8_t* getFrameBuffer() const { return lent_ ? nullptr : buffer_.data(); }
  uint8_t* lendFrameBufferStorage(uint32_t* sizeOut) {
    if (lent_) return nullptr;
    lent_ = true;
    if (sizeOut) *sizeOut = static_cast<uint32_t>(buffer_.size());
    return buffer_.data();
  }
  void returnFrameBufferStorage() {
    lent_ = false;
    std::fill(buffer_.begin(), buffer_.end(), 0xFFu);
  }

  void preconditionGrayscale() {}
  void preconditionGrayscale(uint16_t, uint16_t, uint16_t, uint16_t) {}
  void displayGrayscaleBase(RefreshMode fallback = HALF_REFRESH, bool = false) {
    lastRefreshMode = fallback;
    displayGrayscaleBaseCalls++;
  }
  bool displayGrayscaleBase(GrayscaleMode, RefreshMode fallback = HALF_REFRESH, bool = false) {
    lastRefreshMode = fallback;
    displayGrayscaleBaseCalls++;
    return true;
  }
  bool supportsDeferredGrayscaleBase() const { return deferredGrayBaseSupported; }
  bool displayGrayscaleBaseAsync(RefreshMode fallback = FAST_REFRESH) {
    lastRefreshMode = fallback;
    displayGrayscaleBaseAsyncCalls++;
    return true;
  }
  void copyGrayscaleLsbBuffers(const uint8_t*) {}
  void copyGrayscaleMsbBuffers(const uint8_t*) {}
  void cleanupGrayscaleBuffers(const uint8_t*) {}
  void displayGrayBuffer(bool = false) {}

#if CROSSPOINT_SUPPORT_GRAY16
  bool supportsGray16Planes() const { return gray16Supported && !inverted; }
  bool supportsDeferredGray16Base() const { return supportsGray16Planes(); }
  void displayGray16Base(RefreshMode fallback = FAST_REFRESH, bool = false) {
    lastRefreshMode = fallback;
    displayGray16BaseCalls++;
  }
  bool displayGray16BaseAsync(RefreshMode fallback = FAST_REFRESH) {
    lastRefreshMode = fallback;
    displayGray16BaseCalls++;
    return supportsGray16Planes();
  }
  void displayGray16Pass1() {}
  bool displayGray16Pass1Async() { return supportsGray16Planes(); }
  void displayGray16Pass2(bool = false) {}
  bool displayGray16Pass2Async(bool = false) { return supportsGray16Planes(); }
  void beginGray16Cleanup(const uint8_t*) { beginGray16CleanupCalls++; }
  void finishGray16Cleanup() { finishGray16CleanupCalls++; }
#else
  bool supportsGray16Planes() const { return false; }
  bool supportsDeferredGray16Base() const { return false; }
  void displayGray16Base(RefreshMode = FAST_REFRESH, bool = false) {}
  bool displayGray16BaseAsync(RefreshMode = FAST_REFRESH) { return false; }
  void displayGray16Pass1() {}
  bool displayGray16Pass1Async() { return false; }
  void displayGray16Pass2(bool = false) {}
  bool displayGray16Pass2Async(bool = false) { return false; }
  void beginGray16Cleanup(const uint8_t*) {}
  void finishGray16Cleanup() {}
#endif
  bool displayCommitted() const { return committed; }

  void writeGrayscalePlaneStrip(bool, const uint8_t*, uint16_t, uint16_t) {}
  bool writeGrayscalePlaneGateStream(bool, void* ctx, size_t (*nextChunk)(void*, const uint8_t**)) {
    const uint8_t* chunk = nullptr;
    while (nextChunk(ctx, &chunk) > 0) {
    }
    return true;
  }

  uint16_t getDisplayWidth() const { return width_; }
  uint16_t getDisplayHeight() const { return height_; }
  uint16_t getDisplayWidthBytes() const { return widthBytes_; }
  uint32_t getBufferSize() const { return static_cast<uint32_t>(buffer_.size()); }

  bool gray16Supported = true;
  bool asyncRefreshSupported = false;
  bool deferredGrayBaseSupported = true;
  bool inverted = false;
  bool committed = true;
  RefreshMode lastRefreshMode = FAST_REFRESH;
  bool lastTurnOffScreen = false;
  int displayBufferCalls = 0;
  int displayBufferAsyncCalls = 0;
  int displayWindowCalls = 0;
  int displayGrayscaleBaseCalls = 0;
  int displayGrayscaleBaseAsyncCalls = 0;
  int displayGray16BaseCalls = 0;
  int beginGray16CleanupCalls = 0;
  int finishGray16CleanupCalls = 0;
  int prewarmPowerOnCalls = 0;
  uint16_t lastWindowX = 0;
  uint16_t lastWindowY = 0;
  uint16_t lastWindowW = 0;
  uint16_t lastWindowH = 0;

 private:
  uint16_t width_;
  uint16_t height_;
  uint16_t widthBytes_;
  mutable std::vector<uint8_t> buffer_;
  bool lent_ = false;
};
