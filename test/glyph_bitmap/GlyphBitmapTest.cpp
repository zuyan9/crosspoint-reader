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
  glyphBitmap::draw(bitmap.data(), width, height, twoBit, plane, state, target, local);
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
      return 0xFFFCu;
    case Plane::GrayLSB:
      return 0x0F80u;
    case Plane::GrayMSB:
      return 0x0FFCu;
    case Plane::Gray16BW:
      return 0xFFFEu;
    case Plane::Gray16CoarseLSB:
      return 0x1A8Eu;
    case Plane::Gray16CoarseMSB:
      return 0x1BF0u;
    case Plane::Gray16FineLSB:
      return 0x4CD2u;
    case Plane::Gray16FineMSB:
      return 0x6A64u;
    case Plane::Gray16CleanupDTM1:
      return 0xF000u;
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
  glyphBitmap::draw(bitmap.data(), width, height, uint8_t{4}, plane, state, target, local);
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

TEST(GlyphBitmap, MixedOneBitAndTwoBitGlyphsMatchPromotedFourBitOnGray16Planes) {
  constexpr std::array<Plane, 6> GRAY16_PLANES = {Plane::Gray16BW,        Plane::Gray16CoarseLSB,
                                                  Plane::Gray16CoarseMSB, Plane::Gray16FineLSB,
                                                  Plane::Gray16FineMSB,   Plane::Gray16CleanupDTM1};
  // 8 pixels: 2bpp inks [0, 1, 2, 3, 3, 2, 1, 0] -> 4bpp [0, 5, 10, 15, 15, 10, 5, 0]
  const uint8_t bmp2[2] = {0b00'01'10'11u, 0b11'10'01'00u};
  const uint8_t bmp4From2[4] = {0x05u, 0xAFu, 0xFAu, 0x50u};
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
  // 16 pixels wide with ink 0..15 from left (x=0 -> bit 15 of big-endian uint16) to right (x=15 -> bit 0).
  const uint8_t ramp4bpp[8] = {0x01u, 0x23u, 0x45u, 0x67u, 0x89u, 0xABu, 0xCDu, 0xEFu};
  const glyphBitmap::Clip clip{0, 0, 16, 1};
  const glyphBitmap::Frame frame{0, 0, 1, 0, 0, 1};

  auto runPlane = [&](Plane plane, uint8_t clearVal) -> uint16_t {
    uint8_t row[STRIDE];
    std::fill_n(row, STRIDE, clearVal);
    glyphBitmap::draw(ramp4bpp, 16, 1, uint8_t{4}, plane, true, {row, PANEL_WIDTH, STRIDE, 0, 1, frame}, clip);
    // Reverse pixel order so bit `ink` corresponds to pixel x = ink.
    uint16_t bitsByInk = 0;
    for (int ink = 0; ink < 16; ++ink) {
      const bool bitIsOne = ((row[ink / 8] >> (7 - (ink % 8))) & 1u) != 0;
      if (bitIsOne) bitsByInk |= static_cast<uint16_t>(1u << ink);
    }
    return bitsByInk;
  };

  // BW & CleanupDTM1 start at 0xFF (1=White) and clear ink bits to 0 (Black).
  EXPECT_EQ(static_cast<uint16_t>(~0xFFFEu), runPlane(Plane::Gray16BW, 0xFFu));           // Only ink=0 stays 1
  EXPECT_EQ(static_cast<uint16_t>(~0xF000u), runPlane(Plane::Gray16CleanupDTM1, 0xFFu));  // ink 0..11=1, 12..15=0
  // Pass 1 & Pass 2 start at 0x00 and set 1 bits matching Uc8253X3Lut16.h.
  EXPECT_EQ(0x1A8Eu, runPlane(Plane::Gray16CoarseLSB, 0x00u));
  EXPECT_EQ(0x1BF0u, runPlane(Plane::Gray16CoarseMSB, 0x00u));
  EXPECT_EQ(0x4CD2u, runPlane(Plane::Gray16FineLSB, 0x00u));
  EXPECT_EQ(0x6A64u, runPlane(Plane::Gray16FineMSB, 0x00u));
}

TEST(GlyphBitmap, OverlappingSolidBlackGlyphClearsPriorGray16BleachBits) {
  constexpr std::array<Plane, 4> BLEACH_PLANES = {Plane::Gray16CoarseLSB, Plane::Gray16CoarseMSB, Plane::Gray16FineLSB,
                                                  Plane::Gray16FineMSB};
  const glyphBitmap::Frame frame{0, 0, 1, 0, 0, 1};
  const glyphBitmap::Clip clip8{0, 0, 8, 1};

  // Halo glyph paints all bleach bits across x = 0..7 (or pre-filled 0xFF from an underlying gray image).
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
