#include <gtest/gtest.h>

#include <array>
#include <utility>
#include <vector>

#include "lib/GfxRenderer/GlyphBitmap.h"

namespace {
constexpr int PANEL_WIDTH = 40;
constexpr int PANEL_HEIGHT = 32;
constexpr int STRIDE = PANEL_WIDTH / 8;

using glyphBitmap::Plane;

// Logical placements used by the renderer: unrotated text, and side-button
// labels rotated 90 degrees clockwise (glyph x runs up, glyph y runs right).
constexpr std::array<std::array<int, 4>, 2> AXES = {{{1, 0, 0, 1}, {0, -1, 1, 0}}};

std::pair<int, int> physical(int orientation, int x, int y) {
  switch (orientation) {
    case 0:
      return {y, PANEL_HEIGHT - 1 - x};
    case 1:
      return {PANEL_WIDTH - 1 - x, PANEL_HEIGHT - 1 - y};
    case 2:
      return {PANEL_WIDTH - 1 - y, x};
    default:
      return {x, y};
  }
}

// Logical rectangle the renderer clips to; the glyph-local clip is derived from it.
struct Rect {
  int left;
  int top;
  int right;
  int bottom;
};

void compare(int orientation, int rotation, bool twoBit, Plane plane, bool state, int width, int height, int x, int y,
             int originY, int rows, Rect clip) {
  SCOPED_TRACE(::testing::Message() << orientation << ',' << rotation << ',' << twoBit << ',' << static_cast<int>(plane)
                                    << ',' << state << " size=" << width << 'x' << height << " at=" << x << ',' << y
                                    << " band=" << originY << ',' << rows);
  const auto [dxX, dxY, dyX, dyY] = AXES[rotation];
  const glyphBitmap::Frame logical{x, y, dxX, dxY, dyX, dyY};
  std::vector<uint8_t> bitmap((width * height * (twoBit ? 2 : 1) + 7) / 8);
  for (size_t i = 0; i < bitmap.size(); ++i) bitmap[i] = static_cast<uint8_t>(i * 73 + 0x1b);
  // Nonuniform background and guards verify transparent pixels and all off-band bytes.
  std::vector<uint8_t> expected(rows * STRIDE + 32);
  for (size_t i = 0; i < expected.size(); ++i) expected[i] = static_cast<uint8_t>(i * 53 + 0xa5);
  auto actual = expected;
  for (int gy = 0; gy < height; ++gy) {
    for (int gx = 0; gx < width; ++gx) {
      const int lx = x + gx * dxX + gy * dyX;
      const int ly = y + gx * dxY + gy * dyY;
      if (lx < clip.left || ly < clip.top || lx >= clip.right || ly >= clip.bottom) continue;
      const auto [px, py] = physical(orientation, lx, ly);
      if (px < 0 || px >= PANEL_WIDTH || py < originY || py >= originY + rows) continue;
      const int source = gy * width + gx;
      bool draw = false;
      bool black = state;
      if (twoBit) {
        const int value = 3 - ((bitmap[source / 4] >> ((3 - source % 4) * 2)) & 3);
        if (plane == Plane::BW) draw = value < 3;
        if (plane == Plane::GrayLSB) draw = value == 1;
        if (plane == Plane::GrayMSB) draw = value == 1 || value == 2;
        if (plane != Plane::BW) black = false;
      } else {
        draw = (bitmap[source / 8] >> (7 - source % 8)) & 1;
      }
      if (!draw) continue;
      auto& byte = expected[16 + (py - originY) * STRIDE + px / 8];
      if (black)
        byte &= static_cast<uint8_t>(~(0x80 >> (px % 8)));
      else
        byte |= 0x80 >> (px % 8);
    }
  }

  glyphBitmap::Clip local{0, 0, width, height};
  glyphBitmap::clipToRect(logical, clip.left, clip.top, clip.right, clip.bottom, local);

  const auto [px, py] = physical(orientation, x, y);
  const auto [xx, xy] = physical(orientation, x + dxX, y + dxY);
  const auto [yx, yy] = physical(orientation, x + dyX, y + dyY);
  const glyphBitmap::Target target{
      actual.data() + 16, PANEL_WIDTH, STRIDE, originY, rows, {px, py, xx - px, xy - py, yx - px, yy - py}};
  glyphBitmap::draw(bitmap.data(), width, height, static_cast<uint8_t>(twoBit ? 2 : 1), plane, state, target, local);
  EXPECT_EQ(expected, actual);
}
}  // namespace

TEST(GlyphBitmap, MatchesPerPixelReferenceAcrossOrientationsRotationsPlanesAndClipping) {
  for (int orientation = 0; orientation < 4; ++orientation) {
    for (int rotation = 0; rotation < 2; ++rotation) {
      for (bool twoBit : {false, true}) {
        for (Plane plane : {Plane::BW, Plane::GrayLSB, Plane::GrayMSB}) {
          for (bool state : {false, true}) {
            for (int width : {1, 3, 7, 16, 31}) {
              for (const auto [x, y] : {std::pair{-5, -3}, std::pair{0, 0}, std::pair{9, 11}, std::pair{29, 31}}) {
                for (const auto [origin, rows] :
                     {std::pair{0, 32}, std::pair{0, 7}, std::pair{7, 11}, std::pair{28, 4}}) {
                  compare(orientation, rotation, twoBit, plane, state, width, 13, x, y, origin, rows,
                          {-20, -20, 60, 60});
                  compare(orientation, rotation, twoBit, plane, state, width, 13, x, y, origin, rows, {2, 3, 18, 20});
                }
              }
            }
          }
        }
      }
    }
  }
}

TEST(GlyphBitmap, EmptyAndFullyClippedGlyphsDoNotWrite) {
  for (int orientation = 0; orientation < 4; ++orientation) {
    for (int rotation = 0; rotation < 2; ++rotation) {
      compare(orientation, rotation, true, Plane::BW, true, 0, 0, 0, 0, 0, 32, {0, 0, 40, 32});
      compare(orientation, rotation, true, Plane::BW, true, 11, 9, 100, 100, 0, 32, {0, 0, 40, 32});
      compare(orientation, rotation, true, Plane::BW, true, 11, 9, 0, 0, 0, 32, {12, 10, 15, 20});
    }
  }
}

namespace {
uint16_t expected4BitMask(Plane plane) {
  switch (plane) {
    case Plane::BW:
      return glyphBitmap::kBw4BitMask;
    case Plane::GrayLSB:
      return glyphBitmap::kGrayLsb4BitMask;
    case Plane::GrayMSB:
      return glyphBitmap::kGrayMsb4BitMask;
    case Plane::Gray16BW:
      return freeink::gray16::kMaskBwClear;
    case Plane::Gray16CoarseLSB:
      return freeink::gray16::kMaskPass1Dtm1;
    case Plane::Gray16CoarseMSB:
      return freeink::gray16::kMaskPass1Dtm2;
    case Plane::Gray16FineLSB:
      return freeink::gray16::kMaskPass2Dtm1;
    case Plane::Gray16FineMSB:
      return freeink::gray16::kMaskPass2Dtm2;
    case Plane::Gray16CleanupDTM1:
      return freeink::gray16::kMaskCleanupBlack;
  }
  return 0;
}

bool planeUsesStatePolarity(Plane plane) {
  return plane == Plane::BW || plane == Plane::Gray16BW || plane == Plane::Gray16CleanupDTM1;
}

void compare4Bit(int orientation, int rotation, Plane plane, bool state, int width, int height, int x, int y,
                 int originY, int rows, Rect clip) {
  const auto [dxX, dxY, dyX, dyY] = AXES[rotation];
  const glyphBitmap::Frame logical{x, y, dxX, dxY, dyX, dyY};
  std::vector<uint8_t> bitmap((width * height + 1) / 2);
  for (size_t i = 0; i < bitmap.size(); ++i) {
    bitmap[i] = static_cast<uint8_t>(((i * 5 + 3) << 4) | ((i * 11 + 7) & 0x0F));
  }
  if ((width * height) & 1) {
    bitmap.back() &= 0xF0u;
  }

  std::vector<uint8_t> expected(rows * STRIDE + 32);
  for (size_t i = 0; i < expected.size(); ++i) expected[i] = static_cast<uint8_t>(i * 37 + 0x5A);
  auto actual = expected;

  const uint16_t mask = expected4BitMask(plane);
  const bool statePolarity = planeUsesStatePolarity(plane);
  const bool clearBits = statePolarity ? state : false;
  for (int gy = 0; gy < height; ++gy) {
    for (int gx = 0; gx < width; ++gx) {
      const int lx = x + gx * dxX + gy * dyX;
      const int ly = y + gx * dxY + gy * dyY;
      if (lx < clip.left || ly < clip.top || lx >= clip.right || ly >= clip.bottom) continue;
      const auto [px, py] = physical(orientation, lx, ly);
      if (px < 0 || px >= PANEL_WIDTH || py < originY || py >= originY + rows) continue;
      const int source = gy * width + gx;
      const uint8_t ink = (source & 1) ? (bitmap[source >> 1] & 0x0Fu) : (bitmap[source >> 1] >> 4);
      auto& byte = expected[16 + (py - originY) * STRIDE + px / 8];
      const uint8_t bit = static_cast<uint8_t>(0x80 >> (px % 8));
      if ((mask & (1u << ink)) != 0) {
        if (clearBits)
          byte &= static_cast<uint8_t>(~bit);
        else
          byte |= bit;
      } else if (!statePolarity && plane != Plane::GrayLSB && plane != Plane::GrayMSB && state && ink == 15) {
        byte &= static_cast<uint8_t>(~bit);
      }
    }
  }

  glyphBitmap::Clip local{0, 0, width, height};
  glyphBitmap::clipToRect(logical, clip.left, clip.top, clip.right, clip.bottom, local);
  const auto [px, py] = physical(orientation, x, y);
  const auto [xx, xy] = physical(orientation, x + dxX, y + dxY);
  const auto [yx, yy] = physical(orientation, x + dyX, y + dyY);
  const glyphBitmap::Target target{
      actual.data() + 16, PANEL_WIDTH, STRIDE, originY, rows, {px, py, xx - px, xy - py, yx - px, yy - py}};
  glyphBitmap::draw(bitmap.data(), width, height, 4, plane, state, target, local);
  EXPECT_EQ(expected, actual);
}
}  // namespace

TEST(GlyphBitmap, FourBitMatchesReferenceAcrossAllPlanesOrientationsAndOddWidths) {
  constexpr std::array<Plane, 9> ALL_PLANES = {Plane::BW,
                                               Plane::GrayLSB,
                                               Plane::GrayMSB,
                                               Plane::Gray16BW,
                                               Plane::Gray16CoarseLSB,
                                               Plane::Gray16CoarseMSB,
                                               Plane::Gray16FineLSB,
                                               Plane::Gray16FineMSB,
                                               Plane::Gray16CleanupDTM1};
  for (int orientation = 0; orientation < 4; ++orientation) {
    for (int rotation = 0; rotation < 2; ++rotation) {
      for (Plane plane : ALL_PLANES) {
        for (bool state : {false, true}) {
          for (int width : {1, 3, 7, 13, 16, 31}) {
            for (const auto [x, y] : {std::pair{-3, -2}, std::pair{0, 0}, std::pair{7, 9}}) {
              for (const auto [origin, rows] : {std::pair{0, 32}, std::pair{5, 11}}) {
                compare4Bit(orientation, rotation, plane, state, width, 11, x, y, origin, rows, {-10, -10, 50, 50});
                compare4Bit(orientation, rotation, plane, state, width, 11, x, y, origin, rows, {2, 3, 19, 21});
              }
            }
          }
        }
      }
    }
  }
}

TEST(GlyphBitmap, FourBitRampProducesExactFourGrayPlaneMasks) {
  const uint8_t ramp4bpp[8] = {0x01u, 0x23u, 0x45u, 0x67u, 0x89u, 0xABu, 0xCDu, 0xEFu};
  const glyphBitmap::Clip clip{0, 0, 16, 1};
  const glyphBitmap::Frame frame{0, 0, 1, 0, 0, 1};

  auto runPlane = [&](Plane plane, uint8_t clearVal) -> uint16_t {
    uint8_t row[STRIDE];
    std::fill_n(row, STRIDE, clearVal);
    glyphBitmap::draw(ramp4bpp, 16, 1, 4, plane, true, {row, PANEL_WIDTH, STRIDE, 0, 1, frame}, clip);
    uint16_t bitsByInk = 0;
    for (int ink = 0; ink < 16; ++ink) {
      if (((row[ink / 8] >> (7 - (ink % 8))) & 1u) != 0) {
        bitsByInk |= static_cast<uint16_t>(1u << ink);
      }
    }
    return bitsByInk;
  };

  EXPECT_EQ(static_cast<uint16_t>(~0xFFFCu), runPlane(Plane::BW, 0xFFu));
  EXPECT_EQ(0x0F80u, runPlane(Plane::GrayLSB, 0x00u));
  EXPECT_EQ(0x0FFCu, runPlane(Plane::GrayMSB, 0x00u));
}

TEST(GlyphBitmap, MixedOneBitAndTwoBitGlyphsMatchPromotedFourBitOnGray16Planes) {
  constexpr std::array<Plane, 6> GRAY16_PLANES = {Plane::Gray16BW,        Plane::Gray16CoarseLSB,
                                                  Plane::Gray16CoarseMSB, Plane::Gray16FineLSB,
                                                  Plane::Gray16FineMSB,   Plane::Gray16CleanupDTM1};
  // 8 pixels: 2bpp inks [0, 1, 2, 3, 3, 2, 1, 0] -> 4bpp [0, 6, 9, 15, 15, 9, 6, 0]
  const uint8_t bmp2[2] = {0b00'01'10'11u, 0b11'10'01'00u};
  const uint8_t bmp4From2[4] = {0x06u, 0x9Fu, 0xF9u, 0x60u};
  // 8 pixels: 1bpp inks [0, 1, 0, 1, 1, 0, 0, 1] -> 4bpp [0, 15, 0, 15, 15, 0, 0, 15]
  const uint8_t bmp1[1] = {0b01011001u};
  const uint8_t bmp4From1[4] = {0x0Fu, 0x0Fu, 0xF0u, 0x0Fu};

  for (Plane plane : GRAY16_PLANES) {
    const uint8_t initByte = planeUsesStatePolarity(plane) ? 0xFFu : 0x00u;
    std::vector<uint8_t> out2(STRIDE, initByte), out4From2(STRIDE, initByte);
    std::vector<uint8_t> out1(STRIDE, initByte), out4From1(STRIDE, initByte);
    const glyphBitmap::Clip clip{0, 0, 8, 1};
    const glyphBitmap::Frame frame{0, 0, 1, 0, 0, 1};

    glyphBitmap::draw(bmp2, 8, 1, uint8_t{2}, plane, true, {out2.data(), PANEL_WIDTH, STRIDE, 0, 1, frame}, clip);
    glyphBitmap::draw(bmp4From2, 8, 1, uint8_t{4}, plane, true, {out4From2.data(), PANEL_WIDTH, STRIDE, 0, 1, frame},
                      clip);
    EXPECT_EQ(out4From2, out2) << static_cast<int>(plane);

    glyphBitmap::draw(bmp1, 8, 1, uint8_t{1}, plane, true, {out1.data(), PANEL_WIDTH, STRIDE, 0, 1, frame}, clip);
    glyphBitmap::draw(bmp4From1, 8, 1, uint8_t{4}, plane, true, {out4From1.data(), PANEL_WIDTH, STRIDE, 0, 1, frame},
                      clip);
    EXPECT_EQ(out4From1, out1) << static_cast<int>(plane);
  }
}

TEST(GlyphBitmap, SixteenGrayRampProducesExactLut16Bitmasks) {
  const uint8_t ramp4bpp[8] = {0x01u, 0x23u, 0x45u, 0x67u, 0x89u, 0xABu, 0xCDu, 0xEFu};
  const glyphBitmap::Clip clip{0, 0, 16, 1};
  const glyphBitmap::Frame frame{0, 0, 1, 0, 0, 1};

  auto runPlane = [&](Plane plane, uint8_t clearVal) -> uint16_t {
    uint8_t row[STRIDE];
    std::fill_n(row, STRIDE, clearVal);
    glyphBitmap::draw(ramp4bpp, 16, 1, uint8_t{4}, plane, true, {row, PANEL_WIDTH, STRIDE, 0, 1, frame}, clip);
    uint16_t bitsByInk = 0;
    for (int ink = 0; ink < 16; ++ink) {
      const bool bitIsOne = ((row[ink / 8] >> (7 - (ink % 8))) & 1u) != 0;
      if (bitIsOne) bitsByInk |= static_cast<uint16_t>(1u << ink);
    }
    return bitsByInk;
  };

  // BW & CleanupDTM1 start at 0xFF (1=White) and clear ink bits to 0 (Black).
  const uint16_t bwBits = runPlane(Plane::Gray16BW, 0xFFu);
  const uint16_t cleanupBits = runPlane(Plane::Gray16CleanupDTM1, 0xFFu);
  const uint16_t cLsbBits = runPlane(Plane::Gray16CoarseLSB, 0x00u);
  const uint16_t cMsbBits = runPlane(Plane::Gray16CoarseMSB, 0x00u);
  const uint16_t fLsbBits = runPlane(Plane::Gray16FineLSB, 0x00u);
  const uint16_t fMsbBits = runPlane(Plane::Gray16FineMSB, 0x00u);

  EXPECT_EQ(static_cast<uint16_t>(~0xFF00u), bwBits);       // ink 0..7 stay White (1), ink 8..15 clear to Black (0)
  EXPECT_EQ(static_cast<uint16_t>(~0xFFFEu), cleanupBits);  // CleanupDTM1 clears all ink 1..15 to 0
  EXPECT_EQ(0x24EEu, cLsbBits);
  EXPECT_EQ(0x3756u, cMsbBits);
  EXPECT_EQ(0x71DAu, fLsbBits);
  EXPECT_EQ(0x6D0Au, fMsbBits);

  // Split-Rail Center-Anchored invariants:
  //   - Light outer AA (ink 1..7) stays on White rail (bw=1) in Gray16BW.
  //   - Endpoints ink=0 (G15) and ink=15 (G00) stay passive (0,0) in both Pass 1 and Pass 2.
  //   - Every ink 0..15 maps bijectively through kDecodeGray16[32] to gray4 == 15 - ink.
  EXPECT_EQ(0x00FFu, static_cast<uint16_t>(bwBits & 0x00FFu));
  EXPECT_EQ(0u, static_cast<uint16_t>((cLsbBits | cMsbBits | fLsbBits | fMsbBits) & 0x8001u));
  for (int ink = 0; ink < 16; ++ink) {
    const uint8_t code5 = static_cast<uint8_t>((((bwBits >> ink) & 1u) << 4) | (((cLsbBits >> ink) & 1u) << 3) |
                                               (((cMsbBits >> ink) & 1u) << 2) | (((fLsbBits >> ink) & 1u) << 1) |
                                               ((fMsbBits >> ink) & 1u));
    EXPECT_EQ(static_cast<uint8_t>(15 - ink), freeink::gray16::kDecodeGray16[code5]) << "ink=" << ink;
  }
}

TEST(GlyphBitmap, OverlappingSolidBlackGlyphClearsPriorGray16BleachBits) {
  constexpr std::array<Plane, 4> BLEACH_PLANES = {Plane::Gray16CoarseLSB, Plane::Gray16CoarseMSB, Plane::Gray16FineLSB,
                                                  Plane::Gray16FineMSB};
  const glyphBitmap::Frame frame{0, 0, 1, 0, 0, 1};
  const glyphBitmap::Clip clip8{0, 0, 8, 1};

  const uint8_t solid4bpp[4] = {0xF0u, 0x0Fu, 0xF0u, 0x0Fu};  // inks [15, 0, 0, 15, 15, 0, 0, 15]
  const uint8_t solid2bpp[2] = {0b11'00'00'11u, 0b11'00'00'11u};
  const uint8_t solid1bpp[1] = {0b10011001u};
  constexpr uint8_t expectedAfterClear = 0b01100110u;

  for (Plane plane : BLEACH_PLANES) {
    uint8_t row4[STRIDE], row2[STRIDE], row1[STRIDE], rowWhite[STRIDE];
    std::fill_n(row4, STRIDE, 0xFFu);
    std::fill_n(row2, STRIDE, 0xFFu);
    std::fill_n(row1, STRIDE, 0xFFu);
    std::fill_n(rowWhite, STRIDE, 0xFFu);

    glyphBitmap::draw(solid4bpp, 8, 1, uint8_t{4}, plane, true, {row4, PANEL_WIDTH, STRIDE, 0, 1, frame}, clip8);
    glyphBitmap::draw(solid2bpp, 8, 1, uint8_t{2}, plane, true, {row2, PANEL_WIDTH, STRIDE, 0, 1, frame}, clip8);
    glyphBitmap::draw(solid1bpp, 8, 1, uint8_t{1}, plane, true, {row1, PANEL_WIDTH, STRIDE, 0, 1, frame}, clip8);
    glyphBitmap::draw(solid1bpp, 8, 1, uint8_t{1}, plane, false, {rowWhite, PANEL_WIDTH, STRIDE, 0, 1, frame}, clip8);

    EXPECT_EQ(expectedAfterClear, row4[0]) << static_cast<int>(plane);
    EXPECT_EQ(expectedAfterClear, row2[0]) << static_cast<int>(plane);
    EXPECT_EQ(expectedAfterClear, row1[0]) << static_cast<int>(plane);
    EXPECT_EQ(0xFFu, rowWhite[0]) << static_cast<int>(plane);
  }
}

TEST(GlyphBitmap, ContrastPresetsPreserveSplitRailCalibrationAndMonotonicStemWeight) {
  using freeink::gray16::kDecodeGray16;
  using freeink::gray16::kMask4BitBw;
  using freeink::gray16::kMask4BitGrayLsb;
  using freeink::gray16::kMask4BitGrayMsb;
  using freeink::gray16::kMaskBwClear;
  using freeink::gray16::kMaskCleanupBlack;
  using freeink::gray16::kMaskPass1Dtm1;
  using freeink::gray16::kMaskPass1Dtm2;
  using freeink::gray16::kMaskPass2Dtm1;
  using freeink::gray16::kMaskPass2Dtm2;
  using glyphBitmap::gray16ContrastMasks;
  using glyphBitmap::Gray16MaskSet;
  using glyphBitmap::sanitizeTextContrast;
  using glyphBitmap::TextContrast;

  // (a) Crisp preset (and out-of-range fallback) is 100% bit-identical to calibrated base masks.
  const Gray16MaskSet& crisp = gray16ContrastMasks(TextContrast::Crisp);
  EXPECT_EQ(kMaskBwClear, crisp.bwClear);
  EXPECT_EQ(kMaskPass1Dtm1, crisp.pass1Dtm1);
  EXPECT_EQ(kMaskPass1Dtm2, crisp.pass1Dtm2);
  EXPECT_EQ(kMaskPass2Dtm1, crisp.pass2Dtm1);
  EXPECT_EQ(kMaskPass2Dtm2, crisp.pass2Dtm2);
  EXPECT_EQ(kMask4BitBw, crisp.bw4Bit);
  EXPECT_EQ(kMask4BitGrayLsb, crisp.grayLsb4Bit);
  EXPECT_EQ(kMask4BitGrayMsb, crisp.grayMsb4Bit);
  EXPECT_EQ(TextContrast::Crisp, sanitizeTextContrast(99));

  // (b) Render a 16-pixel 4bpp ramp [0..15] through all 4 presets and decode via kDecodeGray16[32].
  const uint8_t ramp4bpp[8] = {0x01u, 0x23u, 0x45u, 0x67u, 0x89u, 0xABu, 0xCDu, 0xEFu};
  const glyphBitmap::Clip clip16{0, 0, 16, 1};
  const glyphBitmap::Frame frame{0, 0, 1, 0, 0, 1};

  constexpr std::array<TextContrast, 4> PRESETS = {TextContrast::Light, TextContrast::Crisp, TextContrast::Medium,
                                                   TextContrast::Dark};

  auto runRampPlane = [&](Plane plane, uint8_t initVal, TextContrast contrast) -> uint16_t {
    uint8_t row[STRIDE];
    std::fill_n(row, STRIDE, initVal);
    glyphBitmap::draw(ramp4bpp, 16, 1, uint8_t{4}, plane, true, {row, PANEL_WIDTH, STRIDE, 0, 1, frame}, clip16,
                      contrast);
    uint16_t bitsByInk = 0;
    for (int ink = 0; ink < 16; ++ink) {
      if (((row[ink / 8] >> (7 - (ink % 8))) & 1u) != 0) {
        bitsByInk |= static_cast<uint16_t>(1u << ink);
      }
    }
    return bitsByInk;
  };

  for (TextContrast preset : PRESETS) {
    const uint16_t bwBits = runRampPlane(Plane::Gray16BW, 0xFFu, preset);
    const uint16_t cleanupBits = runRampPlane(Plane::Gray16CleanupDTM1, 0xFFu, preset);
    const uint16_t cLsbBits = runRampPlane(Plane::Gray16CoarseLSB, 0x00u, preset);
    const uint16_t cMsbBits = runRampPlane(Plane::Gray16CoarseMSB, 0x00u, preset);
    const uint16_t fLsbBits = runRampPlane(Plane::Gray16FineLSB, 0x00u, preset);
    const uint16_t fMsbBits = runRampPlane(Plane::Gray16FineMSB, 0x00u, preset);

    // CleanupDTM1 clears ink 1..15 identically across all presets because remap(1..15) >= 1.
    EXPECT_EQ(static_cast<uint16_t>(~kMaskCleanupBlack), cleanupBits) << static_cast<int>(preset);
    // Transparent ink=0 and solid black ink=15 stay passive (0,0) in both Pass 1 and Pass 2.
    EXPECT_EQ(0u, static_cast<uint16_t>((cLsbBits | cMsbBits | fLsbBits | fMsbBits) & 0x8001u))
        << static_cast<int>(preset);

    std::array<uint8_t, 16> decodedGray{};
    for (int ink = 0; ink < 16; ++ink) {
      const uint8_t code5 = static_cast<uint8_t>((((bwBits >> ink) & 1u) << 4) | (((cLsbBits >> ink) & 1u) << 3) |
                                                 (((cMsbBits >> ink) & 1u) << 2) | (((fLsbBits >> ink) & 1u) << 1) |
                                                 ((fMsbBits >> ink) & 1u));
      decodedGray[ink] = kDecodeGray16[code5];
    }

    EXPECT_EQ(15u, decodedGray[0]) << "ink=0 must stay G15 white";
    EXPECT_EQ(0u, decodedGray[15]) << "ink=15 must stay G00 solid black";
    for (int ink = 0; ink < 15; ++ink) {
      EXPECT_GE(decodedGray[ink], decodedGray[ink + 1])
          << "preset=" << static_cast<int>(preset) << " non-monotonic at ink=" << ink;
    }
    if (preset == TextContrast::Crisp) {
      for (int ink = 0; ink < 16; ++ink) {
        EXPECT_EQ(static_cast<uint8_t>(15 - ink), decodedGray[ink]);
      }
    }
  }

  // (c) Optical ink density on test stem [0, 4, 12, 15, 8, 2, 0]: Dark > Medium > Crisp > Light.
  // 7 pixels packed MSB-first into 4 bytes: (0,4), (12,15), (8,2), (0,0).
  const uint8_t stem4bpp[4] = {0x04u, 0xCFu, 0x82u, 0x00u};
  const glyphBitmap::Clip clip7{0, 0, 7, 1};
  constexpr std::array<double, 16> CARTA_LIGHTNESS_PCT = {
      0.00, 3.97, 9.32, 18.51, 22.45, 27.34, 35.61, 47.29, 49.30, 59.59, 70.36, 74.66, 83.93, 89.61, 94.28, 100.00,
  };

  auto measureStemDensity = [&](TextContrast contrast) -> std::pair<int, double> {
    uint8_t bwRow[STRIDE], cLsbRow[STRIDE], cMsbRow[STRIDE], fLsbRow[STRIDE], fMsbRow[STRIDE];
    std::fill_n(bwRow, STRIDE, 0xFFu);
    std::fill_n(cLsbRow, STRIDE, 0x00u);
    std::fill_n(cMsbRow, STRIDE, 0x00u);
    std::fill_n(fLsbRow, STRIDE, 0x00u);
    std::fill_n(fMsbRow, STRIDE, 0x00u);

    glyphBitmap::draw(stem4bpp, 7, 1, uint8_t{4}, Plane::Gray16BW, true, {bwRow, PANEL_WIDTH, STRIDE, 0, 1, frame},
                      clip7, contrast);
    glyphBitmap::draw(stem4bpp, 7, 1, uint8_t{4}, Plane::Gray16CoarseLSB, true,
                      {cLsbRow, PANEL_WIDTH, STRIDE, 0, 1, frame}, clip7, contrast);
    glyphBitmap::draw(stem4bpp, 7, 1, uint8_t{4}, Plane::Gray16CoarseMSB, true,
                      {cMsbRow, PANEL_WIDTH, STRIDE, 0, 1, frame}, clip7, contrast);
    glyphBitmap::draw(stem4bpp, 7, 1, uint8_t{4}, Plane::Gray16FineLSB, true,
                      {fLsbRow, PANEL_WIDTH, STRIDE, 0, 1, frame}, clip7, contrast);
    glyphBitmap::draw(stem4bpp, 7, 1, uint8_t{4}, Plane::Gray16FineMSB, true,
                      {fMsbRow, PANEL_WIDTH, STRIDE, 0, 1, frame}, clip7, contrast);

    int digitalInkSum = 0;
    double opticalAbsorptionSum = 0.0;
    for (int x = 0; x < 7; ++x) {
      const int shift = 7 - x;
      const uint8_t code5 = static_cast<uint8_t>(
          (((bwRow[0] >> shift) & 1u) << 4) | (((cLsbRow[0] >> shift) & 1u) << 3) |
          (((cMsbRow[0] >> shift) & 1u) << 2) | (((fLsbRow[0] >> shift) & 1u) << 1) | ((fMsbRow[0] >> shift) & 1u));
      const uint8_t gray = kDecodeGray16[code5];
      if (x == 0 || x == 6) {
        EXPECT_EQ(15u, gray) << "transparent flank must remain G15 white";
      }
      if (x == 3) {
        EXPECT_EQ(0u, gray) << "stem center ink=15 must remain G00 black";
      }
      digitalInkSum += (15 - gray);
      opticalAbsorptionSum += (100.0 - CARTA_LIGHTNESS_PCT[gray]);
    }
    return {digitalInkSum, opticalAbsorptionSum};
  };

  const auto [lightInk, lightOpt] = measureStemDensity(TextContrast::Light);
  const auto [crispInk, crispOpt] = measureStemDensity(TextContrast::Crisp);
  const auto [mediumInk, mediumOpt] = measureStemDensity(TextContrast::Medium);
  const auto [darkInk, darkOpt] = measureStemDensity(TextContrast::Dark);

  EXPECT_EQ(37, lightInk);
  EXPECT_EQ(41, crispInk);
  EXPECT_EQ(50, mediumInk);
  EXPECT_EQ(56, darkInk);
  EXPECT_GT(darkInk, mediumInk);
  EXPECT_GT(mediumInk, crispInk);
  EXPECT_GT(crispInk, lightInk);
  EXPECT_GT(darkOpt, mediumOpt);
  EXPECT_GT(mediumOpt, crispOpt);
  EXPECT_GT(crispOpt, lightOpt);

  // Verify 1bpp glyphs stay invariant across contrast presets on all planes, and 2bpp UI glyphs stay invariant
  // across 4-gray planes (BW, GrayLSB, GrayMSB) and Gray16CleanupDTM1.
  const uint8_t ui2bpp[1] = {0b00'01'10'11u};
  const uint8_t ui1bpp[1] = {0b0110'0000u};
  const glyphBitmap::Clip clip4{0, 0, 4, 1};
  for (Plane plane : {Plane::BW, Plane::GrayLSB, Plane::GrayMSB, Plane::Gray16BW, Plane::Gray16CoarseLSB,
                      Plane::Gray16CoarseMSB, Plane::Gray16FineLSB, Plane::Gray16FineMSB, Plane::Gray16CleanupDTM1}) {
    const uint8_t initByte = planeUsesStatePolarity(plane) ? 0xFFu : 0x00u;
    uint8_t base1[STRIDE], dark1[STRIDE], light1[STRIDE];
    std::fill_n(base1, STRIDE, initByte);
    std::fill_n(dark1, STRIDE, initByte);
    std::fill_n(light1, STRIDE, initByte);
    glyphBitmap::draw(ui1bpp, 4, 1, uint8_t{1}, plane, true, {base1, PANEL_WIDTH, STRIDE, 0, 1, frame}, clip4,
                      TextContrast::Crisp);
    glyphBitmap::draw(ui1bpp, 4, 1, uint8_t{1}, plane, true, {dark1, PANEL_WIDTH, STRIDE, 0, 1, frame}, clip4,
                      TextContrast::Dark);
    glyphBitmap::draw(ui1bpp, 4, 1, uint8_t{1}, plane, true, {light1, PANEL_WIDTH, STRIDE, 0, 1, frame}, clip4,
                      TextContrast::Light);
    EXPECT_EQ(base1[0], dark1[0]) << "1bpp plane=" << static_cast<int>(plane);
    EXPECT_EQ(base1[0], light1[0]) << "1bpp plane=" << static_cast<int>(plane);
  }
  for (Plane plane : {Plane::BW, Plane::GrayLSB, Plane::GrayMSB, Plane::Gray16CleanupDTM1}) {
    const uint8_t initByte = planeUsesStatePolarity(plane) ? 0xFFu : 0x00u;
    uint8_t base2[STRIDE], dark2[STRIDE], light2[STRIDE];
    std::fill_n(base2, STRIDE, initByte);
    std::fill_n(dark2, STRIDE, initByte);
    std::fill_n(light2, STRIDE, initByte);
    glyphBitmap::draw(ui2bpp, 4, 1, uint8_t{2}, plane, true, {base2, PANEL_WIDTH, STRIDE, 0, 1, frame}, clip4,
                      TextContrast::Crisp);
    glyphBitmap::draw(ui2bpp, 4, 1, uint8_t{2}, plane, true, {dark2, PANEL_WIDTH, STRIDE, 0, 1, frame}, clip4,
                      TextContrast::Dark);
    glyphBitmap::draw(ui2bpp, 4, 1, uint8_t{2}, plane, true, {light2, PANEL_WIDTH, STRIDE, 0, 1, frame}, clip4,
                      TextContrast::Light);
    EXPECT_EQ(base2[0], dark2[0]) << "2bpp 4-gray/cleanup plane=" << static_cast<int>(plane);
    EXPECT_EQ(base2[0], light2[0]) << "2bpp 4-gray/cleanup plane=" << static_cast<int>(plane);
  }
}

TEST(GlyphBitmap, TwoBitGray16ContrastPresetsMatchSplitRailAnchorsAndMonotonicDensity) {
  using freeink::gray16::kDecodeGray16;
  using freeink::gray16::kMaskBwClear;
  using freeink::gray16::kMaskCleanupBlack;
  using freeink::gray16::kMaskPass1Dtm1;
  using freeink::gray16::kMaskPass1Dtm2;
  using freeink::gray16::kMaskPass2Dtm1;
  using freeink::gray16::kMaskPass2Dtm2;
  using freeink::gray16::mask2BitFrom4Bit;
  using glyphBitmap::gray16ContrastMasks;
  using glyphBitmap::Gray16MaskSet;
  using glyphBitmap::TextContrast;

  // (a) Crisp 2-bit Gray16 masks are 100% bit-identical to mask2BitFrom4Bit(kMask*).
  const Gray16MaskSet& crisp = gray16ContrastMasks(TextContrast::Crisp);
  EXPECT_EQ(mask2BitFrom4Bit(kMaskBwClear), crisp.bwClear2Bit);
  EXPECT_EQ(mask2BitFrom4Bit(kMaskPass1Dtm1), crisp.pass1Dtm1_2Bit);
  EXPECT_EQ(mask2BitFrom4Bit(kMaskPass1Dtm2), crisp.pass1Dtm2_2Bit);
  EXPECT_EQ(mask2BitFrom4Bit(kMaskPass2Dtm1), crisp.pass2Dtm1_2Bit);
  EXPECT_EQ(mask2BitFrom4Bit(kMaskPass2Dtm2), crisp.pass2Dtm2_2Bit);

  // (b) Render a 4-pixel 2bpp ramp [0, 1, 2, 3] across all 4 presets and verify Split-Rail anchors:
  //   Light:  {0, 3, 8, 15}  -> decoded gray {15, 12, 7, 0}, v=1 stays on White rail (bw=1)
  //   Crisp:  {0, 6, 9, 15}  -> decoded gray {15, 9, 6, 0},  v=1 stays on White rail (bw=1)
  //   Medium: {0, 7, 11, 15} -> decoded gray {15, 8, 4, 0},  v=1 stays on White rail (bw=1)
  //   Dark:   {0, 9, 14, 15} -> decoded gray {15, 6, 1, 0},  v=1 enters Black rail (bw=0)
  const uint8_t ramp2bpp[1] = {0b00'01'10'11u};
  const glyphBitmap::Clip clip4{0, 0, 4, 1};
  const glyphBitmap::Frame frame{0, 0, 1, 0, 0, 1};

  struct PresetExpectation {
    TextContrast preset;
    std::array<uint8_t, 4> expectedInk;
    uint8_t expectedV1BwBit;
  };
  constexpr std::array<PresetExpectation, 4> EXPECTATIONS = {{
      {TextContrast::Light, {0, 3, 8, 15}, 1u},
      {TextContrast::Crisp, {0, 6, 9, 15}, 1u},
      {TextContrast::Medium, {0, 7, 11, 15}, 1u},
      {TextContrast::Dark, {0, 9, 14, 15}, 0u},
  }};

  for (const auto& exp : EXPECTATIONS) {
    uint8_t bwRow[STRIDE], clRow[STRIDE], cmRow[STRIDE], flRow[STRIDE], fmRow[STRIDE], cleanRow[STRIDE];
    std::fill_n(bwRow, STRIDE, 0xFFu);
    std::fill_n(clRow, STRIDE, 0x00u);
    std::fill_n(cmRow, STRIDE, 0x00u);
    std::fill_n(flRow, STRIDE, 0x00u);
    std::fill_n(fmRow, STRIDE, 0x00u);
    std::fill_n(cleanRow, STRIDE, 0xFFu);

    glyphBitmap::draw(ramp2bpp, 4, 1, uint8_t{2}, Plane::Gray16BW, true, {bwRow, PANEL_WIDTH, STRIDE, 0, 1, frame},
                      clip4, exp.preset);
    glyphBitmap::draw(ramp2bpp, 4, 1, uint8_t{2}, Plane::Gray16CoarseLSB, true,
                      {clRow, PANEL_WIDTH, STRIDE, 0, 1, frame}, clip4, exp.preset);
    glyphBitmap::draw(ramp2bpp, 4, 1, uint8_t{2}, Plane::Gray16CoarseMSB, true,
                      {cmRow, PANEL_WIDTH, STRIDE, 0, 1, frame}, clip4, exp.preset);
    glyphBitmap::draw(ramp2bpp, 4, 1, uint8_t{2}, Plane::Gray16FineLSB, true, {flRow, PANEL_WIDTH, STRIDE, 0, 1, frame},
                      clip4, exp.preset);
    glyphBitmap::draw(ramp2bpp, 4, 1, uint8_t{2}, Plane::Gray16FineMSB, true, {fmRow, PANEL_WIDTH, STRIDE, 0, 1, frame},
                      clip4, exp.preset);
    glyphBitmap::draw(ramp2bpp, 4, 1, uint8_t{2}, Plane::Gray16CleanupDTM1, true,
                      {cleanRow, PANEL_WIDTH, STRIDE, 0, 1, frame}, clip4, exp.preset);

    for (int v = 0; v < 4; ++v) {
      const int shift = 7 - v;
      const uint8_t bw = (bwRow[0] >> shift) & 1u;
      const uint8_t cl = (clRow[0] >> shift) & 1u;
      const uint8_t cm = (cmRow[0] >> shift) & 1u;
      const uint8_t fl = (flRow[0] >> shift) & 1u;
      const uint8_t fm = (fmRow[0] >> shift) & 1u;
      const uint8_t cln = (cleanRow[0] >> shift) & 1u;
      const uint8_t code5 = static_cast<uint8_t>((bw << 4) | (cl << 3) | (cm << 2) | (fl << 1) | fm);
      EXPECT_EQ(static_cast<uint8_t>(15 - exp.expectedInk[v]), kDecodeGray16[code5])
          << "preset=" << static_cast<int>(exp.preset) << " v=" << v;
      EXPECT_EQ(v >= 1 ? 0u : 1u, cln) << "cleanup preset=" << static_cast<int>(exp.preset) << " v=" << v;
      if (v == 1) {
        EXPECT_EQ(exp.expectedV1BwBit, bw) << "preset=" << static_cast<int>(exp.preset);
      }
      if (v == 2 || v == 3) {
        EXPECT_EQ(0u, bw) << "preset=" << static_cast<int>(exp.preset) << " v=" << v;
      }
      if (v == 0 || v == 3) {
        EXPECT_EQ(0u, cl | cm | fl | fm) << "passive endpoints preset=" << static_cast<int>(exp.preset) << " v=" << v;
      }
    }

    // Overlapping solid-black v=3 clears prior bleach bits across all presets:
    uint8_t prefilled[STRIDE];
    std::fill_n(prefilled, STRIDE, 0xFFu);
    glyphBitmap::draw(ramp2bpp, 4, 1, uint8_t{2}, Plane::Gray16CoarseLSB, true,
                      {prefilled, PANEL_WIDTH, STRIDE, 0, 1, frame}, clip4, exp.preset);
    EXPECT_EQ(0u, (prefilled[0] >> 4) & 1u) << "v=3 at x=3 must clear prior bleach bit";
  }

  // (c) Monotonic stem density on 2bpp stem [0, 1, 2, 3, 2, 1, 0]: Dark > Medium > Crisp > Light.
  const uint8_t stem2bpp[2] = {0b00'01'10'11u, 0b10'01'00'00u};
  const glyphBitmap::Clip clip7{0, 0, 7, 1};
  constexpr std::array<double, 16> CARTA_LIGHTNESS_PCT = {
      0.00, 3.97, 9.32, 18.51, 22.45, 27.34, 35.61, 47.29, 49.30, 59.59, 70.36, 74.66, 83.93, 89.61, 94.28, 100.00,
  };
  auto measure2BitStem = [&](TextContrast contrast) -> std::pair<int, double> {
    uint8_t bwRow[STRIDE], clRow[STRIDE], cmRow[STRIDE], flRow[STRIDE], fmRow[STRIDE];
    std::fill_n(bwRow, STRIDE, 0xFFu);
    std::fill_n(clRow, STRIDE, 0x00u);
    std::fill_n(cmRow, STRIDE, 0x00u);
    std::fill_n(flRow, STRIDE, 0x00u);
    std::fill_n(fmRow, STRIDE, 0x00u);
    glyphBitmap::draw(stem2bpp, 7, 1, uint8_t{2}, Plane::Gray16BW, true, {bwRow, PANEL_WIDTH, STRIDE, 0, 1, frame},
                      clip7, contrast);
    glyphBitmap::draw(stem2bpp, 7, 1, uint8_t{2}, Plane::Gray16CoarseLSB, true,
                      {clRow, PANEL_WIDTH, STRIDE, 0, 1, frame}, clip7, contrast);
    glyphBitmap::draw(stem2bpp, 7, 1, uint8_t{2}, Plane::Gray16CoarseMSB, true,
                      {cmRow, PANEL_WIDTH, STRIDE, 0, 1, frame}, clip7, contrast);
    glyphBitmap::draw(stem2bpp, 7, 1, uint8_t{2}, Plane::Gray16FineLSB, true, {flRow, PANEL_WIDTH, STRIDE, 0, 1, frame},
                      clip7, contrast);
    glyphBitmap::draw(stem2bpp, 7, 1, uint8_t{2}, Plane::Gray16FineMSB, true, {fmRow, PANEL_WIDTH, STRIDE, 0, 1, frame},
                      clip7, contrast);
    int inkSum = 0;
    double optSum = 0.0;
    for (int x = 0; x < 7; ++x) {
      const int shift = 7 - x;
      const uint8_t code5 = static_cast<uint8_t>((((bwRow[0] >> shift) & 1u) << 4) | (((clRow[0] >> shift) & 1u) << 3) |
                                                 (((cmRow[0] >> shift) & 1u) << 2) | (((flRow[0] >> shift) & 1u) << 1) |
                                                 ((fmRow[0] >> shift) & 1u));
      const uint8_t gray = kDecodeGray16[code5];
      inkSum += (15 - gray);
      optSum += (100.0 - CARTA_LIGHTNESS_PCT[gray]);
    }
    return {inkSum, optSum};
  };

  const auto [lightInk, lightOpt] = measure2BitStem(TextContrast::Light);
  const auto [crispInk, crispOpt] = measure2BitStem(TextContrast::Crisp);
  const auto [mediumInk, mediumOpt] = measure2BitStem(TextContrast::Medium);
  const auto [darkInk, darkOpt] = measure2BitStem(TextContrast::Dark);

  EXPECT_EQ(37, lightInk);   // 0 + 3 + 8 + 15 + 8 + 3 + 0
  EXPECT_EQ(45, crispInk);   // 0 + 6 + 9 + 15 + 9 + 6 + 0
  EXPECT_EQ(51, mediumInk);  // 0 + 7 + 11 + 15 + 11 + 7 + 0
  EXPECT_EQ(61, darkInk);    // 0 + 9 + 14 + 15 + 14 + 9 + 0
  EXPECT_GT(darkInk, mediumInk);
  EXPECT_GT(mediumInk, crispInk);
  EXPECT_GT(crispInk, lightInk);
  EXPECT_GT(darkOpt, mediumOpt);
  EXPECT_GT(mediumOpt, crispOpt);
  EXPECT_GT(crispOpt, lightOpt);
}

TEST(GlyphBitmap, FourGrayPlanesApplyContrastPresetsMonotonicallyForFourBitGlyphs) {
  using freeink::gray16::kMask4BitBw;
  using freeink::gray16::kMask4BitGrayLsb;
  using freeink::gray16::kMask4BitGrayMsb;
  using glyphBitmap::TextContrast;

  const uint8_t ramp4bpp[8] = {0x01u, 0x23u, 0x45u, 0x67u, 0x89u, 0xABu, 0xCDu, 0xEFu};
  const glyphBitmap::Clip clip16{0, 0, 16, 1};
  const glyphBitmap::Frame frame{0, 0, 1, 0, 0, 1};

  auto runRampPlane = [&](Plane plane, uint8_t initVal, bool state, TextContrast contrast) -> uint16_t {
    uint8_t row[STRIDE];
    std::fill_n(row, STRIDE, initVal);
    glyphBitmap::draw(ramp4bpp, 16, 1, uint8_t{4}, plane, state, {row, PANEL_WIDTH, STRIDE, 0, 1, frame}, clip16,
                      contrast);
    uint16_t bitsByInk = 0;
    for (int ink = 0; ink < 16; ++ink) {
      if (((row[ink / 8] >> (7 - (ink % 8))) & 1u) != 0) {
        bitsByInk |= static_cast<uint16_t>(1u << ink);
      }
    }
    return bitsByInk;
  };

  // Crisp is bit-for-bit identical to {kMask4BitBw, kMask4BitGrayLsb, kMask4BitGrayMsb}.
  EXPECT_EQ(static_cast<uint16_t>(~kMask4BitBw), runRampPlane(Plane::BW, 0xFFu, true, TextContrast::Crisp));
  EXPECT_EQ(kMask4BitBw, runRampPlane(Plane::BW, 0x00u, false, TextContrast::Crisp));
  EXPECT_EQ(kMask4BitGrayLsb, runRampPlane(Plane::GrayLSB, 0x00u, true, TextContrast::Crisp));
  EXPECT_EQ(kMask4BitGrayMsb, runRampPlane(Plane::GrayMSB, 0x00u, true, TextContrast::Crisp));

  // Decode OverlayMasks 4-gray ink (0 = white, 1 = light gray, 2 = dark gray, 3 = black).
  auto decodeRamp4Gray = [&](TextContrast contrast) -> std::pair<std::array<uint8_t, 16>, int> {
    const uint16_t bwBits = runRampPlane(Plane::BW, 0xFFu, true, contrast);
    const uint16_t lsbBits = runRampPlane(Plane::GrayLSB, 0x00u, true, contrast);
    const uint16_t msbBits = runRampPlane(Plane::GrayMSB, 0x00u, true, contrast);
    std::array<uint8_t, 16> levels{};
    int sum = 0;
    for (int ink = 0; ink < 16; ++ink) {
      const bool bwWhite = ((bwBits >> ink) & 1u) != 0;
      const bool lsb = ((lsbBits >> ink) & 1u) != 0;
      const bool msb = ((msbBits >> ink) & 1u) != 0;
      if (bwWhite) {
        EXPECT_FALSE(lsb);
        EXPECT_FALSE(msb);
        levels[ink] = 0;
      } else if (!msb) {
        EXPECT_FALSE(lsb);
        levels[ink] = 3;
      } else {
        levels[ink] = lsb ? 2 : 1;
      }
      sum += levels[ink];
    }
    EXPECT_EQ(0u, levels[0]);
    EXPECT_EQ(3u, levels[15]);
    for (int ink = 0; ink < 15; ++ink) {
      EXPECT_LE(levels[ink], levels[ink + 1]) << "contrast=" << static_cast<int>(contrast) << " ink=" << ink;
    }
    return {levels, sum};
  };

  const auto [lightLevels, lightRampSum] = decodeRamp4Gray(TextContrast::Light);
  const auto [crispLevels, crispRampSum] = decodeRamp4Gray(TextContrast::Crisp);
  const auto [mediumLevels, mediumRampSum] = decodeRamp4Gray(TextContrast::Medium);
  const auto [darkLevels, darkRampSum] = decodeRamp4Gray(TextContrast::Dark);

  for (int ink = 0; ink < 16; ++ink) {
    EXPECT_LE(lightLevels[ink], crispLevels[ink]);
    EXPECT_LE(crispLevels[ink], mediumLevels[ink]);
    EXPECT_LE(mediumLevels[ink], darkLevels[ink]);
  }
  EXPECT_EQ(24, lightRampSum);
  EXPECT_EQ(27, crispRampSum);
  EXPECT_EQ(33, mediumRampSum);
  EXPECT_EQ(37, darkRampSum);
  EXPECT_GT(darkRampSum, mediumRampSum);
  EXPECT_GT(mediumRampSum, crispRampSum);
  EXPECT_GT(crispRampSum, lightRampSum);

  // Subpixel-flank 4bpp stem [0, 1, 6, 12, 15, 10, 3, 0] across 4-gray planes: Dark > Medium > Crisp > Light.
  const uint8_t stem4bpp[4] = {0x01u, 0x6Cu, 0xFAu, 0x30u};
  const glyphBitmap::Clip clip8{0, 0, 8, 1};
  auto measureStem4Gray = [&](TextContrast contrast) -> int {
    uint8_t bwRow[STRIDE], lsbRow[STRIDE], msbRow[STRIDE];
    std::fill_n(bwRow, STRIDE, 0xFFu);
    std::fill_n(lsbRow, STRIDE, 0x00u);
    std::fill_n(msbRow, STRIDE, 0x00u);
    glyphBitmap::draw(stem4bpp, 8, 1, uint8_t{4}, Plane::BW, true, {bwRow, PANEL_WIDTH, STRIDE, 0, 1, frame}, clip8,
                      contrast);
    glyphBitmap::draw(stem4bpp, 8, 1, uint8_t{4}, Plane::GrayLSB, true, {lsbRow, PANEL_WIDTH, STRIDE, 0, 1, frame},
                      clip8, contrast);
    glyphBitmap::draw(stem4bpp, 8, 1, uint8_t{4}, Plane::GrayMSB, true, {msbRow, PANEL_WIDTH, STRIDE, 0, 1, frame},
                      clip8, contrast);
    int sum = 0;
    for (int x = 0; x < 8; ++x) {
      const int shift = 7 - x;
      const bool bwWhite = ((bwRow[0] >> shift) & 1u) != 0;
      const bool lsb = ((lsbRow[0] >> shift) & 1u) != 0;
      const bool msb = ((msbRow[0] >> shift) & 1u) != 0;
      sum += bwWhite ? 0 : (!msb ? 3 : (lsb ? 2 : 1));
    }
    return sum;
  };

  const int lightStem = measureStem4Gray(TextContrast::Light);
  const int crispStem = measureStem4Gray(TextContrast::Crisp);
  const int mediumStem = measureStem4Gray(TextContrast::Medium);
  const int darkStem = measureStem4Gray(TextContrast::Dark);
  EXPECT_EQ(9, lightStem);
  EXPECT_EQ(10, crispStem);
  EXPECT_EQ(13, mediumStem);
  EXPECT_EQ(14, darkStem);
  EXPECT_GT(darkStem, mediumStem);
  EXPECT_GT(mediumStem, crispStem);
  EXPECT_GT(crispStem, lightStem);
}

TEST(GlyphBitmap, Portrait4BitFastPathBitExactAgainstScalarPaintAcrossPlanesContrastsOffsetsWidthsAndClips) {
  using freeink::gray16::kMaskCleanupBlack;
  using glyphBitmap::gray16ContrastMasks;
  using glyphBitmap::Gray16MaskSet;
  using glyphBitmap::TextContrast;

  constexpr int FUZZ_WIDTH = 96;
  constexpr int FUZZ_HEIGHT = 12;
  constexpr int FUZZ_STRIDE = FUZZ_WIDTH / 8;
  constexpr int GUARD_BYTES = 16;

  constexpr std::array<Plane, 9> ALL_PLANES = {
      Plane::BW,
      Plane::GrayLSB,
      Plane::GrayMSB,
      Plane::Gray16BW,
      Plane::Gray16CoarseLSB,
      Plane::Gray16CoarseMSB,
      Plane::Gray16FineLSB,
      Plane::Gray16FineMSB,
      Plane::Gray16CleanupDTM1,
  };
  constexpr std::array<TextContrast, 4> ALL_CONTRASTS = {
      TextContrast::Light,
      TextContrast::Crisp,
      TextContrast::Medium,
      TextContrast::Dark,
  };

  auto computeMasks = [](Plane plane, bool state, TextContrast contrast) -> std::pair<uint16_t, uint16_t> {
    const Gray16MaskSet& m = gray16ContrastMasks(contrast);
    uint16_t setMask = 0;
    uint16_t clearMask = 0;
    const uint16_t solidBlackBit =
        !state ? 0u : static_cast<uint16_t>(m.bwClear & ~(m.pass1Dtm1 | m.pass1Dtm2 | m.pass2Dtm1 | m.pass2Dtm2));
    auto applyPolarity = [&](uint16_t levels) {
      if (state)
        clearMask = levels;
      else
        setMask = levels;
    };
    switch (plane) {
      case Plane::BW:
        applyPolarity(m.bw4Bit);
        break;
      case Plane::GrayLSB:
        setMask = m.grayLsb4Bit;
        break;
      case Plane::GrayMSB:
        setMask = m.grayMsb4Bit;
        break;
      case Plane::Gray16BW:
        applyPolarity(m.bwClear);
        break;
      case Plane::Gray16CoarseLSB:
        setMask = m.pass1Dtm1;
        clearMask = solidBlackBit;
        break;
      case Plane::Gray16CoarseMSB:
        setMask = m.pass1Dtm2;
        clearMask = solidBlackBit;
        break;
      case Plane::Gray16FineLSB:
        setMask = m.pass2Dtm1;
        clearMask = solidBlackBit;
        break;
      case Plane::Gray16FineMSB:
        setMask = m.pass2Dtm2;
        clearMask = solidBlackBit;
        break;
      case Plane::Gray16CleanupDTM1:
        applyPolarity(kMaskCleanupBlack);
        break;
    }
    return {setMask, clearMask};
  };

  uint32_t rng = 0x59B4B170u;
  auto nextByte = [&rng]() -> uint8_t {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return static_cast<uint8_t>(rng & 0xFFu);
  };

  std::vector<uint8_t> expected(FUZZ_HEIGHT * FUZZ_STRIDE + 2 * GUARD_BYTES);
  std::vector<uint8_t> actual(expected.size());
  std::vector<uint8_t> bitmap;

  constexpr int GLYPH_H = 4;
  for (Plane plane : ALL_PLANES) {
    for (TextContrast contrast : ALL_CONTRASTS) {
      for (bool state : {true, false}) {
        const auto [setMask, clearMask] = computeMasks(plane, state, contrast);
        for (int width = 1; width <= 48; ++width) {
          bitmap.resize((width * GLYPH_H + 1) / 2);
          for (size_t i = 0; i < bitmap.size(); ++i) {
            bitmap[i] = nextByte();
          }
          // Embed a full zero span in row 1 to exercise the zeroTransparent 8-pixel skip.
          for (int gx = 0; gx < width; ++gx) {
            const int srcIdx = 1 * width + gx;
            if (srcIdx & 1)
              bitmap[srcIdx >> 1] &= 0xF0u;
            else
              bitmap[srcIdx >> 1] &= 0x0Fu;
          }
          if ((width * GLYPH_H) & 1) {
            bitmap.back() &= 0xF0u;
          }

          const std::array<std::pair<int, int>, 4> clips = {{
              {0, width},
              {1, std::max(1, width - 1)},
              {2, std::max(2, width - 3)},
              {std::min(width, 5), std::max(std::min(width, 5), width - 4)},
          }};

          for (int x = 0; x <= 15; ++x) {
            for (const auto [clipL, clipR] : clips) {
              for (size_t i = 0; i < expected.size(); ++i) {
                expected[i] = static_cast<uint8_t>(i * 41u + x * 13u + width * 7u + 0x5Au);
              }
              actual = expected;

              const int placeX = (clipL == 2 && width >= 24) ? (FUZZ_WIDTH - width + 5)
                                                             : (clipL == 1 && width >= 16 ? -3 + (x & 3) : x);
              const int placeY = 2;
              const glyphBitmap::Frame frame{placeX, placeY, 1, 0, 0, 1};
              const glyphBitmap::Target target{
                  actual.data() + GUARD_BYTES, FUZZ_WIDTH, FUZZ_STRIDE, 0, FUZZ_HEIGHT, frame};
              const glyphBitmap::Clip clip{clipL, 0, clipR, GLYPH_H};

              glyphBitmap::draw(bitmap.data(), width, GLYPH_H, uint8_t{4}, plane, state, target, clip, contrast);

              // Independent scalar reference using glyphBitmap::paint() per pixel.
              for (int gy = std::max(0, clip.top); gy < std::min(GLYPH_H, clip.bottom); ++gy) {
                const int py = placeY + gy;
                if (py < 0 || py >= FUZZ_HEIGHT) continue;
                for (int gx = std::max(0, clip.left); gx < std::min(width, clip.right); ++gx) {
                  const int px = placeX + gx;
                  if (px < 0 || px >= FUZZ_WIDTH) continue;
                  const int srcIdx = gy * width + gx;
                  const uint8_t ink = (srcIdx & 1) ? (bitmap[srcIdx >> 1] & 0x0Fu) : (bitmap[srcIdx >> 1] >> 4);
                  glyphBitmap::paint(expected.data() + GUARD_BYTES, py * (FUZZ_STRIDE * 8) + px, ink, setMask,
                                     clearMask);
                }
              }

              ASSERT_EQ(expected, actual)
                  << "plane=" << static_cast<int>(plane) << " contrast=" << static_cast<int>(contrast)
                  << " state=" << state << " width=" << width << " x=" << x << " placeX=" << placeX << " clip=["
                  << clipL << "," << clipR << ")";
            }
          }
        }
      }
    }
  }

  // Also fuzz the rotated-Portrait 8-row vertical byte-accumulated fast path
  // (dxX == 0, dxY == -1 / +1, dyX == +1, dyY == 0) across widths 1..19, heights 1..48,
  // all 9 planes, all 4 contrasts, bit offsets 0..15, and 4-side clipping bounds.
  constexpr int ROT_W = 64;
  constexpr int ROT_H = 56;
  constexpr int ROT_STRIDE = ROT_W / 8;
  expected.resize(ROT_H * ROT_STRIDE + 2 * GUARD_BYTES);
  actual.resize(expected.size());
  for (Plane plane : ALL_PLANES) {
    for (TextContrast contrast : ALL_CONTRASTS) {
      for (bool state : {true, false}) {
        const auto [setMask, clearMask] = computeMasks(plane, state, contrast);
        for (int dxY : {-1, 1}) {
          for (int height : {1, 2, 7, 8, 9, 15, 16, 17, 23, 24, 25, 33, 48}) {
            for (int width : {1, 2, 3, 4, 7, 8, 9, 15, 16, 19}) {
              bitmap.resize((width * height + 1) / 2);
              for (size_t i = 0; i < bitmap.size(); ++i) bitmap[i] = nextByte();
              // Zero-out column 0 to exercise zeroTransparent 8-row column skip.
              for (int gy = 0; gy < height; ++gy) {
                const int sIdx = gy * width;
                if (sIdx & 1)
                  bitmap[sIdx >> 1] &= 0xF0u;
                else
                  bitmap[sIdx >> 1] &= 0x0Fu;
              }
              if ((width * height) & 1) bitmap.back() &= 0xF0u;

              for (int physX : {0, 1, 2, 3, 5, 7, 8, 11, 15}) {
                for (const auto [clipTop, clipBottom] : {std::pair{0, height}, std::pair{1, std::max(1, height - 2)},
                                                         std::pair{3, std::max(3, height - 5)}}) {
                  for (const auto [clipL, clipR] : {std::pair{0, width}, std::pair{1, std::max(1, width - 1)}}) {
                    for (size_t i = 0; i < expected.size(); ++i) {
                      expected[i] = static_cast<uint8_t>(i * 29u + physX * 17u + height * 5u + 0xA3u);
                    }
                    actual = expected;
                    const int physY = dxY < 0 ? (ROT_H - 3) : 2;
                    const glyphBitmap::Frame frame{physX, physY, 0, dxY, 1, 0};
                    const glyphBitmap::Target target{actual.data() + GUARD_BYTES, ROT_W, ROT_STRIDE, 0, ROT_H, frame};
                    const glyphBitmap::Clip clip{clipL, clipTop, clipR, clipBottom};

                    glyphBitmap::draw(bitmap.data(), width, height, uint8_t{4}, plane, state, target, clip, contrast);

                    for (int gy = std::max(0, clip.top); gy < std::min(height, clip.bottom); ++gy) {
                      const int px = physX + gy;
                      if (px < 0 || px >= ROT_W) continue;
                      for (int gx = std::max(0, clip.left); gx < std::min(width, clip.right); ++gx) {
                        const int py = physY + gx * dxY;
                        if (py < 0 || py >= ROT_H) continue;
                        const int srcIdx = gy * width + gx;
                        const uint8_t ink = (srcIdx & 1) ? (bitmap[srcIdx >> 1] & 0x0Fu) : (bitmap[srcIdx >> 1] >> 4);
                        glyphBitmap::paint(expected.data() + GUARD_BYTES, py * (ROT_STRIDE * 8) + px, ink, setMask,
                                           clearMask);
                      }
                    }

                    ASSERT_EQ(expected, actual) << "RotatedPortrait plane=" << static_cast<int>(plane)
                                                << " contrast=" << static_cast<int>(contrast) << " dxY=" << dxY
                                                << " w=" << width << " h=" << height << " physX=" << physX;
                  }
                }
              }
            }
          }
        }
      }
    }
  }
}
