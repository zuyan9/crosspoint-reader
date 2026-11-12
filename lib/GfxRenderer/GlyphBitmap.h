#pragma once

#include <GrayscaleCapabilities.h>

#include <algorithm>
#include <cstdint>

// Glyph rasterizer that resolves orientation, clipping and framebuffer
// addressing once per glyph instead of once per pixel. It has no dependency
// on GfxRenderer so it can be unit-tested on the host.
namespace glyphBitmap {

// Framebuffer plane a glyph is painted into.
enum class Plane : uint8_t {
  BW,
  GrayLSB,
  GrayMSB,
  Gray16BW,
  Gray16CoarseLSB,
  Gray16CoarseMSB,
  Gray16FineLSB,
  Gray16FineMSB,
  Gray16CleanupDTM1,
};

// Position of glyph pixel (0, 0) plus the step for one move along the glyph's
// x and y axes, all in one coordinate space. The axes must describe an
// orthogonal unit rotation: each is +/-1 on exactly one component.
struct Frame {
  int x;
  int y;
  int dxX;
  int dxY;
  int dyX;
  int dyY;
};

// Where a glyph lands in physical framebuffer space.
struct Target {
  uint8_t* buffer;  // First byte of row originY (framebuffer or strip scratch)
  int width;        // Physical panel width in pixels
  int stride;       // Bytes per physical row
  int originY;      // First physical row held by buffer
  int rows;         // Number of physical rows held by buffer
  Frame frame;      // Physical placement of the glyph
};

// Half-open rectangle in glyph-local pixel coordinates.
struct Clip {
  int left;
  int top;
  int right;
  int bottom;
};

// Narrow [start, end) on one glyph axis so that base + i * step stays within
// [lower, upper). step is +1 or -1 because the transform is orthogonal.
inline void clipAxis(int base, int step, int lower, int upper, int& start, int& end) {
  if (step > 0) {
    start = std::max(start, lower - base);
    end = std::min(end, upper - base);
  } else {
    start = std::max(start, base - upper + 1);
    end = std::min(end, base - lower + 1);
  }
}

// Narrow a glyph-local clip so every kept pixel maps inside the half-open
// rectangle [left, right) x [top, bottom) of the space frame is expressed in.
// Which glyph axis runs along that space's x depends on the rotation:
// dxX != 0 means glyph x does. Runs twice per glyph, so it stays inline
// under -Os.
__attribute__((always_inline)) inline void clipToRect(const Frame& frame, int left, int top, int right, int bottom,
                                                      Clip& clip) {
  if (frame.dxX != 0) {
    clipAxis(frame.x, frame.dxX, left, right, clip.left, clip.right);
    clipAxis(frame.y, frame.dyY, top, bottom, clip.top, clip.bottom);
  } else {
    clipAxis(frame.x, frame.dyX, left, right, clip.top, clip.bottom);
    clipAxis(frame.y, frame.dxY, top, bottom, clip.left, clip.right);
  }
}

// Keep pixel writes inline when decoding packed pixels.
__attribute__((always_inline)) inline void paint(uint8_t* buffer, int destination, uint8_t ink, uint16_t setMask,
                                                 uint16_t clearMask) {
  const uint16_t bit = static_cast<uint16_t>(1u << ink);
  if (((setMask | clearMask) & bit) == 0) return;
  const uint8_t mask = static_cast<uint8_t>(0x80u >> (destination & 7));
  if (clearMask & bit)
    buffer[destination >> 3] &= static_cast<uint8_t>(~mask);
  else
    buffer[destination >> 3] |= mask;
}

// Paint a packed 1bpp, 2bpp, or 4bpp glyph into target.
//
// bitmap: rows are contiguous, MSB first, 1, 2, or 4 bits per pixel; widths
//   need not be byte-aligned. 4bpp values are 0=white .. 15=black; 2bpp values
//   are 0=white, 1=light gray, 2=dark gray, 3=black; 1bpp value 1 is ink.
// Clipping happens before pixel decoding, so fully hidden glyphs cost nothing.
inline void draw(const uint8_t* bitmap, int width, int height, uint8_t bitsPerPixel, Plane plane, bool state,
                 const Target& target, Clip clip) {
  using namespace freeink::gray16;
  uint16_t setMask = 0;
  uint16_t clearMask = 0;
  const auto applyStatePolarity = [&](uint16_t levels) {
    if (state)
      clearMask = levels;
    else
      setMask = levels;
  };
  // Solid black stems (4bpp ink=15, 2bpp ink=3, 1bpp ink=1 with state=true) clear prior bleach bits.
  const uint16_t solidBlackBit = state ? static_cast<uint16_t>(1u << ((1u << bitsPerPixel) - 1u)) : 0u;

  switch (plane) {
    case Plane::BW:
      applyStatePolarity(bitsPerPixel == 4 ? kMask4BitBw : (bitsPerPixel == 2 ? 0x000Eu : 0x0002u));
      break;
    case Plane::GrayLSB: {
      const uint16_t levels = bitsPerPixel == 4 ? kMask4BitGrayLsb : (bitsPerPixel == 2 ? 0x0004u : 0x0002u);
      if (bitsPerPixel > 1)
        setMask = levels;
      else
        applyStatePolarity(levels);
      break;
    }
    case Plane::GrayMSB: {
      const uint16_t levels = bitsPerPixel == 4 ? kMask4BitGrayMsb : (bitsPerPixel == 2 ? 0x0006u : 0x0002u);
      if (bitsPerPixel > 1)
        setMask = levels;
      else
        applyStatePolarity(levels);
      break;
    }
    case Plane::Gray16BW:
      applyStatePolarity(bitsPerPixel == 4 ? kMaskBwClear
                                           : (bitsPerPixel == 2 ? mask2BitFrom4Bit(kMaskBwClear) : 0x0002u));
      break;
    case Plane::Gray16CoarseLSB:
      setMask = bitsPerPixel == 4 ? kMaskPass1Dtm1 : (bitsPerPixel == 2 ? mask2BitFrom4Bit(kMaskPass1Dtm1) : 0u);
      clearMask = solidBlackBit;
      break;
    case Plane::Gray16CoarseMSB:
      setMask = bitsPerPixel == 4 ? kMaskPass1Dtm2 : (bitsPerPixel == 2 ? mask2BitFrom4Bit(kMaskPass1Dtm2) : 0u);
      clearMask = solidBlackBit;
      break;
    case Plane::Gray16FineLSB:
      setMask = bitsPerPixel == 4 ? kMaskPass2Dtm1 : (bitsPerPixel == 2 ? mask2BitFrom4Bit(kMaskPass2Dtm1) : 0u);
      clearMask = solidBlackBit;
      break;
    case Plane::Gray16FineMSB:
      setMask = bitsPerPixel == 4 ? kMaskPass2Dtm2 : (bitsPerPixel == 2 ? mask2BitFrom4Bit(kMaskPass2Dtm2) : 0u);
      clearMask = solidBlackBit;
      break;
    case Plane::Gray16CleanupDTM1:
      applyStatePolarity(bitsPerPixel == 4 ? kMaskCleanupBlack
                                           : (bitsPerPixel == 2 ? mask2BitFrom4Bit(kMaskCleanupBlack) : 0x0002u));
      break;
  }
  if ((setMask | clearMask) == 0) return;

  // Intersect with the glyph bounds, then with the panel width and the
  // buffered row band.
  clip.left = std::max(clip.left, 0);
  clip.top = std::max(clip.top, 0);
  clip.right = std::min(clip.right, width);
  clip.bottom = std::min(clip.bottom, height);
  clipToRect(target.frame, 0, target.originY, target.width, target.originY + target.rows, clip);
  if (clip.left >= clip.right || clip.top >= clip.bottom) return;

  // Walk the framebuffer as a flat bit index. One glyph column advances
  // stepX bits and one glyph row advances stepY bits; each is +/-1 for the
  // axis that maps to physical x, or +/-strideBits for physical y.
  const Frame& frame = target.frame;
  const int strideBits = target.stride * 8;
  const int stepX = frame.dxY * strideBits + frame.dxX;
  const int stepY = frame.dyY * strideBits + frame.dyX;
  int rowBit = (frame.y - target.originY) * strideBits + frame.x + clip.left * stepX + clip.top * stepY;
  for (int y = clip.top; y < clip.bottom; ++y, rowBit += stepY) {
    int source = y * width + clip.left;
    int destination = rowBit;
    int remaining = clip.right - clip.left;
    if (bitsPerPixel == 4) {
      if (remaining && (source & 1)) {
        paint(target.buffer, destination, bitmap[source >> 1] & 0x0Fu, setMask, clearMask);
        ++source;
        destination += stepX;
        --remaining;
      }
      while (remaining >= 2) {
        const uint8_t packed = bitmap[source >> 1];
        paint(target.buffer, destination, packed >> 4, setMask, clearMask);
        paint(target.buffer, destination + stepX, packed & 0x0Fu, setMask, clearMask);
        source += 2;
        destination += 2 * stepX;
        remaining -= 2;
      }
      if (remaining) {
        paint(target.buffer, destination, bitmap[source >> 1] >> 4, setMask, clearMask);
      }
      continue;
    }
    if (bitsPerPixel == 2) {
      // Rows are bit-contiguous, not byte-padded. Align after any clipped prefix.
      while (remaining && (source & 3)) {
        paint(target.buffer, destination, (bitmap[source >> 2] >> (6 - (source & 3) * 2)) & 3, setMask, clearMask);
        ++source;
        destination += stepX;
        --remaining;
      }
      while (remaining >= 4) {
        const uint8_t packed = bitmap[source >> 2];
        paint(target.buffer, destination, packed >> 6, setMask, clearMask);
        paint(target.buffer, destination + stepX, (packed >> 4) & 3, setMask, clearMask);
        paint(target.buffer, destination + 2 * stepX, (packed >> 2) & 3, setMask, clearMask);
        paint(target.buffer, destination + 3 * stepX, packed & 3, setMask, clearMask);
        source += 4;
        destination += 4 * stepX;
        remaining -= 4;
      }
    }
    // One-bit glyphs and the clipped 2-bit tail use scalar decoding.
    while (remaining--) {
      const uint8_t ink = (bitsPerPixel == 2) ? ((bitmap[source >> 2] >> (6 - (source & 3) * 2)) & 3)
                                              : ((bitmap[source >> 3] >> (7 - (source & 7))) & 1);
      paint(target.buffer, destination, ink, setMask, clearMask);
      ++source;
      destination += stepX;
    }
  }
}

inline void draw(const uint8_t* bitmap, int width, int height, bool twoBit, Plane plane, bool state,
                 const Target& target, Clip clip) {
  draw(bitmap, width, height, static_cast<uint8_t>(twoBit ? 2 : 1), plane, state, target, clip);
}

}  // namespace glyphBitmap
