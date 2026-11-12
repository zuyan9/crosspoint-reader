#include <Epub/ParsedText.h>
#include <Epub/ReaderRenderSpec.h>
#include <Epub/blocks/TextBlock.h>
#include <Epub/hyphenation/Hyphenator.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <SdCardFont.h>
#include <Uc8253X3Lut16.h>
#include <Utf8.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace {
size_t gFailNothrowArrayAllocSize = 0;
size_t gDeniedNothrowArrayAllocs = 0;
}  // namespace

void* operator new[](const size_t size) {
  void* allocation = std::malloc(size == 0 ? 1 : size);
  if (!allocation) {
    std::fputs("Unexpected host OOM in TypographyLayoutTest\n", stderr);
    std::exit(EXIT_FAILURE);
  }
  return allocation;
}

void* operator new[](const size_t size, const std::nothrow_t&) noexcept {
  if (gFailNothrowArrayAllocSize != 0 && size == gFailNothrowArrayAllocSize) {
    ++gDeniedNothrowArrayAllocs;
    return nullptr;
  }
  return std::malloc(size == 0 ? 1 : size);
}

void operator delete[](void* allocation) noexcept { std::free(allocation); }
void operator delete[](void* allocation, size_t) noexcept { std::free(allocation); }
void operator delete[](void* allocation, const std::nothrow_t&) noexcept { std::free(allocation); }

namespace {

void putU16(std::vector<uint8_t>& buf, const size_t off, const uint16_t val) {
  buf[off] = static_cast<uint8_t>(val & 0xFF);
  buf[off + 1] = static_cast<uint8_t>((val >> 8) & 0xFF);
}

void putI16(std::vector<uint8_t>& buf, const size_t off, const int16_t val) {
  putU16(buf, off, static_cast<uint16_t>(val));
}

void putU32(std::vector<uint8_t>& buf, const size_t off, const uint32_t val) {
  putU16(buf, off, static_cast<uint16_t>(val & 0xFFFF));
  putU16(buf, off + 2, static_cast<uint16_t>((val >> 16) & 0xFFFF));
}

struct GlyphSpec {
  uint32_t cp;
  uint16_t advanceFP;
};

struct KernPairSpec {
  uint16_t leftCp;
  uint16_t rightCp;
  int8_t kernFP;
};

struct LigSpec {
  uint16_t leftCp;
  uint16_t rightCp;
  uint32_t ligCp;
};

// Builds an in-memory v4 .cpfont containing Bookerly 14pt fp4 metrics,
// class kerning, and typographic ligatures (ff, fi, fl, ffi, ffl).
// When fourBit is true, builds a 2-style (REGULAR + BOLD) 4bpp .cpfont.
void buildBookerly14CpFont(const bool fourBit = false) {
  static constexpr GlyphSpec kSpecs[] = {
      {' ', 100},    {'"', 148},    {'\'', 88},    {',', 125},    {'-', 174},    {'.', 125},    {':', 125},
      {';', 125},    {'A', 344},    {'B', 287},    {'C', 298},    {'D', 348},    {'E', 292},    {'F', 264},
      {'G', 337},    {'H', 387},    {'I', 174},    {'J', 165},    {'K', 333},    {'L', 286},    {'M', 415},
      {'N', 379},    {'O', 360},    {'P', 274},    {'Q', 360},    {'R', 309},    {'S', 255},    {'T', 316},
      {'U', 359},    {'V', 341},    {'W', 515},    {'X', 337},    {'Y', 316},    {'Z', 274},    {'a', 254},
      {'b', 280},    {'c', 235},    {'d', 289},    {'e', 240},    {'f', 187},    {'g', 268},    {'h', 307},
      {'i', 151},    {'j', 129},    {'k', 275},    {'l', 148},    {'m', 462},    {'n', 309},    {'o', 270},
      {'p', 287},    {'q', 276},    {'r', 210},    {'s', 221},    {'t', 189},    {'u', 304},    {'v', 280},
      {'w', 415},    {'x', 273},    {'y', 280},    {'z', 230},    {0x00AB, 200}, {0x00BB, 200}, {0x2018, 96},
      {0x2019, 96},  {0x201C, 128}, {0x201D, 128}, {0x2039, 128}, {0x203A, 128}, {0x3002, 288}, {0x300C, 288},
      {0x300D, 288}, {0x4E2D, 288}, {0x6587, 288}, {0xFB00, 348}, {0xFB01, 313}, {0xFB02, 309}, {0xFB03, 475},
      {0xFB04, 471}, {0xFF0C, 288}, {0xFFFD, 467},
  };

  static constexpr KernPairSpec kKerns[] = {
      {'F', 'a', -14}, {'T', 'o', -28}, {'n', 'y', -26}, {'r', 't', 7}, {'x', 'p', 14}, {'y', ' ', -8},
  };

  static constexpr LigSpec kLigs[] = {
      {'f', 'f', 0xFB00}, {'f', 'i', 0xFB01}, {'f', 'l', 0xFB02}, {0xFB00, 'i', 0xFB03}, {0xFB00, 'l', 0xFB04},
  };

  std::vector<GlyphSpec> sorted(std::begin(kSpecs), std::end(kSpecs));
  std::sort(sorted.begin(), sorted.end(), [](const GlyphSpec& a, const GlyphSpec& b) { return a.cp < b.cp; });

  std::vector<EpdUnicodeInterval> intervals;
  uint32_t startIdx = 0;
  for (size_t i = 1; i <= sorted.size(); ++i) {
    if (i == sorted.size() || sorted[i].cp != sorted[i - 1].cp + 1) {
      intervals.push_back({sorted[startIdx].cp, sorted[i - 1].cp, startIdx});
      startIdx = static_cast<uint32_t>(i);
    }
  }

  std::vector<uint16_t> leftCps, rightCps;
  for (const auto& k : kKerns) {
    if (std::find(leftCps.begin(), leftCps.end(), k.leftCp) == leftCps.end()) leftCps.push_back(k.leftCp);
    if (std::find(rightCps.begin(), rightCps.end(), k.rightCp) == rightCps.end()) rightCps.push_back(k.rightCp);
  }
  std::sort(leftCps.begin(), leftCps.end());
  std::sort(rightCps.begin(), rightCps.end());

  const uint8_t klCount = static_cast<uint8_t>(leftCps.size());
  const uint8_t krCount = static_cast<uint8_t>(rightCps.size());
  std::vector<int8_t> matrix(static_cast<size_t>(klCount) * krCount, 0);
  for (const auto& k : kKerns) {
    const size_t li = static_cast<size_t>(std::find(leftCps.begin(), leftCps.end(), k.leftCp) - leftCps.begin());
    const size_t ri = static_cast<size_t>(std::find(rightCps.begin(), rightCps.end(), k.rightCp) - rightCps.begin());
    matrix[li * krCount + ri] = k.kernFP;
  }

  std::vector<EpdLigaturePair> ligPairs;
  for (const auto& l : kLigs) {
    ligPairs.push_back({(static_cast<uint32_t>(l.leftCp) << 16) | l.rightCp, l.ligCp});
  }
  std::sort(ligPairs.begin(), ligPairs.end(),
            [](const EpdLigaturePair& a, const EpdLigaturePair& b) { return a.pair < b.pair; });

  constexpr size_t HEADER_BYTES = 32;
  constexpr size_t TOC_BYTES = 32;
  const uint8_t styleCount = fourBit ? 2 : 1;
  const size_t firstDataOffset = HEADER_BYTES + styleCount * TOC_BYTES;
  const size_t intervalsBytes = intervals.size() * sizeof(EpdUnicodeInterval);
  const size_t glyphsBytes = sorted.size() * sizeof(EpdGlyph);
  const size_t klBytes = leftCps.size() * sizeof(EpdKernClassEntry);
  const size_t krBytes = rightCps.size() * sizeof(EpdKernClassEntry);
  const size_t matBytes = matrix.size();
  const size_t ligBytes = ligPairs.size() * sizeof(EpdLigaturePair);
  // 1x8 marker bitmap: 1 byte at 1bpp, 4 bytes at 4bpp per non-space glyph.
  const uint16_t perGlyphBmpBytes = fourBit ? 4 : 1;
  const size_t bitmapBytes = sorted.size() * perGlyphBmpBytes;
  const size_t perStyleBytes = intervalsBytes + glyphsBytes + klBytes + krBytes + matBytes + ligBytes + bitmapBytes;

  sdFontTestFile.assign(firstDataOffset + styleCount * perStyleBytes, 0);
  std::memcpy(sdFontTestFile.data(), "CPFONT\0\0", 8);
  putU16(sdFontTestFile, 8, CPFONT_VERSION);
  putU16(sdFontTestFile, 10, fourBit ? SdCardFont::CPFONT_FLAG_4BIT : 0);
  sdFontTestFile[12] = styleCount;

  for (uint8_t si = 0; si < styleCount; ++si) {
    const size_t tocOff = HEADER_BYTES + si * TOC_BYTES;
    const size_t dataOffset = firstDataOffset + si * perStyleBytes;
    sdFontTestFile[tocOff + 0] = si;  // styleId = REGULAR(0), BOLD(1)
    putU32(sdFontTestFile, tocOff + 4, static_cast<uint32_t>(intervals.size()));
    putU32(sdFontTestFile, tocOff + 8, static_cast<uint32_t>(sorted.size()));
    sdFontTestFile[tocOff + 12] = 34;         // advanceY
    putI16(sdFontTestFile, tocOff + 13, 27);  // ascender
    putI16(sdFontTestFile, tocOff + 15, -8);  // descender
    putU16(sdFontTestFile, tocOff + 17, static_cast<uint16_t>(leftCps.size()));
    putU16(sdFontTestFile, tocOff + 19, static_cast<uint16_t>(rightCps.size()));
    sdFontTestFile[tocOff + 21] = klCount;
    sdFontTestFile[tocOff + 22] = krCount;
    sdFontTestFile[tocOff + 23] = static_cast<uint8_t>(ligPairs.size());
    putU32(sdFontTestFile, tocOff + 24, static_cast<uint32_t>(dataOffset));

    size_t cur = dataOffset;
    std::memcpy(sdFontTestFile.data() + cur, intervals.data(), intervalsBytes);
    cur += intervalsBytes;

    uint32_t bmOff = 0;
    for (size_t i = 0; i < sorted.size(); ++i) {
      EpdGlyph g{};
      if (sorted[i].cp != ' ') {
        g.width = 1;
        g.height = 8;
        g.left = 0;
        g.top = 8;
        g.dataLength = perGlyphBmpBytes;
        g.dataOffset = bmOff;
        bmOff += perGlyphBmpBytes;
      }
      g.advanceX = static_cast<uint16_t>(sorted[i].advanceFP + (si == 1 && sorted[i].cp != ' ' ? 16 : 0));
      std::memcpy(sdFontTestFile.data() + cur + i * sizeof(EpdGlyph), &g, sizeof(EpdGlyph));
    }
    cur += glyphsBytes;

    for (size_t i = 0; i < leftCps.size(); ++i) {
      EpdKernClassEntry e{leftCps[i], static_cast<uint8_t>(i + 1)};
      std::memcpy(sdFontTestFile.data() + cur + i * sizeof(e), &e, sizeof(e));
    }
    cur += klBytes;

    for (size_t i = 0; i < rightCps.size(); ++i) {
      EpdKernClassEntry e{rightCps[i], static_cast<uint8_t>(i + 1)};
      std::memcpy(sdFontTestFile.data() + cur + i * sizeof(e), &e, sizeof(e));
    }
    cur += krBytes;

    if (!matrix.empty()) {
      std::memcpy(sdFontTestFile.data() + cur, matrix.data(), matBytes);
      cur += matBytes;
    }
    if (!ligPairs.empty()) {
      std::memcpy(sdFontTestFile.data() + cur, ligPairs.data(), ligBytes);
      cur += ligBytes;
    }
    // 1bpp: 0xFF (8 black pixels); 4bpp: 0xF0 (left nibble ink=15, right padding=0) on each of 8 rows?
    // Width=1, height=8 -> 8 pixels packed MSB-nibble-first into 4 bytes (0xFF = two ink=15 pixels/byte).
    std::memset(sdFontTestFile.data() + cur, 0xFF, bmOff);
  }
}

// Reads logical pixel (x, y) in Portrait orientation from HalDisplay's 792x528 framebuffer.
bool isLogicalPixelBlack(const GfxRenderer& renderer, const int x, const int y) {
  const uint8_t* fb = renderer.getFrameBuffer();
  const int phyX = y;
  const int phyY = static_cast<int>(renderer.getDisplayHeight()) - 1 - x;
  if (phyX < 0 || phyX >= renderer.getDisplayWidth() || phyY < 0 || phyY >= renderer.getDisplayHeight()) {
    return false;
  }
  const size_t byteIdx = static_cast<size_t>(phyY) * renderer.getDisplayWidthBytes() + static_cast<size_t>(phyX / 8);
  const uint8_t mask = static_cast<uint8_t>(1u << (7 - (phyX % 8)));
  return (fb[byteIdx] & mask) == 0;
}

std::vector<int> scanDrawnColumnsAtRow(const GfxRenderer& renderer, const int y, const int maxX = 528) {
  std::vector<int> cols;
  for (int x = 0; x < maxX; ++x) {
    if (isLogicalPixelBlack(renderer, x, y)) {
      cols.push_back(x);
    }
  }
  return cols;
}

int expectedRightOpticalHang(const GfxRenderer& renderer, const int fontId, const std::string& word,
                             const EpdFontFamily::Style style) {
  if (word.empty()) return 0;
  const auto* scan = reinterpret_cast<const unsigned char*>(word.c_str());
  while (*scan) {
    if (utf8IsCjkBreakable(utf8NextCodepoint(&scan))) return 0;
  }
  size_t i = word.size() - 1;
  while (i > 0 && (static_cast<uint8_t>(word[i]) & 0xC0) == 0x80) --i;
  const auto* ptr = reinterpret_cast<const unsigned char*>(word.data() + i);
  const uint32_t cp = utf8NextCodepoint(&ptr);
  int pct = 0;
  switch (cp) {
    case '-':
    case ',':
    case '.':
    case '"':
    case '\'':
    case 0x00BB:
    case 0x2010:
    case 0x2019:
    case 0x201D:
    case 0x203A:
      pct = 40;
      break;
    case ':':
    case ';':
      pct = 25;
      break;
    default:
      return 0;
  }
  char cpBuf[5] = {};
  utf8AppendCodepoint(cpBuf, cp);
  const int adv = renderer.getTextAdvanceX(fontId, cpBuf, style);
  if (adv <= 0) return 0;
  return std::min(adv, std::clamp((adv * pct + 50) / 100, 1, 3));
}

class TypographyLayoutFixture : public ::testing::Test {
 protected:
  static constexpr int kFontId = 42;
  HalDisplay display;
  GfxRenderer renderer{display};
  SdCardFont sdFont;
  std::unique_ptr<FontCacheManager> cacheMgr;

  void SetUp() override {
    renderer.begin();
    renderer.clearScreen(0xFF);
    buildBookerly14CpFont();
    ASSERT_TRUE(sdFont.load("/fonts/Bookerly_14.cpfont"));
    renderer.insertFont(kFontId, EpdFontFamily(14, sdFont.getEpdFont(0)));
    renderer.registerSdCardFont(kFontId, &sdFont);
    cacheMgr =
        std::make_unique<FontCacheManager>(renderer.getFontMap(), renderer.getSdCardFonts(), renderer.getTtfFonts());
    renderer.setFontCacheManager(cacheMgr.get());
    Hyphenator::setPreferredLanguage("en");
  }
};

}  // namespace

// ============================================================================
// Fix 1: SD-Card .cpfont Layout vs Render Kerning & Ligature Parity
// ============================================================================

TEST_F(TypographyLayoutFixture, SdCardLayoutAndRenderWidthsMatchWithZeroDiscrepancy) {
  const char* kWords[] = {"Tony", "office", "first", "expertise", "affair", "often", "min", "oft", "quick-", "minded"};

  // Prepare SD font via the exact paragraph layout entry point (ensureSdCardFontReady).
  for (const char* w : kWords) {
    renderer.ensureSdCardFontReady(kFontId, w, 0x01);
  }

  // Specific Bookerly 14pt regression checks (cumulative fp4 pen + kerning + ligatures):
  //   "Tony"      = 1121 fp4 -> 70 px (unkerned layout previously reserved 74 px)
  //   "office"    = 1220 fp4 -> 76 px with ffi ligature (unligated layout reserved 81 px)
  //   "first"     =  933 fp4 -> 58 px with fi ligature  (unligated layout reserved 62 px)
  //   "expertise" = 2072 fp4 -> 130 px with +x/p, +r/t  (unkerned layout reserved 129 px)
  EXPECT_EQ(renderer.getTextAdvanceX(kFontId, "Tony", EpdFontFamily::REGULAR), 70);
  EXPECT_EQ(renderer.getTextAdvanceX(kFontId, "office", EpdFontFamily::REGULAR), 76);
  EXPECT_EQ(renderer.getTextAdvanceX(kFontId, "first", EpdFontFamily::REGULAR), 58);
  EXPECT_EQ(renderer.getTextAdvanceX(kFontId, "expertise", EpdFontFamily::REGULAR), 130);

  // Verify 0 px discrepancy between Layout mode, Rendered mode (inside PrewarmScope), and drawText()!
  for (const char* w : kWords) {
    SCOPED_TRACE(w);
    const int layoutW = renderer.getTextAdvanceX(kFontId, w, EpdFontFamily::REGULAR, 0, BidiUtils::BidiBaseDir::AUTO,
                                                 GfxRenderer::TextMeasureMode::Layout);

    renderer.clearScreen(0xFF);
    {
      auto scope = cacheMgr->createPrewarmScope();
      renderer.drawText(kFontId, 10, 20, w, true, EpdFontFamily::REGULAR);
      scope.endScanAndPrewarm();
      renderer.drawText(kFontId, 10, 20, w, true, EpdFontFamily::REGULAR);
      const int renderedW = renderer.getTextAdvanceX(
          kFontId, w, EpdFontFamily::REGULAR, 0, BidiUtils::BidiBaseDir::AUTO, GfxRenderer::TextMeasureMode::Rendered);
      EXPECT_EQ(layoutW, renderedW);
    }

    // Check rendered glyph columns in framebuffer (row y = 20 + ascender(27) - top(8) = 39).
    const auto cols = scanDrawnColumnsAtRow(renderer, 39);
    ASSERT_FALSE(cols.empty());
    EXPECT_EQ(cols.front(), 10);
  }

  // Confirm "office" drew 4 glyphs ('o', 'ffi', 'c', 'e') rather than 6 unligated glyphs.
  renderer.clearScreen(0xFF);
  {
    auto scope = cacheMgr->createPrewarmScope();
    renderer.drawText(kFontId, 10, 20, "office", true, EpdFontFamily::REGULAR);
    scope.endScanAndPrewarm();
    renderer.drawText(kFontId, 10, 20, "office", true, EpdFontFamily::REGULAR);
  }
  EXPECT_EQ(scanDrawnColumnsAtRow(renderer, 39).size(), 4u);

  // Flanking space kerning parity on SD font: 'y' + ' ' has kern -8 fp4 -> 100 - 8 = 92 fp4 -> 6 px.
  // Verify getSpaceAdvance consults SD kerning without SD thrashing.
  EXPECT_EQ(renderer.getSpaceAdvance(kFontId, 'y', 'T', EpdFontFamily::REGULAR), fp4::toPixel(100 - 8));
}

// ============================================================================
// Fix 2: Cumulative fp4 (12.4 Fixed-Point) Intra-Word Pen Tracking
// ============================================================================

TEST_F(TypographyLayoutFixture, CumulativeFp4PenPreventsSqueezeAndDriftInNormalAndRotatedText) {
  renderer.ensureSdCardFontReady(kFontId, "often min", 0x01);

  // "often": 'o'(270) + 'f'(187) + 't'(189) = 646 fp4 = 40.375 px -> 'e' must land at startX + 40
  // (per-character snapping gave 17 + 12 + 12 = 41 px, a +1 px drift).
  renderer.clearScreen(0xFF);
  renderer.drawText(kFontId, 10, 20, "often", true, EpdFontFamily::REGULAR);
  const auto oftenCols = scanDrawnColumnsAtRow(renderer, 39);
  ASSERT_EQ(oftenCols.size(), 5u);
  EXPECT_EQ(oftenCols[0], 10);       // 'o': penFP = 0
  EXPECT_EQ(oftenCols[1], 10 + 17);  // 'f': penFP = 270 -> 16.875 -> 17
  EXPECT_EQ(oftenCols[2], 10 + 29);  // 't': penFP = 457 -> 28.5625 -> 29
  EXPECT_EQ(oftenCols[3], 10 + 40);  // 'e': penFP = 646 -> 40.375 -> 40 (NOT 41!)
  EXPECT_EQ(oftenCols[4], 10 + 55);  // 'n': penFP = 886 -> 55.375 -> 55
  EXPECT_EQ(renderer.getTextAdvanceX(kFontId, "often", EpdFontFamily::REGULAR), fp4::toPixel(886 + 309));

  // "min": 'm'(462) + 'i'(151) + 'n'(309) = 922 fp4 = 57.625 px -> 58 px total
  // (per-character snapping rounded 462->29, 151->9, 309->19 = 57 px, losing 1 px across i-n).
  EXPECT_EQ(renderer.getTextAdvanceX(kFontId, "min", EpdFontFamily::REGULAR), 58);

  // Rotated 90CW text uses the same cumulative penFP along -Y:
  renderer.clearScreen(0xFF);
  renderer.drawTextRotated90CW(kFontId, 50, 200, "often", true, EpdFontFamily::REGULAR);
  // In Rotated90CW, frame origin is (x + ascender - top, cursorY - left) = (50 + 27 - 8, cursorY) = (69, cursorY).
  std::vector<int> rotatedYs;
  for (int y = 0; y <= 200; ++y) {
    if (isLogicalPixelBlack(renderer, 69, y)) {
      rotatedYs.push_back(y);
    }
  }
  ASSERT_EQ(rotatedYs.size(), 5u);
  // Sorted ascending in Y: last glyph 'n' is at 200 - 55 = 145, 'e' at 200 - 40 = 160!
  EXPECT_EQ(rotatedYs.back(), 200);
  EXPECT_EQ(rotatedYs[1], 200 - 40);  // 'e' at 160 (no drift!)
  EXPECT_EQ(rotatedYs.front(), 200 - 55);
}

// ============================================================================
// Fix 3: Bresenham Remainder Distribution & Max-Stretch River Guard
// ============================================================================

TEST_F(TypographyLayoutFixture, BresenhamJustificationAlignsFlushRightWithZeroWobbleAndCapsRivers) {
  BlockStyle style;
  style.alignment = CssTextAlign::Justify;
  style.textIndentDefined = true;
  style.textIndent = 0;

  // Test across multiple column widths so spareSpace % gapCount exercises non-zero remainders
  // across 5..9 word gaps per line with 0 px right-margin wobble.
  for (const uint16_t pageWidth : {uint16_t{497}, uint16_t{503}, uint16_t{508}, uint16_t{513}, uint16_t{518}}) {
    ParsedText para(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/false, style, /*paragraphIndentSpaces=*/0);
    const char* words[] = {"When",     "modern",  "portable", "readers",  "render",  "classic", "serif",
                           "type",     "across",  "narrow",   "columns,", "careful", "spacing", "between",
                           "adjacent", "words",   "keeps",    "every",    "margin",  "clean",   "and",
                           "steady",   "through", "every",    "single",   "chapter", "of",      "prose."};
    for (const char* w : words) {
      para.addWord(w, EpdFontFamily::REGULAR);
    }

    std::vector<std::unique_ptr<TextBlock>> lines;
    para.layoutAndExtractLines(renderer, kFontId, pageWidth,
                               [&](std::unique_ptr<TextBlock> block, uint32_t) { lines.push_back(std::move(block)); });
    ASSERT_GE(lines.size(), 3u);

    // Every non-last justified line hits `pageWidth` on solid letter stems (0 px wobble)
    // and `pageWidth + 1..3 px` on protruding right-edge punctuation.
    for (size_t li = 0; li + 1 < lines.size(); ++li) {
      const TextBlock& line = *lines[li];
      ASSERT_GT(line.wordCount(), 1u);
      const uint16_t lastIdx = line.wordCount() - 1;
      const int rightHang =
          expectedRightOpticalHang(renderer, kFontId, line.wordText(lastIdx), line.wordStyle(lastIdx));
      const int rightEdge =
          line.wordXpos(lastIdx) + renderer.getTextAdvanceX(kFontId, line.wordText(lastIdx), line.wordStyle(lastIdx));
      EXPECT_EQ(rightEdge, static_cast<int>(pageWidth) + rightHang) << "line=" << li << " pageWidth=" << pageWidth;
    }
  }

  // Max-Stretch River Guard: a 2-word line before an unbreakable 480-px token with hyphenation OFF
  // must cap space expansion at <= 4x normal space (6 + 18 = 24 px) instead of opening a ~350 px river.
  ParsedText riverPara(/*hyphenationEnabled=*/false, /*focusReadingEnabled=*/false, style, 0);
  riverPara.addWord("brash,", EpdFontFamily::REGULAR);
  riverPara.addWord("quick", EpdFontFamily::REGULAR);
  riverPara.addWord("WWWWWWWWWWWWWWWWWWWWWWWWWWWWWW", EpdFontFamily::REGULAR);  // forces line break before it

  std::vector<std::unique_ptr<TextBlock>> riverLines;
  riverPara.layoutAndExtractLines(renderer, kFontId, 508, [&](std::unique_ptr<TextBlock> block, uint32_t) {
    riverLines.push_back(std::move(block));
  });
  ASSERT_GE(riverLines.size(), 2u);
  ASSERT_EQ(riverLines[0]->wordCount(), 2u);
  const int w0 = renderer.getTextAdvanceX(kFontId, riverLines[0]->wordText(0), EpdFontFamily::REGULAR);
  const int actualGap = riverLines[0]->wordXpos(1) - (riverLines[0]->wordXpos(0) + w0);
  const int normalSpace = renderer.getSpaceWidth(kFontId, EpdFontFamily::REGULAR);  // 6 px
  EXPECT_LE(actualGap, normalSpace * 4) << "River guard failed: gap stretched to " << actualGap << " px";
  EXPECT_GT(actualGap, normalSpace);
}

// ============================================================================
// Fix 4: 2/3 English Hyphenation + Knuth-Plass DP Eliminates X3 Photo Rivers
// ============================================================================

TEST_F(TypographyLayoutFixture, KnuthPlassWithTwoThreeHyphenationEliminatesWhiteRivers) {
  BlockStyle style;
  style.alignment = CssTextAlign::Justify;
  style.textIndentDefined = true;
  style.textIndent = 0;

  // 1. Default 518 px column (528 - 2*5 px): 2/3 hyphenation + bounded inter-word shrink
  //    allows "De-troit" to fit cleanly instead of leaving a loose 4-word line.
  {
    ParsedText para(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/false, style, 0);
    const char* passage[] = {"That", "led",    "him",          "to",           "Tony",      "Fadell,",
                             "a",    "brash,", "quick-minded", "entrepreneur", "from",      "Detroit",
                             "who",  "had",    "developed",    "an",           "expertise", "working",
                             "on",   "mobile", "devices",      "and",          "digital",   "storytelling."};
    for (const char* w : passage) {
      para.addWord(w, EpdFontFamily::REGULAR);
    }

    std::vector<std::unique_ptr<TextBlock>> lines;
    para.layoutAndExtractLines(renderer, kFontId, 518,
                               [&](std::unique_ptr<TextBlock> block, uint32_t) { lines.push_back(std::move(block)); });
    ASSERT_GE(lines.size(), 3u);

    int maxObservedInterWordGap = 0;
    bool sawDeHyphen = false;
    for (size_t li = 0; li + 1 < lines.size(); ++li) {
      const TextBlock& line = *lines[li];
      const uint16_t lastIdx = line.wordCount() - 1;
      const int rightHang =
          expectedRightOpticalHang(renderer, kFontId, line.wordText(lastIdx), line.wordStyle(lastIdx));
      const int rightEdge =
          line.wordXpos(lastIdx) + renderer.getTextAdvanceX(kFontId, line.wordText(lastIdx), line.wordStyle(lastIdx));
      EXPECT_EQ(rightEdge, 518 + rightHang) << "line=" << li;

      for (uint16_t wi = 0; wi < line.wordCount(); ++wi) {
        if (std::string(line.wordText(wi)) == "De-") {
          sawDeHyphen = true;
        }
        // Discretionary hyphens may only ever appear at the end of a line, never mid-line.
        if (line.wordHasDiscretionaryHyphen(wi)) {
          EXPECT_EQ(wi + 1, line.wordCount()) << "Mid-line discretionary hyphen on line " << li;
        }
        if (wi + 1 < line.wordCount()) {
          const int wordW = renderer.getTextAdvanceX(kFontId, line.wordText(wi), line.wordStyle(wi));
          const int gap = line.wordXpos(wi + 1) - (line.wordXpos(wi) + wordW);
          maxObservedInterWordGap = std::max(maxObservedInterWordGap, gap);
        }
      }
    }
    EXPECT_TRUE(sawDeHyphen) << "Expected 2/3 hyphenation + shrink to fit 'De-' at end of line 1";
    EXPECT_LE(maxObservedInterWordGap, 10) << "Unexpected wide inter-word gap: " << maxObservedInterWordGap << " px";
  }

  // 2. Band 2 river at 508 px ("developed an expertise working on mobile devices..."):
  //    Previously produced a 30 px (5.00x) white river because "mo-bile" was blocked by minPrefix=3.
  {
    ParsedText band2(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/false, style, 0);
    const char* words[] = {"oped",    "an",  "expertise", "working", "on",    "mobile",
                           "devices", "and", "building",  "compact", "music", "players."};
    for (const char* w : words) {
      band2.addWord(w, EpdFontFamily::REGULAR);
    }
    std::vector<std::unique_ptr<TextBlock>> lines;
    band2.layoutAndExtractLines(renderer, kFontId, 508,
                                [&](std::unique_ptr<TextBlock> block, uint32_t) { lines.push_back(std::move(block)); });
    ASSERT_GE(lines.size(), 2u);
    int maxGap = 0;
    for (uint16_t wi = 0; wi + 1 < lines[0]->wordCount(); ++wi) {
      const int wordW = renderer.getTextAdvanceX(kFontId, lines[0]->wordText(wi), lines[0]->wordStyle(wi));
      maxGap = std::max(maxGap, lines[0]->wordXpos(wi + 1) - (lines[0]->wordXpos(wi) + wordW));
    }
    EXPECT_LE(maxGap, 14) << "Band 2 river persisted: maxGap=" << maxGap;
  }

  // 3. Consecutive-hyphen demerit: when line 0 already ends with a discretionary hyphen and line 1 has
  //    a clean whole-word break (< 1.8x space), Knuth-Plass avoids a second consecutive hyphen.
  {
    ParsedText consec(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/false, style, 0);
    const char* words[] = {"That",   "led",  "him",     "to",       "Tony",       "Fadell,",
                           "a",      "very", "notable", "creative", "specialist", "working",
                           "within", "many", "modern",  "devices",  "everywhere."};
    for (const char* w : words) {
      consec.addWord(w, EpdFontFamily::REGULAR);
    }
    std::vector<std::unique_ptr<TextBlock>> lines;
    consec.layoutAndExtractLines(
        renderer, kFontId, 518, [&](std::unique_ptr<TextBlock> block, uint32_t) { lines.push_back(std::move(block)); });
    size_t backToBackHyphens = 0;
    for (size_t li = 1; li < lines.size(); ++li) {
      const auto& prev = *lines[li - 1];
      const auto& curr = *lines[li];
      if (prev.wordCount() > 0 && curr.wordCount() > 0 && prev.wordHasDiscretionaryHyphen(prev.wordCount() - 1) &&
          curr.wordHasDiscretionaryHyphen(curr.wordCount() - 1)) {
        ++backToBackHyphens;
      }
    }
    EXPECT_EQ(backToBackHyphens, 0u);
  }

  // 4. Mid-word continuation (`sByteOff > 0`) must not leak `sByteOff` into candidate splits of a
  //    later word `j > w` on the same line (no blocked `"en-gineering"` and no right-margin overflow).
  {
    ParsedText splitCont(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/false, style, 0);
    const char* words[] = {"That",    "led",    "him",    "to",          "Tony",         "Fadell,",
                           "a",       "brash,", "quick-", "minded",      "entrepreneur", "from",
                           "Detroit", "with",   "an",     "engineering", "mind",         "and",
                           "a",       "knack",  "for",    "punchy",      "storytelling."};
    for (const char* w : words) {
      splitCont.addWord(w, EpdFontFamily::REGULAR);
    }
    std::vector<std::unique_ptr<TextBlock>> lines;
    splitCont.layoutAndExtractLines(
        renderer, kFontId, 448, [&](std::unique_ptr<TextBlock> block, uint32_t) { lines.push_back(std::move(block)); });
    ASSERT_GE(lines.size(), 4u);
    for (size_t li = 0; li + 1 < lines.size(); ++li) {
      const TextBlock& line = *lines[li];
      const uint16_t lastIdx = line.wordCount() - 1;
      const int rightHang =
          expectedRightOpticalHang(renderer, kFontId, line.wordText(lastIdx), line.wordStyle(lastIdx));
      const int rightEdge =
          line.wordXpos(lastIdx) + renderer.getTextAdvanceX(kFontId, line.wordText(lastIdx), line.wordStyle(lastIdx));
      EXPECT_EQ(rightEdge, 448 + rightHang) << "Non-final justified line failed right-margin parity on line " << li;
    }
  }
}

// ============================================================================
// Four-Bit .cpfont + 8KB Slabs + GfxRenderer Gray16* + Tight-Heap Layout Parity
// ============================================================================

TEST_F(TypographyLayoutFixture, FourBitTightHeapLayoutRetainsAdvanceTableAndZeroPixelKerningParity) {
  buildBookerly14CpFont(/*fourBit=*/true);
  SdCardFont font4;
  ASSERT_TRUE(font4.load("/fonts/Bookerly4b_14.cpfont"));
  ASSERT_TRUE(font4.is4Bit(0));
  ASSERT_TRUE(font4.hasStyle(1));

  constexpr int kFont4Id = 88;
  renderer.insertFont(kFont4Id, EpdFontFamily(14, font4.getEpdFont(0), font4.getEpdFont(1)));
  renderer.registerSdCardFont(kFont4Id, &font4);

  struct HeapGuard {
    ~HeapGuard() { ESP.freeHeap = 200 * 1024; }
  } heapGuard;
  // Tight ESP32-C3 heap (52 KB < 58 KB first-slab floor): bitmap slabs cannot allocate,
  // yet ensureSdCardFontReady must warm metadata-only kerning/ligatures without calling
  // clearPersistentCache() or dropping advanceTable_.
  ESP.freeHeap = 52 * 1024;

  const char* kWords[] = {"Tony", "Fadell", "office", "first", "expertise", "affair", "storytelling"};
  for (const char* w : kWords) {
    renderer.ensureSdCardFontReady(kFont4Id, w, 0x01);
  }

  EXPECT_TRUE(font4.hasAdvanceTable(0));
  EXPECT_TRUE(font4.isMiniMetadataOnly(0));
  EXPECT_EQ(0u, font4.getStats().bitmapBytes);
  // Ligated codepoints (fi = U+FB01, ffi = U+FB03) must also be populated in advanceTable_.
  EXPECT_EQ(313u, font4.getAdvance(0xFB01, 0));
  EXPECT_EQ(475u, font4.getAdvance(0xFB03, 0));

  EXPECT_EQ(70, renderer.getTextAdvanceX(kFont4Id, "Tony", EpdFontFamily::REGULAR));
  EXPECT_EQ(76, renderer.getTextAdvanceX(kFont4Id, "office", EpdFontFamily::REGULAR));
  EXPECT_EQ(58, renderer.getTextAdvanceX(kFont4Id, "first", EpdFontFamily::REGULAR));
  EXPECT_EQ(130, renderer.getTextAdvanceX(kFont4Id, "expertise", EpdFontFamily::REGULAR));
  EXPECT_EQ(fp4::toPixel(100 - 8), renderer.getSpaceAdvance(kFont4Id, 'y', 'T', EpdFontFamily::REGULAR));

  // Focus Reading paragraph: wordFocusBoundary != 0 must warm BOLD advance + kerning too.
  BlockStyle focusBlockStyle;
  focusBlockStyle.alignment = CssTextAlign::Left;
  ParsedText focusPara(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/true, focusBlockStyle, 0);
  focusPara.addWord("Tony", EpdFontFamily::REGULAR);
  focusPara.addWord("office", EpdFontFamily::REGULAR);
  std::vector<std::unique_ptr<TextBlock>> focusLines;
  focusPara.layoutAndExtractLines(renderer, kFont4Id, 518,
                                  [&](std::unique_ptr<TextBlock> b, uint32_t) { focusLines.push_back(std::move(b)); });
  ASSERT_EQ(1u, focusLines.size());
  EXPECT_TRUE(font4.hasAdvanceTable(EpdFontFamily::BOLD));
  EXPECT_NE(nullptr, font4.getEpdFont(EpdFontFamily::BOLD)->data->kernMatrix);

  // Restore normal heap and verify getTextWidth + direct drawText upgrades metadata-only mini to bitmaps.
  ESP.freeHeap = 200 * 1024;
  EXPECT_GT(renderer.getTextWidth(kFont4Id, "office"), 0);
  EXPECT_TRUE(font4.isMiniMetadataOnly(0));
  renderer.clearScreen(0xFF);
  renderer.drawText(kFont4Id, 10, 20, "office", true, EpdFontFamily::REGULAR);
  EXPECT_FALSE(font4.isMiniMetadataOnly(0));
  EXPECT_EQ(4u, scanDrawnColumnsAtRow(renderer, 39).size());
}

TEST_F(TypographyLayoutFixture, Long750WordParagraphLaysOutOver250LinesWithoutDpNodeTruncation) {
  BlockStyle style;
  style.alignment = CssTextAlign::Justify;
  style.textIndentDefined = true;
  style.textIndent = 0;

  static constexpr const char* kVocab[] = {
      "portable",   "electronic", "typography", "carefully",  "balances",     "interword",  "spacing",
      "throughout", "extended",   "literary",   "paragraphs", "without",      "truncating", "remaining",
      "sentences",  "across",     "narrow",     "columns",    "storytelling", "expertise",  "devices",
      "modern",     "readers",    "affluent",   "office",     "first",
  };

  for (const uint16_t viewportWidth : {uint16_t{190}, uint16_t{518}}) {
    SCOPED_TRACE(viewportWidth);
    ParsedText para(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/false, style, 0);
    std::string expectedConcat;
    for (size_t i = 0; i < 750; ++i) {
      const char* w = kVocab[i % std::size(kVocab)];
      para.addWord(w, EpdFontFamily::REGULAR);
      expectedConcat += w;
    }

    std::vector<std::unique_ptr<TextBlock>> lines;
    para.layoutAndExtractLines(renderer, kFontId, viewportWidth,
                               [&](std::unique_ptr<TextBlock> block, uint32_t) { lines.push_back(std::move(block)); });

    EXPECT_TRUE(para.isEmpty());
    EXPECT_FALSE(para.hadDroppedWords());
    if (viewportWidth == 190) {
      EXPECT_GE(lines.size(), 250u);
    } else {
      EXPECT_GE(lines.size(), 90u);
    }

    std::string actualConcat;
    for (size_t li = 0; li < lines.size(); ++li) {
      const TextBlock& line = *lines[li];
      ASSERT_GT(line.wordCount(), 0u);
      for (uint16_t wi = 0; wi < line.wordCount(); ++wi) {
        std::string token = line.wordText(wi);
        if (line.wordHasDiscretionaryHyphen(wi)) {
          EXPECT_EQ(wi + 1, line.wordCount()) << "Mid-line discretionary hyphen at line " << li;
          ASSERT_FALSE(token.empty());
          ASSERT_EQ('-', token.back());
          token.pop_back();
        }
        actualConcat += token;
      }
    }
    EXPECT_EQ(expectedConcat, actualConcat);
  }
}

TEST(FourBitFontIntegration, RendersFourBitCpfontAcrossGray16StripsCleanupAndScaledSamplers) {
  // Build a minimal v4 4bpp (flags = 0x0002) .cpfont with 'R' (16x4 ramp, column x has ink=x) and U+FFFD.
  constexpr size_t hdrSize = 32;
  constexpr size_t tocSize = 32;
  constexpr size_t ivSize = 2 * sizeof(EpdUnicodeInterval);
  constexpr size_t glSize = 2 * sizeof(EpdGlyph);
  constexpr uint16_t rampBytes = (16 * 4 + 1) / 2;  // 32 bytes
  sdFontTestFile.assign(hdrSize + tocSize + ivSize + glSize + rampBytes * 2, 0);
  std::memcpy(sdFontTestFile.data(), "CPFONT\0\0", 8);
  putU16(sdFontTestFile, 8, CPFONT_VERSION);
  putU16(sdFontTestFile, 10, SdCardFont::CPFONT_FLAG_4BIT);
  sdFontTestFile[12] = 1;  // 1 style

  const size_t dataOff = hdrSize + tocSize;
  sdFontTestFile[hdrSize + 0] = 0;  // Regular
  putU32(sdFontTestFile, hdrSize + 4, 2);
  putU32(sdFontTestFile, hdrSize + 8, 2);
  sdFontTestFile[hdrSize + 12] = 24;
  putI16(sdFontTestFile, hdrSize + 13, 18);
  putI16(sdFontTestFile, hdrSize + 15, -6);
  putU32(sdFontTestFile, hdrSize + 24, static_cast<uint32_t>(dataOff));

  EpdUnicodeInterval ivs[2] = {{'R', 'R', 0}, {0xFFFD, 0xFFFD, 1}};
  std::memcpy(sdFontTestFile.data() + dataOff, ivs, ivSize);

  EpdGlyph gls[2] = {};
  for (int i = 0; i < 2; ++i) {
    gls[i].width = 16;
    gls[i].height = 4;
    gls[i].advanceX = 16 << 4;
    gls[i].left = 0;
    gls[i].top = 4;
    gls[i].dataLength = rampBytes;
    gls[i].dataOffset = static_cast<uint32_t>(i * rampBytes);
  }
  std::memcpy(sdFontTestFile.data() + dataOff + ivSize, gls, glSize);

  uint8_t rowBytes[8];
  for (int b = 0; b < 8; ++b) {
    rowBytes[b] = static_cast<uint8_t>(((b * 2) << 4) | (b * 2 + 1));
  }
  uint8_t* bmpDst = sdFontTestFile.data() + dataOff + ivSize + glSize;
  for (int g = 0; g < 2; ++g) {
    for (int r = 0; r < 4; ++r) {
      std::memcpy(bmpDst + g * rampBytes + r * 8, rowBytes, 8);
    }
  }

  HalDisplay disp;
  GfxRenderer gfx{disp};
  gfx.begin();
  SdCardFont font4;
  ASSERT_TRUE(font4.load("/fonts/Ramp4b_16.cpfont"));
  EXPECT_TRUE(font4.is4Bit(0));

  constexpr int kFont4Id = 99;
  gfx.insertFont(kFont4Id, EpdFontFamily(16, font4.getEpdFont(0)));
  gfx.registerSdCardFont(kFont4Id, &font4);
  FontCacheManager fcm(gfx.getFontMap(), gfx.getSdCardFonts(), gfx.getTtfFonts());
  gfx.setFontCacheManager(&fcm);
  EXPECT_TRUE(gfx.isFont4Bit(kFont4Id));

  auto scope = fcm.createPrewarmScope();
  gfx.drawText(kFont4Id, 16, 10, "R", true, EpdFontFamily::REGULAR);
  scope.endScanAndPrewarm();

  // Render 2-pass 16-gray BW base at logical (16, 10):ascender=18, top=4 -> glyph y spans [10 + 18 - 4 = 24 .. 27].
  gfx.setGray16Base(true);
  gfx.setRenderMode(GfxRenderer::BW);
  gfx.clearScreen(0xFF);
  gfx.drawText(kFont4Id, 16, 10, "R", true, EpdFontFamily::REGULAR);

  // Portrait maps logical x in [16..31] to phyY = 527 - x in [496..511], inside strip [480..528).
  constexpr int kStripY0 = 480;
  constexpr int kStripRows = 48;
  const int gwBytes = gfx.getDisplayWidthBytes();
  std::vector<uint8_t> cLsb(gwBytes * kStripRows, 0), cMsb(gwBytes * kStripRows, 0);
  std::vector<uint8_t> fLsb(gwBytes * kStripRows, 0), fMsb(gwBytes * kStripRows, 0);
  std::vector<uint8_t> cleanDtm1(gwBytes * kStripRows, 0xFF);

  auto renderStrip = [&](GfxRenderer::RenderMode m, std::vector<uint8_t>& buf, uint8_t initVal) {
    gfx.setRenderMode(m);
    gfx.beginStripTarget(buf.data(), kStripY0, kStripRows);
    gfx.clearScreen(initVal);
    gfx.drawText(kFont4Id, 16, 10, "R", true, EpdFontFamily::REGULAR);
    gfx.endStripTarget();
  };

  renderStrip(GfxRenderer::GRAY16_COARSE_LSB, cLsb, 0x00);
  renderStrip(GfxRenderer::GRAY16_COARSE_MSB, cMsb, 0x00);
  renderStrip(GfxRenderer::GRAY16_FINE_LSB, fLsb, 0x00);
  renderStrip(GfxRenderer::GRAY16_FINE_MSB, fMsb, 0x00);
  renderStrip(GfxRenderer::GRAY16_CLEANUP_DTM1, cleanDtm1, 0xFF);
  gfx.setRenderMode(GfxRenderer::BW);
  gfx.setGray16Base(false);

  auto readStripBit = [&](const uint8_t* plane, int lx, int ly, int yOrigin) -> uint8_t {
    const int phyX = ly;
    const int phyY = static_cast<int>(gfx.getDisplayHeight()) - 1 - lx;
    const size_t idx = static_cast<size_t>(phyY - yOrigin) * gwBytes + static_cast<size_t>(phyX / 8);
    return (plane[idx] >> (7 - (phyX % 8))) & 1u;
  };

  const uint8_t* fb = gfx.getFrameBuffer();
  for (int ink = 0; ink < 16; ++ink) {
    const int lx = 16 + ink;
    const int ly = 25;
    const uint8_t bw = readStripBit(fb, lx, ly, 0);
    const uint8_t cl = readStripBit(cLsb.data(), lx, ly, kStripY0);
    const uint8_t cm = readStripBit(cMsb.data(), lx, ly, kStripY0);
    const uint8_t fl = readStripBit(fLsb.data(), lx, ly, kStripY0);
    const uint8_t fm = readStripBit(fMsb.data(), lx, ly, kStripY0);
    const uint8_t cln = readStripBit(cleanDtm1.data(), lx, ly, kStripY0);
    const uint8_t code5 = static_cast<uint8_t>((bw << 4) | (cl << 3) | (cm << 2) | (fl << 1) | fm);
    EXPECT_EQ(15 - ink, freeink::x3_lut16::kDecodeGray16[code5]) << "ink=" << ink;
    EXPECT_EQ(ink >= 12 ? 0u : 1u, cln) << "cleanup ink=" << ink;
  }

  // Scaled samplers (SMALL_CAPS 3/4 and bilinear drop-cap 2.0x) handle 4bpp cleanly:
  gfx.clearScreen(0xFF);
  gfx.drawText(kFont4Id, 40, 10, "r", true, EpdFontFamily::SMALL_CAPS);  // 'r' -> 'R' scaled 3/4
  EXPECT_FALSE(isLogicalPixelBlack(gfx, 40, 25));                        // low-ink left edge stays white
  EXPECT_TRUE(isLogicalPixelBlack(gfx, 40 + 11, 25));                    // high-ink right edge draws black

  gfx.clearScreen(0xFF);
  const int scaledAdv = gfx.drawScaledCodepoint(kFont4Id, 'R', EpdFontFamily::REGULAR, 80, 40, 512);  // 2.0x
  EXPECT_EQ(32, scaledAdv);
  EXPECT_FALSE(isLogicalPixelBlack(gfx, 81, 35));
  EXPECT_TRUE(isLogicalPixelBlack(gfx, 80 + 30, 35));
}

// ============================================================================
// Stage 5C: Optical Margin Alignment (Hanging Punctuation)
// ============================================================================

TEST_F(TypographyLayoutFixture, OpticalMarginAlignmentProtrudesOpeningQuotesAndTrailingPunctuationWithZeroStemWobble) {
  BlockStyle style;
  style.alignment = CssTextAlign::Justify;
  style.textIndentDefined = true;
  style.textIndent = 0;

  // 1. Opening quote ("“If I made...") hangs left by -3 px (40% of 8 px = 3 px) while the right edge of
  //    that same line ending on a solid vertical stem lands at exact pageWidth (0 px stem wobble).
  {
    ParsedText para(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/false, style, /*paragraphIndentSpaces=*/0);
    const char* dialogue[] = {"\xE2\x80\x9CIf", "I",      "made",   "a",     "serious", "mistake",
                              "building",       "modern", "mobile", "tools", "today",   "friend."};
    for (const char* w : dialogue) {
      para.addWord(w, EpdFontFamily::REGULAR);
    }
    std::vector<std::unique_ptr<TextBlock>> lines;
    para.layoutAndExtractLines(renderer, kFontId, 420,
                               [&](std::unique_ptr<TextBlock> b, uint32_t) { lines.push_back(std::move(b)); });
    ASSERT_GE(lines.size(), 2u);
    EXPECT_EQ(lines[0]->wordXpos(0), -3);  // “ (8 px * 40% -> 3 px) hangs into left margin
    const uint16_t last0 = lines[0]->wordCount() - 1;
    const int line0Right = lines[0]->wordXpos(last0) +
                           renderer.getTextAdvanceX(kFontId, lines[0]->wordText(last0), lines[0]->wordStyle(last0));
    const int line0Hang =
        expectedRightOpticalHang(renderer, kFontId, lines[0]->wordText(last0), lines[0]->wordStyle(last0));
    EXPECT_EQ(line0Right, 420 + line0Hang);
  }

  // 2. Indented dialogue ("  “If I made...") shifts from firstLineIndent (2 * 6 = 12 px) to 12 - 3 = 9 px;
  //    single opening quote (‘ = 6 px -> 2 px) hangs by -2 px; wide guillemet (« = 13 px -> 40% = 5 px)
  //    clamps strictly to -3 px.
  {
    ParsedText indented(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/false, BlockStyle{}, 2);
    indented.addWord("\xE2\x80\x9CIf", EpdFontFamily::REGULAR);
    indented.addWord("modern", EpdFontFamily::REGULAR);
    indented.addWord("readers", EpdFontFamily::REGULAR);
    indented.addWord("align", EpdFontFamily::REGULAR);
    indented.addWord("cleanly", EpdFontFamily::REGULAR);
    indented.addWord("today.", EpdFontFamily::REGULAR);
    std::vector<std::unique_ptr<TextBlock>> lines;
    indented.layoutAndExtractLines(renderer, kFontId, 260,
                                   [&](std::unique_ptr<TextBlock> b, uint32_t) { lines.push_back(std::move(b)); });
    ASSERT_GE(lines.size(), 2u);
    EXPECT_EQ(lines[0]->wordXpos(0), 12 - 3);

    ParsedText singleQuote(/*hyphenationEnabled=*/false, /*focusReadingEnabled=*/false, style, 0);
    singleQuote.addWord("\xE2\x80\x98If", EpdFontFamily::REGULAR);
    singleQuote.addWord("only", EpdFontFamily::REGULAR);
    singleQuote.addWord("words", EpdFontFamily::REGULAR);
    singleQuote.addWord("aligned.", EpdFontFamily::REGULAR);
    std::vector<std::unique_ptr<TextBlock>> sqLines;
    singleQuote.layoutAndExtractLines(renderer, kFontId, 140,
                                      [&](std::unique_ptr<TextBlock> b, uint32_t) { sqLines.push_back(std::move(b)); });
    ASSERT_GE(sqLines.size(), 2u);
    EXPECT_EQ(sqLines[0]->wordXpos(0), -2);  // ‘ (6 px * 40% = 2.4 -> 2 px)

    ParsedText guillemet(/*hyphenationEnabled=*/false, /*focusReadingEnabled=*/false, style, 0);
    guillemet.addWord(
        "\xC2\xAB"
        "Bonjour",
        EpdFontFamily::REGULAR);
    guillemet.addWord("tout", EpdFontFamily::REGULAR);
    guillemet.addWord("le", EpdFontFamily::REGULAR);
    guillemet.addWord("monde\xC2\xBB", EpdFontFamily::REGULAR);
    guillemet.addWord("ici.", EpdFontFamily::REGULAR);
    std::vector<std::unique_ptr<TextBlock>> gmLines;
    guillemet.layoutAndExtractLines(renderer, kFontId, 250,
                                    [&](std::unique_ptr<TextBlock> b, uint32_t) { gmLines.push_back(std::move(b)); });
    ASSERT_GE(gmLines.size(), 2u);
    EXPECT_EQ(gmLines[0]->wordXpos(0), -3);  // « (13 px * 40% = 5 px) clamped to <= 3 px!
  }

  // 3. Exact right-edge protrusion & DP/extractLine consistency across:
  //    - trailing hyphen "...indus-" (from "industrial")
  //    - trailing comma+closing quote "...jail,”"
  //    - trailing period "...leader."
  //    - trailing semicolon/colon ("note;", "said:") at 25% -> 2 px
  //    - solid letter stem ("team", "bold") at 0% -> 0 px wobble!
  struct RightCase {
    const char* targetWord;
    bool hyphenate;
    int expectedHang;
    const char* expectedLineEnding;
  };
  const RightCase kCases[] = {
      {"jail,\xE2\x80\x9D", false, 3, "jail,\xE2\x80\x9D"},
      {"leader.", false, 3, "leader."},
      {"note;", false, 2, "note;"},
      {"said:", false, 2, "said:"},
      {"leader", false, 0, "leader"},
  };

  for (const bool focusReading : {false, true}) {
    for (const size_t maxLines : {SIZE_MAX, size_t{1}}) {
      for (const auto& tc : kCases) {
        SCOPED_TRACE(::testing::Message()
                     << "word=" << tc.targetWord << " focus=" << focusReading << " maxLines=" << maxLines);
        // Measure natural width of ["When", tc.targetWord] and test a 1 px tighter column
        // (for expectedHang >= 2) where optical protrusion fits the word on line 0.
        ParsedText probe(false, focusReading, style, 0);
        probe.addWord("When", EpdFontFamily::REGULAR);
        probe.addWord(tc.targetWord, EpdFontFamily::REGULAR);
        int naturalLineW = 0;
        probe.layoutAndExtractLines(renderer, kFontId, 600, [&](std::unique_ptr<TextBlock> b, uint32_t) {
          const uint16_t last = b->wordCount() - 1;
          naturalLineW = b->wordXpos(last) + renderer.getTextAdvanceX(kFontId, b->wordText(last), b->wordStyle(last));
        });
        ASSERT_GT(naturalLineW, 60);

        const uint16_t tightWidth =
            static_cast<uint16_t>(tc.expectedHang >= 2 ? (naturalLineW - 1) : (naturalLineW + 4));

        ParsedText para(tc.hyphenate, focusReading, style, 0);
        para.addWord("When", EpdFontFamily::REGULAR);
        para.addWord(tc.targetWord, EpdFontFamily::REGULAR);
        para.addWord("arrived", EpdFontFamily::REGULAR);
        para.addWord("yesterday.", EpdFontFamily::REGULAR);

        std::vector<std::unique_ptr<TextBlock>> lines;
        para.layoutAndExtractLines(
            renderer, kFontId, tightWidth,
            [&](std::unique_ptr<TextBlock> b, uint32_t) { lines.push_back(std::move(b)); }, /*includeLastLine=*/true,
            /*characterSpacing=*/0, /*wordSpacingPercent=*/100, maxLines);
        ASSERT_GE(lines.size(), 1u);

        const TextBlock& line0 = *lines[0];
        const uint16_t lastIdx = line0.wordCount() - 1;
        // When tc.expectedHang >= 2, `naturalLineW == tightWidth + 1` MUST fit on line 0 thanks to rightHangPx
        // in BOTH Knuth-Plass DP (SIZE_MAX) and Greedy (maxLines=1), preserving spareSpace >= 0!
        std::string reconstructed;
        for (uint16_t wi = 0; wi < line0.wordCount(); ++wi) {
          if (wi > 0 && line0.wordXpos(wi) >
                            line0.wordXpos(wi - 1) +
                                renderer.getTextAdvanceX(kFontId, line0.wordText(wi - 1), line0.wordStyle(wi - 1))) {
            reconstructed += ' ';
          }
          reconstructed += line0.wordText(wi);
        }
        EXPECT_EQ(reconstructed, std::string("When ") + tc.expectedLineEnding);
        const int rightEdge = line0.wordXpos(lastIdx) +
                              renderer.getTextAdvanceX(kFontId, line0.wordText(lastIdx), line0.wordStyle(lastIdx));
        EXPECT_EQ(rightEdge, static_cast<int>(tightWidth) + tc.expectedHang);
      }
    }
  }

  // 4. Hyphenated "...indus-" right-edge protrusion (both Knuth-Plass DP and Greedy maxLines=1):
  renderer.ensureSdCardFontReady(kFontId, "When industrial-", 0x01);
  const int whenPlusSpace = renderer.getTextAdvanceX(kFontId, "When", EpdFontFamily::REGULAR) +
                            renderer.getSpaceAdvance(kFontId, 'n', 'i', EpdFontFamily::REGULAR);
  const int indusHyphenW = renderer.getTextAdvanceX(kFontId, "indus-", EpdFontFamily::REGULAR);
  // Set column width 2 px narrower than ("When " + "indus-") so ONLY the +3 px hyphen hang fits "indus-"!
  const uint16_t indusColW = static_cast<uint16_t>(whenPlusSpace + indusHyphenW - 2);
  for (const size_t maxLines : {SIZE_MAX, size_t{1}}) {
    ParsedText hyphPara(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/false, style, 0);
    hyphPara.addWord("When", EpdFontFamily::REGULAR);
    hyphPara.addWord("industrial", EpdFontFamily::REGULAR);
    hyphPara.addWord("design", EpdFontFamily::REGULAR);
    hyphPara.addWord("matters.", EpdFontFamily::REGULAR);
    std::vector<std::unique_ptr<TextBlock>> lines;
    hyphPara.layoutAndExtractLines(
        renderer, kFontId, indusColW, [&](std::unique_ptr<TextBlock> b, uint32_t) { lines.push_back(std::move(b)); },
        true, 0, 100, maxLines);
    ASSERT_GE(lines.size(), 1u);
    const uint16_t lastIdx = lines[0]->wordCount() - 1;
    EXPECT_STREQ(lines[0]->wordText(lastIdx), "indus-");
    EXPECT_TRUE(lines[0]->wordHasDiscretionaryHyphen(lastIdx));
    const int rightEdge = lines[0]->wordXpos(lastIdx) +
                          renderer.getTextAdvanceX(kFontId, lines[0]->wordText(lastIdx), lines[0]->wordStyle(lastIdx));
    EXPECT_EQ(rightEdge, static_cast<int>(indusColW) + 3);
  }
}

TEST_F(TypographyLayoutFixture, OpticalMarginAlignmentLeavesNonJustifiedLastLinesRiverClampedAndCjkUnaffected) {
  // 1. Non-justified alignments (Left, Center, Right) must have 0 px optical hang on both margins.
  for (const CssTextAlign align : {CssTextAlign::Left, CssTextAlign::Center, CssTextAlign::Right}) {
    BlockStyle style;
    style.alignment = align;
    style.textIndentDefined = true;
    style.textIndent = 0;

    ParsedText para(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/false, style, 0);
    const char* words[] = {"\xE2\x80\x9CIf",    "I",    "made", "a",     "bold",   "move", "in",
                           "jail,\xE2\x80\x9D", "said", "the",  "brash", "leader."};
    for (const char* w : words) {
      para.addWord(w, EpdFontFamily::REGULAR);
    }

    constexpr uint16_t kWidth = 240;
    std::vector<std::unique_ptr<TextBlock>> lines;
    para.layoutAndExtractLines(renderer, kFontId, kWidth,
                               [&](std::unique_ptr<TextBlock> b, uint32_t) { lines.push_back(std::move(b)); });
    ASSERT_GE(lines.size(), 2u);

    for (size_t li = 0; li < lines.size(); ++li) {
      const TextBlock& line = *lines[li];
      ASSERT_GT(line.wordCount(), 0u);
      const uint16_t lastIdx = line.wordCount() - 1;
      const int rightEdge =
          line.wordXpos(lastIdx) + renderer.getTextAdvanceX(kFontId, line.wordText(lastIdx), line.wordStyle(lastIdx));
      EXPECT_GE(line.wordXpos(0), 0) << "align=" << static_cast<int>(align) << " line=" << li;
      EXPECT_LE(rightEdge, static_cast<int>(kWidth)) << "align=" << static_cast<int>(align) << " line=" << li;
      if (align == CssTextAlign::Left) {
        EXPECT_EQ(line.wordXpos(0), 0);
      } else if (align == CssTextAlign::Right) {
        EXPECT_EQ(rightEdge, static_cast<int>(kWidth));
      }
    }
  }

  // 2. Last paragraph line in Justify mode: trailing period / closing quote must NOT protrude past pageWidth
  //    or trigger justification, and a single-line / final line starting with “ stays >= -3 px (safe at margin >= 5).
  {
    BlockStyle style;
    style.alignment = CssTextAlign::Justify;
    style.textIndentDefined = true;
    style.textIndent = 0;

    renderer.ensureSdCardFontReady(kFontId, "When modern leader.", 0x01);
    const int singleLineW = renderer.getTextAdvanceX(kFontId, "When", EpdFontFamily::REGULAR) +
                            renderer.getSpaceAdvance(kFontId, 'n', 'm', EpdFontFamily::REGULAR) +
                            renderer.getTextAdvanceX(kFontId, "modern", EpdFontFamily::REGULAR) +
                            renderer.getSpaceAdvance(kFontId, 'n', 'l', EpdFontFamily::REGULAR) +
                            renderer.getTextAdvanceX(kFontId, "leader.", EpdFontFamily::REGULAR);
    // Column is 1 px narrower than singleLineW: because it is the LAST line of the paragraph, rightHangPx MUST
    // be 0, forcing "leader." to wrap to line 1 instead of protruding +1 px on an unjustified final line!
    ParsedText lastLinePara(/*hyphenationEnabled=*/false, /*focusReadingEnabled=*/false, style, 0);
    lastLinePara.addWord("When", EpdFontFamily::REGULAR);
    lastLinePara.addWord("modern", EpdFontFamily::REGULAR);
    lastLinePara.addWord("leader.", EpdFontFamily::REGULAR);
    std::vector<std::unique_ptr<TextBlock>> lines;
    lastLinePara.layoutAndExtractLines(renderer, kFontId, static_cast<uint16_t>(singleLineW - 1),
                                       [&](std::unique_ptr<TextBlock> b, uint32_t) { lines.push_back(std::move(b)); });
    ASSERT_EQ(lines.size(), 2u);
    const TextBlock& finalLine = *lines.back();
    const uint16_t lastIdx = finalLine.wordCount() - 1;
    const int finalRight = finalLine.wordXpos(lastIdx) +
                           renderer.getTextAdvanceX(kFontId, finalLine.wordText(lastIdx), finalLine.wordStyle(lastIdx));
    EXPECT_LE(finalRight, singleLineW - 1);
  }

  // 3. River-clamped ragged fallback line: rightHangPx must be suppressed, gap stays <= 4x normalSpace,
  //    and screen X at minimum 5 px margin never underflows.
  {
    BlockStyle style;
    style.alignment = CssTextAlign::Justify;
    style.textIndentDefined = true;
    style.textIndent = 0;

    ParsedText riverPara(/*hyphenationEnabled=*/false, /*focusReadingEnabled=*/false, style, 0);
    riverPara.addWord(
        "\xE2\x80\x9C"
        "brash,",
        EpdFontFamily::REGULAR);
    riverPara.addWord("quick.\xE2\x80\x9D", EpdFontFamily::REGULAR);
    riverPara.addWord("WWWWWWWWWWWWWWWWWWWWWWWWWWWWWW", EpdFontFamily::REGULAR);
    std::vector<std::unique_ptr<TextBlock>> lines;
    riverPara.layoutAndExtractLines(renderer, kFontId, 508,
                                    [&](std::unique_ptr<TextBlock> b, uint32_t) { lines.push_back(std::move(b)); });
    ASSERT_GE(lines.size(), 2u);
    ASSERT_EQ(lines[0]->wordCount(), 2u);
    constexpr int kMinScreenMargin = 5;
    EXPECT_GE(kMinScreenMargin + lines[0]->wordXpos(0), 0);
    const int w0 = renderer.getTextAdvanceX(kFontId, lines[0]->wordText(0), EpdFontFamily::REGULAR);
    const int w1 = renderer.getTextAdvanceX(kFontId, lines[0]->wordText(1), EpdFontFamily::REGULAR);
    const int gap = lines[0]->wordXpos(1) - (lines[0]->wordXpos(0) + w0);
    const int normalSpace = renderer.getSpaceWidth(kFontId, EpdFontFamily::REGULAR);
    EXPECT_LE(gap, normalSpace * 4);
    EXPECT_LT(lines[0]->wordXpos(1) + w1, 508);
  }

  // 4. CJK fullwidth punctuation (「中文，文。」) and CJK-glued smart quotes (“中文”) must have 0 px hang.
  {
    BlockStyle style;
    style.alignment = CssTextAlign::Justify;
    style.textIndentDefined = true;
    style.textIndent = 0;

    ParsedText cjkPara(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/false, style, 0);
    cjkPara.addWord(
        "\xE3\x80\x8C\xE4\xB8\xAD\xE6\x96\x87\xEF\xBC\x8C\xE4\xB8\xAD\xE6\x96\x87\xE3\x80\x82\xE3\x80\x8D"
        "\xE2\x80\x9C\xE4\xB8\xAD\xE6\x96\x87\xE2\x80\x9D\xE4\xB8\xAD\xE6\x96\x87",
        EpdFontFamily::REGULAR);
    std::vector<std::unique_ptr<TextBlock>> lines;
    cjkPara.layoutAndExtractLines(renderer, kFontId, 160,
                                  [&](std::unique_ptr<TextBlock> b, uint32_t) { lines.push_back(std::move(b)); });
    ASSERT_GE(lines.size(), 2u);
    for (size_t li = 0; li + 1 < lines.size(); ++li) {
      EXPECT_EQ(lines[li]->wordXpos(0), 0) << "CJK line " << li << " unexpectedly hung left";
      const uint16_t lastIdx = lines[li]->wordCount() - 1;
      const int rightEdge =
          lines[li]->wordXpos(lastIdx) +
          renderer.getTextAdvanceX(kFontId, lines[li]->wordText(lastIdx), lines[li]->wordStyle(lastIdx));
      EXPECT_EQ(rightEdge, 160) << "CJK line " << li << " unexpectedly hung right";
    }
  }
}

TEST_F(TypographyLayoutFixture, OpticalMarginAlignmentPreservesFramebufferViewportSerializationAndClippingBounds) {
  constexpr int kMinMargin = 5;
  constexpr uint16_t kViewportW = 528 - 2 * kMinMargin;  // 518 px on X3 Portrait

  BlockStyle style;
  style.alignment = CssTextAlign::Justify;
  style.textIndentDefined = true;
  style.textIndent = 0;

  ParsedText para(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/false, style, 0);
  const uint8_t linkId = para.addLinkTarget("#note-1");
  ASSERT_GT(linkId, 0u);
  para.addWord("\xE2\x80\x9CIf", EpdFontFamily::REGULAR, /*underline=*/false, /*attachToPrevious=*/false,
               /*visibleTextOffset=*/0, linkId);
  const char* rest[] = {"Tony",  "Fadell", "built", "modern",    "portable", "music",  "players",
                        "for",   "the",    "whole", "industry,", "every",    "single", "detail",
                        "would", "align",  "with",  "careful",   "craft."};
  uint32_t visOff = 4;
  for (const char* w : rest) {
    para.addWord(w, EpdFontFamily::REGULAR, false, false, visOff);
    visOff += static_cast<uint32_t>(std::strlen(w)) + 1;
  }

  std::vector<std::unique_ptr<TextBlock>> lines;
  para.layoutAndExtractLines(renderer, kFontId, kViewportW,
                             [&](std::unique_ptr<TextBlock> b, uint32_t) { lines.push_back(std::move(b)); });
  ASSERT_GE(lines.size(), 2u);

  // Verify signed negative wordXpos(0) == -3 survives TextBlock binary serialization round-trip
  // and link span extraction without unsigned underflow!
  const auto links = lines[0]->takeLinkSpans();
  ASSERT_EQ(links.size(), 1u);
  EXPECT_EQ(links[0].x, -3);
  EXPECT_GT(links[0].width, 0);
  EXPECT_GE(kMinMargin + links[0].x, 2);

  std::vector<uint8_t> serializedBytes;
  {
    HalFile writeFile;
    writeFile.openScratchBuffer(serializedBytes, /*writeMode=*/true);
    ASSERT_TRUE(lines[0]->serialize(writeFile));
  }
  HalFile readFile;
  readFile.openScratchBuffer(serializedBytes, /*writeMode=*/false);
  auto restored = TextBlock::deserialize(readFile);
  ASSERT_NE(restored, nullptr);
  ASSERT_EQ(restored->wordCount(), lines[0]->wordCount());
  EXPECT_EQ(restored->wordXpos(0), -3);

  // Strict monotonicity of wordXpos ensures clippingWordRect / ClipSelectionActivity widths stay positive.
  for (uint16_t i = 0; i + 1 < restored->wordCount(); ++i) {
    EXPECT_GT(restored->wordXpos(i + 1), restored->wordXpos(i));
  }

  // Render on X3 528 px logical Portrait framebuffer at minimum margin x = 5:
  // No glyph pixel may clip at x < 0 or x >= 528!
  renderer.clearScreen(0xFF);
  {
    auto scope = cacheMgr->createPrewarmScope();
    restored->render(renderer, kFontId, kMinMargin, 20);
    scope.endScanAndPrewarm();
    restored->render(renderer, kFontId, kMinMargin, 20);
  }
  const auto drawnCols = scanDrawnColumnsAtRow(renderer, 39, 528);
  ASSERT_FALSE(drawnCols.empty());
  EXPECT_EQ(drawnCols.front(), kMinMargin - 3);  // 5 - 3 = 2 >= 0 (unclipped left overhang!)
  EXPECT_LT(drawnCols.back(), 528);              // strictly inside right screen border!

  ReaderRenderSpec spec;
  spec.fontId = kFontId;
  spec.viewportWidth = kViewportW;
  spec.viewportHeight = 782;
  spec.hyphenationEnabled = true;
  EXPECT_NE(readerRenderSpecSignature(spec), 0u);
}

// ============================================================================
// Knuth-Plass Compaction, Oversized Token Pre-Split & OOM Split Fallback
// ============================================================================

TEST_F(TypographyLayoutFixture, KnuthPlassCompactionPreservesAlreadyReachedOptimalTerminalPath) {
  BlockStyle style;
  style.alignment = CssTextAlign::Justify;
  style.textIndentDefined = true;
  style.textIndent = 0;

  renderer.ensureSdCardFontReady(kFontId, "storytelling electronic reader. ", 0x01);
  const int wStory = renderer.getTextAdvanceX(kFontId, "storytelling", EpdFontFamily::REGULAR);
  const int wElec = renderer.getTextAdvanceX(kFontId, "electronic", EpdFontFamily::REGULAR);
  const int wReader = renderer.getTextAdvanceX(kFontId, "reader", EpdFontFamily::REGULAR);
  const int wReaderDot = renderer.getTextAdvanceX(kFontId, "reader.", EpdFontFamily::REGULAR);
  const int sp = renderer.getSpaceWidth(kFontId, EpdFontFamily::REGULAR);

  const int prefixLineW = wStory + sp + wStory + sp + wReader;
  const int tailLineW = wStory + sp + wElec + sp + wReaderDot;
  const uint16_t kColW = static_cast<uint16_t>(std::max(prefixLineW, tailLineW) + 4);

  for (size_t prefixGroups = 25; prefixGroups <= 55; ++prefixGroups) {
    SCOPED_TRACE(prefixGroups);
    ParsedText para(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/false, style, 0);
    for (size_t g = 0; g < prefixGroups; ++g) {
      para.addWord("storytelling", EpdFontFamily::REGULAR);
      para.addWord("storytelling", EpdFontFamily::REGULAR);
      para.addWord("reader", EpdFontFamily::REGULAR);
    }
    para.addWord("storytelling", EpdFontFamily::REGULAR);
    para.addWord("electronic", EpdFontFamily::REGULAR);
    para.addWord("reader.", EpdFontFamily::REGULAR);

    std::vector<std::unique_ptr<TextBlock>> lines;
    para.layoutAndExtractLines(renderer, kFontId, kColW,
                               [&](std::unique_ptr<TextBlock> b, uint32_t) { lines.push_back(std::move(b)); });

    ASSERT_EQ(lines.size(), prefixGroups + 1);
    const TextBlock& lastLine = *lines.back();
    ASSERT_EQ(lastLine.wordCount(), 3u);
    EXPECT_STREQ(lastLine.wordText(0), "storytelling");
    EXPECT_STREQ(lastLine.wordText(1), "electronic");
    EXPECT_STREQ(lastLine.wordText(2), "reader.");
  }

  // Hanging indent (-300 px) lets w = 0 fit all 254 words on one line (recording terminal (254, 0)
  // at nodeCount == 255), while w = 1 overflows at word 252 and pushes nodeCount to MAX_DP_NODES (256).
  {
    BlockStyle hangingStyle;
    hangingStyle.alignment = CssTextAlign::Left;
    hangingStyle.textIndentDefined = true;
    hangingStyle.textIndent = -300;

    renderer.ensureSdCardFontReady(kFontId, "I internationalization-", 0x01);
    const int wI = renderer.getTextAdvanceX(kFontId, "I", EpdFontFamily::REGULAR);
    const int spI = renderer.getSpaceAdvance(kFontId, 'I', 'I', EpdFontFamily::REGULAR);
    const int spToLong = renderer.getSpaceAdvance(kFontId, 'I', 'i', EpdFontFamily::REGULAR);
    const int wLong = renderer.getTextAdvanceX(kFontId, "internationalization", EpdFontFamily::REGULAR);
    const int spLong = renderer.getSpaceAdvance(kFontId, 'n', 'i', EpdFontFamily::REGULAR);

    constexpr size_t kShortCount = 252;
    const int totalW = static_cast<int>(kShortCount - 1) * (wI + spI) + wI + spToLong + wLong + spLong + wLong;
    const uint16_t viewportW = static_cast<uint16_t>(totalW - 290);

    ParsedText para(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/false, hangingStyle, 0);
    for (size_t i = 0; i < kShortCount; ++i) {
      para.addWord("I", EpdFontFamily::REGULAR);
    }
    para.addWord("internationalization", EpdFontFamily::REGULAR);
    para.addWord("internationalization", EpdFontFamily::REGULAR);
    ASSERT_EQ(para.size(), 254u);

    std::vector<std::unique_ptr<TextBlock>> lines;
    para.layoutAndExtractLines(renderer, kFontId, viewportW,
                               [&](std::unique_ptr<TextBlock> b, uint32_t) { lines.push_back(std::move(b)); });

    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0]->wordCount(), 254u);
  }
}

TEST_F(TypographyLayoutFixture, OversizedHyphenatableWordOver220BytesPreSplitsCleanlyWithoutOverflow) {
  // Tokens longer than MAX_SPLIT_PREFIX_BYTES (200) exceed uint8_t DpBreakNode::byteOffset and
  // must pre-split cleanly even when early syllables fit within effectiveWidth.
  std::string softHyphenWord;
  std::string expectedStrippedSoft;
  for (int i = 0; i < 16; ++i) {
    if (i > 0) {
      softHyphenWord += "\xC2\xAD";
    }
    softHyphenWord += (i % 2 == 0) ? "international" : "storytelling";
    expectedStrippedSoft += (i % 2 == 0) ? "international" : "storytelling";
  }
  ASSERT_GT(softHyphenWord.size(), 220u);
  ASSERT_FALSE(Hyphenator::breakOffsets(softHyphenWord, /*includeFallback=*/false).empty());

  std::string apostropheWord;
  for (int i = 0; i < 18; ++i) {
    if (i > 0) apostropheWord += '\'';
    apostropheWord += (i % 2 == 0) ? "international" : "storytelling";
  }
  ASSERT_GT(apostropheWord.size(), 220u);
  ASSERT_FALSE(Hyphenator::breakOffsets(apostropheWord, /*includeFallback=*/false).empty());

  renderer.ensureSdCardFontReady(kFontId, "See international'storytelling-", 0x01);

  for (const auto& [longWord, expectedClean] :
       {std::pair{softHyphenWord, expectedStrippedSoft}, std::pair{apostropheWord, apostropheWord}}) {
    for (const CssTextAlign align : {CssTextAlign::Left, CssTextAlign::Justify}) {
      for (const uint16_t colWidth : {uint16_t{180}, uint16_t{518}}) {
        BlockStyle style;
        style.alignment = align;
        style.textIndentDefined = true;
        style.textIndent = 0;

        ParsedText para(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/false, style, 0);
        para.addWord("See", EpdFontFamily::REGULAR);
        para.addWord(longWord, EpdFontFamily::REGULAR);

        std::vector<std::unique_ptr<TextBlock>> lines;
        para.layoutAndExtractLines(renderer, kFontId, colWidth,
                                   [&](std::unique_ptr<TextBlock> b, uint32_t) { lines.push_back(std::move(b)); });

        ASSERT_GE(lines.size(), 3u);
        EXPECT_TRUE(para.isEmpty());

        std::string reconstructed;
        const int maxAllowedHang = (align == CssTextAlign::Justify) ? 3 : 0;
        for (size_t li = 0; li < lines.size(); ++li) {
          const TextBlock& line = *lines[li];
          ASSERT_TRUE(line.valid());
          ASSERT_GT(line.wordCount(), 0u);
          const uint16_t lastIdx = line.wordCount() - 1;
          const int rightEdge = line.wordXpos(lastIdx) +
                                renderer.getTextAdvanceX(kFontId, line.wordText(lastIdx), line.wordStyle(lastIdx));
          const int lineLimit = static_cast<int>(colWidth) + (li + 1 < lines.size() ? maxAllowedHang : 0);
          EXPECT_LE(rightEdge, lineLimit) << "Overflow on line " << li << " at colWidth=" << colWidth;

          for (uint16_t wi = 0; wi < line.wordCount(); ++wi) {
            std::string piece = line.wordText(wi);
            if (line.wordHasDiscretionaryHyphen(wi)) {
              EXPECT_EQ(wi + 1, line.wordCount()) << "Mid-line discretionary hyphen on line " << li;
              ASSERT_FALSE(piece.empty());
              ASSERT_EQ(piece.back(), '-');
              piece.pop_back();
            }
            reconstructed += piece;
          }
        }
        EXPECT_EQ(reconstructed, std::string("See") + expectedClean);
      }
    }
  }
}

TEST_F(TypographyLayoutFixture, SplitMaterializationOomFallbackProducesStrictlyIncreasingBreaksAndZeroEmptyBlocks) {
  renderer.ensureSdCardFontReady(kFontId, "first characteristically oft.", 0x01);

  for (const bool includeLastLine : {true, false}) {
    SCOPED_TRACE(includeLastLine);
    BlockStyle style;
    style.alignment = CssTextAlign::Justify;
    style.textIndentDefined = true;
    style.textIndent = 0;

    ParsedText para(/*hyphenationEnabled=*/true, /*focusReadingEnabled=*/false, style, 0);

    // Advance WordStore's 2048-byte initial bump chunk to 2022 used bytes (337 * ("first\0" = 6 B)),
    // then consume those words so para is empty while chunk 0 retains only 26 free bytes.
    for (int i = 0; i < 337; ++i) {
      para.addWord("first", EpdFontFamily::REGULAR);
    }
    para.layoutAndExtractLines(
        renderer, kFontId, 518, [](std::unique_ptr<TextBlock>, uint32_t) {}, /*includeLastLine=*/true);
    ASSERT_TRUE(para.isEmpty());
    para.resetFirstLineIndent();

    // Add 2 words consuming 19 + 5 = 24 bytes -> 2046 / 2048 bytes used (only 2 bytes left in chunk 0).
    // At colWidth = 95 px, "characteristically" (~145 px) starts line 0 (curWordIdx == prevBreak == 0)
    // and is split across multiple lines by Knuth-Plass DP before "oft." (~30 px, fits 95 px whole).
    para.addWord("characteristically", EpdFontFamily::REGULAR, /*underline=*/false, /*attachToPrevious=*/false,
                 /*visibleTextOffset=*/0);
    para.addWord("oft.", EpdFontFamily::REGULAR, /*underline=*/false, /*attachToPrevious=*/false,
                 /*visibleTextOffset=*/19);

    gDeniedNothrowArrayAllocs = 0;
    gFailNothrowArrayAllocSize = 2048;  // Deny WordStore's fresh 2048-byte chunk inside splitWordAtOffset().
    std::vector<std::unique_ptr<TextBlock>> lines;
    std::vector<uint32_t> lineOffsets;
    para.layoutAndExtractLines(
        renderer, kFontId, /*viewportWidth=*/95,
        [&](std::unique_ptr<TextBlock> b, const uint32_t visOff) {
          lines.push_back(std::move(b));
          lineOffsets.push_back(visOff);
        },
        includeLastLine);
    gFailNothrowArrayAllocSize = 0;

    EXPECT_GT(gDeniedNothrowArrayAllocs, 0u);

    if (!includeLastLine) {
      // Flush the remaining final line cleanly once OOM clears.
      para.layoutAndExtractLines(
          renderer, kFontId, /*viewportWidth=*/95,
          [&](std::unique_ptr<TextBlock> b, const uint32_t visOff) {
            lines.push_back(std::move(b));
            lineOffsets.push_back(visOff);
          },
          /*includeLastLine=*/true);
    }

    ASSERT_EQ(lines.size(), 2u);
    for (size_t li = 0; li < lines.size(); ++li) {
      ASSERT_NE(lines[li], nullptr);
      EXPECT_TRUE(lines[li]->valid()) << "Invalid/empty TextBlock at line " << li;
      EXPECT_GT(lines[li]->wordCount(), 0u) << "Zero-word TextBlock at line " << li;
      if (li > 0) {
        EXPECT_GT(lineOffsets[li], lineOffsets[li - 1]) << "Non-monotonic break at line " << li;
      }
    }
    EXPECT_STREQ(lines[0]->wordText(0), "characteristically");
    EXPECT_STREQ(lines[1]->wordText(0), "oft.");
    EXPECT_TRUE(para.isEmpty());
  }
}
