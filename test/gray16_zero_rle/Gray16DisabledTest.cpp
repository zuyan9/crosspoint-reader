#include <Epub/Epub/converters/DirectPixelWriter.h>
#include <GfxRenderer.h>
#include <GlyphBitmap.h>
#include <gtest/gtest.h>

static_assert(!CROSSPOINT_SUPPORT_GRAY16, "Gray16DisabledTest must compile with CROSSPOINT_SUPPORT_GRAY16 == 0");

TEST(Gray16DisabledTest, NonX3BuildInlinesGray16StubsAndFallsBackDisplayWindowToDisplayBuffer) {
  HalDisplay hal(800, 480);
  GfxRenderer gfx(hal);
  gfx.begin();

  EXPECT_FALSE(hal.supportsGray16Planes());
  EXPECT_FALSE(hal.supportsDeferredGray16Base());
  EXPECT_FALSE(hal.prewarmPowerOn());
  EXPECT_FALSE(hal.serviceIdlePower());
  EXPECT_FALSE(gfx.supportsGray16Planes());
  EXPECT_FALSE(gfx.supportsDeferredGray16Base());
  EXPECT_FALSE(gfx.prewarmPowerOn());
  gfx.setGray16Base(true);
  EXPECT_FALSE(gfx.isGray16Base());
  EXPECT_FALSE(gfx.consumeDirtyGray16Baseline());
  gfx.setRenderMode(GfxRenderer::BW);
  EXPECT_FALSE(gfx.isOverlayGrayPlane());
  gfx.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  EXPECT_TRUE(gfx.isOverlayGrayPlane());
  gfx.setRenderMode(GfxRenderer::GRAY16_COARSE_LSB);
  EXPECT_FALSE(gfx.isOverlayGrayPlane());
  gfx.setRenderMode(GfxRenderer::BW);

  // On non-X3 builds (SSD1677 / X4 Pro / Sticky), GfxRenderer::displayWindow must
  // route through displayBuffer(FAST_REFRESH) rather than HalDisplay::displayWindow.
  gfx.displayWindow(24, 32, 160, 64);
  EXPECT_EQ(0, hal.displayWindowCalls);
  EXPECT_EQ(1, hal.displayBufferCalls);
  EXPECT_EQ(HalDisplay::FAST_REFRESH, hal.lastRefreshMode);

  // DirectPixelWriter BW mode uses standard 4-gray threshold (pixelValue < 3) when Gray16 is compiled out.
  gfx.setOrientation(GfxRenderer::Portrait);
  gfx.setRenderMode(GfxRenderer::BW);
  gfx.clearScreen(0xFFu);
  DirectPixelWriter writer{};
  writer.init(gfx);
  writer.beginRow(16);
  writer.writePixel(10, 2);  // light gray -> black in 4-gray base
  writer.writePixel(11, 3);  // white -> untouched
  const int gwBytes = gfx.getDisplayWidthBytes();
  const uint8_t* fb = gfx.getFrameBuffer();
  const auto readBit = [&](int lx, int ly) -> uint8_t {
    const int phyX = ly;
    const int phyY = gfx.getDisplayHeight() - 1 - lx;
    return (fb[static_cast<size_t>(phyY) * gwBytes + static_cast<size_t>(phyX / 8)] >> (7 - (phyX % 8))) & 1u;
  };
  EXPECT_EQ(0u, readBit(10, 16));
  EXPECT_EQ(1u, readBit(11, 16));
}

TEST(Gray16DisabledTest, TextContrastAdjustsFourBitGlyphsAcrossFourGrayPlanesWhenGray16Disabled) {
  static_assert(sizeof(glyphBitmap::Gray16MaskSet) == 3 * sizeof(uint16_t));
  static_assert(sizeof(glyphBitmap::kGray16ContrastPresets) == 24);

  HalDisplay hal(800, 480);
  GfxRenderer gfx(hal);
  gfx.begin();
  gfx.setOrientation(GfxRenderer::LandscapeCounterClockwise);

  EXPECT_EQ(GfxRenderer::TextContrast::Crisp, gfx.getTextContrast());
  gfx.setTextContrast(uint8_t{99});
  EXPECT_EQ(GfxRenderer::TextContrast::Crisp, gfx.getTextContrast());

  static constexpr uint8_t kRamp4Bpp[8] = {0x01u, 0x23u, 0x45u, 0x67u, 0x89u, 0xABu, 0xCDu, 0xEFu};
  static constexpr EpdGlyph kGlyphR4[1] = {{16, 1, 16 << 4, 0, 1, 8, 0}};
  static constexpr EpdUnicodeInterval kIntervalR[1] = {{'R', 'R', 0}};
  EpdFontData font4Data{};
  font4Data.bitmap = kRamp4Bpp;
  font4Data.glyph = kGlyphR4;
  font4Data.intervals = kIntervalR;
  font4Data.intervalCount = 1;
  font4Data.advanceY = 16;
  font4Data.ascender = 12;
  font4Data.descender = -4;
  font4Data.is4Bit = true;
  EpdFont font4(&font4Data);
  gfx.insertFont(10, EpdFontFamily(14, &font4));

  static constexpr uint8_t kRamp2Bpp[1] = {0b00'01'10'11u};
  static constexpr EpdGlyph kGlyphR2[1] = {{4, 1, 4 << 4, 0, 1, 1, 0}};
  EpdFontData font2Data{};
  font2Data.bitmap = kRamp2Bpp;
  font2Data.glyph = kGlyphR2;
  font2Data.intervals = kIntervalR;
  font2Data.intervalCount = 1;
  font2Data.advanceY = 16;
  font2Data.ascender = 12;
  font2Data.descender = -4;
  font2Data.is2Bit = true;
  EpdFont font2(&font2Data);
  gfx.insertFont(11, EpdFontFamily(14, &font2));

  const int gwBytes = gfx.getDisplayWidthBytes();
  constexpr int kTextY = 16;
  constexpr int kPhysRow = kTextY + 12 - 1;

  auto measure4BitRamp = [&](GfxRenderer::TextContrast contrast) -> int {
    gfx.setTextContrast(contrast);
    EXPECT_EQ(contrast, gfx.getTextContrast());

    gfx.setRenderMode(GfxRenderer::BW);
    gfx.clearScreen(0xFFu);
    gfx.drawText(10, 0, kTextY, "R");
    std::array<uint8_t, 2> bwBytes = {gfx.getFrameBuffer()[kPhysRow * gwBytes],
                                      gfx.getFrameBuffer()[kPhysRow * gwBytes + 1]};

    gfx.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    gfx.clearScreen(0x00u);
    gfx.drawText(10, 0, kTextY, "R");
    std::array<uint8_t, 2> lsbBytes = {gfx.getFrameBuffer()[kPhysRow * gwBytes],
                                       gfx.getFrameBuffer()[kPhysRow * gwBytes + 1]};

    gfx.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    gfx.clearScreen(0x00u);
    gfx.drawText(10, 0, kTextY, "R");
    std::array<uint8_t, 2> msbBytes = {gfx.getFrameBuffer()[kPhysRow * gwBytes],
                                       gfx.getFrameBuffer()[kPhysRow * gwBytes + 1]};

    int sum = 0;
    for (int ink = 0; ink < 16; ++ink) {
      const int byteIdx = ink / 8;
      const int shift = 7 - (ink % 8);
      const bool bwWhite = ((bwBytes[byteIdx] >> shift) & 1u) != 0;
      const bool lsb = ((lsbBytes[byteIdx] >> shift) & 1u) != 0;
      const bool msb = ((msbBytes[byteIdx] >> shift) & 1u) != 0;
      sum += bwWhite ? 0 : (!msb ? 3 : (lsb ? 2 : 1));
    }
    return sum;
  };

  EXPECT_EQ(24, measure4BitRamp(GfxRenderer::TextContrast::Light));
  EXPECT_EQ(27, measure4BitRamp(GfxRenderer::TextContrast::Crisp));
  EXPECT_EQ(33, measure4BitRamp(GfxRenderer::TextContrast::Medium));
  EXPECT_EQ(37, measure4BitRamp(GfxRenderer::TextContrast::Dark));

  // 2-bit fonts remain invariant across contrast presets on 4-gray planes, and GRAY16_* is a safe no-op.
  auto capture2BitByte = [&](GfxRenderer::RenderMode mode, uint8_t initVal, GfxRenderer::TextContrast c) -> uint8_t {
    gfx.setTextContrast(c);
    gfx.setRenderMode(mode);
    gfx.clearScreen(initVal);
    gfx.drawText(11, 0, kTextY, "R");
    return gfx.getFrameBuffer()[kPhysRow * gwBytes];
  };
  for (const auto [mode, initVal] :
       {std::pair{GfxRenderer::BW, uint8_t{0xFFu}}, std::pair{GfxRenderer::GRAYSCALE_LSB, uint8_t{0x00u}},
        std::pair{GfxRenderer::GRAYSCALE_MSB, uint8_t{0x00u}}}) {
    const uint8_t crisp = capture2BitByte(mode, initVal, GfxRenderer::TextContrast::Crisp);
    EXPECT_EQ(crisp, capture2BitByte(mode, initVal, GfxRenderer::TextContrast::Light));
    EXPECT_EQ(crisp, capture2BitByte(mode, initVal, GfxRenderer::TextContrast::Dark));
  }
  EXPECT_EQ(0x00u, capture2BitByte(GfxRenderer::GRAY16_COARSE_LSB, 0x00u, GfxRenderer::TextContrast::Dark));
}
