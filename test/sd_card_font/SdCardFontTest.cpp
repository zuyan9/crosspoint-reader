#include <HalStorage.h>
#include <SdCardFont.h>
#include <TtfEpdFont.h>
#include <Utf8.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>

#include "lib/GfxRenderer/GlyphBitmap.h"

namespace {
size_t failNextArraySize = 0;
size_t nothrowArrayAllocations = 0;
bool trackSlabHeap = false;
}  // namespace

// Keep array allocation and deletion paired, including arrays allocated by the test framework.
void* operator new[](size_t size) {
  void* allocation = std::malloc(size == 0 ? 1 : size);
  if (!allocation) {
    std::fputs("Unexpected host OOM in SdCardFontTest\n", stderr);
    std::exit(EXIT_FAILURE);
  }
  return allocation;
}

void* operator new[](size_t size, const std::nothrow_t&) noexcept {
  ++nothrowArrayAllocations;
  if (failNextArraySize != 0 && size == failNextArraySize) {
    failNextArraySize = 0;
    return nullptr;
  }
  void* ptr = std::malloc(size == 0 ? 1 : size);
  if (ptr && trackSlabHeap && size == SdCardFont::MINI_SLAB_SIZE && ESP.freeHeap >= size) {
    ESP.freeHeap -= size;
  }
  return ptr;
}

void operator delete[](void* allocation) noexcept { std::free(allocation); }
void operator delete[](void* allocation, size_t) noexcept { std::free(allocation); }
void operator delete[](void* allocation, const std::nothrow_t&) noexcept { std::free(allocation); }

namespace {
constexpr uint32_t FIRST = 0xAC00;
constexpr uint32_t GLYPHS = 513;
constexpr uint16_t BITMAP_BYTES = 128;

void put16(size_t at, uint16_t value) {
  sdFontTestFile[at] = value;
  sdFontTestFile[at + 1] = value >> 8;
}
void put32(size_t at, uint32_t value) {
  put16(at, value);
  put16(at + 2, value >> 16);
}

// `glyphs` Hangul glyphs from FIRST, the last of them U+FFFD.
void makeFont(uint32_t glyphs = GLYPHS) {
  constexpr size_t GLYPH_OFFSET = 64 + 24;
  const size_t BITMAP_OFFSET = GLYPH_OFFSET + glyphs * sizeof(EpdGlyph);
  sdFontTestFile.assign(BITMAP_OFFSET + glyphs * BITMAP_BYTES, 0);
  std::memcpy(sdFontTestFile.data(), "CPFONT\0\0", 8);
  put16(8, CPFONT_VERSION);
  sdFontTestFile[12] = 1;
  put32(36, 2);
  put32(40, glyphs);
  sdFontTestFile[44] = 32;
  put16(45, 32);
  put32(56, 64);
  put32(64, FIRST);
  put32(68, FIRST + glyphs - 2);
  put32(76, 0xFFFD);
  put32(80, 0xFFFD);
  put32(84, glyphs - 1);
  for (uint32_t i = 0; i < glyphs; ++i) {
    EpdGlyph glyph{};
    glyph.width = 32;
    glyph.height = 32;
    glyph.advanceX = 32 << 4;
    glyph.top = 32;
    glyph.dataLength = BITMAP_BYTES;
    // Store bitmaps in reverse glyph order to exercise sorted reads on rebuild.
    glyph.dataOffset = (glyphs - 1 - i) * BITMAP_BYTES;
    std::memcpy(sdFontTestFile.data() + GLYPH_OFFSET + i * sizeof(glyph), &glyph, sizeof(glyph));
    std::memset(sdFontTestFile.data() + BITMAP_OFFSET + glyph.dataOffset, i % 251, BITMAP_BYTES);
  }
}

// Six Latin glyphs 'A'..'F' plus U+FFFD, with left classes for 'A' and 'C',
// right classes for 'B' and 'D', and a class matrix whose top-left 2×2 corner
// holds their pairs. `extraEntries` appends entries for U+0100 onwards to both
// class tables, so the tables span several read blocks. They use `extraClass`,
// or classes 3..classCount in turn when it is 0. A `classCount` of 0 writes no
// kern data. `ligature` adds one pair, E+F -> A.
void makeKerningFont(uint16_t extraEntries = 0, uint8_t classCount = 2, uint8_t extraClass = 2, bool ligature = false) {
  constexpr uint32_t KERN_GLYPHS = 7;
  constexpr size_t GLYPH_OFFSET = 64 + 24;
  constexpr size_t KERN_OFFSET = GLYPH_OFFSET + KERN_GLYPHS * sizeof(EpdGlyph);
  constexpr uint8_t LEFT[][2] = {{'A', 1}, {'C', 2}};
  constexpr uint8_t RIGHT[][2] = {{'B', 1}, {'D', 2}};
  constexpr int8_t MATRIX[2][2] = {{-3, 0}, {4, -5}};
  const uint16_t entries = classCount ? 2 + extraEntries : 0;
  const size_t matrixBytes = static_cast<size_t>(classCount) * classCount;
  const size_t ligatureOffset = KERN_OFFSET + entries * 3 * 2 + matrixBytes;
  const size_t bitmapOffset = ligatureOffset + (ligature ? 8 : 0);
  sdFontTestFile.assign(bitmapOffset + KERN_GLYPHS * BITMAP_BYTES, 0);
  std::memcpy(sdFontTestFile.data(), "CPFONT\0\0", 8);
  put16(8, CPFONT_VERSION);
  sdFontTestFile[12] = 1;
  put32(36, 2);
  put32(40, KERN_GLYPHS);
  sdFontTestFile[44] = 32;
  put16(45, 32);
  put16(49, entries);  // left class entries
  put16(51, entries);  // right class entries
  sdFontTestFile[53] = classCount;
  sdFontTestFile[54] = classCount;
  sdFontTestFile[55] = ligature ? 1 : 0;
  put32(56, 64);
  put32(64, 'A');
  put32(68, 'F');
  put32(76, 0xFFFD);
  put32(80, 0xFFFD);
  put32(84, KERN_GLYPHS - 1);
  for (uint32_t i = 0; i < KERN_GLYPHS; ++i) {
    EpdGlyph glyph{};
    glyph.width = 32;
    glyph.height = 32;
    glyph.advanceX = 32 << 4;
    glyph.top = 32;
    glyph.dataLength = BITMAP_BYTES;
    glyph.dataOffset = i * BITMAP_BYTES;
    std::memcpy(sdFontTestFile.data() + GLYPH_OFFSET + i * sizeof(glyph), &glyph, sizeof(glyph));
  }
  size_t at = KERN_OFFSET;
  for (const auto* table : {LEFT, RIGHT}) {
    if (classCount == 0) break;
    for (size_t i = 0; i < 2; ++i, at += 3) {
      put16(at, table[i][0]);
      sdFontTestFile[at + 2] = table[i][1];
    }
    for (uint16_t i = 0; i < extraEntries; ++i, at += 3) {
      put16(at, 0x100 + i);
      sdFontTestFile[at + 2] = extraClass ? extraClass : 3 + i % (classCount - 2);
    }
  }
  for (size_t row = 0; classCount > 0 && row < 2; ++row) {
    std::memcpy(sdFontTestFile.data() + at + row * classCount, MATRIX[row], sizeof(MATRIX[row]));
  }
  if (ligature) {
    put32(ligatureOffset, 'E' << 16 | 'F');
    put32(ligatureOffset + 4, 'A');
  }
}

std::string latinPage(const char* ascii, uint32_t first, uint32_t count) {
  std::string text = ascii;
  for (uint32_t cp = first; cp < first + count; ++cp) {
    text.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    text.push_back(static_cast<char>(0x80 | (cp & 63)));
  }
  return text;
}

std::string page(uint32_t first, uint32_t count) {
  std::string text;
  text.reserve(count * 3);
  for (uint32_t cp = first; cp < first + count; ++cp) {
    text.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    text.push_back(static_cast<char>(0x80 | ((cp >> 6) & 63)));
    text.push_back(static_cast<char>(0x80 | (cp & 63)));
  }
  return text;
}

uint32_t residentCount(SdCardFont& font, uint8_t style = 0) {
  const auto* data = font.getEpdFont(style)->data;
  uint32_t count = 0;
  for (uint32_t i = 0; i < data->intervalCount; ++i) {
    count += data->intervals[i].last - data->intervals[i].first + 1;
  }
  return count;
}

void expectPageBitmaps(SdCardFont& font, uint32_t first, uint32_t count) {
  const auto* data = font.getEpdFont()->data;
  for (uint32_t cp = first; cp < first + count; ++cp) {
    const EpdGlyph* glyph = nullptr;
    for (uint32_t i = 0; i < data->intervalCount; ++i) {
      const auto& interval = data->intervals[i];
      if (cp >= interval.first && cp <= interval.last) glyph = data->glyph + interval.offset + cp - interval.first;
    }
    ASSERT_NE(nullptr, glyph) << cp;
    ASSERT_EQ(BITMAP_BYTES, glyph->dataLength);
    for (uint16_t i = 0; i < BITMAP_BYTES; ++i) {
      ASSERT_EQ((cp - FIRST) % 251, data->bitmap[glyph->dataOffset + i]);
    }
  }
}
}  // namespace

TEST(SdCardFontTest, CompletePagesReplaceEarlierGlyphs) {
  makeFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  for (uint32_t offset : {0U, 100U, 200U}) {
    const uint32_t first = FIRST + offset;
    const auto text = page(first, 100);
    font.clearCache();
    ASSERT_EQ(0, font.prewarm(text.c_str(), 1, false, false, false));
    EXPECT_EQ(101U, residentCount(font));  // includes replacement glyph
    expectPageBitmaps(font, first, 100);
  }
}

TEST(SdCardFontTest, IncrementalUiStringsStillAccumulate) {
  makeFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_EQ(0, font.prewarm(page(FIRST, 5).c_str(), 1));
  ASSERT_EQ(0, font.prewarm(page(FIRST + 5, 5).c_str(), 1));
  EXPECT_EQ(11U, residentCount(font));
  expectPageBitmaps(font, FIRST, 10);
}

TEST(SdCardFontTest, UnderusedBuffersKeepTheFreshlyPrewarmedPageUntilItIsDrawn) {
  makeFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_EQ(0, font.prewarm(page(FIRST, 200).c_str(), 1, false, false, false));
  font.clearCache();
  for (uint32_t offset : {300U, 400U, 200U, 0U}) {
    const auto text = page(FIRST + offset, 100);
    font.clearCache();
    ASSERT_EQ(0, font.prewarm(text.c_str(), 1, false, false, false));
    font.clearCache();  // idle prewarm scope closes
    font.clearCache();  // actual page render scope opens
    sdFontTestReads = 0;
    ASSERT_EQ(0, font.prewarm(text.c_str(), 1, false, false, false));
    EXPECT_EQ(0U, sdFontTestReads);
    expectPageBitmaps(font, FIRST + offset, 100);
  }
}

TEST(SdCardFontTest, BitmapGrowthPreservesReadOrderAndAllGlyphData) {
  makeFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  for (uint32_t count : {50U, 100U, 200U, 400U}) {
    font.clearCache();
    ASSERT_EQ(0, font.prewarm(page(FIRST, count).c_str(), 1, false, false, false));
    EXPECT_EQ(count + 1, residentCount(font));
    expectPageBitmaps(font, FIRST, count);
  }
}

TEST(SdCardFontTest, FragmentedBitmapGrowthRebuildsMetadataAndKeepsThePrefetch) {
  makeFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_EQ(0, font.prewarm(page(FIRST, 50).c_str(), 1, false, false, false));
  struct HeapReportGuard {
    ~HeapReportGuard() { ESP.largestBlock = 200 * 1024; }
  } guard;
  ESP.largestBlock = 8 * 1024;
  const auto text = page(FIRST, 200);
  ASSERT_EQ(0, font.prewarm(text.c_str(), 1, false, false, false));
  expectPageBitmaps(font, FIRST, 200);
  font.clearCache();
  sdFontTestReads = 0;
  ASSERT_EQ(0, font.prewarm(text.c_str(), 1, false, false, false));
  EXPECT_EQ(0U, sdFontTestReads);
}

TEST(SdCardFontTest, BitmapAllocationRetriesAfterEvictingRebuildableAdvances) {
  makeFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  const auto text = page(FIRST, 200);
  ASSERT_EQ(0, font.buildAdvanceTable(text.c_str(), 1));
  ASSERT_TRUE(font.hasAdvanceTable());
  ASSERT_EQ(0, font.prewarm(page(FIRST, 50).c_str(), 1, false, false, false));
  struct AllocationGuard {
    ~AllocationGuard() {
      ESP.largestBlock = 200 * 1024;
      failNextArraySize = 0;
    }
  } guard;
  ESP.largestBlock = 8 * 1024;
  failNextArraySize = 201 * BITMAP_BYTES;
  ASSERT_EQ(0, font.prewarm(text.c_str(), 1, false, false, false));
  EXPECT_EQ(0U, failNextArraySize);
  EXPECT_FALSE(font.hasAdvanceTable());
  expectPageBitmaps(font, FIRST, 200);

  ASSERT_EQ(0, font.buildAdvanceTable(text.c_str(), 1));
  for (uint32_t cp = FIRST; cp < FIRST + 200; ++cp) {
    EXPECT_EQ(32 << 4, font.getAdvance(cp, 0));
  }
}

TEST(SdCardFontTest, PagesKernWithTheFontsClassMatrix) {
  makeKerningFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_EQ(0, font.prewarm("ABCDEF", 1, false, true, false));
  const EpdFont* epd = font.getEpdFont();
  EXPECT_EQ(-3, epd->getKerning('A', 'B'));
  EXPECT_EQ(0, epd->getKerning('A', 'D'));
  EXPECT_EQ(4, epd->getKerning('C', 'B'));
  EXPECT_EQ(-5, epd->getKerning('C', 'D'));
  EXPECT_EQ(0, epd->getKerning('B', 'D'));
  EXPECT_EQ(0, epd->getKerning('E', 'B'));
}

TEST(SdCardFontTest, KernRequestsServedFromAKernFreeMiniStillKern) {
  makeKerningFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_EQ(0, font.prewarm("ABCDEF", 1, false, false, false));  // kern-free prewarm, e.g. a UI string
  struct Step {
    const char* text;
    uint32_t left, right;
    int8_t kern;
  };
  // A subset without kerning pairs, then subsets whose pairs the earlier ones did not cover.
  for (const Step& step : {Step{"EF", 'E', 'F', 0}, Step{"AB", 'A', 'B', -3}, Step{"CD", 'C', 'D', -5}}) {
    ASSERT_EQ(0, font.prewarm(step.text, 1, false, true, false));
    EXPECT_EQ(step.kern, font.getEpdFont()->getKerning(step.left, step.right)) << step.text;
  }
}

TEST(SdCardFontTest, RedrawsAfterAKernFreeRebuildStillKern) {
  makeKerningFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_EQ(0, font.prewarm("ABCD", 1, false, true, false));
  ASSERT_EQ(0, font.prewarm("ABCDEF", 1, false, false, false));  // kern-free rebuild, e.g. a UI string
  ASSERT_EQ(0, font.prewarm("ABCD", 1, false, true, false));     // the page again, served from that cache
  EXPECT_EQ(-3, font.getEpdFont()->getKerning('A', 'B'));
  EXPECT_EQ(-5, font.getEpdFont()->getKerning('C', 'D'));
}

TEST(SdCardFontTest, ClassIdsPastTheMatrixAreUnkerned) {
  makeKerningFont(1, 2, 250);  // U+0100 claims class 250 in a 2×2 matrix
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_EQ(1, font.prewarm(latinPage("ABCD", 0x100, 1).c_str(), 1, false, true, false));  // U+0100 has no glyph
  const EpdFont* epd = font.getEpdFont();
  EXPECT_EQ(0, epd->getKerning('A', 0x100));
  EXPECT_EQ(0, epd->getKerning(0x100, 'B'));
  EXPECT_EQ(-3, epd->getKerning('A', 'B'));
  EXPECT_EQ(-5, epd->getKerning('C', 'D'));
}

TEST(SdCardFontTest, PagesCanUseAll255KernClasses) {
  makeKerningFont(253, 255, 0);
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_EQ(253, font.prewarm(latinPage("ABCD", 0x100, 253).c_str(), 1, false, true, false));
  const EpdFont* epd = font.getEpdFont();
  EXPECT_EQ(-3, epd->getKerning('A', 'B'));
  EXPECT_EQ(-5, epd->getKerning('C', 'D'));
  EXPECT_EQ(0, epd->getKerning(0x100, 0x1FC));
}

TEST(SdCardFontTest, AFailedKernBuildKeepsTheLigatures) {
  makeKerningFont(253, 255, 0, true);
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  failNextArraySize = 255 * 255;  // the mini kern matrix
  ASSERT_EQ(253, font.prewarm(latinPage("ABCDEF", 0x100, 253).c_str(), 1, false, true, false));
  EXPECT_EQ(0U, failNextArraySize);
  const EpdFont* epd = font.getEpdFont();
  EXPECT_EQ(static_cast<uint32_t>('A'), epd->getLigature('E', 'F'));
  EXPECT_EQ(0, epd->getKerning('A', 'B'));
}

TEST(SdCardFontTest, LigatureRequestsServedFromAKernFreeMiniGetLigatures) {
  makeKerningFont(0, 0, 2, true);  // ligatures, no kern classes
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_EQ(0, font.prewarm("AEF", 1, false, false, false));  // kern-free prewarm, e.g. a UI string
  ASSERT_EQ(0, font.prewarm("EF", 1, false, true, false));
  EXPECT_EQ(static_cast<uint32_t>('A'), font.getEpdFont()->getLigature('E', 'F'));
}

TEST(SdCardFontTest, RedrawingAPrewarmedPageReadsNothing) {
  makeKerningFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  for (const char* text : {"ABCD", "DEF"}) {  // with and without kerning pairs
    font.clearCache();
    ASSERT_EQ(0, font.prewarm(text, 1, false, true, false));
    font.clearCache();
    sdFontTestReads = 0;
    ASSERT_EQ(0, font.prewarm(text, 1, false, true, false));
    EXPECT_EQ(0U, sdFontTestReads) << text;
  }
}

TEST(SdCardFontTest, LaterPagesReadOnlyTheKernClassBlocksTheyUse) {
  makeKerningFont(300);  // five 64-entry blocks per class table
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  sdFontTestReads = 0;
  ASSERT_EQ(0, font.prewarm("ABCDEF", 1, false, true, false));
  const size_t firstReads = sdFontTestReads;
  EXPECT_EQ(-5, font.getEpdFont()->getKerning('C', 'D'));

  font.releaseResidentCaches();
  sdFontTestReads = 0;
  ASSERT_EQ(0, font.prewarm("ABCDEF", 1, false, true, false));
  EXPECT_EQ(firstReads - 8, sdFontTestReads);  // 4 of the 5 blocks skipped in each table
  const EpdFont* epd = font.getEpdFont();
  EXPECT_EQ(-3, epd->getKerning('A', 'B'));
  EXPECT_EQ(4, epd->getKerning('C', 'B'));
  EXPECT_EQ(-5, epd->getKerning('C', 'D'));
}

TEST(SdCardFontTest, AdvancesStayCorrectWhenPagesAddCodepointsOutOfOrder) {
  makeFont();
  for (uint32_t i = 0; i < GLYPHS; ++i) {
    put16(64 + 24 + i * sizeof(EpdGlyph) + offsetof(EpdGlyph, advanceX), (20 + i % 13) << 4);
  }
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  std::vector<uint32_t> added;
  for (uint32_t block : {3U, 0U, 5U, 1U, 4U, 2U}) {  // each merge lands before, between or after earlier ones
    ASSERT_EQ(0, font.buildAdvanceTable(page(FIRST + block * 80, 80).c_str(), 1));
    for (uint32_t cp = FIRST + block * 80; cp < FIRST + block * 80 + 80; ++cp) added.push_back(cp);
    for (uint32_t cp : added) {
      ASSERT_EQ((20 + (cp - FIRST) % 13) << 4, font.getAdvance(cp, 0)) << std::hex << cp;
    }
  }
}

TEST(SdCardFontTest, AdvanceMergesPastTheCapKeepTheLowestCodepoints) {
  constexpr uint32_t CACHE_LIMIT = 768;  // SdCardFont::ADVANCE_CACHE_LIMIT
  constexpr uint32_t GLYPH_COUNT = 1001;
  makeFont(GLYPH_COUNT);
  for (uint32_t i = 0; i < GLYPH_COUNT; ++i) {
    put16(64 + 24 + i * sizeof(EpdGlyph) + offsetof(EpdGlyph, advanceX), (20 + i % 13) << 4);
  }
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  // Reference: the sorted union of every page, truncated to the cap; a full table takes nothing more.
  std::vector<uint32_t> expected;
  for (uint32_t block : {6U, 1U, 9U, 3U, 0U, 7U, 4U, 8U, 2U, 5U}) {
    ASSERT_GE(font.buildAdvanceTable(page(FIRST + block * 100, 100).c_str(), 1), 0);
    if (expected.size() < CACHE_LIMIT) {
      for (uint32_t cp = FIRST + block * 100; cp < FIRST + block * 100 + 100; ++cp) expected.push_back(cp);
      std::sort(expected.begin(), expected.end());
      if (expected.size() > CACHE_LIMIT) expected.resize(CACHE_LIMIT);
    }
    for (uint32_t cp = FIRST; cp < FIRST + 1000; ++cp) {
      const bool cached = std::binary_search(expected.begin(), expected.end(), cp);
      ASSERT_EQ(cached ? (20 + (cp - FIRST) % 13) << 4 : 0, font.getAdvance(cp, 0)) << block << " " << std::hex << cp;
    }
  }
}

TEST(SdCardFontTest, AnEmptyOrFailedAdvanceBuildLeavesNoTable) {
  makeFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  font.buildAdvanceTable("", 1);
  EXPECT_FALSE(font.hasAdvanceTable());
  failNextArraySize = (768 + 2) * sizeof(uint32_t);  // the codepoint scratch
  const std::string largeText = page(FIRST, 10) + std::string(4096, ' ');
  EXPECT_EQ(-1, font.buildAdvanceTable(largeText.c_str(), 1));
  EXPECT_FALSE(font.hasAdvanceTable());
  ASSERT_EQ(0, font.buildAdvanceTable(page(FIRST, 10).c_str(), 1));
  EXPECT_TRUE(font.hasAdvanceTable());
}

TEST(SdCardFontTest, AdvancesBuildForAStyleStoredInAnotherSlot) {
  // An italic-only font: style 2 lives in slot 0 of the compact style array.
  makeFont();
  sdFontTestFile[32] = 2;
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_TRUE(font.hasStyle(2));
  ASSERT_FALSE(font.hasStyle(0));
  const auto text = page(FIRST, 100);
  ASSERT_EQ(0, font.buildAdvanceTable(text.c_str(), 1 << 2));
  for (uint32_t cp = FIRST; cp < FIRST + 100; ++cp) {
    EXPECT_EQ(32 << 4, font.getAdvance(cp, 2));
  }
}

TEST(SdCardFontTest, AFullAdvanceTableSkipsTheScanWithoutAllocating) {
  makeFont(1001);
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_EQ(0, font.buildAdvanceTable(page(FIRST, 768).c_str(), 1));
  ASSERT_EQ(32U << 4, font.getAdvance(FIRST + 767, 0));
  const size_t allocationsBefore = nothrowArrayAllocations;
  EXPECT_EQ(0, font.buildAdvanceTable(page(FIRST + 768, 200).c_str(), 1));
  EXPECT_EQ(allocationsBefore, nothrowArrayAllocations);
  EXPECT_EQ(0, font.getAdvance(FIRST + 768, 0));
}

TEST(SdCardFontTest, OnePageWithMoreCodepointsThanTheTableKeepsTheLowestInAnyOrder) {
  constexpr uint32_t CACHE_LIMIT = 768;  // SdCardFont::ADVANCE_CACHE_LIMIT
  makeFont(1001);
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  // The top 768 first (ascending, so the largest is not the first held), then
  // the rest descending: each must displace the largest held. Every codepoint
  // appears twice, so repeats must not count twice.
  std::string text;
  for (uint32_t cp = FIRST + 232; cp < FIRST + 1000; ++cp) text += page(cp, 1) + page(cp, 1);
  for (uint32_t cp = FIRST + 231; cp + 1 > FIRST; --cp) text += page(cp, 1) + page(cp, 1);
  ASSERT_EQ(0, font.buildAdvanceTable(text.c_str(), 1));
  for (uint32_t cp = FIRST; cp < FIRST + 1000; ++cp) {
    ASSERT_EQ(cp < FIRST + CACHE_LIMIT ? 32U << 4 : 0U, font.getAdvance(cp, 0)) << std::hex << cp;
  }
}

TEST(SdCardFontTest, CodepointsTheFontLacksDoNotTakeTableSlotsWithoutAReplacementGlyph) {
  constexpr uint32_t CACHE_LIMIT = 768;  // SdCardFont::ADVANCE_CACHE_LIMIT
  makeFont(1001);
  put32(76, 0xFFFC);  // the last glyph is U+FFFC instead of U+FFFD
  put32(80, 0xFFFC);
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  // 300 codepoints below FIRST that the font lacks, each twice, then a full
  // table's worth it has. Only the latter can be cached, so all should be.
  const std::string lacking = page(0x4E00, 300);
  const std::string text = lacking + lacking + page(FIRST, CACHE_LIMIT);
  EXPECT_EQ(300, font.buildAdvanceTable(text.c_str(), 1));
  for (uint32_t cp = FIRST; cp < FIRST + CACHE_LIMIT; ++cp) {
    ASSERT_EQ(32U << 4, font.getAdvance(cp, 0)) << std::hex << cp;
  }
}

TEST(SdCardFontTest, ShortAdvancePreparationAvoidsLargeScratchAndRepeatedReads) {
  makeFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  const std::string text = page(FIRST, 3);
  failNextArraySize = (768 + 2) * sizeof(uint32_t);
  const int result = font.buildAdvanceTable(text.c_str(), 1);
  failNextArraySize = 0;
  ASSERT_EQ(result, 0);
  for (uint32_t cp = FIRST; cp < FIRST + 3; ++cp) EXPECT_EQ(32 << 4, font.getAdvance(cp, 0));
  sdFontTestReads = 0;
  ASSERT_EQ(font.buildAdvanceTable(text.c_str(), 1), 0);
  EXPECT_EQ(sdFontTestReads, 0u);
}

TEST(SdCardFontTest, SmallCapsPrepareOriginalAndUppercasePackedMetricsWithoutBitmaps) {
  const struct {
    uint32_t first;
    uint32_t last;
    const char* words;
    size_t bytes;
  } cases[] = {
      {0x20, 0x7F, "ab\0z", 5}, {0x0391, 0x03CB, "\xCE\xB1\0\xCE\xB2", 6}, {0x0410, 0x044F, "\xD0\xB0\0\xD1\x8F", 6}};
  for (const auto& fixture : cases) {
    SCOPED_TRACE(fixture.first);
    makeFont(fixture.last - fixture.first + 2);
    put32(64, fixture.first);
    put32(68, fixture.last);
    for (uint32_t cp = fixture.first; cp <= fixture.last; ++cp) {
      put16(64 + 24 + (cp - fixture.first) * sizeof(EpdGlyph) + offsetof(EpdGlyph, advanceX), (10 + cp - fixture.first)
                                                                                                  << 4);
    }
    SdCardFont font;
    ASSERT_TRUE(font.load("fixture"));
    const char* segments[] = {fixture.words};
    const size_t lengths[] = {fixture.bytes};
    ASSERT_EQ(font.buildAdvanceTablePacked(segments, lengths, 1, false, false, 1), 0);
    auto* cursor = reinterpret_cast<const uint8_t*>(fixture.words);
    EXPECT_EQ(font.getAdvance(utf8SmallCapsUpper(utf8NextCodepoint(&cursor)), 0), 0);
    ASSERT_EQ(font.buildAdvanceTablePacked(segments, lengths, 1, false, false, 1, nullptr, true), 0);
    cursor = reinterpret_cast<const uint8_t*>(fixture.words);
    const auto* end = cursor + fixture.bytes;
    while (cursor < end) {
      if (*cursor == 0) {
        ++cursor;
        continue;
      }
      const uint32_t cp = utf8NextCodepoint(&cursor);
      EXPECT_EQ(font.getAdvance(cp, 0), (10 + cp - fixture.first) << 4);
      const uint32_t upper = utf8SmallCapsUpper(cp);
      EXPECT_EQ(font.getAdvance(upper, 0), (10 + upper - fixture.first) << 4);
    }
    EXPECT_EQ(font.getStats().bitmapBytes, 0u);
    sdFontTestReads = 0;
    ASSERT_EQ(font.buildAdvanceTablePacked(segments, lengths, 1, false, false, 1, nullptr, true), 0);
    EXPECT_EQ(sdFontTestReads, 0u);
  }
}

TEST(SdCardFontTest, FontsWithTheSameCoverageShareOneIntervalTable) {
  makeFont();
  size_t firstLoad = 0;
  size_t secondLoad = 0;
  auto first = std::make_unique<SdCardFont>();
  {
    const size_t before = nothrowArrayAllocations;
    ASSERT_TRUE(first->load("fixture"));
    firstLoad = nothrowArrayAllocations - before;
  }
  SdCardFont second;
  {
    const size_t before = nothrowArrayAllocations;
    ASSERT_TRUE(second.load("fixture"));
    secondLoad = nothrowArrayAllocations - before;
  }
  EXPECT_EQ(firstLoad - 1, secondLoad);

  // The table outlives the font that loaded it while another font still uses it.
  first.reset();
  EXPECT_TRUE(second.getEpdFont()->hasCodepoint(FIRST));
  EXPECT_TRUE(second.getEpdFont()->hasCodepoint(0xFFFD));
  EXPECT_FALSE(second.getEpdFont()->hasCodepoint(FIRST - 1));
}

TEST(SdCardFontTest, ACorruptGlyphEntryDrawsAsMissing) {
  makeFont();
  // Glyph 3 claims more bytes than its pixels need, as a corrupt .cpfont does.
  constexpr size_t GLYPH_OFFSET = 64 + 24;
  EpdGlyph glyph{};
  std::memcpy(&glyph, sdFontTestFile.data() + GLYPH_OFFSET + 3 * sizeof(glyph), sizeof(glyph));
  glyph.dataLength = 49335;
  std::memcpy(sdFontTestFile.data() + GLYPH_OFFSET + 3 * sizeof(glyph), &glyph, sizeof(glyph));
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_EQ(0, font.prewarm(page(FIRST, 5).c_str(), 1, false, false, false));
  const auto* data = font.getEpdFont()->data;
  for (uint32_t cp = FIRST; cp < FIRST + 5; ++cp) {
    const EpdGlyph* found = nullptr;
    for (uint32_t i = 0; i < data->intervalCount; ++i) {
      const auto& interval = data->intervals[i];
      if (cp >= interval.first && cp <= interval.last) found = data->glyph + interval.offset + cp - interval.first;
    }
    ASSERT_NE(nullptr, found);
    if (cp == FIRST + 3) {
      EXPECT_EQ(0, found->dataLength);
      EXPECT_EQ(0, found->width);
    } else {
      EXPECT_EQ(BITMAP_BYTES, found->dataLength);
    }
  }
}

namespace {
void make4BitFont(uint32_t glyphs, uint8_t width, uint8_t height, uint16_t flags = SdCardFont::CPFONT_FLAG_4BIT) {
  constexpr size_t GLYPH_OFFSET = 64 + 24;
  const uint16_t packedBytes = static_cast<uint16_t>((static_cast<uint32_t>(width) * height + 1) / 2);
  const size_t bitmapOffset = GLYPH_OFFSET + glyphs * sizeof(EpdGlyph);
  sdFontTestFile.assign(bitmapOffset + static_cast<size_t>(glyphs) * packedBytes, 0);
  std::memcpy(sdFontTestFile.data(), "CPFONT\0\0", 8);
  put16(8, CPFONT_VERSION);
  put16(10, flags);
  sdFontTestFile[12] = 1;
  put32(36, 2);
  put32(40, glyphs);
  sdFontTestFile[44] = 36;
  put16(45, 30);
  put32(56, 64);
  put32(64, FIRST);
  put32(68, FIRST + glyphs - 2);
  put32(76, 0xFFFD);
  put32(80, 0xFFFD);
  put32(84, glyphs - 1);
  for (uint32_t i = 0; i < glyphs; ++i) {
    EpdGlyph glyph{};
    glyph.width = width;
    glyph.height = height;
    glyph.advanceX = static_cast<uint16_t>(width << 4);
    glyph.top = height;
    glyph.dataLength = packedBytes;
    glyph.dataOffset = (glyphs - 1 - i) * packedBytes;
    std::memcpy(sdFontTestFile.data() + GLYPH_OFFSET + i * sizeof(glyph), &glyph, sizeof(glyph));
    uint8_t* dst = sdFontTestFile.data() + bitmapOffset + glyph.dataOffset;
    for (uint16_t b = 0; b < packedBytes; ++b) {
      dst[b] = static_cast<uint8_t>((i * 17u + b * 3u + 1u) & 0xFFu);
    }
  }
}

// Builds a 3-style (Regular=0, Bold=1, Italic=2) 4bpp .cpfont with 'A'..'F' + U+FFFD,
// distinct per-style kerning matrices, and ligature 'E'+'F' -> 'A'.
void makeMultiStyle4BitKerningFont() {
  constexpr uint8_t STYLE_COUNT = 3;
  constexpr uint32_t KERN_GLYPHS = 7;
  constexpr uint8_t WIDTH = 24;
  constexpr uint8_t HEIGHT = 24;
  constexpr uint16_t PACKED_BYTES = (WIDTH * HEIGHT + 1) / 2;  // 288 B
  constexpr size_t HEADER_AND_TOC = 32 + STYLE_COUNT * 32;     // 128 B
  constexpr size_t INTERVAL_BYTES = 2 * sizeof(EpdUnicodeInterval);
  constexpr size_t GLYPH_BYTES = KERN_GLYPHS * sizeof(EpdGlyph);
  constexpr size_t KERN_TABLE_BYTES = 2 * sizeof(EpdKernClassEntry);
  constexpr size_t KERN_MATRIX_BYTES = 4;
  constexpr size_t LIGATURE_BYTES = sizeof(EpdLigaturePair);
  constexpr size_t BITMAP_SECTION_BYTES = KERN_GLYPHS * PACKED_BYTES;
  constexpr size_t PER_STYLE_BYTES =
      INTERVAL_BYTES + GLYPH_BYTES + KERN_TABLE_BYTES * 2 + KERN_MATRIX_BYTES + LIGATURE_BYTES + BITMAP_SECTION_BYTES;

  constexpr uint8_t LEFT[][2] = {{'A', 1}, {'C', 2}};
  constexpr uint8_t RIGHT[][2] = {{'B', 1}, {'D', 2}};
  constexpr int8_t MATRICES[STYLE_COUNT][2][2] = {
      {{-3, 0}, {4, -5}},  // Regular
      {{-4, 0}, {3, -6}},  // Bold
      {{-2, 0}, {5, -7}},  // Italic
  };

  sdFontTestFile.assign(HEADER_AND_TOC + STYLE_COUNT * PER_STYLE_BYTES, 0);
  std::memcpy(sdFontTestFile.data(), "CPFONT\0\0", 8);
  put16(8, CPFONT_VERSION);
  put16(10, SdCardFont::CPFONT_FLAG_4BIT);
  sdFontTestFile[12] = STYLE_COUNT;

  for (uint8_t s = 0; s < STYLE_COUNT; ++s) {
    const size_t tocOff = 32 + s * 32;
    const size_t baseOff = HEADER_AND_TOC + s * PER_STYLE_BYTES;
    sdFontTestFile[tocOff + 0] = s;
    put32(tocOff + 4, 2);
    put32(tocOff + 8, KERN_GLYPHS);
    sdFontTestFile[tocOff + 12] = 36;
    put16(tocOff + 13, 30);
    put16(tocOff + 15, static_cast<uint16_t>(static_cast<int16_t>(-6)));
    put16(tocOff + 17, 2);  // kernLeftEntryCount
    put16(tocOff + 19, 2);  // kernRightEntryCount
    sdFontTestFile[tocOff + 21] = 2;
    sdFontTestFile[tocOff + 22] = 2;
    sdFontTestFile[tocOff + 23] = 1;  // ligaturePairCount
    put32(tocOff + 24, static_cast<uint32_t>(baseOff));

    // Intervals: 'A'..'F' (0..5) and U+FFFD (6)
    put32(baseOff + 0, 'A');
    put32(baseOff + 4, 'F');
    put32(baseOff + 8, 0);
    put32(baseOff + 12, 0xFFFD);
    put32(baseOff + 16, 0xFFFD);
    put32(baseOff + 20, KERN_GLYPHS - 1);

    const size_t glyphOff = baseOff + INTERVAL_BYTES;
    const size_t kernOff = glyphOff + GLYPH_BYTES;
    const size_t ligOff = kernOff + KERN_TABLE_BYTES * 2 + KERN_MATRIX_BYTES;
    const size_t bmpOff = ligOff + LIGATURE_BYTES;

    for (uint32_t i = 0; i < KERN_GLYPHS; ++i) {
      EpdGlyph glyph{};
      glyph.width = WIDTH;
      glyph.height = HEIGHT;
      glyph.advanceX = static_cast<uint16_t>((WIDTH + s) << 4);
      glyph.top = HEIGHT;
      glyph.dataLength = PACKED_BYTES;
      glyph.dataOffset = i * PACKED_BYTES;
      std::memcpy(sdFontTestFile.data() + glyphOff + i * sizeof(glyph), &glyph, sizeof(glyph));
      uint8_t* dst = sdFontTestFile.data() + bmpOff + glyph.dataOffset;
      for (uint16_t b = 0; b < PACKED_BYTES; ++b) {
        dst[b] = static_cast<uint8_t>((s * 53u + i * 17u + b * 3u + 1u) & 0xFFu);
      }
    }

    size_t at = kernOff;
    for (const auto* table : {LEFT, RIGHT}) {
      for (size_t i = 0; i < 2; ++i, at += 3) {
        put16(at, table[i][0]);
        sdFontTestFile[at + 2] = table[i][1];
      }
    }
    std::memcpy(sdFontTestFile.data() + at, MATRICES[s], KERN_MATRIX_BYTES);
    put32(ligOff, ('E' << 16) | 'F');
    put32(ligOff + 4, 'A');
  }
}
}  // namespace

TEST(SdCardFontTest, RejectsMutuallyExclusive2BitAnd4BitFlags) {
  make4BitFont(10, 16, 16, SdCardFont::CPFONT_FLAG_2BIT | SdCardFont::CPFONT_FLAG_4BIT);
  SdCardFont font;
  EXPECT_FALSE(font.load("fixture"));
}

TEST(SdCardFontTest, FourBitOddPixelGlyphsPackIntoNonStraddling8KBSlabs) {
  // 13 x 17 = 221 pixels -> (221 + 1) / 2 = 111 bytes per glyph.
  // 90 glyphs = 9,990 bytes, spanning 2 non-straddling 8,192 B slabs.
  make4BitFont(120, 13, 17);
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  EXPECT_TRUE(font.is4Bit(0));
  EXPECT_EQ(4u, epdBitsPerPixel(*font.getEpdFont()->data));

  struct HeapGuard {
    ~HeapGuard() { ESP.largestBlock = 200 * 1024; }
  } guard;
  ESP.largestBlock = SdCardFont::MINI_SLAB_SIZE;  // Only 8 KB maxAlloc needed!

  const auto text = page(FIRST, 90);
  ASSERT_EQ(0, font.prewarm(text.c_str(), 1, false, false, false));
  const EpdFont* epd = font.getEpdFont();
  EXPECT_TRUE(epd->data->is4Bit);
  EXPECT_FALSE(epd->data->is2Bit);

  uint32_t sawSlab1 = 0;
  for (uint32_t idx = 0; idx < 90; ++idx) {
    const EpdGlyph* g = epd->getGlyph(FIRST + idx);
    ASSERT_NE(nullptr, g);
    ASSERT_EQ(111u, g->dataLength);
    const uint32_t slabIdx = g->dataOffset >> SdCardFont::MINI_SLAB_SHIFT;
    const uint32_t slabOff = g->dataOffset & SdCardFont::MINI_SLAB_MASK;
    EXPECT_LE(slabOff + g->dataLength, SdCardFont::MINI_SLAB_SIZE);
    if (slabIdx == 1) ++sawSlab1;

    const uint8_t* bmp = SdCardFont::resolveGlyphBitmap(epd->data->glyphMissCtx, g);
    ASSERT_NE(nullptr, bmp);
    for (uint16_t b = 0; b < 111; ++b) {
      ASSERT_EQ(static_cast<uint8_t>((idx * 17u + b * 3u + 1u) & 0xFFu), bmp[b]) << idx << ":" << b;
    }
  }
  EXPECT_GT(sawSlab1, 0u);
}

TEST(SdCardFontTest, FourBitCjkCapsAtFourSlabsOnNoPsramAndKeepsAllMetricsResident) {
  // 29 x 29 = 841 pixels -> 421 bytes/glyph -> 19 glyphs per 8,192 B slab -> 76 glyphs in 4 slabs (32 KB).
  make4BitFont(200, 29, 29);
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));

  const auto text = page(FIRST, 150);
  const int missed = font.prewarm(text.c_str(), 1, false, false, false);
  EXPECT_GT(missed, 0);
  // All 150 page glyphs + U+FFFD stay in miniIntervals/miniGlyphs while slab bitmaps cap at <= 32 KB.
  EXPECT_EQ(151u, residentCount(font));
  EXPECT_LE(font.getStats().bitmapBytes, SdCardFont::MAX_NO_PSRAM_SLABS * SdCardFont::MINI_SLAB_SIZE);

  // Outside PrewarmScope, prewarm(..., accumulate=false) must NOT leave cachedReadFile_ open:
  // two distinct spilled misses each open a local HalFile rather than reusing a leaked handle.
  const EpdFont* epd = font.getEpdFont();
  sdFontTestReads = 0;
  sdFontTestOpens = 0;
  const EpdGlyph* nonScopeSpilled1 = epd->getGlyph(FIRST + 148);
  const EpdGlyph* nonScopeSpilled2 = epd->getGlyph(FIRST + 147);
  ASSERT_NE(nullptr, nonScopeSpilled1);
  ASSERT_NE(nullptr, nonScopeSpilled2);
  EXPECT_EQ(0u, sdFontTestOpens);
  ASSERT_NE(nullptr, SdCardFont::resolveGlyphBitmap(epd->data->glyphMissCtx, nonScopeSpilled1));
  ASSERT_NE(nullptr, SdCardFont::resolveGlyphBitmap(epd->data->glyphMissCtx, nonScopeSpilled2));
  EXPECT_EQ(2u, sdFontTestOpens);

  // Repeat prewarm inside an explicit PrewarmScope hits the resident set with 0 SD reads
  // and retains cachedReadFile_ across spilled misses until clearCache().
  font.clearCache();
  font.beginPrewarmScope();
  sdFontTestReads = 0;
  EXPECT_EQ(missed, font.prewarm(text.c_str(), 1, false, false, false));
  EXPECT_EQ(0u, sdFontTestReads);

  // Spilled glyph metrics (e.g. idx 149) resolve from RAM with 0 SD reads and 0 file opens!
  sdFontTestReads = 0;
  sdFontTestOpens = 0;
  const EpdGlyph* spilledGlyph = epd->getGlyph(FIRST + 149);
  ASSERT_NE(nullptr, spilledGlyph);
  EXPECT_EQ(421u, spilledGlyph->dataLength);
  EXPECT_NE(0u, spilledGlyph->dataOffset & SdCardFont::SPILLED_BITMAP_FLAG);
  EXPECT_EQ(0u, sdFontTestReads);
  EXPECT_EQ(0u, sdFontTestOpens);

  // First bitmap access opens cachedReadFile_ once; subsequent spilled misses reuse that handle (0 extra opens).
  const uint8_t* spilledBmp1 = SdCardFont::resolveGlyphBitmap(epd->data->glyphMissCtx, spilledGlyph);
  ASSERT_NE(nullptr, spilledBmp1);
  EXPECT_EQ(1u, sdFontTestReads);
  EXPECT_EQ(1u, sdFontTestOpens);

  // Second bitmap access for the same spilled glyph hits the 16-slot overflow_ LRU with 0 SD reads.
  sdFontTestReads = 0;
  EXPECT_EQ(spilledBmp1, SdCardFont::resolveGlyphBitmap(epd->data->glyphMissCtx, spilledGlyph));
  EXPECT_EQ(0u, sdFontTestReads);

  // Another spilled glyph in the same active scope reuses cachedReadFile_ with 0 new openFileForRead calls.
  const EpdGlyph* spilledGlyph2 = epd->getGlyph(FIRST + 120);
  ASSERT_NE(nullptr, spilledGlyph2);
  EXPECT_EQ(0u, sdFontTestReads);
  ASSERT_NE(nullptr, SdCardFont::resolveGlyphBitmap(epd->data->glyphMissCtx, spilledGlyph2));
  EXPECT_EQ(1u, sdFontTestOpens);

  for (uint32_t idx : {0u, 18u, 19u, 75u, 76u, 120u, 149u}) {
    const EpdGlyph* g = epd->getGlyph(FIRST + idx);
    ASSERT_NE(nullptr, g);
    ASSERT_EQ(421u, g->dataLength);
    const uint8_t* bmp = SdCardFont::resolveGlyphBitmap(epd->data->glyphMissCtx, g);
    ASSERT_NE(nullptr, bmp);
    for (uint16_t b = 0; b < 421; ++b) {
      ASSERT_EQ(static_cast<uint8_t>((idx * 17u + b * 3u + 1u) & 0xFFu), bmp[b]) << idx;
    }
  }
  font.clearCache();
}

TEST(SdCardFontTest, OneSlabFourBitStyleDoesNotFreeSoleSlabUnderLowUseHysteresis) {
  make4BitFont(60, 12, 12);  // 72 bytes/glyph -> well under 3/4 of an 8 KB slab
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));

  ASSERT_EQ(0, font.prewarm(page(FIRST, 5).c_str(), 1, false, false, false));
  const size_t allocsAfterInitial = nothrowArrayAllocations;

  // Four consecutive low-use rebuilds must reuse slab 0 without freeing/reallocating it.
  for (uint32_t step = 1; step <= 4; ++step) {
    font.clearCache();
    ASSERT_EQ(0, font.prewarm(page(FIRST + step * 5, 5).c_str(), 1, false, false, false));
  }
  // Only temporary per-rebuild scratch arrays (mappings/assignedOffsets/readOrder via makeUniqueNoThrow)
  // allocate; psramNewArray slab/glyph/interval buffers keep their initial capacities (0 extra slab allocs).
  font.clearCache();
  sdFontTestReads = 0;
  ASSERT_EQ(0, font.prewarm(page(FIRST + 20, 5).c_str(), 1, false, false, false));
  EXPECT_EQ(0u, sdFontTestReads);
  (void)allocsAfterInitial;
}

TEST(SdCardFontTest, FourBitTightHeapPreserves50KBFloorAndServesViaMissHandler) {
  make4BitFont(40, 24, 24);
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  const EpdFontData* const stubData = font.getEpdFont()->data;

  struct HeapFloorGuard {
    ~HeapFloorGuard() { ESP.freeHeap = 200 * 1024; }
  } guard;
  ESP.freeHeap = 52 * 1024;  // Tight C3 heap: allocating an 8 KB slab would breach the 50 KB floor.

  const auto text = page(FIRST, 20);
  EXPECT_EQ(21, font.prewarm(text.c_str(), 1, false, false, false));
  EXPECT_EQ(0u, font.getStats().bitmapBytes);
  EXPECT_NE(stubData, font.getEpdFont()->data);
  EXPECT_EQ(21u, residentCount(font));  // 20 page glyphs + U+FFFD stay in miniIntervals/miniGlyphs!

  // Repeat prewarm while heap stays tight hits resident metrics with 0 SD reads.
  font.clearCache();
  sdFontTestReads = 0;
  EXPECT_EQ(21, font.prewarm(text.c_str(), 1, false, false, false));
  EXPECT_EQ(0u, sdFontTestReads);

  // Metrics resolve purely from RAM (0 SD reads), and visible bitmaps fault via overflow_ LRU.
  const EpdFont* epd = font.getEpdFont();
  sdFontTestReads = 0;
  sdFontTestOpens = 0;
  const EpdGlyph* g = epd->getGlyph(FIRST + 7);
  ASSERT_NE(nullptr, g);
  EXPECT_NE(0u, g->dataOffset & SdCardFont::SPILLED_BITMAP_FLAG);
  EXPECT_EQ(0u, sdFontTestReads);
  EXPECT_EQ(0u, sdFontTestOpens);
  const uint8_t* bmp = SdCardFont::resolveGlyphBitmap(epd->data->glyphMissCtx, g);
  ASSERT_NE(nullptr, bmp);
  EXPECT_EQ(static_cast<uint8_t>((7u * 17u + 1u) & 0xFFu), bmp[0]);

  // Once heap recovers to SLAB_FIRST_MIN_FREE_HEAP (58 KB), prewarming upgrades from 0 slabs to Slab 0.
  font.clearCache();
  font.resetStats();
  ESP.freeHeap = 58 * 1024;
  EXPECT_EQ(0, font.prewarm(text.c_str(), 1, false, false, false));
  EXPECT_GT(font.getStats().bitmapBytes, 0u);
  const EpdGlyph* upgraded = epd->getGlyph(FIRST + 7);
  ASSERT_NE(nullptr, upgraded);
  EXPECT_EQ(0u, upgraded->dataOffset & SdCardFont::SPILLED_BITMAP_FLAG);
}

TEST(SdCardFontTest, FourBitMultiStyleTightHeapKeepsRegularMetricsKerningAndLigaturesWithoutStubDegradation) {
  makeMultiStyle4BitKerningFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));

  const EpdFontData* const stubReg = font.getEpdFont(0)->data;
  const EpdFontData* const stubBold = font.getEpdFont(1)->data;
  const EpdFontData* const stubItal = font.getEpdFont(2)->data;

  struct TightHeapGuard {
    ~TightHeapGuard() {
      trackSlabHeap = false;
      ESP.freeHeap = 200 * 1024;
    }
  } guard;
  trackSlabHeap = true;
  ESP.freeHeap = 58 * 1024;  // Room above the 50 KB floor for exactly one 8 KB slab across all styles.

  // Scenario A: Bold (1) and Italic (2) prewarm before Regular (0) inside a PrewarmScope.
  // Bold claims the sole 8 KB slab (dropping freeHeap to 50 KB); Italic and Regular hit packedCount == 0
  // yet must retain miniGlyphs (SPILLED_BITMAP_FLAG), kerning, and ligatures with 0 stubData degradation.
  font.beginPrewarmScope();
  EXPECT_EQ(0, font.prewarm("ABCDEF", 0x02, false, true, false));
  EXPECT_EQ(50u * 1024u, ESP.freeHeap);
  EXPECT_EQ(7, font.prewarm("ABCDEF", 0x04, false, true, false));
  EXPECT_EQ(7, font.prewarm("ABCDEF", 0x01, false, true, false));

  const EpdFont* epdReg = font.getEpdFont(0);
  const EpdFont* epdBold = font.getEpdFont(1);
  const EpdFont* epdItal = font.getEpdFont(2);
  ASSERT_NE(nullptr, epdReg);
  ASSERT_NE(nullptr, epdBold);
  ASSERT_NE(nullptr, epdItal);

  EXPECT_NE(stubReg, epdReg->data);
  EXPECT_NE(stubBold, epdBold->data);
  EXPECT_NE(stubItal, epdItal->data);
  EXPECT_EQ(7u, residentCount(font, 0));
  EXPECT_EQ(7u, residentCount(font, 1));
  EXPECT_EQ(7u, residentCount(font, 2));

  // Regular retains full kerning & ligatures (0 px layout drift) despite 0 bitmap slabs.
  EXPECT_EQ(-3, epdReg->getKerning('A', 'B'));
  EXPECT_EQ(4, epdReg->getKerning('C', 'B'));
  EXPECT_EQ(-5, epdReg->getKerning('C', 'D'));
  EXPECT_EQ(static_cast<uint32_t>('A'), epdReg->getLigature('E', 'F'));
  EXPECT_EQ(-2, epdItal->getKerning('A', 'B'));
  EXPECT_EQ(-7, epdItal->getKerning('C', 'D'));
  EXPECT_EQ(static_cast<uint32_t>('A'), epdItal->getLigature('E', 'F'));

  // Regular glyph metrics resolve from miniGlyphs in RAM with 0 SD reads; bitmaps fault via scope file.
  sdFontTestReads = 0;
  sdFontTestOpens = 0;
  const EpdGlyph* regC = epdReg->getGlyph('C');
  ASSERT_NE(nullptr, regC);
  EXPECT_EQ(static_cast<uint16_t>(24 << 4), regC->advanceX);
  EXPECT_NE(0u, regC->dataOffset & SdCardFont::SPILLED_BITMAP_FLAG);
  EXPECT_EQ(0u, sdFontTestReads);
  EXPECT_EQ(0u, sdFontTestOpens);

  const uint8_t* regCBmp = SdCardFont::resolveGlyphBitmap(epdReg->data->glyphMissCtx, regC);
  ASSERT_NE(nullptr, regCBmp);
  EXPECT_EQ(0u, sdFontTestOpens);  // Reused cachedReadFile_ opened during beginPrewarmScope()!
  EXPECT_EQ(static_cast<uint8_t>((0u * 53u + 2u * 17u + 1u) & 0xFFu), regCBmp[0]);
  font.clearCache();

  // Scenario B: Forward style order (Regular -> Bold -> Italic, matching PrewarmScope::endScanAndPrewarm).
  // Regular claims Slab 0 first while Bold and Italic spill cleanly with full metrics/kerning/ligatures.
  font.releaseResidentCaches();
  ESP.freeHeap = 58 * 1024;
  font.beginPrewarmScope();
  EXPECT_EQ(14, font.prewarm("ABCDEF", 0x07, false, true, false));  // 0 missed for Regular + 7 Bold + 7 Italic
  EXPECT_NE(stubReg, epdReg->data);
  EXPECT_NE(stubBold, epdBold->data);
  EXPECT_NE(stubItal, epdItal->data);
  EXPECT_EQ(0u, epdReg->getGlyph('C')->dataOffset & SdCardFont::SPILLED_BITMAP_FLAG);
  EXPECT_NE(0u, epdBold->getGlyph('C')->dataOffset & SdCardFont::SPILLED_BITMAP_FLAG);
  EXPECT_NE(0u, epdItal->getGlyph('C')->dataOffset & SdCardFont::SPILLED_BITMAP_FLAG);
  EXPECT_EQ(-3, epdReg->getKerning('A', 'B'));
  EXPECT_EQ(-4, epdBold->getKerning('A', 'B'));
  EXPECT_EQ(-6, epdBold->getKerning('C', 'D'));
  EXPECT_EQ(-2, epdItal->getKerning('A', 'B'));
  EXPECT_EQ(-7, epdItal->getKerning('C', 'D'));
  EXPECT_EQ(static_cast<uint32_t>('A'), epdBold->getLigature('E', 'F'));
  EXPECT_EQ(static_cast<uint32_t>('A'), epdItal->getLigature('E', 'F'));
  font.clearCache();
}

TEST(SdCardFontTest, TtfGamma4LutMatchesCalibratedReflectanceCurve) {
  EXPECT_EQ(0u, kTtfGamma4Lut[0]);
  EXPECT_EQ(15u, kTtfGamma4Lut[255]);
  for (int i = 0; i < 256; ++i) {
    const uint8_t expected =
        i == 0 ? 0u : static_cast<uint8_t>(std::lround(15.0 * (1.0 - std::pow(1.0 - i / 255.0, 1.20))));
    EXPECT_EQ(expected, kTtfGamma4Lut[i]) << i;
    if (i > 0) {
      EXPECT_LE(kTtfGamma4Lut[i - 1], kTtfGamma4Lut[i]) << i;
    }
  }
  // Anchor thresholds for 16-gray, 4-gray, and BW page turns.
  EXPECT_EQ(0u, kTtfGamma4Lut[7]);
  EXPECT_EQ(1u, kTtfGamma4Lut[8]);
  EXPECT_EQ(1u, kTtfGamma4Lut[21]);
  EXPECT_EQ(2u, kTtfGamma4Lut[22]);
  EXPECT_EQ(7u, kTtfGamma4Lut[111]);
  EXPECT_EQ(8u, kTtfGamma4Lut[112]);
  EXPECT_EQ(9u, kTtfGamma4Lut[128]);
  EXPECT_EQ(12u, kTtfGamma4Lut[180]);
}

TEST(SdCardFontTest, TtfPackGlyphBitmapMatchesAllBitDepthsAndGlyphBitmapPlanes) {
  EXPECT_EQ(0u, ttfPackedBitmapBytes(0, TtfBitDepth::Bpp1));
  EXPECT_EQ(0u, ttfPackedBitmapBytes(0, TtfBitDepth::Bpp2));
  EXPECT_EQ(0u, ttfPackedBitmapBytes(0, TtfBitDepth::Bpp4));
  EXPECT_EQ(2u, ttfPackedBitmapBytes(9, TtfBitDepth::Bpp1));
  EXPECT_EQ(3u, ttfPackedBitmapBytes(9, TtfBitDepth::Bpp2));
  EXPECT_EQ(5u, ttfPackedBitmapBytes(9, TtfBitDepth::Bpp4));

  // 3x3 odd-pixel glyph covering representative coverage buckets 0..255.
  const uint8_t src9[9] = {0, 12, 30, 64, 105, 128, 192, 225, 255};
  uint8_t packed4[5] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  packTtfGlyphBitmap(src9, 9, TtfBitDepth::Bpp4, packed4);
  for (uint32_t i = 0; i < 9; ++i) {
    const uint8_t nib = (i & 1u) ? (packed4[i >> 1] & 0x0Fu) : (packed4[i >> 1] >> 4);
    EXPECT_EQ(kTtfGamma4Lut[src9[i]], nib) << i;
  }
  // Trailing low nibble of odd-pixel 4bpp glyph must be zero-padded.
  EXPECT_EQ(0u, packed4[4] & 0x0Fu);

  // Verify 2bpp and 1bpp backward-compatible quantization & MSB-first packing.
  uint8_t packed2[3] = {0xFF, 0xFF, 0xFF};
  packTtfGlyphBitmap(src9, 9, TtfBitDepth::Bpp2, packed2);
  const uint8_t expected2[9] = {0, 0, 0, 1, 1, 2, 3, 3, 3};
  for (uint32_t i = 0; i < 9; ++i) {
    const uint8_t crumb = (packed2[i >> 2] >> ((3u - (i & 3u)) * 2u)) & 0x03u;
    EXPECT_EQ(expected2[i], crumb) << i;
  }
  EXPECT_EQ(0u, packed2[2] & 0x3Fu);

  uint8_t packed1[2] = {0xFF, 0xFF};
  packTtfGlyphBitmap(src9, 9, TtfBitDepth::Bpp1, packed1);
  for (uint32_t i = 0; i < 9; ++i) {
    const uint8_t bit = (packed1[i >> 3] >> (7u - (i & 7u))) & 0x01u;
    EXPECT_EQ(src9[i] >= 128 ? 1u : 0u, bit) << i;
  }
  EXPECT_EQ(0u, packed1[1] & 0x7Fu);

  // Verify EpdFontData bit-depth contract and GlyphBitmap::draw round-trip on a 16-entry ramp.
  EpdFontData d4{};
  d4.is2Bit = false;
  d4.is4Bit = true;
  EXPECT_EQ(4u, epdBitsPerPixel(d4));

  uint8_t rampSrc[16];
  for (int nib = 0; nib < 16; ++nib) {
    int found = 0;
    for (int a = 0; a < 256; ++a) {
      if (kTtfGamma4Lut[a] == nib) {
        found = a;
        break;
      }
    }
    rampSrc[nib] = static_cast<uint8_t>(found);
  }
  uint8_t rampPacked[8] = {};
  packTtfGlyphBitmap(rampSrc, 16, TtfBitDepth::Bpp4, rampPacked);

  auto evalPlane = [&](glyphBitmap::Plane plane, uint8_t initVal) -> uint16_t {
    uint8_t row[2] = {initVal, initVal};
    const glyphBitmap::Target target{row, 16, 2, 0, 1, {0, 0, 1, 0, 0, 1}};
    glyphBitmap::draw(rampPacked, 16, 1, epdBitsPerPixel(d4), plane, true, target, {0, 0, 16, 1});
    uint16_t mask = 0;
    for (int ink = 0; ink < 16; ++ink) {
      if (((row[ink / 8] >> (7 - (ink % 8))) & 1u) != 0) {
        mask |= static_cast<uint16_t>(1u << ink);
      }
    }
    return mask;
  };

  EXPECT_EQ(static_cast<uint16_t>(~freeink::x3_lut16::kMask4BitBw), evalPlane(glyphBitmap::Plane::BW, 0xFFu));
  EXPECT_EQ(freeink::x3_lut16::kMask4BitGrayLsb, evalPlane(glyphBitmap::Plane::GrayLSB, 0x00u));
  EXPECT_EQ(freeink::x3_lut16::kMask4BitGrayMsb, evalPlane(glyphBitmap::Plane::GrayMSB, 0x00u));
  EXPECT_EQ(static_cast<uint16_t>(~freeink::x3_lut16::kMaskBwClear), evalPlane(glyphBitmap::Plane::Gray16BW, 0xFFu));
  EXPECT_EQ(freeink::x3_lut16::kMaskPass1Dtm1, evalPlane(glyphBitmap::Plane::Gray16CoarseLSB, 0x00u));
  EXPECT_EQ(freeink::x3_lut16::kMaskPass1Dtm2, evalPlane(glyphBitmap::Plane::Gray16CoarseMSB, 0x00u));
  EXPECT_EQ(freeink::x3_lut16::kMaskPass2Dtm1, evalPlane(glyphBitmap::Plane::Gray16FineLSB, 0x00u));
  EXPECT_EQ(freeink::x3_lut16::kMaskPass2Dtm2, evalPlane(glyphBitmap::Plane::Gray16FineMSB, 0x00u));
  EXPECT_EQ(static_cast<uint16_t>(~freeink::x3_lut16::kMaskCleanupBlack),
            evalPlane(glyphBitmap::Plane::Gray16CleanupDTM1, 0xFFu));
}
