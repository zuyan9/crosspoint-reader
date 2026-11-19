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
