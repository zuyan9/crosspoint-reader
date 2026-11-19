#include <HalDisplay.h>
#include <HalGPIO.h>

// Global HalDisplay instance
HalDisplay display;

#define SD_SPI_MISO 7

HalDisplay::HalDisplay() : einkDisplay(EPD_SCLK, EPD_MOSI, EPD_CS, EPD_DC, EPD_RST, EPD_BUSY) {}

HalDisplay::~HalDisplay() {}

HalDisplay::Controller HalDisplay::getController() const { return BoardConfig::ACTIVE.displayController; }

void HalDisplay::begin(bool seamless) {
  // Set X3-specific panel mode before initializing.
  if (gpio.deviceIsX3()) {
    einkDisplay.setDisplayX3();
  }

  einkDisplay.begin();

  if (seamless) {
    // Defuse the SDK's X3 _x3InitialFullSyncsRemaining counter (no-op on X4)
    // so the first paint isn't promoted to FULL (~770ms). Skips the wakeup-
    // gated requestResync() below for the same reason.
    einkDisplay.skipInitialResync();
    return;
  }
  // Request resync after specific wakeup events to ensure clean display state.
  const auto wakeupReason = gpio.getWakeupReason();
  if (wakeupReason == HalGPIO::WakeupReason::PowerButton || wakeupReason == HalGPIO::WakeupReason::AfterFlash ||
      wakeupReason == HalGPIO::WakeupReason::Other) {
    einkDisplay.requestResync();
  }
}

void HalDisplay::clearScreen(uint8_t color) const { einkDisplay.clearScreen(color); }

void HalDisplay::drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           bool fromProgmem) const {
  einkDisplay.drawImage(imageData, x, y, w, h, fromProgmem);
}

EInkDisplay::RefreshMode convertRefreshMode(HalDisplay::RefreshMode mode) {
  switch (mode) {
    case HalDisplay::FULL_REFRESH:
      return EInkDisplay::FULL_REFRESH;
    case HalDisplay::HALF_REFRESH:
      return EInkDisplay::HALF_REFRESH;
    case HalDisplay::FAST_REFRESH:
    default:
      return EInkDisplay::FAST_REFRESH;
  }
}

void HalDisplay::displayBuffer(HalDisplay::RefreshMode mode, bool turnOffScreen) {
  if (gpio.deviceIsX3() && mode == RefreshMode::HALF_REFRESH) {
    einkDisplay.requestResync(1);
  }

  einkDisplay.displayBuffer(convertRefreshMode(mode), turnOffScreen);
}

void HalDisplay::displayWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool turnOffScreen) {
  einkDisplay.displayWindow(x, y, w, h, turnOffScreen);
}

void HalDisplay::displayBufferAsync(HalDisplay::RefreshMode mode) {
  if (gpio.deviceIsX3() && mode == RefreshMode::HALF_REFRESH) {
    einkDisplay.requestResync(1);
  }

  einkDisplay.displayBufferAsyncNoShadow(convertRefreshMode(mode));
}

void HalDisplay::waitRefreshComplete() { einkDisplay.waitRefreshComplete(); }

bool HalDisplay::supportsAsyncRefresh() const { return einkDisplay.supportsAsyncRefresh(); }

HalDisplay::GrayscaleCapabilities HalDisplay::grayscaleCapabilities(GrayscaleMode mode) const {
  return einkDisplay.grayscaleCapabilities(mode);
}

void HalDisplay::setInverted(bool inverted) { einkDisplay.setInverted(inverted); }

bool HalDisplay::isInverted() const { return einkDisplay.isInverted(); }

#if CROSSPOINT_SUPPORT_GRAY16
bool HalDisplay::prewarmPowerOn() { return einkDisplay.prewarmPowerOn(); }

bool HalDisplay::serviceIdlePower() { return einkDisplay.serviceIdlePower(); }
#endif

void HalDisplay::powerOffBetweenFrames() { einkDisplay.powerOffBetweenFrames(); }

void HalDisplay::deepSleep() { einkDisplay.deepSleep(); }

void HalDisplay::controllerIdle() { einkDisplay.controllerIdle(); }

uint8_t* HalDisplay::getFrameBuffer() const { return einkDisplay.getFrameBuffer(); }

uint8_t* HalDisplay::lendFrameBufferStorage(uint32_t* sizeOut) { return einkDisplay.lendBuildStorage(sizeOut); }

void HalDisplay::returnFrameBufferStorage() { einkDisplay.returnBuildStorage(); }

bool HalDisplay::displayGrayscaleBase(GrayscaleMode mode, RefreshMode fallback, bool turnOffScreen) {
  if (gpio.deviceIsX3() && (fallback == FULL_REFRESH || fallback == HALF_REFRESH)) {
    einkDisplay.requestResync(1);
  }
  return einkDisplay.displayGrayscaleBase(mode, convertRefreshMode(fallback), turnOffScreen);
}

void HalDisplay::displayGrayscaleBase(RefreshMode fallback, bool turnOffScreen) {
  if (gpio.deviceIsX3() && (fallback == RefreshMode::FULL_REFRESH || fallback == RefreshMode::HALF_REFRESH)) {
    einkDisplay.requestResync(1);
  }

  einkDisplay.displayGrayscaleBase(convertRefreshMode(fallback), turnOffScreen);
}

bool HalDisplay::supportsDeferredGrayscaleBase() const { return einkDisplay.supportsDeferredGrayscaleBase(); }

bool HalDisplay::displayGrayscaleBaseAsync(RefreshMode fallback) {
  if (gpio.deviceIsX3() && (fallback == RefreshMode::FULL_REFRESH || fallback == RefreshMode::HALF_REFRESH)) {
    einkDisplay.requestResync(1);
  }
  return einkDisplay.displayGrayscaleBaseAsync(convertRefreshMode(fallback));
}

void HalDisplay::preconditionGrayscale() { einkDisplay.preconditionGrayscale(); }

void HalDisplay::preconditionGrayscale(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
  einkDisplay.preconditionGrayscale(x, y, w, h);
}

void HalDisplay::copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer) { einkDisplay.copyGrayscaleLsbBuffers(lsbBuffer); }

void HalDisplay::copyGrayscaleMsbBuffers(const uint8_t* msbBuffer) { einkDisplay.copyGrayscaleMsbBuffers(msbBuffer); }

void HalDisplay::cleanupGrayscaleBuffers(const uint8_t* bwBuffer) { einkDisplay.cleanupGrayscaleBuffers(bwBuffer); }

void HalDisplay::displayGrayBuffer(bool turnOffScreen) { einkDisplay.displayGrayBuffer(turnOffScreen); }

#if CROSSPOINT_SUPPORT_GRAY16
bool HalDisplay::supportsGray16Planes() const { return einkDisplay.supportsGray16Planes(); }

bool HalDisplay::supportsDeferredGray16Base() const { return einkDisplay.supportsDeferredGray16Base(); }

void HalDisplay::displayGray16Base(RefreshMode fallback, bool turnOffScreen) {
  // X3 with Gray16 runs a single-DRF DC-balanced bipolar GC scrub natively on
  // HALF_REFRESH; only FULL_REFRESH (or non-Gray16 / inverted modes) needs the
  // multi-pass requestResync(1) clear.
  if (gpio.deviceIsX3() && (fallback == RefreshMode::FULL_REFRESH ||
                            (fallback == RefreshMode::HALF_REFRESH && !einkDisplay.supportsGray16Planes()))) {
    einkDisplay.requestResync(1);
  }
  einkDisplay.displayGray16Base(convertRefreshMode(fallback), turnOffScreen);
}

bool HalDisplay::displayGray16BaseAsync(RefreshMode fallback) {
  if (gpio.deviceIsX3() && (fallback == RefreshMode::FULL_REFRESH ||
                            (fallback == RefreshMode::HALF_REFRESH && !einkDisplay.supportsGray16Planes()))) {
    einkDisplay.requestResync(1);
  }
  return einkDisplay.displayGray16BaseAsync(convertRefreshMode(fallback));
}

void HalDisplay::displayGray16Pass1() { einkDisplay.displayGray16Pass1(); }

bool HalDisplay::displayGray16Pass1Async() { return einkDisplay.displayGray16Pass1Async(); }

void HalDisplay::displayGray16Pass2(bool turnOffScreen) { einkDisplay.displayGray16Pass2(turnOffScreen); }

bool HalDisplay::displayGray16Pass2Async(bool turnOffScreen) {
  return einkDisplay.displayGray16Pass2Async(turnOffScreen);
}

void HalDisplay::beginGray16Cleanup(const uint8_t* bwBuffer) { einkDisplay.beginGray16Cleanup(bwBuffer); }

void HalDisplay::finishGray16Cleanup() { einkDisplay.finishGray16Cleanup(); }
#endif

bool HalDisplay::displayCommitted() const { return einkDisplay.displayCommitted(); }

void HalDisplay::writeGrayscalePlaneStrip(bool lsbPlane, const uint8_t* rows, uint16_t yStart, uint16_t numRows) {
  einkDisplay.writeGrayscalePlaneStrip(lsbPlane ? EInkDisplay::GRAY_PLANE_LSB : EInkDisplay::GRAY_PLANE_MSB, rows,
                                       yStart, numRows);
}

bool HalDisplay::writeGrayscalePlaneGateStream(bool lsbPlane, void* ctx,
                                               size_t (*nextChunk)(void* ctx, const uint8_t** outChunk)) {
  return einkDisplay.writeGrayscalePlaneGateStream(lsbPlane ? EInkDisplay::GRAY_PLANE_LSB : EInkDisplay::GRAY_PLANE_MSB,
                                                   ctx, nextChunk);
}

uint16_t HalDisplay::getDisplayWidth() const { return einkDisplay.getDisplayWidth(); }

uint16_t HalDisplay::getDisplayHeight() const { return einkDisplay.getDisplayHeight(); }

uint16_t HalDisplay::getDisplayWidthBytes() const { return einkDisplay.getDisplayWidthBytes(); }

uint32_t HalDisplay::getBufferSize() const { return einkDisplay.getBufferSize(); }
