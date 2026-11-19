#include <Epub/Epub/converters/DirectPixelWriter.h>
#include <GfxRenderer/Gray16ZeroRle.h>
#include <SdCardFont.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

constexpr int X3_WIDTH_BYTES = 792 / 8;  // 99
constexpr int X3_HEIGHT = 528;
constexpr int STRIP_ROWS = 80;
constexpr size_t PLANE_BYTES = static_cast<size_t>(X3_WIDTH_BYTES) * X3_HEIGHT;  // 52,272

struct StripUpload {
  bool lsbPlane;
  int yStart;
  int numRows;
  std::vector<uint8_t> bytes;

  bool operator==(const StripUpload& other) const {
    return lsbPlane == other.lsbPlane && yStart == other.yStart && numRows == other.numRows && bytes == other.bytes;
  }
};

// Simulates EpubReaderActivity's direct synchronous strip streamer (`streamGray16Plane`).
void simulateDirectStream(const std::vector<uint8_t>& plane, const bool lsbPlane, std::vector<StripUpload>& out) {
  for (int y = 0; y < X3_HEIGHT; y += STRIP_ROWS) {
    const int rows = (X3_HEIGHT - y < STRIP_ROWS) ? (X3_HEIGHT - y) : STRIP_ROWS;
    const size_t off = static_cast<size_t>(y) * X3_WIDTH_BYTES;
    const size_t stripBytes = static_cast<size_t>(rows) * X3_WIDTH_BYTES;
    out.push_back({lsbPlane, y, rows, std::vector<uint8_t>(plane.begin() + off, plane.begin() + off + stripBytes)});
  }
}

// Simulates EpubReaderActivity's 32 KB RLE overlap encoder + overflow fallback for one 2-plane pass.
void simulateRlePassWithFallback(const std::vector<uint8_t>& lsbPlane, const std::vector<uint8_t>& msbPlane,
                                 uint8_t* rleBufWithGuard, const size_t rleCap, std::vector<StripUpload>& out,
                                 bool& outLsbRleHit, bool& outMsbRleHit, size_t& outUsedBytes) {
  std::vector<uint8_t> scratch(static_cast<size_t>(X3_WIDTH_BYTES) * STRIP_ROWS, 0);

  auto encodePlane = [&](const std::vector<uint8_t>& plane, size_t& cursor) -> bool {
    size_t nextCursor = cursor;
    for (int y = 0; y < X3_HEIGHT; y += STRIP_ROWS) {
      const int rows = (X3_HEIGHT - y < STRIP_ROWS) ? (X3_HEIGHT - y) : STRIP_ROWS;
      const size_t stripBytes = static_cast<size_t>(rows) * X3_WIDTH_BYTES;
      std::memcpy(scratch.data(), plane.data() + static_cast<size_t>(y) * X3_WIDTH_BYTES, stripBytes);
      if (!Gray16ZeroRle::encodeStrip(scratch.data(), static_cast<size_t>(rows), static_cast<size_t>(X3_WIDTH_BYTES),
                                      rleBufWithGuard, rleCap, nextCursor)) {
        return false;
      }
    }
    cursor = nextCursor;
    return true;
  };

  auto flushDecoded = [&](const bool lsb, const size_t start, const size_t end) {
    size_t readCursor = start;
    for (int y = 0; y < X3_HEIGHT; y += STRIP_ROWS) {
      const int rows = (X3_HEIGHT - y < STRIP_ROWS) ? (X3_HEIGHT - y) : STRIP_ROWS;
      const size_t stripBytes = static_cast<size_t>(rows) * X3_WIDTH_BYTES;
      ASSERT_TRUE(Gray16ZeroRle::decodeStrip(rleBufWithGuard, end, readCursor, scratch.data(),
                                             static_cast<size_t>(rows), static_cast<size_t>(X3_WIDTH_BYTES)));
      out.push_back({lsb, y, rows, std::vector<uint8_t>(scratch.data(), scratch.data() + stripBytes)});
    }
    EXPECT_EQ(readCursor, end);
  };

  size_t cursor = 0;
  outLsbRleHit = encodePlane(lsbPlane, cursor);
  const size_t lsbEnd = cursor;
  outMsbRleHit = false;
  if (outLsbRleHit) {
    outMsbRleHit = encodePlane(msbPlane, cursor);
  }
  outUsedBytes = cursor;

  if (outLsbRleHit) {
    flushDecoded(true, 0, lsbEnd);
  } else {
    simulateDirectStream(lsbPlane, true, out);
  }
  if (outMsbRleHit) {
    flushDecoded(false, lsbEnd, cursor);
  } else {
    simulateDirectStream(msbPlane, false, out);
  }
}

// Synthesizes a Portrait X3 prose plane matching real BookerlyVoyage 16pt density (~16-20% non-zero bytes).
std::vector<uint8_t> makeSyntheticProseGray16Plane(const uint32_t seed) {
  std::vector<uint8_t> plane(PLANE_BYTES, 0x00u);
  uint32_t rng = seed;
  auto nextRng = [&]() {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
  };
  // Portrait X3: 24 text lines across physical X bytes [7..90], physical Y rows [36..492].
  for (int line = 0; line < 24; ++line) {
    const int xByte = 7 + line * 3;
    if (xByte + 1 >= X3_WIDTH_BYTES) break;
    for (int y = 36; y < 492; ++y) {
      if ((y % 19) < 5) continue;  // inter-word whitespace
      plane[static_cast<size_t>(y) * X3_WIDTH_BYTES + xByte] = static_cast<uint8_t>((nextRng() & 0x7Fu) | 0x01u);
      if ((y % 5) == 0) {
        plane[static_cast<size_t>(y) * X3_WIDTH_BYTES + xByte + 1] = static_cast<uint8_t>(nextRng() & 0xFFu);
      }
    }
  }
  return plane;
}

// Reference UC8253 gate-space RAM layout (`EpdBus::sendPlaneFlipped`):
// emitted row `g` (`0..H-1`) equals logical row `y = H - 1 - g`.
std::vector<uint8_t> makeExpectedGateSpacePlane(const std::vector<uint8_t>& logicalPlane, const size_t height,
                                                const size_t widthBytes) {
  std::vector<uint8_t> gateRam(height * widthBytes, 0x00u);
  for (size_t gate = 0; gate < height; ++gate) {
    const size_t logicalY = height - 1u - gate;
    std::memcpy(gateRam.data() + gate * widthBytes, logicalPlane.data() + logicalY * widthBytes, widthBytes);
  }
  return gateRam;
}

// Simulates full-plane gate-order streaming (`CMD_DTM1` / `CMD_DTM2` with 0 PTIN/PTL/PTOUT windows)
// using `Gray16ZeroRleEncoder` + `PlaneStripIndex` + `decodePlaneGateOrder`, falling back to
// direct bottom-up gate-order streaming (`sendPlaneFlipped` equivalent) on 32 KB RLE overflow.
void simulateGateOrderPassWithFallback(const std::vector<uint8_t>& lsbPlane, const std::vector<uint8_t>& msbPlane,
                                       uint8_t* rleBufWithGuard, const size_t rleCap,
                                       std::vector<uint8_t>& outLsbGateRam, std::vector<uint8_t>& outMsbGateRam,
                                       std::vector<size_t>& outLsbChunkSizes, std::vector<size_t>& outMsbChunkSizes,
                                       bool& outLsbRleHit, bool& outMsbRleHit, size_t& outUsedBytes) {
  alignas(4) std::array<uint8_t, static_cast<size_t>(X3_WIDTH_BYTES) * STRIP_ROWS> scratch{};
  Gray16ZeroRle::PlaneStripIndex lsbIndex{};
  Gray16ZeroRle::PlaneStripIndex msbIndex{};

  auto encodeIndexedPlane = [&](const std::vector<uint8_t>& plane, size_t& cursor,
                                Gray16ZeroRle::PlaneStripIndex& index) -> bool {
    Gray16ZeroRleEncoder enc(rleBufWithGuard, rleCap, cursor, index);
    for (int y = 0; y < X3_HEIGHT; y += STRIP_ROWS) {
      const int rows = (X3_HEIGHT - y < STRIP_ROWS) ? (X3_HEIGHT - y) : STRIP_ROWS;
      const size_t stripBytes = static_cast<size_t>(rows) * X3_WIDTH_BYTES;
      std::memcpy(scratch.data(), plane.data() + static_cast<size_t>(y) * X3_WIDTH_BYTES, stripBytes);
      if (!enc.appendStrip(scratch.data(), static_cast<size_t>(rows), static_cast<size_t>(X3_WIDTH_BYTES))) {
        return false;
      }
    }
    return enc.finish();
  };

  auto streamGateDecodedPlane = [&](const size_t rleEnd, const Gray16ZeroRle::PlaneStripIndex& index,
                                    std::vector<uint8_t>& dstGateRam, std::vector<size_t>& chunkSizes) {
    dstGateRam.clear();
    chunkSizes.clear();
    ASSERT_TRUE(Gray16ZeroRle::decodePlaneGateOrder(rleBufWithGuard, rleEnd, index, scratch.data(), scratch.size(),
                                                    static_cast<size_t>(X3_HEIGHT), static_cast<size_t>(STRIP_ROWS),
                                                    static_cast<size_t>(X3_WIDTH_BYTES),
                                                    [&](const uint8_t* chunk, const size_t len) {
                                                      dstGateRam.insert(dstGateRam.end(), chunk, chunk + len);
                                                      chunkSizes.push_back(len);
                                                    }));
  };

  size_t cursor = 0;
  outLsbRleHit = encodeIndexedPlane(lsbPlane, cursor, lsbIndex);
  const size_t lsbEnd = cursor;
  outMsbRleHit = false;
  if (outLsbRleHit) {
    outMsbRleHit = encodeIndexedPlane(msbPlane, cursor, msbIndex);
  }
  outUsedBytes = cursor;

  if (outLsbRleHit) {
    streamGateDecodedPlane(lsbEnd, lsbIndex, outLsbGateRam, outLsbChunkSizes);
  } else {
    outLsbGateRam = makeExpectedGateSpacePlane(lsbPlane, X3_HEIGHT, X3_WIDTH_BYTES);
    outLsbChunkSizes = {PLANE_BYTES};
  }

  if (outMsbRleHit) {
    streamGateDecodedPlane(cursor, msbIndex, outMsbGateRam, outMsbChunkSizes);
  } else {
    outMsbGateRam = makeExpectedGateSpacePlane(msbPlane, X3_HEIGHT, X3_WIDTH_BYTES);
    outMsbChunkSizes = {PLANE_BYTES};
  }
}

}  // namespace

TEST(Gray16ZeroRleTest, BoundaryZeroRunLengthsAndNonMultipleOfEightStripsRoundTripBitExact) {
  const std::array<size_t, 12> runLengths = {0, 1, 7, 8, 9, 63, 64, 2039, 2040, 2048, 2049, 4105};
  for (size_t offset = 0; offset < 9; ++offset) {
    for (const size_t zeroLen : runLengths) {
      std::vector<uint8_t> raw(offset + zeroLen + 13, 0xA5u);
      for (size_t i = 0; i < offset; ++i) raw[i] = static_cast<uint8_t>(0x11u + i);
      std::memset(raw.data() + offset, 0x00, zeroLen);
      for (size_t i = 0; i < 13; ++i) raw[offset + zeroLen + i] = static_cast<uint8_t>(0xC0u + i);

      std::vector<uint8_t> encoded(raw.size() * 2 + 32, 0xEEu);
      size_t encCursor = 0;
      ASSERT_TRUE(Gray16ZeroRle::encodeAppend(raw.data(), raw.size(), encoded.data(), encoded.size(), encCursor));

      std::vector<uint8_t> decoded(raw.size(), 0xFFu);
      size_t decCursor = 0;
      ASSERT_TRUE(Gray16ZeroRle::decodeChunk(encoded.data(), encCursor, decCursor, decoded.data(), decoded.size()));
      EXPECT_EQ(decCursor, encCursor);
      EXPECT_EQ(decoded, raw);
    }
  }

  // Also test 2D strips with both multiple-of-8 rows (80, 48) and non-multiple-of-8 rows (17, 53).
  for (const size_t rows : {size_t{17}, size_t{48}, size_t{53}, size_t{80}}) {
    std::vector<uint8_t> strip(rows * X3_WIDTH_BYTES, 0x00u);
    for (size_t i = 0; i < strip.size(); i += 11) {
      strip[i] = static_cast<uint8_t>((i % 251u) + 1u);
    }
    std::vector<uint8_t> encoded(strip.size() * 2, 0u);
    size_t encCursor = 0;
    ASSERT_TRUE(
        Gray16ZeroRle::encodeStrip(strip.data(), rows, X3_WIDTH_BYTES, encoded.data(), encoded.size(), encCursor));
    std::vector<uint8_t> decoded(strip.size(), 0xAAu);
    size_t decCursor = 0;
    ASSERT_TRUE(Gray16ZeroRle::decodeStrip(encoded.data(), encCursor, decCursor, decoded.data(), rows, X3_WIDTH_BYTES));
    EXPECT_EQ(decCursor, encCursor);
    EXPECT_EQ(decoded, strip);
  }
}

TEST(Gray16ZeroRleTest, RealisticTwoPassGray16FitsIn32KBAndMatchesDirectStripsBitForBit) {
  const auto cLsb = makeSyntheticProseGray16Plane(0x12345678u);
  const auto cMsb = makeSyntheticProseGray16Plane(0x9ABCDEF0u);
  const auto fLsb = makeSyntheticProseGray16Plane(0xCAFEBABEu);
  const auto fMsb = makeSyntheticProseGray16Plane(0x0F0E0D0Cu);

  std::vector<StripUpload> directUploads;
  simulateDirectStream(cLsb, true, directUploads);
  simulateDirectStream(cMsb, false, directUploads);
  simulateDirectStream(fLsb, true, directUploads);
  simulateDirectStream(fMsb, false, directUploads);

  constexpr size_t GUARD_BYTES = 64;
  std::vector<uint8_t> rleBuf(Gray16ZeroRle::BUFFER_BYTES + GUARD_BYTES, 0xDEu);

  std::vector<StripUpload> rleUploads;
  bool p1LsbHit = false, p1MsbHit = false, p2LsbHit = false, p2MsbHit = false;
  size_t p1Bytes = 0, p2Bytes = 0;
  simulateRlePassWithFallback(cLsb, cMsb, rleBuf.data(), Gray16ZeroRle::BUFFER_BYTES, rleUploads, p1LsbHit, p1MsbHit,
                              p1Bytes);
  // Reuse the exact same 32 KB buffer for Pass 2 (cursor reset to 0).
  simulateRlePassWithFallback(fLsb, fMsb, rleBuf.data(), Gray16ZeroRle::BUFFER_BYTES, rleUploads, p2LsbHit, p2MsbHit,
                              p2Bytes);

  EXPECT_TRUE(p1LsbHit);
  EXPECT_TRUE(p1MsbHit);
  EXPECT_TRUE(p2LsbHit);
  EXPECT_TRUE(p2MsbHit);
  EXPECT_LT(p1Bytes, Gray16ZeroRle::BUFFER_BYTES);
  EXPECT_LT(p2Bytes, Gray16ZeroRle::BUFFER_BYTES);
  EXPECT_EQ(rleUploads, directUploads);
  for (size_t g = 0; g < GUARD_BYTES; ++g) {
    EXPECT_EQ(rleBuf[Gray16ZeroRle::BUFFER_BYTES + g], 0xDEu);
  }
}

TEST(Gray16ZeroRleTest, OverflowFallbackPreservesBitExactPlanesAndNeverWritesOutOfBounds) {
  // Case 1: LSB fits (~21 KB), MSB (~21 KB) overflows 32 KB -> LSB decodes from RLE, MSB falls back to direct strips.
  std::vector<uint8_t> semiDenseLsb(PLANE_BYTES, 0x00u);
  std::vector<uint8_t> semiDenseMsb(PLANE_BYTES, 0x00u);
  for (size_t i = 0; i < PLANE_BYTES; i += 4) {
    semiDenseLsb[i] = static_cast<uint8_t>((i % 251u) + 1u);
    semiDenseMsb[i] = static_cast<uint8_t>(((i + 77u) % 251u) + 1u);
  }

  std::vector<StripUpload> expectedCase1;
  simulateDirectStream(semiDenseLsb, true, expectedCase1);
  simulateDirectStream(semiDenseMsb, false, expectedCase1);

  constexpr size_t GUARD_BYTES = 64;
  std::vector<uint8_t> rleBuf(Gray16ZeroRle::BUFFER_BYTES + GUARD_BYTES, 0xABu);
  std::vector<StripUpload> actualCase1;
  bool lsbHit1 = false, msbHit1 = false;
  size_t used1 = 0;
  simulateRlePassWithFallback(semiDenseLsb, semiDenseMsb, rleBuf.data(), Gray16ZeroRle::BUFFER_BYTES, actualCase1,
                              lsbHit1, msbHit1, used1);
  EXPECT_TRUE(lsbHit1);
  EXPECT_FALSE(msbHit1);
  EXPECT_LE(used1, Gray16ZeroRle::BUFFER_BYTES);
  EXPECT_EQ(actualCase1, expectedCase1);
  for (size_t g = 0; g < GUARD_BYTES; ++g) {
    EXPECT_EQ(rleBuf[Gray16ZeroRle::BUFFER_BYTES + g], 0xABu);
  }

  // Case 2: Fully dense incompressible plane (52,272 non-zero bytes > 32,768 B) overflows on LSB -> both fall back
  // cleanly.
  std::vector<uint8_t> denseLsb(PLANE_BYTES);
  std::vector<uint8_t> denseMsb(PLANE_BYTES);
  for (size_t i = 0; i < PLANE_BYTES; ++i) {
    denseLsb[i] = static_cast<uint8_t>((i % 251u) + 1u);
    denseMsb[i] = static_cast<uint8_t>(((i + 31u) % 253u) + 1u);
  }

  std::vector<StripUpload> expectedCase2;
  simulateDirectStream(denseLsb, true, expectedCase2);
  simulateDirectStream(denseMsb, false, expectedCase2);

  std::fill(rleBuf.begin(), rleBuf.end(), 0xCDu);
  std::vector<StripUpload> actualCase2;
  bool lsbHit2 = true, msbHit2 = true;
  size_t used2 = 999;
  simulateRlePassWithFallback(denseLsb, denseMsb, rleBuf.data(), Gray16ZeroRle::BUFFER_BYTES, actualCase2, lsbHit2,
                              msbHit2, used2);
  EXPECT_FALSE(lsbHit2);
  EXPECT_FALSE(msbHit2);
  EXPECT_EQ(used2, 0u);
  EXPECT_EQ(actualCase2, expectedCase2);
  for (size_t g = 0; g < GUARD_BYTES; ++g) {
    EXPECT_EQ(rleBuf[Gray16ZeroRle::BUFFER_BYTES + g], 0xCDu);
  }
}

TEST(Gray16ZeroRleTest, MalformedStreamsFailCleanlyWithoutAdvancingCursor) {
  std::array<uint8_t, 16> out{};

  // Truncated `{0x00}` missing run count.
  const std::array<uint8_t, 1> truncatedZeroRun = {0x00u};
  size_t cursor = 0;
  EXPECT_FALSE(Gray16ZeroRle::decodeStrip(truncatedZeroRun.data(), truncatedZeroRun.size(), cursor, out.data(), 8, 2));
  EXPECT_EQ(cursor, 0u);

  // Truncated non-zero tag `{0x03}` (needs 2 literal bytes, only 1 supplied).
  const std::array<uint8_t, 2> truncatedTag = {0x03u, 0xAAu};
  EXPECT_FALSE(Gray16ZeroRle::decodeStrip(truncatedTag.data(), truncatedTag.size(), cursor, out.data(), 8, 2));
  EXPECT_EQ(cursor, 0u);

  // Zero-group run `{0x00, 0x05}` (= 6 groups = 48 bytes) exceeding `out` (2 groups = 16 bytes).
  const std::array<uint8_t, 2> overrun = {0x00u, 0x05u};
  EXPECT_FALSE(Gray16ZeroRle::decodeStrip(overrun.data(), overrun.size(), cursor, out.data(), 8, 2));
  EXPECT_EQ(cursor, 0u);
}

TEST(Gray16ZeroRleTest, GateOrderStripDecodeMatchesRowReversalForMultipleAndNonMultipleOfEightRows) {
  for (const size_t widthBytes : {size_t{1}, size_t{13}, size_t{X3_WIDTH_BYTES}}) {
    for (const size_t rows : {size_t{1}, size_t{7}, size_t{8}, size_t{17}, size_t{48}, size_t{53}, size_t{80}}) {
      std::vector<uint8_t> strip(rows * widthBytes, 0x00u);
      // Asymmetric coordinate-dependent pattern so any row or column transpose fails immediately.
      for (size_t r = 0; r < rows; ++r) {
        for (size_t c = 0; c < widthBytes; ++c) {
          if (((r * 7u + c * 3u) % 5u) == 0u) {
            strip[r * widthBytes + c] = static_cast<uint8_t>(((r * 31u + c * 17u) % 251u) + 1u);
          }
        }
      }

      std::vector<uint8_t> encoded(strip.size() * 2 + 16, 0xCCu);
      size_t encCursor = 0;
      ASSERT_TRUE(
          Gray16ZeroRle::encodeStrip(strip.data(), rows, widthBytes, encoded.data(), encoded.size(), encCursor));

      std::vector<uint8_t> gateActual(strip.size(), 0xEEu);
      size_t decCursor = 0;
      ASSERT_TRUE(Gray16ZeroRle::decodeStripGateOrder(encoded.data(), encCursor, decCursor, gateActual.data(), rows,
                                                      widthBytes));
      EXPECT_EQ(decCursor, encCursor);

      const auto gateExpected = makeExpectedGateSpacePlane(strip, rows, widthBytes);
      EXPECT_EQ(gateActual, gateExpected);
    }
  }
}

TEST(Gray16ZeroRleTest, GateOrderFullPlaneBurstProducesExact52272ByteUc8253GateRamAcrossEmptyProseAndDenseInputs) {
  static_assert(sizeof(Gray16ZeroRle::PlaneStripIndex) == 26, "PlaneStripIndex must stay 26 bytes");
  const std::vector<size_t> expected7ChunkSizes = {
      48u * X3_WIDTH_BYTES,  // strip 6: y = 480..527 -> gate = 0..47   (4,752 B)
      80u * X3_WIDTH_BYTES,  // strip 5: y = 400..479 -> gate = 48..127 (7,920 B)
      80u * X3_WIDTH_BYTES,  // strip 4: y = 320..399 -> gate = 128..207
      80u * X3_WIDTH_BYTES,  // strip 3: y = 240..319 -> gate = 208..287
      80u * X3_WIDTH_BYTES,  // strip 2: y = 160..239 -> gate = 288..367
      80u * X3_WIDTH_BYTES,  // strip 1: y = 80..159  -> gate = 368..447
      80u * X3_WIDTH_BYTES,  // strip 0: y = 0..79    -> gate = 448..527
  };

  constexpr size_t GUARD_BYTES = 64;
  std::vector<uint8_t> rleBuf(Gray16ZeroRle::BUFFER_BYTES + GUARD_BYTES, 0x5Au);

  // 1. Completely empty 52,272 B plane + exact strip-seam corner/boundary markers plane.
  const std::vector<uint8_t> emptyPlane(PLANE_BYTES, 0x00u);
  std::vector<uint8_t> seamPlane(PLANE_BYTES, 0x00u);
  seamPlane[0 * X3_WIDTH_BYTES + 0] = 0x11u;                     // top-left (y=0 -> gate=527)
  seamPlane[0 * X3_WIDTH_BYTES + (X3_WIDTH_BYTES - 1)] = 0x22u;  // top-right (y=0 -> gate=527)
  seamPlane[79 * X3_WIDTH_BYTES + 17] = 0x33u;                   // strip 0 bottom row (y=79 -> gate=448)
  seamPlane[80 * X3_WIDTH_BYTES + 17] = 0x44u;                   // strip 1 top row (y=80 -> gate=447)
  seamPlane[479 * X3_WIDTH_BYTES + 49] = 0x55u;                  // strip 5 bottom row (y=479 -> gate=48)
  seamPlane[480 * X3_WIDTH_BYTES + 49] = 0x66u;                  // strip 6 top row (y=480 -> gate=47)
  seamPlane[527 * X3_WIDTH_BYTES + 0] = 0x77u;                   // bottom-left (y=527 -> gate=0)
  seamPlane[527 * X3_WIDTH_BYTES + (X3_WIDTH_BYTES - 1)] = 0x88u;

  std::vector<uint8_t> emptyGateActual, seamGateActual;
  std::vector<size_t> emptyChunks, seamChunks;
  bool emptyHit = false, seamHit = false;
  size_t emptyUsed = 0;
  simulateGateOrderPassWithFallback(emptyPlane, seamPlane, rleBuf.data(), Gray16ZeroRle::BUFFER_BYTES, emptyGateActual,
                                    seamGateActual, emptyChunks, seamChunks, emptyHit, seamHit, emptyUsed);
  EXPECT_TRUE(emptyHit);
  EXPECT_TRUE(seamHit);
  EXPECT_LT(emptyUsed, 256u);
  EXPECT_EQ(emptyChunks, expected7ChunkSizes);
  EXPECT_EQ(seamChunks, expected7ChunkSizes);
  EXPECT_EQ(emptyGateActual, makeExpectedGateSpacePlane(emptyPlane, X3_HEIGHT, X3_WIDTH_BYTES));
  EXPECT_EQ(seamGateActual, makeExpectedGateSpacePlane(seamPlane, X3_HEIGHT, X3_WIDTH_BYTES));
  EXPECT_EQ(seamGateActual[0 * X3_WIDTH_BYTES + 0], 0x77u);
  EXPECT_EQ(seamGateActual[47 * X3_WIDTH_BYTES + 49], 0x66u);
  EXPECT_EQ(seamGateActual[48 * X3_WIDTH_BYTES + 49], 0x55u);
  EXPECT_EQ(seamGateActual[527 * X3_WIDTH_BYTES + 0], 0x11u);

  // 2. Realistic 2-pass BookerlyVoyage 16pt prose planes reusing the exact 32 KB buffer across Pass 1 and Pass 2.
  const auto cLsb = makeSyntheticProseGray16Plane(0x12345678u);
  const auto cMsb = makeSyntheticProseGray16Plane(0x9ABCDEF0u);
  const auto fLsb = makeSyntheticProseGray16Plane(0xCAFEBABEu);
  const auto fMsb = makeSyntheticProseGray16Plane(0x0F0E0D0Cu);

  std::vector<uint8_t> cLsbGate, cMsbGate, fLsbGate, fMsbGate;
  std::vector<size_t> cLsbChunks, cMsbChunks, fLsbChunks, fMsbChunks;
  bool p1LsbHit = false, p1MsbHit = false, p2LsbHit = false, p2MsbHit = false;
  size_t p1Bytes = 0, p2Bytes = 0;
  simulateGateOrderPassWithFallback(cLsb, cMsb, rleBuf.data(), Gray16ZeroRle::BUFFER_BYTES, cLsbGate, cMsbGate,
                                    cLsbChunks, cMsbChunks, p1LsbHit, p1MsbHit, p1Bytes);
  simulateGateOrderPassWithFallback(fLsb, fMsb, rleBuf.data(), Gray16ZeroRle::BUFFER_BYTES, fLsbGate, fMsbGate,
                                    fLsbChunks, fMsbChunks, p2LsbHit, p2MsbHit, p2Bytes);

  EXPECT_TRUE(p1LsbHit);
  EXPECT_TRUE(p1MsbHit);
  EXPECT_TRUE(p2LsbHit);
  EXPECT_TRUE(p2MsbHit);
  EXPECT_LT(p1Bytes, Gray16ZeroRle::BUFFER_BYTES);
  EXPECT_LT(p2Bytes, Gray16ZeroRle::BUFFER_BYTES);
  EXPECT_EQ(cLsbChunks, expected7ChunkSizes);
  EXPECT_EQ(cMsbChunks, expected7ChunkSizes);
  EXPECT_EQ(fLsbChunks, expected7ChunkSizes);
  EXPECT_EQ(fMsbChunks, expected7ChunkSizes);
  EXPECT_EQ(cLsbGate, makeExpectedGateSpacePlane(cLsb, X3_HEIGHT, X3_WIDTH_BYTES));
  EXPECT_EQ(cMsbGate, makeExpectedGateSpacePlane(cMsb, X3_HEIGHT, X3_WIDTH_BYTES));
  EXPECT_EQ(fLsbGate, makeExpectedGateSpacePlane(fLsb, X3_HEIGHT, X3_WIDTH_BYTES));
  EXPECT_EQ(fMsbGate, makeExpectedGateSpacePlane(fMsb, X3_HEIGHT, X3_WIDTH_BYTES));
  for (size_t g = 0; g < GUARD_BYTES; ++g) {
    EXPECT_EQ(rleBuf[Gray16ZeroRle::BUFFER_BYTES + g], 0x5Au);
  }

  // 3. Fully dense 100%-nonzero 52,272 B plane encoded inside a 64 KB buffer (`<= UINT16_MAX` index range).
  std::vector<uint8_t> fullDense(PLANE_BYTES);
  for (size_t y = 0; y < X3_HEIGHT; ++y) {
    for (size_t x = 0; x < X3_WIDTH_BYTES; ++x) {
      fullDense[y * X3_WIDTH_BYTES + x] = static_cast<uint8_t>(((y * 131u + x * 17u) % 255u) + 1u);
    }
  }
  std::vector<uint8_t> maxIndexBuf(UINT16_MAX, 0u);
  Gray16ZeroRle::PlaneStripIndex denseIdx{};
  size_t denseCursor = 0;
  {
    Gray16ZeroRleEncoder enc(maxIndexBuf.data(), maxIndexBuf.size(), denseCursor, denseIdx);
    for (int y = 0; y < X3_HEIGHT; y += STRIP_ROWS) {
      const int rows = (X3_HEIGHT - y < STRIP_ROWS) ? (X3_HEIGHT - y) : STRIP_ROWS;
      ASSERT_TRUE(enc.appendStrip(fullDense.data() + static_cast<size_t>(y) * X3_WIDTH_BYTES, static_cast<size_t>(rows),
                                  static_cast<size_t>(X3_WIDTH_BYTES)));
    }
    ASSERT_TRUE(enc.finish());
  }
  // 100% non-zero: every 8-byte group emits 1 tag + 8 literals = 9/8 * 52,272 = 58,806 B <= 65,535 B!
  EXPECT_EQ(denseCursor, 58806u);
  alignas(4) std::array<uint8_t, static_cast<size_t>(X3_WIDTH_BYTES) * STRIP_ROWS> scratch{};
  std::vector<uint8_t> denseGateActual;
  std::vector<size_t> denseChunks;
  ASSERT_TRUE(Gray16ZeroRle::decodePlaneGateOrder(maxIndexBuf.data(), denseCursor, denseIdx, scratch.data(),
                                                  scratch.size(), static_cast<size_t>(X3_HEIGHT),
                                                  static_cast<size_t>(STRIP_ROWS), static_cast<size_t>(X3_WIDTH_BYTES),
                                                  [&](const uint8_t* chunk, const size_t len) {
                                                    denseGateActual.insert(denseGateActual.end(), chunk, chunk + len);
                                                    denseChunks.push_back(len);
                                                  }));
  EXPECT_EQ(denseChunks, expected7ChunkSizes);
  EXPECT_EQ(denseGateActual, makeExpectedGateSpacePlane(fullDense, X3_HEIGHT, X3_WIDTH_BYTES));
}

TEST(Gray16ZeroRleTest, GateOrderOverflowFallbackPreservesBitExact52272ByteGateRamAndGuardBytes) {
  constexpr size_t GUARD_BYTES = 64;
  std::vector<uint8_t> rleBuf(Gray16ZeroRle::BUFFER_BYTES + GUARD_BYTES, 0xA5u);

  // Case 1: Semi-dense (~21 KB each): LSB fits in 32 KB (7 gate bursts), MSB overflows and falls back cleanly.
  std::vector<uint8_t> semiDenseLsb(PLANE_BYTES, 0x00u);
  std::vector<uint8_t> semiDenseMsb(PLANE_BYTES, 0x00u);
  for (size_t i = 0; i < PLANE_BYTES; i += 4) {
    semiDenseLsb[i] = static_cast<uint8_t>((i % 251u) + 1u);
    semiDenseMsb[i] = static_cast<uint8_t>(((i + 77u) % 251u) + 1u);
  }

  std::vector<uint8_t> lsbGate1, msbGate1;
  std::vector<size_t> lsbChunks1, msbChunks1;
  bool lsbHit1 = false, msbHit1 = true;
  size_t used1 = 0;
  simulateGateOrderPassWithFallback(semiDenseLsb, semiDenseMsb, rleBuf.data(), Gray16ZeroRle::BUFFER_BYTES, lsbGate1,
                                    msbGate1, lsbChunks1, msbChunks1, lsbHit1, msbHit1, used1);
  EXPECT_TRUE(lsbHit1);
  EXPECT_FALSE(msbHit1);
  EXPECT_LE(used1, Gray16ZeroRle::BUFFER_BYTES);
  EXPECT_EQ(lsbChunks1.size(), 7u);
  EXPECT_EQ(msbChunks1.size(), 1u);
  EXPECT_EQ(lsbGate1, makeExpectedGateSpacePlane(semiDenseLsb, X3_HEIGHT, X3_WIDTH_BYTES));
  EXPECT_EQ(msbGate1, makeExpectedGateSpacePlane(semiDenseMsb, X3_HEIGHT, X3_WIDTH_BYTES));
  for (size_t g = 0; g < GUARD_BYTES; ++g) {
    EXPECT_EQ(rleBuf[Gray16ZeroRle::BUFFER_BYTES + g], 0xA5u);
  }

  // Case 2: Fully dense 52,272 B non-zero planes overflow 32 KB on LSB -> both fall back cleanly with cursor=0.
  std::vector<uint8_t> denseLsb(PLANE_BYTES);
  std::vector<uint8_t> denseMsb(PLANE_BYTES);
  for (size_t i = 0; i < PLANE_BYTES; ++i) {
    denseLsb[i] = static_cast<uint8_t>((i % 251u) + 1u);
    denseMsb[i] = static_cast<uint8_t>(((i + 31u) % 253u) + 1u);
  }

  std::fill(rleBuf.begin(), rleBuf.end(), 0x3Cu);
  std::vector<uint8_t> lsbGate2, msbGate2;
  std::vector<size_t> lsbChunks2, msbChunks2;
  bool lsbHit2 = true, msbHit2 = true;
  size_t used2 = 1234;
  simulateGateOrderPassWithFallback(denseLsb, denseMsb, rleBuf.data(), Gray16ZeroRle::BUFFER_BYTES, lsbGate2, msbGate2,
                                    lsbChunks2, msbChunks2, lsbHit2, msbHit2, used2);
  EXPECT_FALSE(lsbHit2);
  EXPECT_FALSE(msbHit2);
  EXPECT_EQ(used2, 0u);
  EXPECT_EQ(lsbGate2, makeExpectedGateSpacePlane(denseLsb, X3_HEIGHT, X3_WIDTH_BYTES));
  EXPECT_EQ(msbGate2, makeExpectedGateSpacePlane(denseMsb, X3_HEIGHT, X3_WIDTH_BYTES));
  for (size_t g = 0; g < GUARD_BYTES; ++g) {
    EXPECT_EQ(rleBuf[Gray16ZeroRle::BUFFER_BYTES + g], 0x3Cu);
  }

  // Case 3: uint16_t index ceiling (> 65,535 B) fails safely without wrapping stripOffset.
  std::vector<uint8_t> hugeBuf(70000, 0u);
  Gray16ZeroRle::PlaneStripIndex hugeIdx{};
  size_t startPast64K = 65536u;
  Gray16ZeroRleEncoder badEnc(hugeBuf.data(), hugeBuf.size(), startPast64K, hugeIdx);
  EXPECT_FALSE(badEnc.ok());
  EXPECT_FALSE(badEnc.appendStrip(denseLsb.data(), STRIP_ROWS, X3_WIDTH_BYTES));
  EXPECT_FALSE(badEnc.finish());
  EXPECT_EQ(startPast64K, 65536u);
}

TEST(Gray16ZeroRleTest, GateOrderMalformedAndCorruptedIndexStreamsFailCleanly) {
  std::array<uint8_t, 16> out{};
  size_t cursor = 0;

  // Truncated zero run & overrun on gate-order strip decoder (multiple of 8 rows).
  const std::array<uint8_t, 1> truncatedZeroRun = {0x00u};
  EXPECT_FALSE(
      Gray16ZeroRle::decodeStripGateOrder(truncatedZeroRun.data(), truncatedZeroRun.size(), cursor, out.data(), 8, 2));
  EXPECT_EQ(cursor, 0u);

  const std::array<uint8_t, 2> overrun = {0x00u, 0x05u};
  EXPECT_FALSE(Gray16ZeroRle::decodeStripGateOrder(overrun.data(), overrun.size(), cursor, out.data(), 8, 2));
  EXPECT_EQ(cursor, 0u);

  // Non-multiple-of-8 tail group with set bit past `totalBytes` (`rows=3, widthBytes=1` -> 3 bytes, bit 4 invalid).
  const std::array<uint8_t, 2> tailBitOverrun = {0x10u, 0x7Fu};
  EXPECT_FALSE(
      Gray16ZeroRle::decodeStripGateOrder(tailBitOverrun.data(), tailBitOverrun.size(), cursor, out.data(), 3, 1));
  EXPECT_EQ(cursor, 0u);

  // Corrupted PlaneStripIndex entries: trailing unconsumed byte, inverted offsets, out-of-range stripIdx, tiny scratch.
  std::array<uint8_t, 16> validEncoded{};
  size_t validLen = 0;
  const std::array<uint8_t, 16> zeroStrip{};
  ASSERT_TRUE(Gray16ZeroRle::encodeStrip(zeroStrip.data(), 8, 2, validEncoded.data(), validEncoded.size(), validLen));
  ASSERT_EQ(validLen, 2u);

  Gray16ZeroRle::PlaneStripIndex trailingGarbageIdx{};
  trailingGarbageIdx.stripOffset[0] = 0;
  trailingGarbageIdx.stripOffset[1] = 3;  // 1 extra byte beyond valid 2-byte stream
  EXPECT_FALSE(Gray16ZeroRle::decodeIndexedStripGateOrder(validEncoded.data(), validEncoded.size(), trailingGarbageIdx,
                                                          0, out.data(), 8, 2));

  Gray16ZeroRle::PlaneStripIndex invertedIdx{};
  invertedIdx.stripOffset[0] = 2;
  invertedIdx.stripOffset[1] = 1;
  EXPECT_FALSE(Gray16ZeroRle::decodeIndexedStripGateOrder(validEncoded.data(), validEncoded.size(), invertedIdx, 0,
                                                          out.data(), 8, 2));

  EXPECT_FALSE(Gray16ZeroRle::decodeIndexedStripGateOrder(validEncoded.data(), validEncoded.size(), trailingGarbageIdx,
                                                          Gray16ZeroRle::MAX_INDEXED_STRIPS, out.data(), 8, 2));

  Gray16ZeroRle::PlaneStripIndex validIdx{};
  validIdx.stripOffset[0] = 0;
  validIdx.stripOffset[1] = static_cast<uint16_t>(validLen);
  std::array<uint8_t, 8> undersizedScratch{};
  EXPECT_FALSE(Gray16ZeroRle::decodePlaneGateOrder(validEncoded.data(), validLen, validIdx, undersizedScratch.data(),
                                                   undersizedScratch.size(), 8, 8, 2, [](const uint8_t*, size_t) {}));
}

TEST(Gray16ZeroRleTest, TwelveIndexedStripsSupport960RowPanelsAndCallbackReader) {
  constexpr size_t kWidthBytes = 68;  // 544 px / 8
  constexpr size_t kHeight = 960;     // 12 * 80 rows
  constexpr size_t kStripRows = 80;
  std::vector<uint8_t> logicalPlane(kHeight * kWidthBytes, 0x00u);
  for (size_t y = 0; y < kHeight; ++y) {
    if ((y % 11u) == 0u) {
      logicalPlane[y * kWidthBytes + (y % kWidthBytes)] = static_cast<uint8_t>(((y * 29u) % 251u) + 1u);
    }
  }

  std::vector<uint8_t> rleBuf(Gray16ZeroRle::BUFFER_BYTES, 0x00u);
  Gray16ZeroRle::PlaneStripIndex idx{};
  size_t cursor = 0;
  {
    Gray16ZeroRleEncoder enc(rleBuf.data(), rleBuf.size(), cursor, idx);
    for (size_t y = 0; y < kHeight; y += kStripRows) {
      ASSERT_TRUE(enc.appendStrip(logicalPlane.data() + y * kWidthBytes, kStripRows, kWidthBytes));
    }
    EXPECT_EQ(enc.stripCount(), 12u);
    ASSERT_TRUE(enc.finish());
  }

  // Verify 13th strip rejection without mutating committed cursor.
  {
    Gray16ZeroRle::PlaneStripIndex overflowIdx{};
    size_t keepCursor = cursor;
    Gray16ZeroRleEncoder enc13(rleBuf.data(), rleBuf.size(), keepCursor, overflowIdx);
    const std::array<uint8_t, 8> tiny{};
    for (size_t s = 0; s < Gray16ZeroRle::MAX_INDEXED_STRIPS; ++s) {
      ASSERT_TRUE(enc13.appendStrip(tiny.data(), 8, 1));
    }
    EXPECT_FALSE(enc13.appendStrip(tiny.data(), 8, 1));
    EXPECT_FALSE(enc13.finish());
    EXPECT_EQ(keepCursor, cursor);
  }

  std::vector<uint8_t> scratch(kStripRows * kWidthBytes, 0x00u);
  Gray16ZeroRle::GateStreamReader reader(rleBuf.data(), cursor, idx, scratch.data(), scratch.size(), kHeight,
                                         kStripRows, kWidthBytes);
  EXPECT_FALSE(reader.ok());  // Not ok until all 12 strips are consumed!

  std::vector<uint8_t> emittedGateRam;
  size_t chunks = 0;
  for (;;) {
    const uint8_t* chunk = nullptr;
    const size_t bytes = Gray16ZeroRle::GateStreamReader::nextCallback(&reader, &chunk);
    if (bytes == 0) break;
    ASSERT_NE(chunk, nullptr);
    EXPECT_EQ(bytes, kStripRows * kWidthBytes);
    emittedGateRam.insert(emittedGateRam.end(), chunk, chunk + bytes);
    ++chunks;
  }
  EXPECT_EQ(chunks, 12u);
  EXPECT_TRUE(reader.ok());
  EXPECT_EQ(emittedGateRam, makeExpectedGateSpacePlane(logicalPlane, kHeight, kWidthBytes));
}

TEST(Gray16ZeroRleTest, InvertedCleanupDtm1PlaneCompressesWhitePaperAndStreamsDualPlanesWithoutTouchingFrameBuffer) {
  // Synthesize a 0xFF-background CleanupDTM1 plane (~85% 0xFF white paper, 0 bits on glyph ink 1..15).
  const std::vector<uint8_t> proseInk = makeSyntheticProseGray16Plane(0x58B01234u);
  std::vector<uint8_t> cleanupDtm1(PLANE_BYTES, 0xFFu);
  for (size_t i = 0; i < PLANE_BYTES; ++i) {
    cleanupDtm1[i] = static_cast<uint8_t>(~proseInk[i]);
  }

  std::vector<uint8_t> rleBuf(Gray16ZeroRle::BUFFER_BYTES, 0x00u);
  std::vector<uint8_t> scratch(static_cast<size_t>(X3_WIDTH_BYTES) * STRIP_ROWS, 0x00u);
  Gray16ZeroRle::PlaneStripIndex idx{};
  size_t cursor = 0;
  {
    Gray16ZeroRleEncoder enc(rleBuf.data(), rleBuf.size(), cursor, idx);
    for (int y = 0; y < X3_HEIGHT; y += STRIP_ROWS) {
      const int rows = (X3_HEIGHT - y < STRIP_ROWS) ? (X3_HEIGHT - y) : STRIP_ROWS;
      const size_t stripBytes = static_cast<size_t>(rows) * X3_WIDTH_BYTES;
      std::memcpy(scratch.data(), cleanupDtm1.data() + static_cast<size_t>(y) * X3_WIDTH_BYTES, stripBytes);
      ASSERT_TRUE(
          enc.appendStripInverted(scratch.data(), static_cast<size_t>(rows), static_cast<size_t>(X3_WIDTH_BYTES)));
    }
    ASSERT_TRUE(enc.finish());
  }
  EXPECT_LT(cursor, Gray16ZeroRle::BUFFER_BYTES / 2u);

  const std::vector<uint8_t> expectedGateRam = makeExpectedGateSpacePlane(cleanupDtm1, X3_HEIGHT, X3_WIDTH_BYTES);
  for (int pass = 0; pass < 2; ++pass) {
    Gray16ZeroRle::GateStreamReader reader(rleBuf.data(), cursor, idx, scratch.data(), scratch.size(), X3_HEIGHT,
                                           STRIP_ROWS, X3_WIDTH_BYTES, /*invertOutput=*/true);
    std::vector<uint8_t> decodedGateRam;
    for (;;) {
      const uint8_t* chunk = nullptr;
      const size_t bytes = Gray16ZeroRle::GateStreamReader::nextCallback(&reader, &chunk);
      if (bytes == 0) break;
      decodedGateRam.insert(decodedGateRam.end(), chunk, chunk + bytes);
    }
    EXPECT_TRUE(reader.ok());
    EXPECT_EQ(decodedGateRam, expectedGateRam);
  }
}

TEST(Gray16ZeroRleTest, TopDownInvertedPlaneDecodeRestoresSpeculativeBaseFrameBufferBitExact) {
  const std::vector<uint8_t> proseInk = makeSyntheticProseGray16Plane(0x59B02468u);
  std::vector<uint8_t> expectedFb(PLANE_BYTES, 0xFFu);
  for (size_t i = 0; i < PLANE_BYTES; ++i) {
    expectedFb[i] = static_cast<uint8_t>(~proseInk[i]);
  }

  std::vector<uint8_t> tempRle(20 * 1024, 0x00u);
  std::vector<uint8_t> scratch(static_cast<size_t>(X3_WIDTH_BYTES) * STRIP_ROWS, 0x00u);
  Gray16ZeroRle::PlaneStripIndex idx{};
  size_t cursor = 0;
  {
    Gray16ZeroRleEncoder enc(tempRle.data(), tempRle.size(), cursor, idx);
    for (int y = 0; y < X3_HEIGHT; y += STRIP_ROWS) {
      const int rows = (X3_HEIGHT - y < STRIP_ROWS) ? (X3_HEIGHT - y) : STRIP_ROWS;
      const size_t stripBytes = static_cast<size_t>(rows) * X3_WIDTH_BYTES;
      std::memcpy(scratch.data(), expectedFb.data() + static_cast<size_t>(y) * X3_WIDTH_BYTES, stripBytes);
      ASSERT_TRUE(
          enc.appendStripInverted(scratch.data(), static_cast<size_t>(rows), static_cast<size_t>(X3_WIDTH_BYTES)));
    }
    ASSERT_TRUE(enc.finish());
  }
  EXPECT_LT(cursor, Gray16ZeroRle::BUFFER_BYTES / 2u);

  // Shrink-to-fit copy, matching speculative idle staging.
  std::vector<uint8_t> shrunkRle(tempRle.begin(), tempRle.begin() + cursor);
  std::vector<uint8_t> decodedFb(PLANE_BYTES, 0x55u);
  ASSERT_TRUE(Gray16ZeroRle::decodePlaneTopDownInverted(shrunkRle.data(), shrunkRle.size(), idx, decodedFb.data(),
                                                        X3_HEIGHT, STRIP_ROWS, X3_WIDTH_BYTES));
  EXPECT_EQ(decodedFb, expectedFb);

  // Also verify non-multiple-of-8 strip heights (e.g. 53 rows) and truncated buffer rejection.
  std::vector<uint8_t> oddStrip(53 * 13, 0xFFu);
  for (size_t i = 0; i < oddStrip.size(); i += 7) oddStrip[i] = static_cast<uint8_t>(0xFEu - (i & 0x7Fu));
  std::vector<uint8_t> oddCopy = oddStrip;
  Gray16ZeroRle::PlaneStripIndex oddIdx{};
  size_t oddLen = 0;
  {
    Gray16ZeroRleEncoder enc(tempRle.data(), tempRle.size(), oddLen, oddIdx);
    ASSERT_TRUE(enc.appendStripInverted(oddCopy.data(), 53, 13));
    ASSERT_TRUE(enc.finish());
  }
  std::vector<uint8_t> oddDecoded(oddStrip.size(), 0x00u);
  ASSERT_TRUE(Gray16ZeroRle::decodePlaneTopDownInverted(tempRle.data(), oddLen, oddIdx, oddDecoded.data(), 53, 80, 13));
  EXPECT_EQ(oddDecoded, oddStrip);
  EXPECT_FALSE(
      Gray16ZeroRle::decodePlaneTopDownInverted(tempRle.data(), oddLen - 1, oddIdx, oddDecoded.data(), 53, 80, 13));
}

TEST(Gray16ZeroRleTest, DirectPixelWriterSplitRailGray16Decodes2BitPixelsAcrossOrientationsAndBands) {
  HalDisplay halDisplay;
  GfxRenderer gfx(halDisplay);
  gfx.begin();
  constexpr int kStripY0 = 480;
  constexpr int kStripRows = 48;
  constexpr int kBaseX = 20;
  constexpr int kRowY = 32;
  const int gwBytes = gfx.getDisplayWidthBytes();

  // Portrait maps logical x in [20..23] to phyY = 527 - x in [504..507], inside strip [480..528).
  gfx.setOrientation(GfxRenderer::Portrait);
  gfx.setGray16Base(true);
  gfx.setRenderMode(GfxRenderer::BW);
  gfx.clearScreen(0xFFu);
  {
    DirectPixelWriter writer{};
    writer.init(gfx);
    writer.beginRow(kRowY);
    for (uint8_t pv = 0; pv < 4; ++pv) {
      writer.writePixel(kBaseX + pv, pv);
    }
  }

  auto renderImageStrip = [&](GfxRenderer::RenderMode mode, uint8_t initVal) {
    std::vector<uint8_t> buf(static_cast<size_t>(gwBytes) * kStripRows, initVal);
    gfx.setRenderMode(mode);
    gfx.beginStripTarget(buf.data(), kStripY0, kStripRows);
    gfx.clearScreen(initVal);
    DirectPixelWriter writer{};
    writer.init(gfx);
    writer.beginRow(kRowY);
    for (uint8_t pv = 0; pv < 4; ++pv) {
      writer.writePixel(kBaseX + pv, pv);
    }
    gfx.endStripTarget();
    return buf;
  };

  const auto cLsb = renderImageStrip(GfxRenderer::GRAY16_COARSE_LSB, 0x00u);
  const auto cMsb = renderImageStrip(GfxRenderer::GRAY16_COARSE_MSB, 0x00u);
  const auto fLsb = renderImageStrip(GfxRenderer::GRAY16_FINE_LSB, 0x00u);
  const auto fMsb = renderImageStrip(GfxRenderer::GRAY16_FINE_MSB, 0x00u);
  const auto cleanDtm1 = renderImageStrip(GfxRenderer::GRAY16_CLEANUP_DTM1, 0xFFu);
  gfx.setRenderMode(GfxRenderer::BW);
  gfx.setGray16Base(false);

  auto readBit = [&](const uint8_t* plane, int lx, int ly, int yOrigin) -> uint8_t {
    const int phyX = ly;
    const int phyY = gfx.getDisplayHeight() - 1 - lx;
    const size_t idx = static_cast<size_t>(phyY - yOrigin) * gwBytes + static_cast<size_t>(phyX / 8);
    return (plane[idx] >> (7 - (phyX % 8))) & 1u;
  };

  const uint8_t* fb = gfx.getFrameBuffer();
  constexpr std::array<uint8_t, 4> kExpectedGray16 = {0, 6, 9, 15};
  constexpr std::array<uint8_t, 4> kExpectedBwBit = {0, 0, 1, 1};       // G00/G06 black (0), G09/G15 white (1)
  constexpr std::array<uint8_t, 4> kExpectedCleanupBit = {0, 0, 0, 1};  // G00/G06/G09 ink (0), G15 white (1)
  for (uint8_t pv = 0; pv < 4; ++pv) {
    const int lx = kBaseX + pv;
    const uint8_t bw = readBit(fb, lx, kRowY, 0);
    const uint8_t cl = readBit(cLsb.data(), lx, kRowY, kStripY0);
    const uint8_t cm = readBit(cMsb.data(), lx, kRowY, kStripY0);
    const uint8_t fl = readBit(fLsb.data(), lx, kRowY, kStripY0);
    const uint8_t fm = readBit(fMsb.data(), lx, kRowY, kStripY0);
    const uint8_t cln = readBit(cleanDtm1.data(), lx, kRowY, kStripY0);
    const uint8_t code5 = static_cast<uint8_t>((bw << 4) | (cl << 3) | (cm << 2) | (fl << 1) | fm);
    EXPECT_EQ(kExpectedBwBit[pv], bw) << "pv=" << static_cast<int>(pv);
    EXPECT_EQ(kExpectedGray16[pv], freeink::gray16::kDecodeGray16[code5]) << "pv=" << static_cast<int>(pv);
    EXPECT_EQ(kExpectedCleanupBit[pv], cln) << "cleanup pv=" << static_cast<int>(pv);
  }

  // With setGray16Base(false) (4-gray overlay mode), pixelValue == 2 still draws black (0) in BW base:
  gfx.setGray16Base(false);
  gfx.setRenderMode(GfxRenderer::BW);
  gfx.clearScreen(0xFFu);
  {
    DirectPixelWriter writer{};
    writer.init(gfx);
    writer.beginRow(kRowY);
    for (uint8_t pv = 0; pv < 4; ++pv) {
      writer.writePixel(kBaseX + pv, pv);
    }
  }
  EXPECT_EQ(0u, readBit(fb, kBaseX + 2, kRowY, 0));
  EXPECT_EQ(1u, readBit(fb, kBaseX + 3, kRowY, 0));

  // With ditherBw = true, GRAY16_COARSE/FINE bleach passes leave dithered B/W images untouched,
  //  while GRAY16_CLEANUP_DTM1 matches the 2x2 Bayer BW dither pattern:
  gfx.setDitherImagesInBw(true);
  gfx.setGray16Base(true);
  gfx.setRenderMode(GfxRenderer::BW);
  gfx.clearScreen(0xFFu);
  {
    DirectPixelWriter writer{};
    writer.init(gfx);
    writer.beginRow(kRowY);
    for (uint8_t pv = 0; pv < 4; ++pv) writer.writePixel(kBaseX + pv, pv);
  }
  const auto ditherCoarse = renderImageStrip(GfxRenderer::GRAY16_COARSE_LSB, 0x00u);
  const auto ditherClean = renderImageStrip(GfxRenderer::GRAY16_CLEANUP_DTM1, 0xFFu);
  for (uint8_t b : ditherCoarse) EXPECT_EQ(0u, b);
  for (uint8_t pv = 0; pv < 4; ++pv) {
    EXPECT_EQ(readBit(fb, kBaseX + pv, kRowY, 0), readBit(ditherClean.data(), kBaseX + pv, kRowY, kStripY0));
  }
}

TEST(Gray16ZeroRleTest, OverlayGrayPlanesPreserveLowLevelSetBitContractAndSkipGuardedWhiteChrome) {
  HalDisplay halDisplay;
  GfxRenderer gfx(halDisplay);
  gfx.begin();
  gfx.setOrientation(GfxRenderer::Portrait);

  constexpr int kStripY0 = 160;
  constexpr int kStripRows = 80;
  const int gwBytes = gfx.getDisplayWidthBytes();
  const size_t stripBytes = static_cast<size_t>(gwBytes) * kStripRows;

  // Logical x in [300..340] maps in Portrait (height=528) to phyY = 527 - x in [187..227], inside [160..240).
  const GfxRenderer::RenderMode overlayModes[] = {
      GfxRenderer::GRAYSCALE_LSB,     GfxRenderer::GRAYSCALE_MSB,   GfxRenderer::GRAY16_COARSE_LSB,
      GfxRenderer::GRAY16_COARSE_MSB, GfxRenderer::GRAY16_FINE_LSB, GfxRenderer::GRAY16_FINE_MSB,
  };

  for (const auto mode : overlayModes) {
    SCOPED_TRACE(static_cast<int>(mode));
    gfx.setRenderMode(mode);
    EXPECT_TRUE(gfx.isOverlayGrayPlane());

    // 1. Low-level bit contract (state=false -> set bit 1, state=true -> clear bit 0) is preserved
    //    so CAL16 / GC4 / drawBitmap callers can set 1 bits in 0x00-cleared gray planes.
    std::vector<uint8_t> grayPatchStrip(stripBytes, 0x00u);
    gfx.beginStripTarget(grayPatchStrip.data(), kStripY0, kStripRows);
    gfx.fillRect(308, 32, 8, 8, false);
    gfx.drawLine(330, 40, 330, 47, false);
    gfx.fillRect(310, 32, 2, 8, true);
    gfx.endStripTarget();

    for (int lx = 308; lx < 316; ++lx) {
      const int phyY = gfx.getDisplayHeight() - 1 - lx;
      const uint8_t expected = (lx >= 310 && lx < 312) ? 0x00u : 0xFFu;
      EXPECT_EQ(expected, grayPatchStrip[static_cast<size_t>(phyY - kStripY0) * gwBytes + (32 / 8)]);
    }
    const int phyYLine = gfx.getDisplayHeight() - 1 - 330;
    EXPECT_EQ(0xFFu, grayPatchStrip[static_cast<size_t>(phyYLine - kStripY0) * gwBytes + (40 / 8)]);

    // 2. maskRoundedRectOutsideCorners(..., Color::White) and high-level !isOverlayGrayPlane()-guarded
    //    white chrome (ImageBlock::renderPlaceholder / BaseTheme::drawBatteryLightningBolt) leave
    //    0x00-cleared overlay/bleach planes untouched.
    std::vector<uint8_t> cleanStrip(stripBytes, 0x00u);
    gfx.beginStripTarget(cleanStrip.data(), kStripY0, kStripRows);
    gfx.maskRoundedRectOutsideCorners(300, 20, 32, 24, 6, Color::White);
    gfx.fillRect(300, 56, 16, 16, true);
    if (!gfx.isOverlayGrayPlane()) {
      gfx.fillRect(301, 57, 14, 14, false);
      gfx.drawLine(304, 60, 308, 64, false);
    }
    gfx.endStripTarget();
    for (size_t i = 0; i < stripBytes; ++i) {
      ASSERT_EQ(0x00u, cleanStrip[i]) << "mode=" << static_cast<int>(mode) << " byte=" << i;
    }
  }

  // 3. In BW and GRAY16_CLEANUP_DTM1 modes (0 = black, 1 = white, !isOverlayGrayPlane()),
  //    fillRect(..., false), drawLine(..., false), maskRoundedRectOutsideCorners(..., Color::White),
  //    and !isOverlayGrayPlane()-guarded white chrome all carve 1 bits out of black (0x00).
  for (const auto whiteSetsOneMode : {GfxRenderer::BW, GfxRenderer::GRAY16_CLEANUP_DTM1}) {
    std::vector<uint8_t> blackStrip(stripBytes, 0x00u);
    gfx.setRenderMode(whiteSetsOneMode);
    EXPECT_FALSE(gfx.isOverlayGrayPlane());
    gfx.beginStripTarget(blackStrip.data(), kStripY0, kStripRows);
    gfx.fillRect(308, 32, 8, 8, false);
    gfx.drawLine(330, 40, 330, 47, false);
    gfx.maskRoundedRectOutsideCorners(300, 16, 16, 16, 6, Color::White);
    if (!gfx.isOverlayGrayPlane()) {
      gfx.fillRect(320, 64, 8, 8, false);
    }
    gfx.endStripTarget();
    const int phyYRect = gfx.getDisplayHeight() - 1 - 310;
    EXPECT_EQ(0xFFu, blackStrip[static_cast<size_t>(phyYRect - kStripY0) * gwBytes + (32 / 8)]);
    const int phyYLine = gfx.getDisplayHeight() - 1 - 330;
    EXPECT_EQ(0xFFu, blackStrip[static_cast<size_t>(phyYLine - kStripY0) * gwBytes + (40 / 8)]);
    const int phyYCorner = gfx.getDisplayHeight() - 1 - 300;
    EXPECT_NE(0x00u, blackStrip[static_cast<size_t>(phyYCorner - kStripY0) * gwBytes + (16 / 8)]);
    const int phyYGuarded = gfx.getDisplayHeight() - 1 - 322;
    EXPECT_EQ(0xFFu, blackStrip[static_cast<size_t>(phyYGuarded - kStripY0) * gwBytes + (64 / 8)]);
  }
  gfx.setRenderMode(GfxRenderer::BW);
}

TEST(Gray16ZeroRleTest, DirtyGray16BaselineAndDisplayWindowGate) {
  HalDisplay halDisplay;
  GfxRenderer gfx(halDisplay);
  gfx.begin();

  // supportsDeferredGray16Base() reports UC8253 Gray16 async-base capability even before setGray16Base(true)
  // (on UC8253 X3, 4-gray supportsDeferredGrayscaleBase() is false while supportsDeferredGray16Base() is true).
  halDisplay.deferredGrayBaseSupported = false;
  EXPECT_FALSE(gfx.isGray16Base());
  EXPECT_FALSE(gfx.supportsDeferredGrayscaleBase());
  EXPECT_TRUE(gfx.supportsDeferredGray16Base());
  gfx.setGray16Base(true);
  EXPECT_TRUE(gfx.supportsDeferredGrayscaleBase());
  gfx.setGray16Base(false);
  EXPECT_FALSE(gfx.supportsDeferredGrayscaleBase());
  EXPECT_TRUE(gfx.supportsDeferredGray16Base());

  // Before any Gray16 cleanup has settled on the glass, displayBuffer does not mark Gray16 dirty.
  EXPECT_FALSE(gfx.consumeDirtyGray16Baseline());
  gfx.displayBuffer(HalDisplay::FAST_REFRESH);
  EXPECT_FALSE(gfx.consumeDirtyGray16Baseline());

  // Once finishGray16Cleanup settles a Gray16 page on the glass, baseline is clean until chrome/popup draws.
  gfx.finishGray16Cleanup();
  EXPECT_FALSE(gfx.consumeDirtyGray16Baseline());

  // 1. Windowed popup on X3 UC8253 (supportsGray16Planes == true) uses HalDisplay::displayWindow and marks baseline
  // dirty.
  const int prevBufCalls = halDisplay.displayBufferCalls;
  gfx.displayWindow(40, 60, 200, 48);
  EXPECT_EQ(1, halDisplay.displayWindowCalls);
  EXPECT_EQ(prevBufCalls, halDisplay.displayBufferCalls);
  EXPECT_TRUE(gfx.consumeDirtyGray16Baseline());
  // Consuming clears the one-shot dirty latch.
  EXPECT_FALSE(gfx.consumeDirtyGray16Baseline());

  // 2. Full-frame displayBuffer and displayBufferAsync after finishGray16Cleanup also mark baseline dirty.
  gfx.finishGray16Cleanup();
  gfx.displayBuffer(HalDisplay::FAST_REFRESH);
  EXPECT_TRUE(gfx.consumeDirtyGray16Baseline());

  gfx.finishGray16Cleanup();
  gfx.displayBufferAsync(HalDisplay::FAST_REFRESH);
  EXPECT_TRUE(gfx.consumeDirtyGray16Baseline());

  // 3. A subsequent Gray16 render finishing with finishGray16Cleanup resets any unconsumed dirty latch.
  gfx.finishGray16Cleanup();
  gfx.displayWindow(40, 60, 200, 48);
  gfx.displayGray16Base(HalDisplay::HALF_REFRESH);
  gfx.finishGray16Cleanup();
  EXPECT_FALSE(gfx.consumeDirtyGray16Baseline());

  // 4. When supportsGray16Planes() is false (X4 / SSD1677 / Night Mode), displayWindow falls back to displayBuffer.
  halDisplay.gray16Supported = false;
  EXPECT_FALSE(gfx.supportsDeferredGray16Base());
  const int winCallsBefore = halDisplay.displayWindowCalls;
  const int bufCallsBefore = halDisplay.displayBufferCalls;
  gfx.displayWindow(40, 60, 200, 48);
  EXPECT_EQ(winCallsBefore, halDisplay.displayWindowCalls);
  EXPECT_EQ(bufCallsBefore + 1, halDisplay.displayBufferCalls);
}

TEST(Gray16ZeroRleTest, DrawTextLocksBoldAndItalicToRegularBaselineAndDetectsMultiBitFallbacks) {
  HalDisplay halDisplay;
  GfxRenderer gfx(halDisplay);
  gfx.begin();
  gfx.setOrientation(GfxRenderer::LandscapeCounterClockwise);

  // Single 1x1 ink pixel positioned right on the baseline (top = 1 -> y = baseline - 1).
  static constexpr uint8_t kOnePixelBmp[1] = {0x80u};
  static constexpr EpdGlyph kGlyphA[1] = {{1, 1, 8 << 4, 0, 1, 1, 0}};
  static constexpr EpdUnicodeInterval kIntervalA[1] = {{'A', 'A', 0}};

  // Intentionally divergent ascender / advanceY on BOLD and ITALIC vs REGULAR:
  EpdFontData regularData{};
  regularData.bitmap = kOnePixelBmp;
  regularData.glyph = kGlyphA;
  regularData.intervals = kIntervalA;
  regularData.intervalCount = 1;
  regularData.advanceY = 24;
  regularData.ascender = 18;
  regularData.descender = -6;

  EpdFontData boldData = regularData;
  boldData.advanceY = 30;
  boldData.ascender = 23;
  boldData.descender = -7;

  EpdFontData italicData = regularData;
  italicData.advanceY = 20;
  italicData.ascender = 14;

  EpdFont regularFont(&regularData);
  EpdFont boldFont(&boldData);
  EpdFont italicFont(&italicData);
  EpdFontFamily family(16, &regularFont, &boldFont, &italicFont, &boldFont);
  gfx.insertFont(1, family);

  const int gwBytes = gfx.getDisplayWidthBytes();
  const uint8_t* fb = gfx.getFrameBuffer();
  const auto findPaintedY = [&](int xCol) -> int {
    for (int y = 0; y < gfx.getDisplayHeight(); ++y) {
      const uint8_t bit = (fb[static_cast<size_t>(y) * gwBytes + (xCol / 8)] >> (7 - (xCol % 8))) & 1u;
      if (bit == 0u) return y;
    }
    return -1;
  };

  gfx.clearScreen(0xFFu);
  gfx.drawText(1, 16, 40, "A", true, EpdFontFamily::REGULAR);
  gfx.drawText(1, 32, 40, "A", true, EpdFontFamily::BOLD);
  gfx.drawText(1, 48, 40, "A", true, EpdFontFamily::ITALIC);

  // All three styles must lock to REGULAR's baseline (40 + 18 - 1 = 57).
  EXPECT_EQ(57, findPaintedY(16));
  EXPECT_EQ(57, findPaintedY(32));
  EXPECT_EQ(57, findPaintedY(48));

  // Verify textUsesMultiBitFont: 1bpp primary UI font -> false for ASCII, true when redirected to 4bpp CJK fallback.
  static constexpr uint8_t kFourBitPixel[1] = {0xF0u};
  static constexpr EpdUnicodeInterval kCjkInterval[1] = {{0x4E2D, 0x4E2D, 0}};
  EpdFontData cjk4BitData{};
  cjk4BitData.bitmap = kFourBitPixel;
  cjk4BitData.glyph = kGlyphA;
  cjk4BitData.intervals = kCjkInterval;
  cjk4BitData.intervalCount = 1;
  cjk4BitData.advanceY = 16;
  cjk4BitData.ascender = 12;
  cjk4BitData.descender = -4;
  cjk4BitData.is4Bit = true;

  EpdFont cjkFont(&cjk4BitData);
  SdCardFont sdFallback(&cjkFont);
  gfx.insertFont(2, EpdFontFamily(12, &cjkFont));
  gfx.registerSdCardFont(2, &sdFallback);
  gfx.setFallbackFont(1, 2);

  EXPECT_FALSE(gfx.textUsesMultiBitFont(1, "Chapter 1"));
  EXPECT_TRUE(gfx.textUsesMultiBitFont(1, "\xE4\xB8\xAD"));
}
