#include "UsbDevSerial.h"

#if defined(CROSSPOINT_USB_DEV) && CROSSPOINT_USB_DEV

#include <Arduino.h>
#include <BoardConfig.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalMemory.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <SdCardFont.h>
#include <SerialTransport.h>
#include <Uc8253X3Lut16.h>
#include <esp_rom_crc.h>
#include <strings.h>

#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>

#include "CrossPointSettings.h"
#include "SdCardFontSystem.h"
#include "activities/ActivityManager.h"
#include "activities/RenderLock.h"
#include "fontIds.h"

#ifndef CROSSPOINT_VERSION
#define CROSSPOINT_VERSION "dev"
#endif

namespace {

constexpr size_t RX_BUF_SIZE = 320;
constexpr size_t TX_BUF_SIZE = 448;
constexpr size_t CDC_CHUNK_BYTES = 64;
constexpr unsigned long CDC_DISCONNECT_TIMEOUT_MS = 500;
constexpr size_t PUT_DECODE_BYTES = 128;
constexpr size_t PUT_MAX_PATH_LEN = 160;
constexpr unsigned long PUT_INACTIVITY_TIMEOUT_MS = 5000;
constexpr size_t KEY_QUEUE_CAPACITY = 8;
constexpr uint16_t KEY_DEFAULT_HOLD_MS = 40;
constexpr uint16_t KEY_MIN_HOLD_MS = 20;
constexpr uint16_t KEY_MAX_HOLD_MS = 3000;
constexpr uint16_t KEY_POWER_SAFE_MAX_MS = 250;
constexpr uint16_t KEY_RELEASE_GAP_MS = 25;
constexpr uint8_t MAX_CMDS_PER_POLL = 32;
constexpr unsigned long MAX_POLL_SLICE_MS = 40;

enum class KeyPhase : uint8_t { Idle, Holding, ReleaseGap };
enum class StreamEnc : uint8_t { Raw, Base64 };
enum class RefreshKind : uint8_t { None, Clean, Fast };

struct QueuedKey {
  uint8_t rawBtn = 0;
  uint16_t holdMs = 0;
};

struct KeyState {
  QueuedKey queue[KEY_QUEUE_CAPACITY];
  uint8_t head = 0;
  uint8_t count = 0;
  KeyPhase phase = KeyPhase::Idle;
  uint8_t activeBtn = 0;
  uint16_t activeHoldMs = 0;
  uint8_t phasePolls = 0;
  unsigned long phaseStartMs = 0;
};

struct PutState {
  HalFile file;
  char path[PUT_MAX_PATH_LEN] = {};
  uint32_t expectedBytes = 0;
  uint32_t writtenBytes = 0;
  uint32_t crc32 = 0;
  unsigned long lastActivityMs = 0;
  bool active = false;
};

char rxBuf[RX_BUF_SIZE];
uint16_t rxLen = 0;
bool rxOverflow = false;

char txBuf[TX_BUF_SIZE];
uint8_t putDecodeBuf[PUT_DECODE_BYTES];
uint8_t fallbackRowBuf[HalDisplay::DISPLAY_WIDTH_BYTES * 4];  // Serves both SNAP2 (2bpp) and SNAP4 (4bpp)
char b64LineBuf[68];
uint8_t lutRowScratch[HalDisplay::DEV_LUT_LEN];
char lutHexScratch[HalDisplay::DEV_LUT_LEN * 2 + 1];
uint8_t savedGcLut[HalDisplay::DEV_LUT_ROLES][HalDisplay::DEV_LUT_LEN];
bool cal16OnScreen = false;

KeyState keyState;
PutState putState;

constexpr uint8_t SPREAD_NIBBLE[16] = {0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15,
                                       0x40, 0x41, 0x44, 0x45, 0x50, 0x51, 0x54, 0x55};
constexpr char B64_CHARS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

bool writeBulkCdc(const uint8_t* src, size_t len) {
  size_t sent = 0;
  unsigned long lastProgressMs = millis();
  while (sent < len) {
    const size_t chunk = (len - sent > CDC_CHUNK_BYTES) ? CDC_CHUNK_BYTES : (len - sent);
    const size_t written = logSerial.write(src + sent, chunk);
    BoardConfig::serviceSerialTx();
    if (written > 0) {
      sent += written;
      lastProgressMs = millis();
    } else {
      if (millis() - lastProgressMs > CDC_DISCONNECT_TIMEOUT_MS) {
        return false;
      }
      delay(1);
    }
  }
  return true;
}

bool sendLineFmt(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
bool sendLineFmt(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  int n = vsnprintf(txBuf, sizeof(txBuf), fmt, args);
  va_end(args);
  if (n <= 0) return false;
  const size_t len = (static_cast<size_t>(n) >= sizeof(txBuf)) ? (sizeof(txBuf) - 1) : static_cast<size_t>(n);
  return writeBulkCdc(reinterpret_cast<const uint8_t*>(txBuf), len);
}

size_t encodeBase64Chunk(const uint8_t* in, size_t len, char* out) {
  size_t pos = 0;
  size_t i = 0;
  while (i + 3 <= len) {
    const uint32_t v = (static_cast<uint32_t>(in[i]) << 16) | (static_cast<uint32_t>(in[i + 1]) << 8) | in[i + 2];
    out[pos++] = B64_CHARS[(v >> 18) & 0x3F];
    out[pos++] = B64_CHARS[(v >> 12) & 0x3F];
    out[pos++] = B64_CHARS[(v >> 6) & 0x3F];
    out[pos++] = B64_CHARS[v & 0x3F];
    i += 3;
  }
  const size_t rem = len - i;
  if (rem == 1) {
    const uint32_t v = static_cast<uint32_t>(in[i]) << 16;
    out[pos++] = B64_CHARS[(v >> 18) & 0x3F];
    out[pos++] = B64_CHARS[(v >> 12) & 0x3F];
    out[pos++] = '=';
    out[pos++] = '=';
  } else if (rem == 2) {
    const uint32_t v = (static_cast<uint32_t>(in[i]) << 16) | (static_cast<uint32_t>(in[i + 1]) << 8);
    out[pos++] = B64_CHARS[(v >> 18) & 0x3F];
    out[pos++] = B64_CHARS[(v >> 12) & 0x3F];
    out[pos++] = B64_CHARS[(v >> 6) & 0x3F];
    out[pos++] = '=';
  }
  out[pos++] = '\n';
  return pos;
}

bool writeBulkBase64(const uint8_t* src, size_t len) {
  size_t offset = 0;
  while (offset < len) {
    const size_t chunk = (len - offset > 48) ? 48 : (len - offset);
    const size_t outLen = encodeBase64Chunk(src + offset, chunk, b64LineBuf);
    if (!writeBulkCdc(reinterpret_cast<const uint8_t*>(b64LineBuf), outLen)) return false;
    offset += chunk;
  }
  return true;
}

char* nextToken(char*& cursor) {
  while (*cursor == ' ' || *cursor == '\t') {
    ++cursor;
  }
  if (*cursor == '\0') return nullptr;
  if (*cursor == '"') {
    ++cursor;
    char* start = cursor;
    while (*cursor != '\0' && *cursor != '"') {
      ++cursor;
    }
    if (*cursor == '"') {
      *cursor = '\0';
      ++cursor;
    }
    return start;
  }
  char* start = cursor;
  while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t') {
    ++cursor;
  }
  if (*cursor != '\0') {
    *cursor = '\0';
    ++cursor;
  }
  return start;
}

int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}

bool decodeHexBytes(const char* hex, uint8_t* out, size_t maxBytes, size_t& outLen) {
  const size_t charLen = strlen(hex);
  if (charLen == 0 || (charLen & 1u) != 0 || (charLen / 2) > maxBytes) return false;
  const size_t byteLen = charLen / 2;
  for (size_t i = 0; i < byteLen; ++i) {
    const int hi = hexNibble(hex[i * 2]);
    const int lo = hexNibble(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  outLen = byteLen;
  return true;
}

bool parseUint32Dec(const char* s, uint32_t& out) {
  if (!s || *s == '\0') return false;
  char* end = nullptr;
  const unsigned long val = strtoul(s, &end, 10);
  if (!end || *end != '\0') return false;
  out = static_cast<uint32_t>(val);
  return true;
}

bool parseUint8Dec(const char* s, uint8_t& out) {
  uint32_t v = 0;
  if (!parseUint32Dec(s, v) || v > 255) return false;
  out = static_cast<uint8_t>(v);
  return true;
}

bool parseUint8Hex(const char* s, uint8_t& out) {
  if (!s || *s == '\0') return false;
  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
  if (*s == '\0' || strlen(s) > 2) return false;
  char* end = nullptr;
  const unsigned long val = strtoul(s, &end, 16);
  if (!end || *end != '\0' || val > 255) return false;
  out = static_cast<uint8_t>(val);
  return true;
}

bool parseUint32Hex8(const char* s, uint32_t& out) {
  if (!s || *s == '\0') return false;
  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
  if (strlen(s) != 8) return false;
  for (size_t i = 0; i < 8; ++i) {
    if (hexNibble(s[i]) < 0) return false;
  }
  char* end = nullptr;
  out = static_cast<uint32_t>(strtoul(s, &end, 16));
  return end && *end == '\0';
}

const char* grayscaleModeName(HalDisplay::GrayscaleMode mode) {
  switch (mode) {
    case HalDisplay::GrayscaleMode::Overlay:
      return "OVERLAY";
    case HalDisplay::GrayscaleMode::Absolute:
      return "ABSOLUTE";
    case HalDisplay::GrayscaleMode::Direct:
      return "DIRECT";
  }
  return "OVERLAY";
}

const char* gray16ModeName(HalDisplay::DevGray16Mode mode) {
  switch (mode) {
    case HalDisplay::DevGray16Mode::Auto:
      return "AUTO";
    case HalDisplay::DevGray16Mode::ForceOn:
      return "ON";
    case HalDisplay::DevGray16Mode::ForceOff:
      return "OFF";
  }
  return "AUTO";
}

const char* lutBankName(HalDisplay::DevLutBank bank) {
  switch (bank) {
    case HalDisplay::DevLutBank::Gc:
      return "gc";
    case HalDisplay::DevLutBank::Pre:
      return "pre";
    case HalDisplay::DevLutBank::Direct:
      return "direct";
    case HalDisplay::DevLutBank::Lut16Pre:
      return "lut16_pre";
    case HalDisplay::DevLutBank::Lut16Pass1:
      return "lut16_pass1";
    case HalDisplay::DevLutBank::Lut16Pass2:
      return "lut16_pass2";
  }
  return "gc";
}

const char* lutRoleName(HalDisplay::DevLutRole role) {
  switch (role) {
    case HalDisplay::DevLutRole::Vcom:
      return "vcom";
    case HalDisplay::DevLutRole::Ww:
      return "ww";
    case HalDisplay::DevLutRole::Bw:
      return "bw";
    case HalDisplay::DevLutRole::Wb:
      return "wb";
    case HalDisplay::DevLutRole::Bb:
      return "bb";
  }
  return "vcom";
}

bool parseLutBank(const char* s, HalDisplay::DevLutBank& out) {
  if (!s) return false;
  if (strcasecmp(s, "gc") == 0) {
    out = HalDisplay::DevLutBank::Gc;
    return true;
  }
  if (strcasecmp(s, "pre") == 0) {
    out = HalDisplay::DevLutBank::Pre;
    return true;
  }
  if (strcasecmp(s, "direct") == 0) {
    out = HalDisplay::DevLutBank::Direct;
    return true;
  }
  if (strcasecmp(s, "lut16_pre") == 0 || strcasecmp(s, "pre16") == 0) {
    out = HalDisplay::DevLutBank::Lut16Pre;
    return true;
  }
  if (strcasecmp(s, "lut16_pass1") == 0 || strcasecmp(s, "pass1") == 0) {
    out = HalDisplay::DevLutBank::Lut16Pass1;
    return true;
  }
  if (strcasecmp(s, "lut16_pass2") == 0 || strcasecmp(s, "pass2") == 0) {
    out = HalDisplay::DevLutBank::Lut16Pass2;
    return true;
  }
  return false;
}

bool parseLutRole(const char* s, HalDisplay::DevLutRole& out) {
  if (!s) return false;
  if (strcasecmp(s, "vcom") == 0) {
    out = HalDisplay::DevLutRole::Vcom;
    return true;
  }
  if (strcasecmp(s, "ww") == 0) {
    out = HalDisplay::DevLutRole::Ww;
    return true;
  }
  if (strcasecmp(s, "bw") == 0) {
    out = HalDisplay::DevLutRole::Bw;
    return true;
  }
  if (strcasecmp(s, "wb") == 0) {
    out = HalDisplay::DevLutRole::Wb;
    return true;
  }
  if (strcasecmp(s, "bb") == 0) {
    out = HalDisplay::DevLutRole::Bb;
    return true;
  }
  return false;
}

bool parseRefreshKind(const char* s, RefreshKind& out) {
  if (!s) return false;
  if (strcasecmp(s, "CLEAN") == 0) {
    out = RefreshKind::Clean;
    return true;
  }
  if (strcasecmp(s, "FAST") == 0) {
    out = RefreshKind::Fast;
    return true;
  }
  return false;
}

void triggerDevRefresh(GfxRenderer& renderer, RefreshKind kind, const bool reRenderNonReader = false) {
  (void)renderer;
  if (kind == RefreshKind::None) return;
  cal16OnScreen = false;
  switch (kind) {
    case RefreshKind::None:
      return;
    case RefreshKind::Clean:
      activityManager.renderDevUpdateAndWait(/*forceClean=*/true, reRenderNonReader);
      return;
    case RefreshKind::Fast:
      activityManager.renderDevUpdateAndWait(/*forceClean=*/false, reRenderNonReader);
      return;
  }
}

uint8_t activeFontBitsPerPixel(const GfxRenderer& renderer, int fontId) {
  const auto& fm = renderer.getFontMap();
  const auto it = fm.find(fontId);
  if (it == fm.end()) return 1;
  const EpdFontData* d = it->second.getData(EpdFontFamily::REGULAR);
  return d ? epdBitsPerPixel(*d) : 1;
}

struct CompareBandSpec {
  uint8_t lightTp;
  uint8_t darkTp;
};

constexpr const char* COMPARE_SAMPLE_LINES[4] = {
    "The quick brown fox jumps over lazy dogs.",
    "Sphinx of black quartz, judge my vow!",
    "\"When business is great, everybody wins.\"",
    "Curves & diagonals: wave, vivid, cozy, glyph.",
};

constexpr const char* CAL16_SAMPLE_LINES[4] = {
    "Bookerly 4bpp 16-Gray: minimum, storytelling, office, first.",
    "\"Tony Fadell\" — crisp stems, round counters & smooth arches.",
    "Sphinx of black quartz, judge my vow! 0123456789 (258.7 PPI)",
    "Italic & Bold contrast: affluent, expertise, wave, cozy.",
};

const char* comparePresetLabel(uint8_t lightTp, uint8_t darkTp) {
  if (lightTp == 3 && darkTp == 2) return "Stock";
  if (lightTp == 5 && darkTp == 3) return "Linear";
  if (lightTp == 8 && darkTp == 4) return "Soft";
  return "Custom";
}

void drawCompareBandBw(GfxRenderer& renderer, int readerFontId, int bandIdx, CompareBandSpec spec) {
  constexpr int BAND_H = 264;
  constexpr int SWATCH_W = 28;
  constexpr int SWATCH_H = 18;
  constexpr int SWATCH_STEP = 32;
  constexpr int SWATCH_X0 = 384;
  const int bandTop = bandIdx * BAND_H;
  const int swY = bandTop + 10;

  renderer.setClipRect(0, bandTop, 528, BAND_H);
  if (bandIdx > 0) {
    renderer.fillRect(16, bandTop, 496, 1, true);
  }

  const char* fontName = (SETTINGS.sdFontFamilyName[0] != '\0') ? SETTINGS.sdFontFamilyName : "NotoSans";
  char header[64];
  snprintf(header, sizeof(header), "[%c] %s (%u / %u frames)  %.18s %upt", static_cast<char>('A' + bandIdx),
           comparePresetLabel(spec.lightTp, spec.darkTp), static_cast<unsigned>(spec.lightTp),
           static_cast<unsigned>(spec.darkTp), fontName, static_cast<unsigned>(SETTINGS.fontPointSize));
  renderer.drawText(SMALL_FONT_ID, 16, bandTop + 13, header, true, EpdFontFamily::REGULAR);

  renderer.fillRect(SWATCH_X0, swY, SWATCH_W, SWATCH_H, true);
  renderer.fillRect(SWATCH_X0 + SWATCH_STEP, swY, SWATCH_W, SWATCH_H, true);
  renderer.fillRect(SWATCH_X0 + SWATCH_STEP * 2, swY, SWATCH_W, SWATCH_H, true);
  renderer.drawRect(SWATCH_X0 + SWATCH_STEP * 3, swY, SWATCH_W, SWATCH_H, true);

  const int rawLh = renderer.getLineHeight(readerFontId);
  const int lineStep = std::clamp((rawLh > 0 ? rawLh : 28) + 6, 28, 48);
  const int textStartY = bandTop + 40;
  for (int i = 0; i < 4; ++i) {
    renderer.drawText(readerFontId, 16, textStartY + i * lineStep, COMPARE_SAMPLE_LINES[i], true,
                      EpdFontFamily::REGULAR);
  }
}

void drawCompareBandGray(GfxRenderer& renderer, int readerFontId, int bandIdx, bool lsbPlane) {
  constexpr int BAND_H = 264;
  constexpr int SWATCH_W = 28;
  constexpr int SWATCH_H = 18;
  constexpr int SWATCH_STEP = 32;
  constexpr int SWATCH_X0 = 384;
  const int bandTop = bandIdx * BAND_H;
  const int swY = bandTop + 10;

  renderer.setClipRect(0, bandTop, 528, BAND_H);
  renderer.fillRect(SWATCH_X0 + SWATCH_STEP, swY, SWATCH_W, SWATCH_H, false);
  if (!lsbPlane) {
    renderer.fillRect(SWATCH_X0 + SWATCH_STEP * 2, swY, SWATCH_W, SWATCH_H, false);
  }

  const int rawLh = renderer.getLineHeight(readerFontId);
  const int lineStep = std::clamp((rawLh > 0 ? rawLh : 28) + 6, 28, 48);
  const int textStartY = bandTop + 40;
  for (int i = 0; i < 4; ++i) {
    renderer.drawText(readerFontId, 16, textStartY + i * lineStep, COMPARE_SAMPLE_LINES[i], true,
                      EpdFontFamily::REGULAR);
  }
}

void handleLutCompare(GfxRenderer& renderer, char*& cursor, const char* ctrlName) {
  CompareBandSpec bands[3] = {{3, 2}, {5, 3}, {8, 4}};
  if (const char* firstTok = nextToken(cursor)) {
    const char* toks[6] = {firstTok,          nextToken(cursor), nextToken(cursor),
                           nextToken(cursor), nextToken(cursor), nextToken(cursor)};
    for (int b = 0; b < 3; ++b) {
      if (!parseUint8Dec(toks[b * 2], bands[b].lightTp) || !parseUint8Dec(toks[b * 2 + 1], bands[b].darkTp)) {
        sendLineFmt("@DEV ERR LUT reason=\"bad_compare_args\"\n");
        return;
      }
    }
  }

  if (!display.devLutSupported()) {
    sendLineFmt("@DEV ERR LUT reason=\"unsupported_controller\" ctrl=\"%s\"\n", ctrlName);
    return;
  }

  RenderLock lock;
  if (!renderer.hasFrameBuffer() || !renderer.getFrameBuffer()) {
    sendLineFmt("@DEV ERR LUT reason=\"framebuffer_loaned\"\n");
    return;
  }
  if (renderer.getDisplayWidth() != 792 || renderer.getDisplayHeight() != 528) {
    sendLineFmt("@DEV ERR LUT reason=\"bad_panel_geometry\"\n");
    return;
  }

  constexpr int COMPARE_STRIP_ROWS = 80;
  const int gh = renderer.getDisplayHeight();
  const int gwBytes = renderer.getDisplayWidthBytes();
  auto scratch = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(gwBytes) * COMPARE_STRIP_ROWS);
  if (!scratch) {
    LOG_ERR("DEV", "LUT COMPARE OOM (%d bytes)", gwBytes * COMPARE_STRIP_ROWS);
    sendLineFmt("@DEV ERR LUT reason=\"oom\"\n");
    return;
  }

  static constexpr HalDisplay::DevLutRole ALL_ROLES[HalDisplay::DEV_LUT_ROLES] = {
      HalDisplay::DevLutRole::Vcom, HalDisplay::DevLutRole::Ww, HalDisplay::DevLutRole::Bw, HalDisplay::DevLutRole::Wb,
      HalDisplay::DevLutRole::Bb};

  for (size_t r = 0; r < HalDisplay::DEV_LUT_ROLES; ++r) {
    if (!display.devLutRead(HalDisplay::DevLutBank::Gc, ALL_ROLES[r], savedGcLut[r])) {
      sendLineFmt("@DEV ERR LUT reason=\"read_failed\"\n");
      return;
    }
  }

  struct CompareStateGuard {
    GfxRenderer& renderer;
    GfxRenderer::Orientation prevOrientation;
    bool prevInverted;
    std::array<int, 4> prevClip;
    ~CompareStateGuard() {
      renderer.endStripTarget();
      renderer.setRenderMode(GfxRenderer::BW);
      renderer.setClipRect(prevClip[0], prevClip[1], prevClip[2], prevClip[3]);
      renderer.setOrientation(prevOrientation);
      display.setInverted(prevInverted);
      for (size_t r = 0; r < HalDisplay::DEV_LUT_ROLES; ++r) {
        display.devLutWriteRaw(HalDisplay::DevLutBank::Gc, ALL_ROLES[r], savedGcLut[r]);
      }
    }
  } stateGuard{renderer, renderer.getOrientation(), display.isInverted(), renderer.getClipRect()};

  cal16OnScreen = false;
  display.setInverted(false);
  renderer.setOrientation(GfxRenderer::Portrait);
  renderer.setRenderMode(GfxRenderer::BW);

  sdFontSystem.ensureLoaded(renderer);
  int readerFontId = SETTINGS.getReaderFontId();
  if (readerFontId == 0 || renderer.getFontMap().count(readerFontId) == 0) {
    readerFontId = NOTOSANS_14_FONT_ID;
  }

  std::optional<FontCacheManager::PrewarmScope> scope;
  if (auto* fcm = renderer.getFontCacheManager()) {
    scope.emplace(fcm->createPrewarmScope());
    for (const char* line : COMPARE_SAMPLE_LINES) {
      renderer.drawText(readerFontId, 16, 40, line, true, EpdFontFamily::REGULAR);
    }
    scope->endScanAndPrewarm();
  }

  activityManager.notifyDevFrameBufferOverwritten();
  renderer.clearScreen(0xFF);
  for (int b = 0; b < 3; ++b) {
    drawCompareBandBw(renderer, readerFontId, b, bands[b]);
  }
  renderer.setClipRect(0, 0, 528, 792);

  renderer.displayBuffer(HalDisplay::HALF_REFRESH, /*keepPowered=*/true);
  renderer.preconditionGrayscale();

  constexpr size_t BAND_COL_BYTES = 33;
  for (int b = 0; b < 3; ++b) {
    display.devLutApplyGcQuick(bands[b].lightTp, bands[b].darkTp, 0x80, 0x20);
    const size_t keepStart = static_cast<size_t>(b) * BAND_COL_BYTES;
    const size_t keepEnd = keepStart + BAND_COL_BYTES;

    for (int plane = 0; plane < 2; ++plane) {
      const bool lsbPlane = (plane == 0);
      renderer.setRenderMode(lsbPlane ? GfxRenderer::GRAYSCALE_LSB : GfxRenderer::GRAYSCALE_MSB);
      for (int py = 0; py < gh; py += COMPARE_STRIP_ROWS) {
        const int rows = (gh - py < COMPARE_STRIP_ROWS) ? (gh - py) : COMPARE_STRIP_ROWS;
        renderer.beginStripTarget(scratch.get(), py, rows);
        renderer.clearScreen(0x00);
        drawCompareBandGray(renderer, readerFontId, b, lsbPlane);
        renderer.endStripTarget();

        for (int r = 0; r < rows; ++r) {
          uint8_t* const rowPtr = scratch.get() + static_cast<size_t>(r) * gwBytes;
          if (keepStart > 0) {
            memset(rowPtr, 0x00, keepStart);
          }
          if (keepEnd < static_cast<size_t>(gwBytes)) {
            memset(rowPtr + keepEnd, 0x00, static_cast<size_t>(gwBytes) - keepEnd);
          }
        }
        renderer.writeGrayscalePlaneStrip(lsbPlane, scratch.get(), py, rows);
      }
    }

    renderer.setRenderMode(GfxRenderer::BW);
    display.displayGrayBuffer(b == 2 && SETTINGS.fadingFix != 0);
  }

  for (size_t r = 0; r < HalDisplay::DEV_LUT_ROLES; ++r) {
    display.devLutWriteRaw(HalDisplay::DevLutBank::Gc, ALL_ROLES[r], savedGcLut[r]);
  }
  renderer.cleanupGrayscaleWithFrameBuffer();

  sendLineFmt("@DEV OK LUT op=\"COMPARE\" bands=\"%u/%u,%u/%u,%u/%u\"\n", static_cast<unsigned>(bands[0].lightTp),
              static_cast<unsigned>(bands[0].darkTp), static_cast<unsigned>(bands[1].lightTp),
              static_cast<unsigned>(bands[1].darkTp), static_cast<unsigned>(bands[2].lightTp),
              static_cast<unsigned>(bands[2].darkTp));
}

bool isCpfontFile4Bit(const char* path) {
  HalFile f;
  if (!Storage.openFileForRead("DEV", path, f)) return false;
  uint8_t hdr[16] = {};
  if (f.read(hdr, sizeof(hdr)) != static_cast<int>(sizeof(hdr))) return false;
  if (memcmp(hdr, "CPFONT\0\0", 8) != 0) return false;
  const uint16_t ver = static_cast<uint16_t>(hdr[8] | (hdr[9] << 8));
  const uint16_t flags = static_cast<uint16_t>(hdr[10] | (hdr[11] << 8));
  return ver == CPFONT_VERSION && (flags & SdCardFont::CPFONT_FLAG_4BIT) != 0 &&
         (flags & SdCardFont::CPFONT_FLAG_2BIT) == 0;
}

// Resolves a 4bpp reader font for CAL16 without mutating SETTINGS or persisting to SD:
// prefers the active reader font when already 4bpp, otherwise temporarily loads the
// first installed 4bpp SD .cpfont family for the duration of this scope.
struct ScopedCal16Font {
  GfxRenderer& renderer;
  std::unique_ptr<SdCardFont> tempFont;
  int tempFontId = 0;
  int fontId = NOTOSANS_14_FONT_ID;
  uint8_t pointSize = CrossPointSettings::DEFAULT_FONT_POINT_SIZE;
  char label[32] = "NotoSans";

  explicit ScopedCal16Font(GfxRenderer& r) : renderer(r) {
    sdFontSystem.refreshIfDirty();
    pointSize = SETTINGS.fontPointSize;
    const int activeId = SETTINGS.getReaderFontId();
    if (activeId != 0 && renderer.isFont4Bit(activeId)) {
      fontId = activeId;
      if (SETTINGS.sdFontFamilyName[0] != '\0') {
        strncpy(label, SETTINGS.sdFontFamilyName, sizeof(label) - 1);
        label[sizeof(label) - 1] = '\0';
      }
      return;
    }

    for (const auto& fam : sdFontSystem.registry().getFamilies()) {
      if (fam.vector) continue;
      const auto* file = fam.findNearestSize(SETTINGS.fontPointSize);
      if (!file) continue;
      char path[SdCardFontFamilyInfo::PATH_BUFFER_SIZE];
      if (!fam.filePath(*file, path, sizeof(path)) || !isCpfontFile4Bit(path)) continue;
      auto candidate = makeUniqueNoThrow<SdCardFont>();
      if (!candidate || !candidate->load(path) || !candidate->is4Bit(0)) continue;

      int candidateId = -0x7FFF4B16;
      while (candidateId == 0 || renderer.getFontMap().count(candidateId) != 0) {
        ++candidateId;
      }
      renderer.registerSdCardFont(candidateId, candidate.get());
      renderer.insertFont(candidateId,
                          EpdFontFamily(file->pointSize, candidate->getEpdFont(0), candidate->getEpdFont(1),
                                        candidate->getEpdFont(2), candidate->getEpdFont(3)));
      tempFont = std::move(candidate);
      tempFontId = candidateId;
      fontId = candidateId;
      pointSize = file->pointSize;
      strncpy(label, fam.name.c_str(), sizeof(label) - 1);
      label[sizeof(label) - 1] = '\0';
      return;
    }

    if (activeId != 0 && renderer.getFontMap().count(activeId) != 0) {
      fontId = activeId;
      if (SETTINGS.sdFontFamilyName[0] != '\0') {
        strncpy(label, SETTINGS.sdFontFamilyName, sizeof(label) - 1);
        label[sizeof(label) - 1] = '\0';
      }
    }
  }

  ~ScopedCal16Font() {
    if (tempFontId != 0) {
      renderer.removeFont(tempFontId);
    }
  }

  ScopedCal16Font(const ScopedCal16Font&) = delete;
  ScopedCal16Font& operator=(const ScopedCal16Font&) = delete;
};

void drawCal16Pass(GfxRenderer& renderer, int readerFontId, const char* fontLabel, uint8_t pointSize,
                   GfxRenderer::RenderMode mode) {
  constexpr int GRID_X0 = 24;
  constexpr int GRID_Y0 = 48;
  constexpr int PATCH_W = 112;
  constexpr int PATCH_H = 82;
  constexpr int GAP_X = 12;
  constexpr int ROW_STEP = 102;

  const bool isChromePass = (mode == GfxRenderer::BW || mode == GfxRenderer::GRAY16_CLEANUP_DTM1);
  uint16_t patchMask = 0;
  bool clearBits = true;
  switch (mode) {
    case GfxRenderer::BW:
      patchMask = freeink::x3_lut16::kMaskBwClear;
      clearBits = true;
      break;
    case GfxRenderer::GRAY16_COARSE_LSB:
      patchMask = freeink::x3_lut16::kMaskPass1Dtm1;
      clearBits = false;
      break;
    case GfxRenderer::GRAY16_COARSE_MSB:
      patchMask = freeink::x3_lut16::kMaskPass1Dtm2;
      clearBits = false;
      break;
    case GfxRenderer::GRAY16_FINE_LSB:
      patchMask = freeink::x3_lut16::kMaskPass2Dtm1;
      clearBits = false;
      break;
    case GfxRenderer::GRAY16_FINE_MSB:
      patchMask = freeink::x3_lut16::kMaskPass2Dtm2;
      clearBits = false;
      break;
    case GfxRenderer::GRAY16_CLEANUP_DTM1:
      patchMask = freeink::x3_lut16::kMaskCleanupBlack;
      clearBits = true;
      break;
    case GfxRenderer::GRAYSCALE_LSB:
    case GfxRenderer::GRAYSCALE_MSB:
      return;
  }

  if (isChromePass) {
    uint8_t pllBase = freeink::x3_lut16::kPllBase;
    uint8_t pll1 = freeink::x3_lut16::kPllPass1Coarse;
    uint8_t pll2 = freeink::x3_lut16::kPllPass2Fine;
    uint8_t vdl2 = freeink::x3_lut16::kPwrPass2Fine[3];
    display.devLut16ReadParams(pllBase, pll1, pll2, vdl2);
    char title[80];
    snprintf(title, sizeof(title), "X3 16-Gray Cal  %.16s %upt (%ubpp)  P1:%02X P2:%02X/%02X", fontLabel,
             static_cast<unsigned>(pointSize), static_cast<unsigned>(activeFontBitsPerPixel(renderer, readerFontId)),
             static_cast<unsigned>(pll1), static_cast<unsigned>(pll2), static_cast<unsigned>(vdl2));
    renderer.drawText(SMALL_FONT_ID, 24, 18, title, true, EpdFontFamily::REGULAR);
    renderer.fillRect(24, 38, 480, 1, true);
    // Corner registration fiducials for camera homography
    renderer.fillRect(6, 6, 8, 8, true);
    renderer.fillRect(514, 6, 8, 8, true);
    renderer.fillRect(6, 778, 8, 8, true);
    renderer.fillRect(514, 778, 8, 8, true);
  }

  for (int row = 0; row < 4; ++row) {
    for (int col = 0; col < 4; ++col) {
      const int grayIdx = row * 4 + col;  // G0 (Black) .. G15 (White)
      const uint8_t ink = 15 - grayIdx;   // ink 15 (Black) .. 0 (White)
      const int px = GRID_X0 + col * (PATCH_W + GAP_X);
      const int py = GRID_Y0 + row * ROW_STEP;

      if (isChromePass) {
        renderer.drawRect(px, py, PATCH_W, PATCH_H, true);
        char lbl[20];
        snprintf(lbl, sizeof(lbl), "G%d (I%u)", grayIdx, static_cast<unsigned>(ink));
        renderer.drawText(SMALL_FONT_ID, px + 4, py + PATCH_H + 3, lbl, true, EpdFontFamily::REGULAR);
      }
      if ((patchMask & (1u << ink)) != 0) {
        renderer.fillRect(px + 2, py + 2, PATCH_W - 4, PATCH_H - 4, clearBits);
      }
    }
  }

  if (isChromePass) {
    renderer.fillRect(24, 462, 480, 1, true);
  }

  const int rawLh = renderer.getLineHeight(readerFontId);
  const int lineStep = std::clamp((rawLh > 0 ? rawLh : 34) + 6, 30, 52);
  const int textY0 = 476;
  renderer.drawText(readerFontId, 24, textY0, CAL16_SAMPLE_LINES[0], true, EpdFontFamily::REGULAR);
  renderer.drawText(readerFontId, 24, textY0 + lineStep, CAL16_SAMPLE_LINES[1], true, EpdFontFamily::REGULAR);
  renderer.drawText(readerFontId, 24, textY0 + lineStep * 2, CAL16_SAMPLE_LINES[2], true, EpdFontFamily::ITALIC);
  renderer.drawText(readerFontId, 24, textY0 + lineStep * 3, CAL16_SAMPLE_LINES[3], true, EpdFontFamily::BOLD);
}

void handleCal16(GfxRenderer& renderer, char*& cursor) {
  RefreshKind refKind = RefreshKind::Clean;
  if (const char* tok = nextToken(cursor)) {
    if (!parseRefreshKind(tok, refKind)) {
      sendLineFmt("@DEV ERR CAL16 reason=\"bad_refresh_arg\"\n");
      return;
    }
  }

  const char* ctrlName = display.controllerName() ? display.controllerName() : "UNKNOWN";
  if (!display.devLutSupported()) {
    sendLineFmt("@DEV ERR CAL16 reason=\"unsupported_controller\" ctrl=\"%s\"\n", ctrlName);
    return;
  }

  RenderLock lock;
  if (!renderer.hasFrameBuffer() || !renderer.getFrameBuffer()) {
    sendLineFmt("@DEV ERR CAL16 reason=\"framebuffer_loaned\"\n");
    return;
  }
  if (renderer.getDisplayWidth() != 792 || renderer.getDisplayHeight() != 528) {
    sendLineFmt("@DEV ERR CAL16 reason=\"bad_panel_geometry\"\n");
    return;
  }

  struct Cal16Guard {
    GfxRenderer& renderer;
    GfxRenderer::Orientation prevOrient;
    bool prevInv;
    bool prevGray16Base;
    std::array<int, 4> prevClip;
    ~Cal16Guard() {
      renderer.endStripTarget();
      renderer.setRenderMode(GfxRenderer::BW);
      renderer.setGray16Base(prevGray16Base);
      renderer.setClipRect(prevClip[0], prevClip[1], prevClip[2], prevClip[3]);
      renderer.setOrientation(prevOrient);
      display.setInverted(prevInv);
    }
  } guard{renderer, renderer.getOrientation(), display.isInverted(), renderer.isGray16Base(), renderer.getClipRect()};

  // Clear Night Mode before checking supportsGray16(), which requires un-inverted output.
  display.setInverted(false);
  if (!renderer.supportsGray16()) {
    sendLineFmt("@DEV ERR CAL16 reason=\"unsupported_controller\" ctrl=\"%s\"\n", ctrlName);
    return;
  }

  constexpr int STRIP_ROWS = 80;
  const int gh = renderer.getDisplayHeight();
  const int gwBytes = renderer.getDisplayWidthBytes();
  auto scratch = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(gwBytes) * STRIP_ROWS);
  if (!scratch) {
    LOG_ERR("DEV", "CAL16 OOM (%d bytes)", gwBytes * STRIP_ROWS);
    sendLineFmt("@DEV ERR CAL16 reason=\"oom\"\n");
    return;
  }

  ScopedCal16Font calFont(renderer);
  const int readerFontId = calFont.fontId;
  const char* fontLabel = calFont.label;
  const uint8_t pointSize = calFont.pointSize;

  renderer.setOrientation(GfxRenderer::Portrait);
  renderer.setClipRect(0, 0, 528, 792);
  renderer.setGray16Base(true);

  std::optional<FontCacheManager::PrewarmScope> scope;
  if (auto* fcm = renderer.getFontCacheManager()) {
    scope.emplace(fcm->createPrewarmScope());
    drawCal16Pass(renderer, readerFontId, fontLabel, pointSize, GfxRenderer::BW);
    scope->endScanAndPrewarm();
  }

  activityManager.notifyDevFrameBufferOverwritten();
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.clearScreen(0xFF);
  drawCal16Pass(renderer, readerFontId, fontLabel, pointSize, GfxRenderer::BW);

  if (refKind == RefreshKind::Fast) {
    renderer.displayGrayscaleBase(HalDisplay::FAST_REFRESH, /*keepPowered=*/true);
  } else {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH, /*keepPowered=*/true);
    renderer.preconditionGrayscale();
  }

  auto streamPlane = [&](GfxRenderer::RenderMode mode, bool lsbPlane) {
    renderer.setRenderMode(mode);
    for (int y = 0; y < gh; y += STRIP_ROWS) {
      const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
      renderer.beginStripTarget(scratch.get(), y, rows);
      renderer.clearScreen(0x00);
      drawCal16Pass(renderer, readerFontId, fontLabel, pointSize, mode);
      renderer.endStripTarget();
      renderer.writeGrayscalePlaneStrip(lsbPlane, scratch.get(), y, rows);
    }
  };

  streamPlane(GfxRenderer::GRAY16_COARSE_LSB, true);
  streamPlane(GfxRenderer::GRAY16_COARSE_MSB, false);
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.displayGray16Pass1();

  streamPlane(GfxRenderer::GRAY16_FINE_LSB, true);
  streamPlane(GfxRenderer::GRAY16_FINE_MSB, false);
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.displayGray16Pass2();

  renderer.beginGray16Cleanup();
  renderer.setRenderMode(GfxRenderer::GRAY16_CLEANUP_DTM1);
  uint8_t* const fb = renderer.getFrameBuffer();
  for (int y = 0; y < gh; y += STRIP_ROWS) {
    const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
    const size_t stripBytes = static_cast<size_t>(rows) * gwBytes;
    renderer.beginStripTarget(scratch.get(), y, rows);
    renderer.clearScreen(0xFF);
    drawCal16Pass(renderer, readerFontId, fontLabel, pointSize, GfxRenderer::GRAY16_CLEANUP_DTM1);
    renderer.endStripTarget();
    renderer.writeGrayscalePlaneStrip(true, scratch.get(), y, rows);
    if (fb) {
      memcpy(fb + static_cast<size_t>(y) * gwBytes, scratch.get(), stripBytes);
    }
  }
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.finishGray16Cleanup();
  cal16OnScreen = true;

  uint8_t pllBase = freeink::x3_lut16::kPllBase;
  uint8_t pll1 = freeink::x3_lut16::kPllPass1Coarse;
  uint8_t pll2 = freeink::x3_lut16::kPllPass2Fine;
  uint8_t vdl2 = freeink::x3_lut16::kPwrPass2Fine[3];
  display.devLut16ReadParams(pllBase, pll1, pll2, vdl2);
  sendLineFmt("@DEV OK CAL16 refresh=\"%s\" font=\"%s\" pt=%u bpp=%u pll1=0x%02x pll2=0x%02x vdl2=0x%02x\n",
              refKind == RefreshKind::Fast ? "FAST" : "CLEAN", fontLabel, static_cast<unsigned>(pointSize),
              static_cast<unsigned>(activeFontBitsPerPixel(renderer, readerFontId)), static_cast<unsigned>(pll1),
              static_cast<unsigned>(pll2), static_cast<unsigned>(vdl2));
}

bool areSideButtonsSwapped(GfxRenderer::Orientation orient) {
  switch (orient) {
    case GfxRenderer::Portrait:
      return false;
    case GfxRenderer::PortraitInverted:
      return true;
    case GfxRenderer::LandscapeClockwise:
    case GfxRenderer::LandscapeCounterClockwise:
      return SETTINGS.sideButtonFollowOrientation != 0;
  }
  return false;
}

bool parseEncodingArg(char*& cursor, const char* cmdName, StreamEnc& encOut) {
  const char* arg = nextToken(cursor);
  if (!arg || strcasecmp(arg, "RAW") == 0) {
    encOut = StreamEnc::Raw;
    return true;
  }
  if (strcasecmp(arg, "B64") == 0) {
    encOut = StreamEnc::Base64;
    return true;
  }
  sendLineFmt("@DEV ERR %s reason=\"bad_encoding\"\n", cmdName);
  return false;
}

void handlePing() { sendLineFmt("@DEV OK PING ts=%lu ver=\"%s\"\n", millis(), CROSSPOINT_VERSION); }

void handleStatus(GfxRenderer& renderer) {
  RenderLock lock(RenderLock::Mode::Try);
  const bool renderBusy = !lock.ownsLock();
  const auto heap = HalMemory::getInternalHeap();
#if FREEINK_DEVICE_X4 || FREEINK_DEVICE_X3
  const char* devName = gpio.deviceIsX3() ? "X3" : "X4";
#else
  const char* devName = BoardConfig::ACTIVE.name ? BoardConfig::ACTIVE.name : "UNKNOWN";
#endif
  const char* boardName = BoardConfig::ACTIVE.name ? BoardConfig::ACTIVE.name : "unknown";
  const char* ctrlName = display.controllerName() ? display.controllerName() : "UNKNOWN";
  const char* actName = activityManager.currentActivityName();
  const size_t stack = activityManager.stackDepth();
  const bool hasFb = renderer.hasFrameBuffer() && renderer.getFrameBuffer() != nullptr;
  const char* fontName = (SETTINGS.sdFontFamilyName[0] != '\0') ? SETTINGS.sdFontFamilyName : "NotoSans";
  const int fontId = SETTINGS.getReaderFontId();
  const uint8_t fontBpp = lock.ownsLock() ? activeFontBitsPerPixel(renderer, fontId) : 0;

  int offset = snprintf(
      txBuf, sizeof(txBuf),
      "@DEV OK STATUS ver=\"%s\" dev=\"%s\" board=\"%s\" ctrl=\"%s\" w=%u h=%u orient=%u inv=%u fb=%u busy=%u "
      "heap_free=%zu heap_min=%zu heap_max_blk=%zu heap_total=%zu act=\"%s\" stack=%zu "
      "font=\"%s\" font_id=%d font_bpp=%u pt=%u aa=%u ls=%u margin=%u fade=%u img=%u "
      "lut_mode=\"%s\" gray16=\"%s\" gray16_clean=%u lut_mod=%u",
      CROSSPOINT_VERSION, devName, boardName, ctrlName, static_cast<unsigned>(renderer.getDisplayWidth()),
      static_cast<unsigned>(renderer.getDisplayHeight()), static_cast<unsigned>(renderer.getOrientation()),
      display.isInverted() ? 1u : 0u, hasFb ? 1u : 0u, renderBusy ? 1u : 0u, heap.freeBytes, heap.minFreeBytes,
      heap.largestBlockBytes, heap.totalBytes, actName, stack, fontName, fontId, static_cast<unsigned>(fontBpp),
      static_cast<unsigned>(SETTINGS.fontPointSize), SETTINGS.textAntiAliasing ? 1u : 0u,
      static_cast<unsigned>(SETTINGS.lineSpacing), static_cast<unsigned>(SETTINGS.screenMargin),
      SETTINGS.fadingFix ? 1u : 0u, static_cast<unsigned>(SETTINGS.imageRendering),
      grayscaleModeName(display.devGetGrayscaleModeOverride()), gray16ModeName(display.devGetGray16Mode()),
      display.devIsGray16CleanupActive() ? 1u : 0u, display.devLutModified() ? 1u : 0u);

  if (lock.ownsLock() && offset > 0 && static_cast<size_t>(offset) + 64 < sizeof(txBuf)) {
    const ScreenshotInfo info = activityManager.getScreenshotInfo();
    const char* readerKind = nullptr;
    switch (info.readerType) {
      case ScreenshotInfo::ReaderType::None:
        break;
      case ScreenshotInfo::ReaderType::Epub:
        readerKind = "epub";
        break;
      case ScreenshotInfo::ReaderType::Txt:
        readerKind = "txt";
        break;
      case ScreenshotInfo::ReaderType::Xtc:
        readerKind = "xtc";
        break;
    }
    if (readerKind) {
      offset += snprintf(txBuf + offset, sizeof(txBuf) - static_cast<size_t>(offset),
                         " reader=\"%s\" ch=%d page=%d pages=%d pct=%d", readerKind, info.spineIndex, info.currentPage,
                         info.totalPages, info.progressPercent);
    }
  }

  if (offset > 0) {
    const size_t pos =
        (static_cast<size_t>(offset) + 2 < sizeof(txBuf)) ? static_cast<size_t>(offset) : (sizeof(txBuf) - 2);
    txBuf[pos] = '\n';
    txBuf[pos + 1] = '\0';
    writeBulkCdc(reinterpret_cast<const uint8_t*>(txBuf), pos + 1);
  }
}

void handleSnap1(GfxRenderer& renderer, char*& cursor) {
  StreamEnc enc = StreamEnc::Raw;
  if (!parseEncodingArg(cursor, "SNAP1", enc)) return;

  RenderLock lock;
  if (!renderer.hasFrameBuffer() || !renderer.getFrameBuffer()) {
    sendLineFmt("@DEV ERR SNAP1 reason=\"framebuffer_loaned\"\n");
    return;
  }
  if (!cal16OnScreen && activityManager.isCurrentPageBufferStale()) {
    sendLineFmt("@DEV ERR SNAP1 reason=\"framebuffer_stale\"\n");
    return;
  }

  const uint8_t* fb = renderer.getFrameBuffer();
  const size_t bytes = renderer.getBufferSize();
  const uint32_t crc = esp_rom_crc32_le(0, fb, bytes);
  const char* encName = (enc == StreamEnc::Raw) ? "RAW" : "B64";

  if (!sendLineFmt("@SNAP1 ENC=%s W=%u H=%u STRIDE=%u BYTES=%zu ORIENT=%u INV=%u CRC32=%08" PRIx32 "\n", encName,
                   static_cast<unsigned>(renderer.getDisplayWidth()),
                   static_cast<unsigned>(renderer.getDisplayHeight()),
                   static_cast<unsigned>(renderer.getDisplayWidthBytes()), bytes,
                   static_cast<unsigned>(cal16OnScreen ? GfxRenderer::Portrait : renderer.getOrientation()),
                   cal16OnScreen ? 0u : (display.isInverted() ? 1u : 0u), crc)) {
    return;
  }

  switch (enc) {
    case StreamEnc::Raw:
      if (!writeBulkCdc(fb, bytes)) return;
      sendLineFmt("\n@END SNAP1 CRC32=%08" PRIx32 "\n", crc);
      return;
    case StreamEnc::Base64:
      if (!writeBulkBase64(fb, bytes)) return;
      sendLineFmt("@END SNAP1 CRC32=%08" PRIx32 "\n", crc);
      return;
  }
}

struct SnapStreamCtx {
  const char* tag = "SNAP2";
  const StripCaptureMeta* meta = nullptr;
  StreamEnc enc = StreamEnc::Raw;
  uint16_t width = 0;
  uint16_t height = 0;
  size_t stride = 0;
  size_t totalBytes = 0;
  unsigned orient = 0;
  unsigned inv = 0;
  uint32_t crc = 0;
  uint8_t b64Carry[48];
  uint8_t b64CarryLen = 0;
  bool headerSent = false;
};

SnapStreamCtx snapCtx;

bool emitSnapHeader(SnapStreamCtx& ctx, const char* srcName, uint16_t stripRows) {
  const char* encName = (ctx.enc == StreamEnc::Raw) ? "RAW" : "B64";
  return sendLineFmt("@%s ENC=%s SRC=%s STRIP_ROWS=%u W=%u H=%u STRIDE=%zu BYTES=%zu ORIENT=%u INV=%u\n", ctx.tag,
                     encName, srcName, static_cast<unsigned>(stripRows), static_cast<unsigned>(ctx.width),
                     static_cast<unsigned>(ctx.height), ctx.stride, ctx.totalBytes, ctx.orient, ctx.inv);
}

bool emitSnapBytes(SnapStreamCtx& ctx, const uint8_t* rowData, size_t len) {
  ctx.crc = esp_rom_crc32_le(ctx.crc, rowData, len);
  switch (ctx.enc) {
    case StreamEnc::Raw:
      return writeBulkCdc(rowData, len);
    case StreamEnc::Base64: {
      size_t idx = 0;
      while (idx < len) {
        const size_t space = sizeof(ctx.b64Carry) - ctx.b64CarryLen;
        const size_t take = (len - idx < space) ? (len - idx) : space;
        memcpy(ctx.b64Carry + ctx.b64CarryLen, rowData + idx, take);
        ctx.b64CarryLen = static_cast<uint8_t>(ctx.b64CarryLen + take);
        idx += take;
        if (ctx.b64CarryLen == sizeof(ctx.b64Carry)) {
          const size_t outLen = encodeBase64Chunk(ctx.b64Carry, sizeof(ctx.b64Carry), b64LineBuf);
          ctx.b64CarryLen = 0;
          if (!writeBulkCdc(reinterpret_cast<const uint8_t*>(b64LineBuf), outLen)) return false;
        }
      }
      return true;
    }
  }
  return false;
}

bool flushSnapEnd(SnapStreamCtx& ctx) {
  switch (ctx.enc) {
    case StreamEnc::Raw:
      return sendLineFmt("\n@END %s CRC32=%08" PRIx32 "\n", ctx.tag, ctx.crc);
    case StreamEnc::Base64:
      if (ctx.b64CarryLen > 0) {
        const size_t outLen = encodeBase64Chunk(ctx.b64Carry, ctx.b64CarryLen, b64LineBuf);
        ctx.b64CarryLen = 0;
        if (!writeBulkCdc(reinterpret_cast<const uint8_t*>(b64LineBuf), outLen)) return false;
      }
      return sendLineFmt("@END %s CRC32=%08" PRIx32 "\n", ctx.tag, ctx.crc);
  }
  return false;
}

bool snapRowCallback(void* rawCtx, const uint8_t* rowData, size_t len) {
  auto* ctx = static_cast<SnapStreamCtx*>(rawCtx);
  if (!ctx->headerSent) {
    const char* src = (ctx->meta && ctx->meta->srcTag) ? ctx->meta->srcTag : "AA";
    if (!emitSnapHeader(*ctx, src, ctx->meta ? ctx->meta->stripRows : 0)) return false;
    ctx->headerSent = true;
  }
  return emitSnapBytes(*ctx, rowData, len);
}

void handleSnap2(GfxRenderer& renderer, char*& cursor) {
  StreamEnc enc = StreamEnc::Raw;
  if (!parseEncodingArg(cursor, "SNAP2", enc)) return;

  RenderLock lock;
  if (!renderer.hasFrameBuffer() || !renderer.getFrameBuffer()) {
    sendLineFmt("@DEV ERR SNAP2 reason=\"framebuffer_loaned\"\n");
    return;
  }
  if (!cal16OnScreen && activityManager.isCurrentPageBufferStale()) {
    sendLineFmt("@DEV ERR SNAP2 reason=\"framebuffer_stale\"\n");
    return;
  }

  const uint16_t gwBytes = renderer.getDisplayWidthBytes();
  const size_t stride2bpp = static_cast<size_t>(gwBytes) * 2;
  if (stride2bpp > sizeof(fallbackRowBuf)) {
    sendLineFmt("@DEV ERR SNAP2 reason=\"panel_too_wide\"\n");
    return;
  }

  StripCaptureMeta meta;
  snapCtx = {};
  snapCtx.tag = "SNAP2";
  snapCtx.meta = &meta;
  snapCtx.enc = enc;
  snapCtx.width = renderer.getDisplayWidth();
  snapCtx.height = renderer.getDisplayHeight();
  snapCtx.stride = stride2bpp;
  snapCtx.totalBytes = stride2bpp * snapCtx.height;
  snapCtx.orient = static_cast<unsigned>(cal16OnScreen ? GfxRenderer::Portrait : renderer.getOrientation());
  snapCtx.inv = cal16OnScreen ? 0u : (display.isInverted() ? 1u : 0u);

  const bool aaOk = !cal16OnScreen && activityManager.captureComposite2bpp(&snapCtx, &snapRowCallback, meta);
  if (meta.framebufferStale) {
    sendLineFmt("@DEV ERR SNAP2 reason=\"framebuffer_stale\"\n");
    return;
  }
  if (meta.heapFloorHit) {
    sendLineFmt("@DEV ERR SNAP2 reason=\"heap_floor\" free=%zu\n", meta.freeBytes);
    return;
  }
  if (meta.streamAborted) {
    return;
  }

  if (aaOk && meta.usedAa) {
    flushSnapEnd(snapCtx);
    return;
  }

  if (!emitSnapHeader(snapCtx, "1BPP", 1)) return;
  const uint8_t* fb = renderer.getFrameBuffer();
  for (uint16_t y = 0; y < snapCtx.height; ++y) {
    const uint8_t* bwRow = fb + static_cast<size_t>(y) * gwBytes;
    for (uint16_t col = 0; col < gwBytes; ++col) {
      const uint8_t b = bwRow[col];
      fallbackRowBuf[col * 2] = static_cast<uint8_t>(SPREAD_NIBBLE[b >> 4] * 3u);
      fallbackRowBuf[col * 2 + 1] = static_cast<uint8_t>(SPREAD_NIBBLE[b & 0x0F] * 3u);
    }
    if (!emitSnapBytes(snapCtx, fallbackRowBuf, stride2bpp)) return;
  }
  flushSnapEnd(snapCtx);
}

bool streamCal16Snap4(GfxRenderer& renderer, SnapStreamCtx& ctx) {
  constexpr int CAL_STRIP_ROWS = 16;
  const int gh = renderer.getDisplayHeight();
  const int gwBytes = renderer.getDisplayWidthBytes();
  const size_t planeBytes = static_cast<size_t>(gwBytes) * CAL_STRIP_ROWS;
  auto scratch = makeUniqueNoThrow<uint8_t[]>(planeBytes * 5);
  if (!scratch) return false;

  struct CalSnapGuard {
    GfxRenderer& renderer;
    GfxRenderer::Orientation prevOrient;
    bool prevInv;
    bool prevGray16Base;
    std::array<int, 4> prevClip;
    ~CalSnapGuard() {
      renderer.endStripTarget();
      renderer.setRenderMode(GfxRenderer::BW);
      renderer.setGray16Base(prevGray16Base);
      renderer.setClipRect(prevClip[0], prevClip[1], prevClip[2], prevClip[3]);
      renderer.setOrientation(prevOrient);
      display.setInverted(prevInv);
    }
  } guard{renderer, renderer.getOrientation(), display.isInverted(), renderer.isGray16Base(), renderer.getClipRect()};

  ScopedCal16Font calFont(renderer);
  const int readerFontId = calFont.fontId;
  const char* fontLabel = calFont.label;
  const uint8_t pointSize = calFont.pointSize;

  display.setInverted(false);
  renderer.setOrientation(GfxRenderer::Portrait);
  renderer.setClipRect(0, 0, 528, 792);
  renderer.setGray16Base(true);

  std::optional<FontCacheManager::PrewarmScope> scope;
  if (auto* fcm = renderer.getFontCacheManager()) {
    scope.emplace(fcm->createPrewarmScope());
    drawCal16Pass(renderer, readerFontId, fontLabel, pointSize, GfxRenderer::BW);
    scope->endScanAndPrewarm();
  }

  if (!emitSnapHeader(ctx, "CAL16", CAL_STRIP_ROWS)) return true;
  ctx.headerSent = true;

  uint8_t* const bwBuf = scratch.get();
  uint8_t* const cLsb = bwBuf + planeBytes;
  uint8_t* const cMsb = cLsb + planeBytes;
  uint8_t* const fLsb = cMsb + planeBytes;
  uint8_t* const fMsb = fLsb + planeBytes;

  auto renderPlane = [&](GfxRenderer::RenderMode mode, uint8_t clearVal, uint8_t* dst, int y, int rows) {
    renderer.setRenderMode(mode);
    renderer.beginStripTarget(dst, y, rows);
    renderer.clearScreen(clearVal);
    drawCal16Pass(renderer, readerFontId, fontLabel, pointSize, mode);
    renderer.endStripTarget();
  };

  for (int y = 0; y < gh; y += CAL_STRIP_ROWS) {
    const int rows = (gh - y < CAL_STRIP_ROWS) ? (gh - y) : CAL_STRIP_ROWS;
    renderPlane(GfxRenderer::BW, 0xFF, bwBuf, y, rows);
    renderPlane(GfxRenderer::GRAY16_COARSE_LSB, 0x00, cLsb, y, rows);
    renderPlane(GfxRenderer::GRAY16_COARSE_MSB, 0x00, cMsb, y, rows);
    renderPlane(GfxRenderer::GRAY16_FINE_LSB, 0x00, fLsb, y, rows);
    renderPlane(GfxRenderer::GRAY16_FINE_MSB, 0x00, fMsb, y, rows);

    for (int r = 0; r < rows; ++r) {
      const size_t rowOff = static_cast<size_t>(r) * gwBytes;
      const uint8_t* const bwRow = bwBuf + rowOff;
      for (int col = 0; col < gwBytes; ++col) {
        const uint8_t bw = bwRow[col];
        const uint8_t cl = cLsb[rowOff + col];
        const uint8_t cm = cMsb[rowOff + col];
        const uint8_t fl = fLsb[rowOff + col];
        const uint8_t fm = fMsb[rowOff + col];
        for (int pair = 0; pair < 4; ++pair) {
          const int hiBit = 7 - pair * 2;
          const int loBit = 6 - pair * 2;
          const uint8_t c0 =
              static_cast<uint8_t>((((bw >> hiBit) & 1u) << 4) | (((cl >> hiBit) & 1u) << 3) |
                                   (((cm >> hiBit) & 1u) << 2) | (((fl >> hiBit) & 1u) << 1) | ((fm >> hiBit) & 1u));
          const uint8_t c1 =
              static_cast<uint8_t>((((bw >> loBit) & 1u) << 4) | (((cl >> loBit) & 1u) << 3) |
                                   (((cm >> loBit) & 1u) << 2) | (((fl >> loBit) & 1u) << 1) | ((fm >> loBit) & 1u));
          fallbackRowBuf[col * 4 + pair] =
              static_cast<uint8_t>((freeink::x3_lut16::kDecodeGray16[c0] << 4) | freeink::x3_lut16::kDecodeGray16[c1]);
        }
      }
      if (!emitSnapBytes(ctx, fallbackRowBuf, ctx.stride)) return true;
    }
  }
  flushSnapEnd(ctx);
  return true;
}

void handleSnap4(GfxRenderer& renderer, char*& cursor) {
  StreamEnc enc = StreamEnc::Raw;
  if (!parseEncodingArg(cursor, "SNAP4", enc)) return;

  RenderLock lock;
  if (!renderer.hasFrameBuffer() || !renderer.getFrameBuffer()) {
    sendLineFmt("@DEV ERR SNAP4 reason=\"framebuffer_loaned\"\n");
    return;
  }
  if (!cal16OnScreen && activityManager.isCurrentPageBufferStale()) {
    sendLineFmt("@DEV ERR SNAP4 reason=\"framebuffer_stale\"\n");
    return;
  }

  const uint16_t gwBytes = renderer.getDisplayWidthBytes();
  const size_t stride4bpp = static_cast<size_t>(gwBytes) * 4;
  if (stride4bpp > sizeof(fallbackRowBuf)) {
    sendLineFmt("@DEV ERR SNAP4 reason=\"panel_too_wide\"\n");
    return;
  }

  StripCaptureMeta meta;
  snapCtx = {};
  snapCtx.tag = "SNAP4";
  snapCtx.meta = &meta;
  snapCtx.enc = enc;
  snapCtx.width = renderer.getDisplayWidth();
  snapCtx.height = renderer.getDisplayHeight();
  snapCtx.stride = stride4bpp;
  snapCtx.totalBytes = stride4bpp * snapCtx.height;
  snapCtx.orient = static_cast<unsigned>(cal16OnScreen ? GfxRenderer::Portrait : renderer.getOrientation());
  snapCtx.inv = cal16OnScreen ? 0u : (display.isInverted() ? 1u : 0u);

  if (cal16OnScreen && streamCal16Snap4(renderer, snapCtx)) {
    return;
  }

  const bool aaOk = !cal16OnScreen && activityManager.captureComposite4bpp(&snapCtx, &snapRowCallback, meta);
  if (meta.framebufferStale) {
    sendLineFmt("@DEV ERR SNAP4 reason=\"framebuffer_stale\"\n");
    return;
  }
  if (meta.heapFloorHit) {
    sendLineFmt("@DEV ERR SNAP4 reason=\"heap_floor\" free=%zu\n", meta.freeBytes);
    return;
  }
  if (meta.streamAborted) {
    return;
  }

  if (aaOk && meta.usedAa) {
    flushSnapEnd(snapCtx);
    return;
  }

  if (!emitSnapHeader(snapCtx, "1BPP", 1)) return;
  const uint8_t* fb = renderer.getFrameBuffer();
  for (uint16_t y = 0; y < snapCtx.height; ++y) {
    const uint8_t* bwRow = fb + static_cast<size_t>(y) * gwBytes;
    for (uint16_t col = 0; col < gwBytes; ++col) {
      const uint8_t b = bwRow[col];
      for (int pair = 0; pair < 4; ++pair) {
        const uint8_t n0 = ((b >> (7 - pair * 2)) & 1u) ? 0x0Fu : 0x00u;
        const uint8_t n1 = ((b >> (6 - pair * 2)) & 1u) ? 0x0Fu : 0x00u;
        fallbackRowBuf[col * 4 + pair] = static_cast<uint8_t>((n0 << 4) | n1);
      }
    }
    if (!emitSnapBytes(snapCtx, fallbackRowBuf, stride4bpp)) return;
  }
  flushSnapEnd(snapCtx);
}

void handleGray16(GfxRenderer& renderer, char*& cursor) {
  const char* tok = nextToken(cursor);
  RefreshKind refresh = RefreshKind::None;
  bool setMode = false;
  HalDisplay::DevGray16Mode mode = display.devGetGray16Mode();

  if (tok && strcasecmp(tok, "STATUS") != 0) {
    if (strcasecmp(tok, "AUTO") == 0) {
      mode = HalDisplay::DevGray16Mode::Auto;
      setMode = true;
    } else if (strcasecmp(tok, "ON") == 0) {
      mode = HalDisplay::DevGray16Mode::ForceOn;
      setMode = true;
    } else if (strcasecmp(tok, "OFF") == 0) {
      mode = HalDisplay::DevGray16Mode::ForceOff;
      setMode = true;
    } else {
      sendLineFmt("@DEV ERR GRAY16 reason=\"bad_mode\"\n");
      return;
    }
  }

  if (const char* refTok = nextToken(cursor)) {
    if (!parseRefreshKind(refTok, refresh)) {
      sendLineFmt("@DEV ERR GRAY16 reason=\"bad_refresh_arg\"\n");
      return;
    }
  }

  uint8_t pllBase = freeink::x3_lut16::kPllBase;
  uint8_t pll1 = freeink::x3_lut16::kPllPass1Coarse;
  uint8_t pll2 = freeink::x3_lut16::kPllPass2Fine;
  uint8_t vdl2 = freeink::x3_lut16::kPwrPass2Fine[3];
  bool supported = false;
  bool cleanupActive = false;
  uint8_t fontBpp = 1;
  {
    RenderLock lock;
    if (setMode) {
      display.devSetGray16Mode(mode);
    }
    supported = renderer.supportsGray16();
    cleanupActive = display.devIsGray16CleanupActive();
    display.devLut16ReadParams(pllBase, pll1, pll2, vdl2);
    fontBpp = activeFontBitsPerPixel(renderer, SETTINGS.getReaderFontId());
  }

  triggerDevRefresh(renderer, refresh);
  sendLineFmt(
      "@DEV OK GRAY16 mode=\"%s\" supported=%u cleanup_active=%u font_bpp=%u pll1=0x%02x pll2=0x%02x vdl2=0x%02x\n",
      gray16ModeName(mode), supported ? 1u : 0u, cleanupActive ? 1u : 0u, static_cast<unsigned>(fontBpp),
      static_cast<unsigned>(pll1), static_cast<unsigned>(pll2), static_cast<unsigned>(vdl2));
}

void handleLut16(GfxRenderer& renderer, char*& cursor) {
  const char* op = nextToken(cursor);
  if (!op) op = "SHOW";

  const char* ctrlName = display.controllerName() ? display.controllerName() : "UNKNOWN";
  if (!display.devLutSupported()) {
    sendLineFmt("@DEV ERR LUT16 reason=\"unsupported_controller\" ctrl=\"%s\"\n", ctrlName);
    return;
  }

  if (strcasecmp(op, "SHOW") == 0) {
    bool filterSingleBank = false;
    HalDisplay::DevLutBank onlyBank = HalDisplay::DevLutBank::Lut16Pass1;
    if (const char* bankTok = nextToken(cursor)) {
      if (!parseLutBank(bankTok, onlyBank)) {
        sendLineFmt("@DEV ERR LUT16 reason=\"bad_bank\"\n");
        return;
      }
      filterSingleBank = true;
    }

    uint8_t pllBase = freeink::x3_lut16::kPllBase;
    uint8_t pll1 = freeink::x3_lut16::kPllPass1Coarse;
    uint8_t pll2 = freeink::x3_lut16::kPllPass2Fine;
    uint8_t vdl2 = freeink::x3_lut16::kPwrPass2Fine[3];
    bool modified = false;
    {
      RenderLock lock;
      display.devLut16ReadParams(pllBase, pll1, pll2, vdl2);
      modified = display.devLut16Modified();
    }
    if (!sendLineFmt("@LUT16 PARAMS pll_base=0x%02x pll1=0x%02x pll2=0x%02x vdl2=0x%02x modified=%u\n",
                     static_cast<unsigned>(pllBase), static_cast<unsigned>(pll1), static_cast<unsigned>(pll2),
                     static_cast<unsigned>(vdl2), modified ? 1u : 0u)) {
      return;
    }

    static constexpr HalDisplay::DevLutBank BANKS16[3] = {
        HalDisplay::DevLutBank::Lut16Pre, HalDisplay::DevLutBank::Lut16Pass1, HalDisplay::DevLutBank::Lut16Pass2};
    static constexpr HalDisplay::DevLutRole ALL_ROLES[HalDisplay::DEV_LUT_ROLES] = {
        HalDisplay::DevLutRole::Vcom, HalDisplay::DevLutRole::Ww, HalDisplay::DevLutRole::Bw,
        HalDisplay::DevLutRole::Wb, HalDisplay::DevLutRole::Bb};
    static constexpr char HEX_DIGITS[] = "0123456789abcdef";

    for (const auto bank : BANKS16) {
      if (filterSingleBank && bank != onlyBank) continue;
      for (const auto role : ALL_ROLES) {
        {
          RenderLock lock;
          if (!display.devLutRead(bank, role, lutRowScratch)) {
            sendLineFmt("@DEV ERR LUT16 reason=\"read_failed\"\n");
            return;
          }
        }
        for (size_t i = 0; i < HalDisplay::DEV_LUT_LEN; ++i) {
          lutHexScratch[i * 2] = HEX_DIGITS[lutRowScratch[i] >> 4];
          lutHexScratch[i * 2 + 1] = HEX_DIGITS[lutRowScratch[i] & 0x0F];
        }
        lutHexScratch[HalDisplay::DEV_LUT_LEN * 2] = '\0';
        if (!sendLineFmt("@LUT BANK=%s ROLE=%s HEX=%s\n", lutBankName(bank), lutRoleName(role), lutHexScratch)) return;
      }
    }
    sendLineFmt("@DEV OK LUT16 op=\"SHOW\" pll1=0x%02x pll2=0x%02x vdl2=0x%02x modified=%u\n",
                static_cast<unsigned>(pll1), static_cast<unsigned>(pll2), static_cast<unsigned>(vdl2),
                modified ? 1u : 0u);
    return;
  }

  if (strcasecmp(op, "TUNE") == 0) {
    const char* pll1Tok = nextToken(cursor);
    const char* pll2Tok = nextToken(cursor);
    const char* vdl2Tok = nextToken(cursor);
    uint8_t pll1 = 0, pll2 = 0, vdl2 = 0;
    if (!parseUint8Hex(pll1Tok, pll1) || !parseUint8Hex(pll2Tok, pll2) || !parseUint8Hex(vdl2Tok, vdl2)) {
      sendLineFmt("@DEV ERR LUT16 reason=\"bad_tune_args\"\n");
      return;
    }
    RefreshKind refresh = RefreshKind::None;
    if (const char* refTok = nextToken(cursor)) {
      if (!parseRefreshKind(refTok, refresh)) {
        sendLineFmt("@DEV ERR LUT16 reason=\"bad_refresh_arg\"\n");
        return;
      }
    }
    uint8_t pllBase = freeink::x3_lut16::kPllBase;
    {
      RenderLock lock;
      display.devLut16Tune(pll1, pll2, vdl2);
      display.devLut16ReadParams(pllBase, pll1, pll2, vdl2);
    }
    triggerDevRefresh(renderer, refresh);
    sendLineFmt("@DEV OK LUT16 op=\"TUNE\" pll1=0x%02x pll2=0x%02x vdl2=0x%02x\n", static_cast<unsigned>(pll1),
                static_cast<unsigned>(pll2), static_cast<unsigned>(vdl2));
    return;
  }

  if (strcasecmp(op, "RESET") == 0) {
    RefreshKind refresh = RefreshKind::None;
    if (const char* refTok = nextToken(cursor)) {
      if (!parseRefreshKind(refTok, refresh)) {
        sendLineFmt("@DEV ERR LUT16 reason=\"bad_refresh_arg\"\n");
        return;
      }
    }
    uint8_t pllBase = freeink::x3_lut16::kPllBase;
    uint8_t pll1 = freeink::x3_lut16::kPllPass1Coarse;
    uint8_t pll2 = freeink::x3_lut16::kPllPass2Fine;
    uint8_t vdl2 = freeink::x3_lut16::kPwrPass2Fine[3];
    {
      RenderLock lock;
      display.devLut16Reset();
      display.devLut16ReadParams(pllBase, pll1, pll2, vdl2);
    }
    triggerDevRefresh(renderer, refresh);
    sendLineFmt("@DEV OK LUT16 op=\"RESET\" pll1=0x%02x pll2=0x%02x vdl2=0x%02x\n", static_cast<unsigned>(pll1),
                static_cast<unsigned>(pll2), static_cast<unsigned>(vdl2));
    return;
  }

  sendLineFmt("@DEV ERR LUT16 reason=\"unknown_op\"\n");
}

void handleKey(GfxRenderer& renderer, char*& cursor) {
  char* btnTok = nextToken(cursor);
  if (!btnTok) {
    sendLineFmt("@DEV ERR KEY reason=\"missing_btn\"\n");
    return;
  }
  for (char* p = btnTok; *p != '\0'; ++p) {
    *p = static_cast<char>(std::toupper(static_cast<unsigned char>(*p)));
  }

  uint8_t rawBtn = 0xFF;
  bool isLogicalPower = false;
  const bool sideSwapped = areSideButtonsSwapped(renderer.getOrientation());

  if (strcmp(btnTok, "BACK") == 0) {
    rawBtn = SETTINGS.frontButtonBack;
  } else if (strcmp(btnTok, "CONFIRM") == 0 || strcmp(btnTok, "OK") == 0) {
    rawBtn = SETTINGS.frontButtonConfirm;
  } else if (strcmp(btnTok, "LEFT") == 0) {
    rawBtn = SETTINGS.frontButtonLeft;
  } else if (strcmp(btnTok, "RIGHT") == 0) {
    rawBtn = SETTINGS.frontButtonRight;
  } else if (strcmp(btnTok, "UP") == 0) {
    rawBtn = HalGPIO::BTN_UP;
  } else if (strcmp(btnTok, "DOWN") == 0) {
    rawBtn = HalGPIO::BTN_DOWN;
  } else if (strcmp(btnTok, "POWER") == 0) {
    rawBtn = HalGPIO::BTN_POWER;
    isLogicalPower = true;
  } else if (strcmp(btnTok, "NEXT") == 0 || strcmp(btnTok, "PAGE_FORWARD") == 0) {
    switch (SETTINGS.sideButtonLayout) {
      case CrossPointSettings::PREV_NEXT:
        rawBtn = sideSwapped ? HalGPIO::BTN_UP : HalGPIO::BTN_DOWN;
        break;
      case CrossPointSettings::NEXT_PREV:
        rawBtn = sideSwapped ? HalGPIO::BTN_DOWN : HalGPIO::BTN_UP;
        break;
      case CrossPointSettings::NEXT_NEXT:
        rawBtn = HalGPIO::BTN_DOWN;
        break;
      default:
        sendLineFmt("@DEV ERR KEY reason=\"side_button_disabled\"\n");
        return;
    }
  } else if (strcmp(btnTok, "PREV") == 0 || strcmp(btnTok, "PAGE_BACK") == 0) {
    switch (SETTINGS.sideButtonLayout) {
      case CrossPointSettings::PREV_NEXT:
        rawBtn = sideSwapped ? HalGPIO::BTN_DOWN : HalGPIO::BTN_UP;
        break;
      case CrossPointSettings::NEXT_PREV:
        rawBtn = sideSwapped ? HalGPIO::BTN_UP : HalGPIO::BTN_DOWN;
        break;
      case CrossPointSettings::PREV_PREV:
        rawBtn = HalGPIO::BTN_UP;
        break;
      default:
        sendLineFmt("@DEV ERR KEY reason=\"side_button_disabled\"\n");
        return;
    }
  } else if (strcmp(btnTok, "RAW_BACK") == 0) {
    rawBtn = HalGPIO::BTN_BACK;
  } else if (strcmp(btnTok, "RAW_CONFIRM") == 0) {
    rawBtn = HalGPIO::BTN_CONFIRM;
  } else if (strcmp(btnTok, "RAW_LEFT") == 0) {
    rawBtn = HalGPIO::BTN_LEFT;
  } else if (strcmp(btnTok, "RAW_RIGHT") == 0) {
    rawBtn = HalGPIO::BTN_RIGHT;
  } else if (strcmp(btnTok, "RAW_UP") == 0) {
    rawBtn = HalGPIO::BTN_UP;
  } else if (strcmp(btnTok, "RAW_DOWN") == 0) {
    rawBtn = HalGPIO::BTN_DOWN;
  } else if (strcmp(btnTok, "RAW_POWER") == 0) {
    rawBtn = HalGPIO::BTN_POWER;
  } else {
    sendLineFmt("@DEV ERR KEY reason=\"unknown_btn\" btn=\"%s\"\n", btnTok);
    return;
  }

  if (rawBtn > HalGPIO::BTN_POWER) {
    sendLineFmt("@DEV ERR KEY reason=\"invalid_raw_btn\"\n");
    return;
  }

  uint16_t holdMs = KEY_DEFAULT_HOLD_MS;
  if (const char* holdTok = nextToken(cursor)) {
    uint32_t parsedHold = 0;
    if (!parseUint32Dec(holdTok, parsedHold)) {
      sendLineFmt("@DEV ERR KEY reason=\"bad_hold_ms\"\n");
      return;
    }
    if (parsedHold < KEY_MIN_HOLD_MS) parsedHold = KEY_MIN_HOLD_MS;
    if (parsedHold > KEY_MAX_HOLD_MS) parsedHold = KEY_MAX_HOLD_MS;
    holdMs = static_cast<uint16_t>(parsedHold);
  }
  if (isLogicalPower && holdMs > KEY_POWER_SAFE_MAX_MS) {
    holdMs = KEY_POWER_SAFE_MAX_MS;
  }

  if (keyState.count >= KEY_QUEUE_CAPACITY) {
    sendLineFmt("@DEV ERR KEY reason=\"queue_full\"\n");
    return;
  }

  cal16OnScreen = false;
  const uint8_t tail = static_cast<uint8_t>((keyState.head + keyState.count) % KEY_QUEUE_CAPACITY);
  keyState.queue[tail] = {rawBtn, holdMs};
  ++keyState.count;

  sendLineFmt("@DEV OK KEY btn=\"%s\" raw=%u hold_ms=%u\n", btnTok, static_cast<unsigned>(rawBtn),
              static_cast<unsigned>(holdMs));
}

void handleLut(GfxRenderer& renderer, char*& cursor) {
  const char* op = nextToken(cursor);
  if (!op) {
    sendLineFmt("@DEV ERR LUT reason=\"missing_op\"\n");
    return;
  }

  const char* ctrlName = display.controllerName() ? display.controllerName() : "UNKNOWN";

  if (strcasecmp(op, "SHOW") == 0) {
    HalDisplay::DevLutBank bank = HalDisplay::DevLutBank::Gc;
    if (const char* bankTok = nextToken(cursor)) {
      if (!parseLutBank(bankTok, bank)) {
        sendLineFmt("@DEV ERR LUT reason=\"bad_bank\"\n");
        return;
      }
    }
    if (!display.devLutSupported()) {
      sendLineFmt("@DEV ERR LUT reason=\"unsupported_controller\" ctrl=\"%s\"\n", ctrlName);
      return;
    }

    const char* bName = lutBankName(bank);
    const char* mName = grayscaleModeName(display.devGetGrayscaleModeOverride());
    if (!sendLineFmt("@LUT BANK=%s CTRL=\"%s\" MODE=\"%s\"\n", bName, ctrlName, mName)) return;

    static constexpr HalDisplay::DevLutRole ALL_ROLES[HalDisplay::DEV_LUT_ROLES] = {
        HalDisplay::DevLutRole::Vcom, HalDisplay::DevLutRole::Ww, HalDisplay::DevLutRole::Bw,
        HalDisplay::DevLutRole::Wb, HalDisplay::DevLutRole::Bb};
    static constexpr char HEX_DIGITS[] = "0123456789abcdef";

    for (const auto role : ALL_ROLES) {
      {
        RenderLock lock;
        if (!display.devLutRead(bank, role, lutRowScratch)) {
          sendLineFmt("@DEV ERR LUT reason=\"read_failed\"\n");
          return;
        }
      }
      for (size_t i = 0; i < HalDisplay::DEV_LUT_LEN; ++i) {
        lutHexScratch[i * 2] = HEX_DIGITS[lutRowScratch[i] >> 4];
        lutHexScratch[i * 2 + 1] = HEX_DIGITS[lutRowScratch[i] & 0x0F];
      }
      lutHexScratch[HalDisplay::DEV_LUT_LEN * 2] = '\0';
      if (!sendLineFmt("@LUT ROLE=%s HEX=%s\n", lutRoleName(role), lutHexScratch)) return;
    }

    sendLineFmt("@DEV OK LUT op=\"SHOW\" bank=\"%s\"\n", bName);
    return;
  }

  if (strcasecmp(op, "GC") == 0) {
    const char* lightTpTok = nextToken(cursor);
    const char* darkTpTok = nextToken(cursor);
    uint8_t lightTp = 0;
    uint8_t darkTp = 0;
    if (!parseUint8Dec(lightTpTok, lightTp) || !parseUint8Dec(darkTpTok, darkTp)) {
      sendLineFmt("@DEV ERR LUT reason=\"bad_gc_args\"\n");
      return;
    }

    uint8_t lvLight = 0x80;
    uint8_t lvDark = 0x20;
    RefreshKind refresh = RefreshKind::None;

    if (const char* opt1 = nextToken(cursor)) {
      if (!parseRefreshKind(opt1, refresh)) {
        if (!parseUint8Hex(opt1, lvLight)) {
          sendLineFmt("@DEV ERR LUT reason=\"bad_lv_light\"\n");
          return;
        }
        if (const char* opt2 = nextToken(cursor)) {
          if (!parseRefreshKind(opt2, refresh)) {
            if (!parseUint8Hex(opt2, lvDark)) {
              sendLineFmt("@DEV ERR LUT reason=\"bad_lv_dark\"\n");
              return;
            }
            if (const char* opt3 = nextToken(cursor)) {
              if (!parseRefreshKind(opt3, refresh)) {
                sendLineFmt("@DEV ERR LUT reason=\"bad_refresh_arg\"\n");
                return;
              }
            }
          }
        }
      }
    }

    bool ok = false;
    {
      RenderLock lock;
      ok = display.devLutApplyGcQuick(lightTp, darkTp, lvLight, lvDark);
    }
    if (!ok) {
      sendLineFmt("@DEV ERR LUT reason=\"unsupported_controller\" ctrl=\"%s\"\n", ctrlName);
      return;
    }
    triggerDevRefresh(renderer, refresh);
    sendLineFmt("@DEV OK LUT op=\"GC\" light_tp=%u dark_tp=%u lv_light=0x%02x lv_dark=0x%02x\n",
                static_cast<unsigned>(lightTp), static_cast<unsigned>(darkTp), static_cast<unsigned>(lvLight),
                static_cast<unsigned>(lvDark));
    return;
  }

  if (strcasecmp(op, "GROUP") == 0) {
    const char* bankTok = nextToken(cursor);
    const char* roleTok = nextToken(cursor);
    const char* grpTok = nextToken(cursor);
    const char* lvlTok = nextToken(cursor);
    const char* tp0Tok = nextToken(cursor);
    const char* tp1Tok = nextToken(cursor);
    const char* tp2Tok = nextToken(cursor);
    const char* tp3Tok = nextToken(cursor);
    const char* rpTok = nextToken(cursor);

    HalDisplay::DevLutBank bank = HalDisplay::DevLutBank::Gc;
    uint8_t group = 0;
    uint8_t level = 0;
    uint8_t tp0 = 0;
    uint8_t tp1 = 0;
    uint8_t tp2 = 0;
    uint8_t tp3 = 0;
    uint8_t rp = 0;

    if (!parseLutBank(bankTok, bank) || !roleTok || !parseUint8Dec(grpTok, group) ||
        group >= HalDisplay::DEV_LUT_GROUPS || !parseUint8Hex(lvlTok, level) || !parseUint8Dec(tp0Tok, tp0) ||
        !parseUint8Dec(tp1Tok, tp1) || !parseUint8Dec(tp2Tok, tp2) || !parseUint8Dec(tp3Tok, tp3) ||
        !parseUint8Dec(rpTok, rp)) {
      sendLineFmt("@DEV ERR LUT reason=\"bad_group_args\"\n");
      return;
    }

    RefreshKind refresh = RefreshKind::None;
    if (const char* refTok = nextToken(cursor)) {
      if (!parseRefreshKind(refTok, refresh)) {
        sendLineFmt("@DEV ERR LUT reason=\"bad_refresh_arg\"\n");
        return;
      }
    }

    bool ok = false;
    const char* roleOut = "timings";
    if (strcasecmp(roleTok, "timings") == 0) {
      RenderLock lock;
      ok = display.devLutWriteTimings(bank, group, tp0, tp1, tp2, tp3, rp);
    } else {
      HalDisplay::DevLutRole role = HalDisplay::DevLutRole::Vcom;
      if (!parseLutRole(roleTok, role)) {
        sendLineFmt("@DEV ERR LUT reason=\"bad_role\"\n");
        return;
      }
      roleOut = lutRoleName(role);
      RenderLock lock;
      ok = display.devLutWriteGroup(bank, role, group, level, tp0, tp1, tp2, tp3, rp);
    }

    if (!ok) {
      sendLineFmt("@DEV ERR LUT reason=\"unsupported_controller\" ctrl=\"%s\"\n", ctrlName);
      return;
    }
    triggerDevRefresh(renderer, refresh);
    sendLineFmt(
        "@DEV OK LUT op=\"GROUP\" bank=\"%s\" role=\"%s\" group=%u level=0x%02x tp0=%u tp1=%u tp2=%u tp3=%u rp=%u\n",
        lutBankName(bank), roleOut, static_cast<unsigned>(group), static_cast<unsigned>(level),
        static_cast<unsigned>(tp0), static_cast<unsigned>(tp1), static_cast<unsigned>(tp2), static_cast<unsigned>(tp3),
        static_cast<unsigned>(rp));
    return;
  }

  if (strcasecmp(op, "RAW") == 0) {
    const char* bankTok = nextToken(cursor);
    const char* roleTok = nextToken(cursor);
    const char* hexTok = nextToken(cursor);

    HalDisplay::DevLutBank bank = HalDisplay::DevLutBank::Gc;
    HalDisplay::DevLutRole role = HalDisplay::DevLutRole::Vcom;
    size_t decodedLen = 0;

    if (!parseLutBank(bankTok, bank) || !parseLutRole(roleTok, role) || !hexTok ||
        !decodeHexBytes(hexTok, lutRowScratch, sizeof(lutRowScratch), decodedLen) ||
        decodedLen != HalDisplay::DEV_LUT_LEN) {
      sendLineFmt("@DEV ERR LUT reason=\"bad_raw_args\"\n");
      return;
    }

    RefreshKind refresh = RefreshKind::None;
    if (const char* refTok = nextToken(cursor)) {
      if (!parseRefreshKind(refTok, refresh)) {
        sendLineFmt("@DEV ERR LUT reason=\"bad_refresh_arg\"\n");
        return;
      }
    }

    bool ok = false;
    {
      RenderLock lock;
      ok = display.devLutWriteRaw(bank, role, lutRowScratch);
    }
    if (!ok) {
      sendLineFmt("@DEV ERR LUT reason=\"unsupported_controller\" ctrl=\"%s\"\n", ctrlName);
      return;
    }
    triggerDevRefresh(renderer, refresh);
    sendLineFmt("@DEV OK LUT op=\"RAW\" bank=\"%s\" role=\"%s\" bytes=%zu\n", lutBankName(bank), lutRoleName(role),
                HalDisplay::DEV_LUT_LEN);
    return;
  }

  if (strcasecmp(op, "MODE") == 0) {
    const char* modeTok = nextToken(cursor);
    if (!modeTok) {
      sendLineFmt("@DEV ERR LUT reason=\"missing_mode\"\n");
      return;
    }
    HalDisplay::GrayscaleMode mode = HalDisplay::GrayscaleMode::Overlay;
    if (strcasecmp(modeTok, "OVERLAY") == 0 || strcasecmp(modeTok, "DEFAULT") == 0) {
      mode = HalDisplay::GrayscaleMode::Overlay;
    } else if (strcasecmp(modeTok, "ABSOLUTE") == 0) {
      mode = HalDisplay::GrayscaleMode::Absolute;
    } else if (strcasecmp(modeTok, "DIRECT") == 0) {
      mode = HalDisplay::GrayscaleMode::Direct;
    } else {
      sendLineFmt("@DEV ERR LUT reason=\"bad_mode\"\n");
      return;
    }

    RefreshKind refresh = RefreshKind::None;
    if (const char* refTok = nextToken(cursor)) {
      if (!parseRefreshKind(refTok, refresh)) {
        sendLineFmt("@DEV ERR LUT reason=\"bad_refresh_arg\"\n");
        return;
      }
    }

    if (!display.devLutSupported()) {
      sendLineFmt("@DEV ERR LUT reason=\"unsupported_controller\" ctrl=\"%s\"\n", ctrlName);
      return;
    }

    {
      RenderLock lock;
      display.devSetGrayscaleModeOverride(mode);
    }
    triggerDevRefresh(renderer, refresh);
    sendLineFmt("@DEV OK LUT op=\"MODE\" mode=\"%s\"\n", grayscaleModeName(mode));
    return;
  }

  if (strcasecmp(op, "REFRESH") == 0) {
    RefreshKind refresh = RefreshKind::Clean;
    if (const char* refTok = nextToken(cursor)) {
      if (!parseRefreshKind(refTok, refresh)) {
        sendLineFmt("@DEV ERR LUT reason=\"bad_refresh_arg\"\n");
        return;
      }
    }
    triggerDevRefresh(renderer, refresh);
    sendLineFmt("@DEV OK LUT op=\"REFRESH\" kind=\"%s\"\n", refresh == RefreshKind::Clean ? "CLEAN" : "FAST");
    return;
  }

  if (strcasecmp(op, "RESET") == 0) {
    const char* targetTok = nextToken(cursor);
    RefreshKind refresh = RefreshKind::None;
    if (const char* refTok = nextToken(cursor)) {
      if (!parseRefreshKind(refTok, refresh)) {
        sendLineFmt("@DEV ERR LUT reason=\"bad_refresh_arg\"\n");
        return;
      }
    }

    bool ok = false;
    const char* targetName = "ALL";
    if (!targetTok || strcasecmp(targetTok, "ALL") == 0) {
      RenderLock lock;
      ok = display.devLutResetAll();
    } else {
      HalDisplay::DevLutBank bank = HalDisplay::DevLutBank::Gc;
      if (!parseLutBank(targetTok, bank)) {
        sendLineFmt("@DEV ERR LUT reason=\"bad_target\"\n");
        return;
      }
      targetName = lutBankName(bank);
      RenderLock lock;
      ok = display.devLutReset(bank);
    }

    if (!ok) {
      sendLineFmt("@DEV ERR LUT reason=\"unsupported_controller\" ctrl=\"%s\"\n", ctrlName);
      return;
    }
    triggerDevRefresh(renderer, refresh);
    sendLineFmt("@DEV OK LUT op=\"RESET\" target=\"%s\"\n", targetName);
    return;
  }

  if (strcasecmp(op, "COMPARE") == 0) {
    handleLutCompare(renderer, cursor, ctrlName);
    return;
  }

  if (strcasecmp(op, "SPLIT_READER") == 0) {
    const char* tok = nextToken(cursor);
    RefreshKind refresh = RefreshKind::None;

    if (tok && strcasecmp(tok, "OFF") == 0) {
      if (const char* refTok = nextToken(cursor)) {
        if (!parseRefreshKind(refTok, refresh)) {
          sendLineFmt("@DEV ERR LUT reason=\"bad_refresh_arg\"\n");
          return;
        }
      }
      {
        RenderLock lock;
        auto cfg = display.devGetSplitReader();
        cfg.enabled = false;
        display.devSetSplitReader(cfg);
      }
      triggerDevRefresh(renderer, refresh);
      sendLineFmt("@DEV OK LUT op=\"SPLIT_READER\" enabled=0\n");
      return;
    }

    if (tok && strcasecmp(tok, "ON") == 0) {
      tok = nextToken(cursor);
    }

    HalDisplay::DevSplitReaderConfig cfg;
    cfg.enabled = true;

    if (tok) {
      if (!parseRefreshKind(tok, refresh)) {
        const char* toks[6] = {
            tok, nextToken(cursor), nextToken(cursor), nextToken(cursor), nextToken(cursor), nextToken(cursor)};
        for (int b = 0; b < 3; ++b) {
          if (!parseUint8Dec(toks[b * 2], cfg.lightTp[b]) || !parseUint8Dec(toks[b * 2 + 1], cfg.darkTp[b])) {
            sendLineFmt("@DEV ERR LUT reason=\"bad_split_reader_args\"\n");
            return;
          }
        }
        if (const char* refTok = nextToken(cursor)) {
          if (!parseRefreshKind(refTok, refresh)) {
            sendLineFmt("@DEV ERR LUT reason=\"bad_refresh_arg\"\n");
            return;
          }
        }
      }
    }

    if (!display.devLutSupported()) {
      sendLineFmt("@DEV ERR LUT reason=\"unsupported_controller\" ctrl=\"%s\"\n", ctrlName);
      return;
    }

    {
      RenderLock lock;
      display.devSetSplitReader(cfg);
    }
    triggerDevRefresh(renderer, refresh);
    sendLineFmt("@DEV OK LUT op=\"SPLIT_READER\" enabled=1 bands=\"%u/%u,%u/%u,%u/%u\"\n",
                static_cast<unsigned>(cfg.lightTp[0]), static_cast<unsigned>(cfg.darkTp[0]),
                static_cast<unsigned>(cfg.lightTp[1]), static_cast<unsigned>(cfg.darkTp[1]),
                static_cast<unsigned>(cfg.lightTp[2]), static_cast<unsigned>(cfg.darkTp[2]));
    return;
  }

  sendLineFmt("@DEV ERR LUT reason=\"unknown_op\"\n");
}

void abortPutTransfer(bool removePartial) {
  if (putState.file.isOpen()) {
    putState.file.close();
  }
  putState.file = HalFile{};
  if (removePartial && putState.path[0] != '\0') {
    Storage.remove(putState.path);
  }
  putState.path[0] = '\0';
  putState.expectedBytes = 0;
  putState.writtenBytes = 0;
  putState.crc32 = 0;
  putState.lastActivityMs = 0;
  putState.active = false;
}

bool normalizeSdPath(const char* rawPath, char* dst, size_t dstCap) {
  if (!rawPath || rawPath[0] == '\0') return false;
  if (strstr(rawPath, "..") != nullptr) return false;
  const int n = (rawPath[0] == '/') ? snprintf(dst, dstCap, "%s", rawPath) : snprintf(dst, dstCap, "/%s", rawPath);
  return n > 1 && static_cast<size_t>(n) < dstCap;
}

void ensureParentDirectories(char* filePath) {
  char* lastSlash = strrchr(filePath, '/');
  if (!lastSlash || lastSlash == filePath) return;
  *lastSlash = '\0';
  Storage.mkdir(filePath, true);
  *lastSlash = '/';
}

void handlePutBegin(char*& cursor) {
  const char* pathTok = nextToken(cursor);
  const char* bytesTok = nextToken(cursor);
  uint32_t expectedBytes = 0;
  if (!pathTok || !parseUint32Dec(bytesTok, expectedBytes)) {
    sendLineFmt("@DEV ERR PUT_BEGIN reason=\"bad_args\"\n");
    return;
  }

  RenderLock lock;
  if (putState.active) {
    abortPutTransfer(true);
  }
  if (!normalizeSdPath(pathTok, putState.path, sizeof(putState.path))) {
    sendLineFmt("@DEV ERR PUT_BEGIN reason=\"bad_path\"\n");
    return;
  }

  ensureParentDirectories(putState.path);
  if (!Storage.openFileForWrite("DEV", putState.path, putState.file) || !putState.file.isOpen()) {
    LOG_ERR("DEV", "PUT_BEGIN failed to open %s", putState.path);
    abortPutTransfer(false);
    sendLineFmt("@DEV ERR PUT_BEGIN reason=\"open_failed\"\n");
    return;
  }

  putState.expectedBytes = expectedBytes;
  putState.writtenBytes = 0;
  putState.crc32 = 0;
  putState.lastActivityMs = millis();
  putState.active = true;
  sendLineFmt("@DEV OK PUT_BEGIN path=\"%s\" expected=%" PRIu32 "\n", putState.path, expectedBytes);
}

void handlePutData(char*& cursor) {
  const char* hexTok = nextToken(cursor);
  if (!putState.active) {
    sendLineFmt("@DEV ERR PUT_DATA reason=\"no_active_put\"\n");
    return;
  }
  if (!hexTok) {
    RenderLock lock;
    abortPutTransfer(true);
    sendLineFmt("@DEV ERR PUT_DATA reason=\"missing_hex\"\n");
    return;
  }

  size_t chunkLen = 0;
  if (!decodeHexBytes(hexTok, putDecodeBuf, sizeof(putDecodeBuf), chunkLen)) {
    RenderLock lock;
    abortPutTransfer(true);
    sendLineFmt("@DEV ERR PUT_DATA reason=\"bad_hex\"\n");
    return;
  }

  RenderLock lock;
  if (chunkLen > putState.expectedBytes - putState.writtenBytes) {
    abortPutTransfer(true);
    sendLineFmt("@DEV ERR PUT_DATA reason=\"size_overflow\"\n");
    return;
  }
  if (putState.file.write(putDecodeBuf, chunkLen) != chunkLen) {
    LOG_ERR("DEV", "PUT_DATA write failed at %" PRIu32 " on %s", putState.writtenBytes, putState.path);
    abortPutTransfer(true);
    sendLineFmt("@DEV ERR PUT_DATA reason=\"write_failed\"\n");
    return;
  }

  putState.crc32 = esp_rom_crc32_le(putState.crc32, putDecodeBuf, chunkLen);
  putState.writtenBytes += static_cast<uint32_t>(chunkLen);
  putState.lastActivityMs = millis();
  sendLineFmt("@DEV OK PUT_DATA written=%" PRIu32 "\n", putState.writtenBytes);
}

void handlePutEnd(char*& cursor) {
  const char* crcTok = nextToken(cursor);
  if (!putState.active) {
    sendLineFmt("@DEV ERR PUT_END reason=\"no_active_put\"\n");
    return;
  }

  uint32_t expectedCrc = 0;
  if (!parseUint32Hex8(crcTok, expectedCrc)) {
    RenderLock lock;
    abortPutTransfer(true);
    sendLineFmt("@DEV ERR PUT_END reason=\"bad_crc_arg\"\n");
    return;
  }

  RenderLock lock;
  putState.file.flush();
  putState.file.close();
  putState.file = HalFile{};

  const uint32_t totalBytes = putState.writtenBytes;
  const uint32_t wantedBytes = putState.expectedBytes;
  const uint32_t actualCrc = putState.crc32;

  if (totalBytes != wantedBytes) {
    LOG_ERR("DEV", "PUT_END size mismatch (%" PRIu32 " != %" PRIu32 ") on %s", totalBytes, wantedBytes, putState.path);
    abortPutTransfer(true);
    sendLineFmt("@DEV ERR PUT_END reason=\"size_mismatch\" written=%" PRIu32 " expected=%" PRIu32 "\n", totalBytes,
                wantedBytes);
    return;
  }
  if (actualCrc != expectedCrc) {
    LOG_ERR("DEV", "PUT_END CRC mismatch (%08" PRIx32 " != %08" PRIx32 ") on %s", actualCrc, expectedCrc,
            putState.path);
    abortPutTransfer(true);
    sendLineFmt("@DEV ERR PUT_END reason=\"crc_mismatch\" got=%08" PRIx32 " expected=%08" PRIx32 "\n", actualCrc,
                expectedCrc);
    return;
  }

  if (strncasecmp(putState.path, "/.fonts/", 8) == 0 || strncasecmp(putState.path, "/fonts/", 7) == 0 ||
      strncasecmp(putState.path, ".fonts/", 7) == 0 || strncasecmp(putState.path, "fonts/", 6) == 0) {
    sdFontSystem.markRegistryDirty();
  }
  putState.path[0] = '\0';
  putState.expectedBytes = 0;
  putState.writtenBytes = 0;
  putState.crc32 = 0;
  putState.lastActivityMs = 0;
  putState.active = false;
  sendLineFmt("@DEV OK PUT_END bytes=%" PRIu32 " crc32=%08" PRIx32 "\n", totalBytes, actualCrc);
}

void handleFontRescan(GfxRenderer& renderer) {
  int familyCount = 0;
  int fontId = 0;
  uint8_t bpp = 1;
  bool shouldReflowForeground = false;
  {
    RenderLock lock;
    const bool hadSdFont = (SETTINGS.sdFontFamilyName[0] != '\0');
    if (hadSdFont) {
      activityManager.notifyDevReaderFontChanged();
      shouldReflowForeground = !cal16OnScreen && activityManager.isForegroundReader();
    }
    sdFontSystem.markRegistryDirty();
    sdFontSystem.ensureLoaded(renderer);
    familyCount = sdFontSystem.registry().getFamilyCount();
    fontId = SETTINGS.getReaderFontId();
    bpp = activeFontBitsPerPixel(renderer, fontId);
  }
  if (shouldReflowForeground) {
    activityManager.renderDevUpdateAndWait(/*forceClean=*/false);
  }
  const char* fontName = (SETTINGS.sdFontFamilyName[0] != '\0') ? SETTINGS.sdFontFamilyName : "NotoSans";
  sendLineFmt("@DEV OK FONT_RESCAN families=%d font=\"%s\" font_id=%d font_bpp=%u pt=%u\n", familyCount, fontName,
              fontId, static_cast<unsigned>(bpp), static_cast<unsigned>(SETTINGS.fontPointSize));
}

void handleFontUse(GfxRenderer& renderer, char*& cursor) {
  const char* famTok = nextToken(cursor);
  if (!famTok) {
    sendLineFmt("@DEV ERR FONT_USE reason=\"missing_family\"\n");
    return;
  }
  uint8_t pt = SETTINGS.fontPointSize;
  RefreshKind refresh = RefreshKind::None;
  if (const char* opt1 = nextToken(cursor)) {
    if (!parseRefreshKind(opt1, refresh)) {
      if (!parseUint8Dec(opt1, pt) || pt < 8 || pt > 36) {
        sendLineFmt("@DEV ERR FONT_USE reason=\"bad_pt\"\n");
        return;
      }
      if (const char* opt2 = nextToken(cursor)) {
        if (!parseRefreshKind(opt2, refresh)) {
          sendLineFmt("@DEV ERR FONT_USE reason=\"bad_refresh_arg\"\n");
          return;
        }
      }
    }
  }

  int fontId = 0;
  uint8_t bpp = 1;
  {
    RenderLock lock;
    const bool builtinRequested = (strcasecmp(famTok, "BUILTIN") == 0 || strcasecmp(famTok, "DEFAULT") == 0);
    char canonicalName[sizeof(SETTINGS.sdFontFamilyName)] = {};
    if (!builtinRequested) {
      const auto* fam = sdFontSystem.registry().findFamily(famTok);
      if (!fam) {
        sdFontSystem.markRegistryDirty();
        sdFontSystem.refreshIfDirty();
        fam = sdFontSystem.registry().findFamily(famTok);
      }
      if (!fam) {
        sendLineFmt("@DEV ERR FONT_USE reason=\"family_not_found\" font=\"%s\"\n", famTok);
        return;
      }
      strncpy(canonicalName, fam->name.c_str(), sizeof(canonicalName) - 1);
    }

    // Release cached reader Section / overlay buffers before loading the new font into internal RAM.
    activityManager.notifyDevReaderFontChanged();

    if (builtinRequested) {
      SETTINGS.sdFontFamilyName[0] = '\0';
    } else {
      memcpy(SETTINGS.sdFontFamilyName, canonicalName, sizeof(SETTINGS.sdFontFamilyName));
    }
    SETTINGS.fontPointSize = pt;
    sdFontSystem.ensureLoaded(renderer);
    SETTINGS.saveToFile();
    fontId = SETTINGS.getReaderFontId();
    bpp = activeFontBitsPerPixel(renderer, fontId);
  }
  triggerDevRefresh(renderer, refresh, /*reRenderNonReader=*/true);
  const char* fontName = (SETTINGS.sdFontFamilyName[0] != '\0') ? SETTINGS.sdFontFamilyName : "NotoSans";
  sendLineFmt("@DEV OK FONT_USE font=\"%s\" font_id=%d font_bpp=%u pt=%u\n", fontName, fontId,
              static_cast<unsigned>(bpp), static_cast<unsigned>(SETTINGS.fontPointSize));
}

void handleLegacyScreenshot(GfxRenderer& renderer) {
  RenderLock lock;
  if (!renderer.hasFrameBuffer() || !renderer.getFrameBuffer()) return;
  const uint8_t* fb = renderer.getFrameBuffer();
  const size_t bufferSize = renderer.getBufferSize();
  if (!sendLineFmt("SCREENSHOT_START:%zu\n", bufferSize)) return;
  if (!writeBulkCdc(fb, bufferSize)) return;
  sendLineFmt("SCREENSHOT_END\n");
}

bool dispatchLine(GfxRenderer& renderer, char* line) {
  while (*line == ' ' || *line == '\t') {
    ++line;
  }
  size_t len = strlen(line);
  while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t')) {
    line[--len] = '\0';
  }
  if (len == 0) return false;

  if (strcasecmp(line, "CMD:SCREENSHOT") == 0) {
    HalPowerManager::Lock powerLock;
    handleLegacyScreenshot(renderer);
    return true;
  }

  if (strncasecmp(line, "DEV:", 4) != 0) {
    return false;
  }

  HalPowerManager::Lock powerLock;
  char* cursor = line + 4;
  const char* cmd = nextToken(cursor);
  if (!cmd) {
    sendLineFmt("@DEV ERR PROTO reason=\"empty_cmd\"\n");
    return true;
  }

  if (strcasecmp(cmd, "PING") == 0) {
    handlePing();
  } else if (strcasecmp(cmd, "STATUS") == 0) {
    handleStatus(renderer);
  } else if (strcasecmp(cmd, "SNAP1") == 0) {
    handleSnap1(renderer, cursor);
  } else if (strcasecmp(cmd, "SNAP2") == 0) {
    handleSnap2(renderer, cursor);
  } else if (strcasecmp(cmd, "SNAP4") == 0) {
    handleSnap4(renderer, cursor);
  } else if (strcasecmp(cmd, "GRAY16") == 0) {
    handleGray16(renderer, cursor);
  } else if (strcasecmp(cmd, "LUT16") == 0) {
    handleLut16(renderer, cursor);
  } else if (strcasecmp(cmd, "CAL16") == 0) {
    handleCal16(renderer, cursor);
  } else if (strcasecmp(cmd, "KEY") == 0) {
    handleKey(renderer, cursor);
  } else if (strcasecmp(cmd, "LUT") == 0) {
    handleLut(renderer, cursor);
  } else if (strcasecmp(cmd, "PUT_BEGIN") == 0) {
    handlePutBegin(cursor);
  } else if (strcasecmp(cmd, "PUT_DATA") == 0) {
    handlePutData(cursor);
  } else if (strcasecmp(cmd, "PUT_END") == 0) {
    handlePutEnd(cursor);
  } else if (strcasecmp(cmd, "PUT_ABORT") == 0) {
    RenderLock lock;
    abortPutTransfer(true);
    sendLineFmt("@DEV OK PUT_ABORT\n");
  } else if (strcasecmp(cmd, "FONT_RESCAN") == 0) {
    handleFontRescan(renderer);
  } else if (strcasecmp(cmd, "FONT_USE") == 0) {
    handleFontUse(renderer, cursor);
  } else {
    sendLineFmt("@DEV ERR PROTO reason=\"unknown_cmd\"\n");
  }
  return true;
}

}  // namespace

void UsbDevSerial::tickInput() {
  const unsigned long now = millis();
  switch (keyState.phase) {
    case KeyPhase::Idle:
      if (keyState.count == 0) {
        gpio.setVirtualButtonMask(0);
        return;
      }
      keyState.activeBtn = keyState.queue[keyState.head].rawBtn;
      keyState.activeHoldMs = keyState.queue[keyState.head].holdMs;
      keyState.head = static_cast<uint8_t>((keyState.head + 1) % KEY_QUEUE_CAPACITY);
      --keyState.count;
      keyState.phase = KeyPhase::Holding;
      keyState.phasePolls = 1;
      keyState.phaseStartMs = now;
      gpio.setVirtualButtonMask(static_cast<uint8_t>(1U << keyState.activeBtn));
      return;

    case KeyPhase::Holding:
      // Require >= 2 polls in addition to holdMs so InputManager's 2-sample debounce
      // always commits the press edge even across a slow loop iteration.
      if (keyState.phasePolls >= 2 && (now - keyState.phaseStartMs >= keyState.activeHoldMs)) {
        keyState.phase = KeyPhase::ReleaseGap;
        keyState.phasePolls = 1;
        keyState.phaseStartMs = now;
        gpio.setVirtualButtonMask(0);
        return;
      }
      if (keyState.phasePolls < 255) ++keyState.phasePolls;
      gpio.setVirtualButtonMask(static_cast<uint8_t>(1U << keyState.activeBtn));
      return;

    case KeyPhase::ReleaseGap:
      if (keyState.phasePolls >= 2 && (now - keyState.phaseStartMs >= KEY_RELEASE_GAP_MS)) {
        keyState.phase = KeyPhase::Idle;
        keyState.phasePolls = 0;
        if (keyState.count > 0) {
          keyState.activeBtn = keyState.queue[keyState.head].rawBtn;
          keyState.activeHoldMs = keyState.queue[keyState.head].holdMs;
          keyState.head = static_cast<uint8_t>((keyState.head + 1) % KEY_QUEUE_CAPACITY);
          --keyState.count;
          keyState.phase = KeyPhase::Holding;
          keyState.phasePolls = 1;
          keyState.phaseStartMs = now;
          gpio.setVirtualButtonMask(static_cast<uint8_t>(1U << keyState.activeBtn));
        } else {
          gpio.setVirtualButtonMask(0);
        }
        return;
      }
      if (keyState.phasePolls < 255) ++keyState.phasePolls;
      gpio.setVirtualButtonMask(0);
      return;
  }
}

bool UsbDevSerial::pollCommands(GfxRenderer& renderer) {
  const unsigned long startMs = millis();
  if (cal16OnScreen && (gpio.wasAnyPressed() || gpio.wasAnyReleased() || gpio.wasTouchActivity() ||
                        gpio.wasCapacitivePagePressed() || gpio.wasHomeKeyPressed())) {
    cal16OnScreen = false;
  }
  if (putState.active && (startMs - putState.lastActivityMs > PUT_INACTIVITY_TIMEOUT_MS)) {
    RenderLock lock(RenderLock::Mode::Try);
    if (lock.ownsLock()) {
      LOG_ERR("DEV", "PUT timed out after %lums on %s; aborting", PUT_INACTIVITY_TIMEOUT_MS, putState.path);
      abortPutTransfer(true);
    }
  }

  bool handledAny = false;
  uint8_t processed = 0;

  while (logSerial.available() > 0) {
    const int ch = logSerial.read();
    if (ch < 0) break;
    if (ch == '\r') continue;

    if (ch == '\n') {
      if (rxOverflow) {
        rxOverflow = false;
        rxLen = 0;
        sendLineFmt("@DEV ERR PROTO reason=\"line_too_long\"\n");
        handledAny = true;
      } else {
        rxBuf[rxLen] = '\0';
        rxLen = 0;
        if (dispatchLine(renderer, rxBuf)) {
          handledAny = true;
        }
      }
      ++processed;
      if (processed >= MAX_CMDS_PER_POLL || (millis() - startMs >= MAX_POLL_SLICE_MS)) {
        break;
      }
      continue;
    }

    if (rxOverflow) continue;
    if (rxLen + 1 >= RX_BUF_SIZE) {
      rxOverflow = true;
    } else {
      rxBuf[rxLen++] = static_cast<char>(ch);
    }
  }

  return handledAny;
}

bool UsbDevSerial::skipLoopDelay() {
  return putState.active || keyState.phase != KeyPhase::Idle || keyState.count > 0 || logSerial.available() > 0;
}

#endif
