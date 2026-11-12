#include <HalDisplay.h>
#include <HalGPIO.h>

#if defined(CROSSPOINT_USB_DEV) && CROSSPOINT_USB_DEV
#include <cstring>
#if FREEINK_DRIVER_UC8253_X3
#include <Uc8253X3Config.h>
#endif
#endif

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

void HalDisplay::drawImageTransparent(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                      bool fromProgmem) const {
  einkDisplay.drawImageTransparent(imageData, x, y, w, h, fromProgmem);
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

bool HalDisplay::supportsAsyncGrayscaleBase() const { return grayscaleCapabilities().asyncBase; }

void HalDisplay::refreshDisplay(HalDisplay::RefreshMode mode, bool turnOffScreen) {
  if (gpio.deviceIsX3() && mode == RefreshMode::HALF_REFRESH) {
    einkDisplay.requestResync(1);
  }

  einkDisplay.refreshDisplay(convertRefreshMode(mode), turnOffScreen);
}

void HalDisplay::setInverted(bool inverted) { einkDisplay.setInverted(inverted); }

bool HalDisplay::toggleInverted() { return einkDisplay.toggleInverted(); }

bool HalDisplay::isInverted() const { return einkDisplay.isInverted(); }

void HalDisplay::powerOffBetweenFrames() { einkDisplay.powerOffBetweenFrames(); }

void HalDisplay::deepSleep() { einkDisplay.deepSleep(); }

uint8_t* HalDisplay::getFrameBuffer() const { return einkDisplay.getFrameBuffer(); }

uint8_t* HalDisplay::lendFrameBufferStorage(uint32_t* sizeOut) { return einkDisplay.lendBuildStorage(sizeOut); }

void HalDisplay::returnFrameBufferStorage() { einkDisplay.returnBuildStorage(); }

bool HalDisplay::displayGrayscaleBase(GrayscaleMode mode, RefreshMode fallback, bool turnOffScreen) {
  if (gpio.deviceIsX3() && fallback == HALF_REFRESH) einkDisplay.requestResync();
  return einkDisplay.displayGrayscaleBase(mode, static_cast<EInkDisplay::RefreshMode>(fallback), turnOffScreen);
}

void HalDisplay::copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t* msbBuffer) {
  einkDisplay.copyGrayscaleBuffers(lsbBuffer, msbBuffer);
}

void HalDisplay::displayGrayscaleBase(RefreshMode fallback, bool turnOffScreen) {
  // X3: a HALF fallback means the caller wants a clean base (e.g. the sleep
  // cover, a full-screen swap from arbitrary prior content). Without this, the
  // X3 grayscale base takes its gentle differential happy path and the prior
  // home/reader frame ghosts through the soft aa_pre_bw_mid waveform. Forcing a
  // resync makes displayGrayscaleBase clear first, matching displayBuffer(HALF).
  // The reader's FAST path is deliberately left on the differential path so
  // per-page grayscale stays cheap.
  if (gpio.deviceIsX3() && fallback == RefreshMode::HALF_REFRESH) {
    einkDisplay.requestResync(1);
  }

  einkDisplay.displayGrayscaleBase(convertRefreshMode(fallback), turnOffScreen);
}

void HalDisplay::preconditionGrayscale() { einkDisplay.preconditionGrayscale(); }

void HalDisplay::preconditionGrayscale(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
  einkDisplay.preconditionGrayscale(x, y, w, h);
}

void HalDisplay::copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer) { einkDisplay.copyGrayscaleLsbBuffers(lsbBuffer); }

void HalDisplay::copyGrayscaleMsbBuffers(const uint8_t* msbBuffer) { einkDisplay.copyGrayscaleMsbBuffers(msbBuffer); }

void HalDisplay::cleanupGrayscaleBuffers(const uint8_t* bwBuffer) { einkDisplay.cleanupGrayscaleBuffers(bwBuffer); }

void HalDisplay::displayGrayBuffer(bool turnOffScreen) { einkDisplay.displayGrayBuffer(turnOffScreen); }

bool HalDisplay::supportsGray16() const { return einkDisplay.supportsGray16(); }

void HalDisplay::displayGray16Pass1() { einkDisplay.displayGray16Pass1(); }

void HalDisplay::displayGray16Pass2(bool turnOffScreen) { einkDisplay.displayGray16Pass2(turnOffScreen); }

void HalDisplay::beginGray16Cleanup(const uint8_t* bwBuffer) { einkDisplay.beginGray16Cleanup(bwBuffer); }

void HalDisplay::finishGray16Cleanup() { einkDisplay.finishGray16Cleanup(); }

void HalDisplay::writeGrayscalePlaneStrip(bool lsbPlane, const uint8_t* rows, uint16_t yStart, uint16_t numRows) {
  einkDisplay.writeGrayscalePlaneStrip(lsbPlane ? EInkDisplay::GRAY_PLANE_LSB : EInkDisplay::GRAY_PLANE_MSB, rows,
                                       yStart, numRows);
}

bool HalDisplay::supportsStripGrayscale() const { return grayscaleCapabilities().stripUploads; }

bool HalDisplay::combinesGrayscaleBase() const { return grayscaleCapabilities().base == GrayscaleBase::Combined; }

uint16_t HalDisplay::getDisplayWidth() const { return einkDisplay.getDisplayWidth(); }

uint16_t HalDisplay::getDisplayHeight() const { return einkDisplay.getDisplayHeight(); }

uint16_t HalDisplay::getDisplayWidthBytes() const { return einkDisplay.getDisplayWidthBytes(); }

uint32_t HalDisplay::getBufferSize() const { return einkDisplay.getBufferSize(); }

#if defined(CROSSPOINT_USB_DEV) && CROSSPOINT_USB_DEV
#if FREEINK_DRIVER_UC8253_X3
namespace {

struct DevUc8253LutStore {
  uint8_t gcRam[HalDisplay::DEV_LUT_ROLES][HalDisplay::DEV_LUT_LEN];
  uint8_t preBwMidRam[HalDisplay::DEV_LUT_ROLES][HalDisplay::DEV_LUT_LEN];
  uint8_t directGrayRam[HalDisplay::DEV_LUT_ROLES][HalDisplay::DEV_LUT_LEN];
  uint8_t lut16PreRam[HalDisplay::DEV_LUT_ROLES][HalDisplay::DEV_LUT_LEN];
  uint8_t lut16Pass1Ram[HalDisplay::DEV_LUT_ROLES][HalDisplay::DEV_LUT_LEN];
  uint8_t lut16Pass2Ram[HalDisplay::DEV_LUT_ROLES][HalDisplay::DEV_LUT_LEN];
  freeink::Uc8253LutBank directGrayBank;
  freeink::Uc8253X3Config cfg;
  bool initialized = false;
};

DevUc8253LutStore devLutStore;

void copyBankToRam(const freeink::Uc8253LutBank& src, uint8_t dst[HalDisplay::DEV_LUT_ROLES][HalDisplay::DEV_LUT_LEN]) {
  memcpy(dst[0], src.vcom, HalDisplay::DEV_LUT_LEN);
  memcpy(dst[1], src.ww, HalDisplay::DEV_LUT_LEN);
  memcpy(dst[2], src.bw, HalDisplay::DEV_LUT_LEN);
  memcpy(dst[3], src.wb, HalDisplay::DEV_LUT_LEN);
  memcpy(dst[4], src.bb, HalDisplay::DEV_LUT_LEN);
}

void bindRamToBank(uint8_t ram[HalDisplay::DEV_LUT_ROLES][HalDisplay::DEV_LUT_LEN], freeink::Uc8253LutBank& dst) {
  dst = {ram[0], ram[1], ram[2], ram[3], ram[4]};
}

bool bankMatchesRam(const freeink::Uc8253LutBank& ref,
                    const uint8_t ram[HalDisplay::DEV_LUT_ROLES][HalDisplay::DEV_LUT_LEN]) {
  return memcmp(ram[0], ref.vcom, HalDisplay::DEV_LUT_LEN) == 0 &&
         memcmp(ram[1], ref.ww, HalDisplay::DEV_LUT_LEN) == 0 && memcmp(ram[2], ref.bw, HalDisplay::DEV_LUT_LEN) == 0 &&
         memcmp(ram[3], ref.wb, HalDisplay::DEV_LUT_LEN) == 0 && memcmp(ram[4], ref.bb, HalDisplay::DEV_LUT_LEN) == 0;
}

void ensureDevLutInitialized() {
  if (devLutStore.initialized) return;
  const auto& def = freeink::uc8253X3DefaultConfig();
  devLutStore.cfg = def;
  copyBankToRam(def.gc, devLutStore.gcRam);
  copyBankToRam(def.preBwMid, devLutStore.preBwMidRam);
  if (def.directGray) {
    copyBankToRam(*def.directGray, devLutStore.directGrayRam);
  } else {
    memset(devLutStore.directGrayRam, 0, sizeof(devLutStore.directGrayRam));
  }
  copyBankToRam(def.lut16.preBwMid, devLutStore.lut16PreRam);
  copyBankToRam(def.lut16.pass1Coarse, devLutStore.lut16Pass1Ram);
  copyBankToRam(def.lut16.pass2Fine, devLutStore.lut16Pass2Ram);
  bindRamToBank(devLutStore.gcRam, devLutStore.cfg.gc);
  bindRamToBank(devLutStore.preBwMidRam, devLutStore.cfg.preBwMid);
  bindRamToBank(devLutStore.directGrayRam, devLutStore.directGrayBank);
  devLutStore.cfg.directGray = &devLutStore.directGrayBank;
  bindRamToBank(devLutStore.lut16PreRam, devLutStore.cfg.lut16.preBwMid);
  bindRamToBank(devLutStore.lut16Pass1Ram, devLutStore.cfg.lut16.pass1Coarse);
  bindRamToBank(devLutStore.lut16Pass2Ram, devLutStore.cfg.lut16.pass2Fine);
  devLutStore.initialized = true;
}

uint8_t (*selectRamBank(HalDisplay::DevLutBank bank)) [HalDisplay::DEV_LUT_LEN] {
  switch (bank) {
    case HalDisplay::DevLutBank::Gc:
      return devLutStore.gcRam;
    case HalDisplay::DevLutBank::Pre:
      return devLutStore.preBwMidRam;
    case HalDisplay::DevLutBank::Direct:
      return devLutStore.directGrayRam;
    case HalDisplay::DevLutBank::Lut16Pre:
      return devLutStore.lut16PreRam;
    case HalDisplay::DevLutBank::Lut16Pass1:
      return devLutStore.lut16Pass1Ram;
    case HalDisplay::DevLutBank::Lut16Pass2:
      return devLutStore.lut16Pass2Ram;
  }
  return devLutStore.gcRam;
}

}  // namespace

namespace freeink {
// Bound by reference inside Uc8253X3Driver so live RAM edits apply on the next SPI LUT load.
const Uc8253X3Config& crosspointUc8253X3DevConfig() {
  ensureDevLutInitialized();
  return devLutStore.cfg;
}
}  // namespace freeink

bool HalDisplay::devLutSupported() const {
#if defined(FREEINK_UC8253_X3_CONFIG)
  return gpio.deviceIsX3() && getController() == Controller::UC8253;
#else
  return false;
#endif
}

bool HalDisplay::devLut16Modified() const {
  if (!devLutSupported()) return false;
  ensureDevLutInitialized();
  const auto& def = freeink::uc8253X3DefaultConfig().lut16;
  const auto& cur = devLutStore.cfg.lut16;
  if (cur.pllBase != def.pllBase || cur.pllPass1 != def.pllPass1 || cur.pllPass2 != def.pllPass2 ||
      cur.vdlPass2 != def.vdlPass2) {
    return true;
  }
  return !bankMatchesRam(def.preBwMid, devLutStore.lut16PreRam) ||
         !bankMatchesRam(def.pass1Coarse, devLutStore.lut16Pass1Ram) ||
         !bankMatchesRam(def.pass2Fine, devLutStore.lut16Pass2Ram);
}

bool HalDisplay::devLutModified() const {
  if (devGrayscaleModeOverride != GrayscaleMode::Overlay || devGray16Mode != DevGray16Mode::Auto ||
      devSplitReader.enabled) {
    return true;
  }
  if (!devLutSupported()) return false;
  ensureDevLutInitialized();
  const auto& def = freeink::uc8253X3DefaultConfig();
  if (!bankMatchesRam(def.gc, devLutStore.gcRam)) return true;
  if (!bankMatchesRam(def.preBwMid, devLutStore.preBwMidRam)) return true;
  if (def.directGray && !bankMatchesRam(*def.directGray, devLutStore.directGrayRam)) return true;
  return devLut16Modified();
}

bool HalDisplay::devLutRead(DevLutBank bank, DevLutRole role, uint8_t out[DEV_LUT_LEN]) const {
  if (!devLutSupported()) return false;
  const auto roleIdx = static_cast<size_t>(role);
  if (roleIdx >= DEV_LUT_ROLES) return false;
  ensureDevLutInitialized();
  const auto* ram = selectRamBank(bank);
  memcpy(out, ram[roleIdx], DEV_LUT_LEN);
  return true;
}

bool HalDisplay::devLutWriteGroup(DevLutBank bank, DevLutRole role, uint8_t group, uint8_t level, uint8_t tp0,
                                  uint8_t tp1, uint8_t tp2, uint8_t tp3, uint8_t rp) {
  if (!devLutSupported()) return false;
  const auto roleIdx = static_cast<size_t>(role);
  if (roleIdx >= DEV_LUT_ROLES || group >= DEV_LUT_GROUPS) return false;
  ensureDevLutInitialized();
  uint8_t* row = selectRamBank(bank)[roleIdx] + static_cast<size_t>(group) * 6;
  row[0] = level;
  row[1] = tp0;
  row[2] = tp1;
  row[3] = tp2;
  row[4] = tp3;
  row[5] = rp;
  return true;
}

bool HalDisplay::devLutWriteTimings(DevLutBank bank, uint8_t group, uint8_t tp0, uint8_t tp1, uint8_t tp2, uint8_t tp3,
                                    uint8_t rp) {
  if (!devLutSupported() || group >= DEV_LUT_GROUPS) return false;
  ensureDevLutInitialized();
  auto* ram = selectRamBank(bank);
  const size_t offset = static_cast<size_t>(group) * 6;
  for (size_t r = 0; r < DEV_LUT_ROLES; ++r) {
    ram[r][offset + 1] = tp0;
    ram[r][offset + 2] = tp1;
    ram[r][offset + 3] = tp2;
    ram[r][offset + 4] = tp3;
    ram[r][offset + 5] = rp;
  }
  return true;
}

bool HalDisplay::devLutWriteRaw(DevLutBank bank, DevLutRole role, const uint8_t bytes[DEV_LUT_LEN]) {
  if (!devLutSupported()) return false;
  const auto roleIdx = static_cast<size_t>(role);
  if (roleIdx >= DEV_LUT_ROLES) return false;
  ensureDevLutInitialized();
  memcpy(selectRamBank(bank)[roleIdx], bytes, DEV_LUT_LEN);
  return true;
}

bool HalDisplay::devLutApplyGcQuick(uint8_t lightTp, uint8_t darkTp, uint8_t lvLight, uint8_t lvDark) {
  if (!devLutSupported()) return false;
  ensureDevLutInitialized();
  memset(devLutStore.gcRam, 0, sizeof(devLutStore.gcRam));
  const uint8_t levels[DEV_LUT_ROLES] = {0x00, lvDark, lvLight, 0x00, 0x00};
  for (size_t r = 0; r < DEV_LUT_ROLES; ++r) {
    devLutStore.gcRam[r][0] = levels[r];
    devLutStore.gcRam[r][1] = lightTp;
    devLutStore.gcRam[r][2] = darkTp;
    devLutStore.gcRam[r][3] = 0;
    devLutStore.gcRam[r][4] = 0;
    devLutStore.gcRam[r][5] = 1;
  }
  return true;
}

bool HalDisplay::devLutReset(DevLutBank bank) {
  if (!devLutSupported()) return false;
  ensureDevLutInitialized();
  const auto& def = freeink::uc8253X3DefaultConfig();
  switch (bank) {
    case DevLutBank::Gc:
      copyBankToRam(def.gc, devLutStore.gcRam);
      return true;
    case DevLutBank::Pre:
      copyBankToRam(def.preBwMid, devLutStore.preBwMidRam);
      return true;
    case DevLutBank::Direct:
      if (def.directGray) {
        copyBankToRam(*def.directGray, devLutStore.directGrayRam);
      } else {
        memset(devLutStore.directGrayRam, 0, sizeof(devLutStore.directGrayRam));
      }
      return true;
    case DevLutBank::Lut16Pre:
      copyBankToRam(def.lut16.preBwMid, devLutStore.lut16PreRam);
      return true;
    case DevLutBank::Lut16Pass1:
      copyBankToRam(def.lut16.pass1Coarse, devLutStore.lut16Pass1Ram);
      return true;
    case DevLutBank::Lut16Pass2:
      copyBankToRam(def.lut16.pass2Fine, devLutStore.lut16Pass2Ram);
      return true;
  }
  return false;
}

bool HalDisplay::devLut16ReadParams(uint8_t& pllBase, uint8_t& pll1, uint8_t& pll2, uint8_t& vdl2) const {
  if (!devLutSupported()) return false;
  ensureDevLutInitialized();
  pllBase = devLutStore.cfg.lut16.pllBase;
  pll1 = devLutStore.cfg.lut16.pllPass1;
  pll2 = devLutStore.cfg.lut16.pllPass2;
  vdl2 = devLutStore.cfg.lut16.vdlPass2;
  return true;
}

bool HalDisplay::devLut16Tune(uint8_t pll1, uint8_t pll2, uint8_t vdl2) {
  if (!devLutSupported()) return false;
  ensureDevLutInitialized();
  devLutStore.cfg.lut16.pllPass1 = static_cast<uint8_t>(pll1 & 0x0Fu);
  devLutStore.cfg.lut16.pllPass2 = static_cast<uint8_t>(pll2 & 0x0Fu);
  devLutStore.cfg.lut16.vdlPass2 = static_cast<uint8_t>(vdl2 & 0x3Fu);
  return true;
}

bool HalDisplay::devLut16Reset() {
  if (!devLutSupported()) return false;
  ensureDevLutInitialized();
  const auto& def = freeink::uc8253X3DefaultConfig().lut16;
  devLutStore.cfg.lut16.pllBase = def.pllBase;
  devLutStore.cfg.lut16.pllPass1 = def.pllPass1;
  devLutStore.cfg.lut16.pllPass2 = def.pllPass2;
  devLutStore.cfg.lut16.vdlPass2 = def.vdlPass2;
  copyBankToRam(def.preBwMid, devLutStore.lut16PreRam);
  copyBankToRam(def.pass1Coarse, devLutStore.lut16Pass1Ram);
  copyBankToRam(def.pass2Fine, devLutStore.lut16Pass2Ram);
  return true;
}

bool HalDisplay::devIsGray16CleanupActive() const {
  if (!devLutSupported()) return false;
  return einkDisplay.isGray16CleanupActive();
}

bool HalDisplay::devLutResetAll() {
  devGrayscaleModeOverride = GrayscaleMode::Overlay;
  devGray16Mode = DevGray16Mode::Auto;
  devSplitReader = {};
  if (!devLutSupported()) return false;
  return devLutReset(DevLutBank::Gc) && devLutReset(DevLutBank::Pre) && devLutReset(DevLutBank::Direct) &&
         devLut16Reset();
}

#else

bool HalDisplay::devLutSupported() const { return false; }

bool HalDisplay::devLut16Modified() const { return false; }

bool HalDisplay::devLutModified() const {
  return devGrayscaleModeOverride != GrayscaleMode::Overlay || devGray16Mode != DevGray16Mode::Auto ||
         devSplitReader.enabled;
}

bool HalDisplay::devLutRead(DevLutBank, DevLutRole, uint8_t[DEV_LUT_LEN]) const { return false; }

bool HalDisplay::devLutWriteGroup(DevLutBank, DevLutRole, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t,
                                  uint8_t) {
  return false;
}

bool HalDisplay::devLutWriteTimings(DevLutBank, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t) { return false; }

bool HalDisplay::devLutWriteRaw(DevLutBank, DevLutRole, const uint8_t[DEV_LUT_LEN]) { return false; }

bool HalDisplay::devLutApplyGcQuick(uint8_t, uint8_t, uint8_t, uint8_t) { return false; }

bool HalDisplay::devLutReset(DevLutBank) { return false; }

bool HalDisplay::devLut16ReadParams(uint8_t&, uint8_t&, uint8_t&, uint8_t&) const { return false; }

bool HalDisplay::devLut16Tune(uint8_t, uint8_t, uint8_t) { return false; }

bool HalDisplay::devLut16Reset() { return false; }

bool HalDisplay::devIsGray16CleanupActive() const { return false; }

bool HalDisplay::devLutResetAll() {
  devGrayscaleModeOverride = GrayscaleMode::Overlay;
  devGray16Mode = DevGray16Mode::Auto;
  devSplitReader = {};
  return false;
}

#endif
#endif
