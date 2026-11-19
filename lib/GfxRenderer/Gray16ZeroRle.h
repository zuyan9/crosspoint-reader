#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

class Gray16ZeroRle {
 private:
  static inline size_t ctz8(const uint8_t v) {
#if defined(__riscv) && !defined(__riscv_zbb)
    // Branchless 8-bit De Bruijn CTZ: avoids ~20,000 out-of-line libgcc
    // `__ctzsi2` calls per 2-plane pass on ESP32-C3 (RV32IMC).
    const uint32_t lsb = static_cast<uint32_t>(v) & (0u - static_cast<uint32_t>(v));
    return (0x34572610u >> (((lsb * 0x1Du) & 0xE0u) >> 3)) & 0x7u;
#else
    return static_cast<size_t>(__builtin_ctz(v));
#endif
  }

 public:
  static constexpr size_t BUFFER_BYTES = 32 * 1024;
  static constexpr size_t MAX_INDEXED_STRIPS = 12;  // up to 12 * 80 = 960 panel rows (7 strips on 528-row X3)

  // Compact 26-byte offset directory for up to 12 top-down RLE strips inside a <= 64 KB
  // RLE buffer. Strip `s` spans `[stripOffset[s], stripOffset[s + 1])`, enabling O(1)
  // bottom-up strip traversal (`s = stripCount - 1 .. 0`) at upload time.
  struct PlaneStripIndex {
    uint16_t stripOffset[MAX_INDEXED_STRIPS + 1] = {};

    constexpr uint16_t beginOffset(const size_t stripIdx) const { return stripOffset[stripIdx]; }
    constexpr uint16_t endOffset(const size_t stripIdx) const { return stripOffset[stripIdx + 1]; }
    constexpr uint16_t planeBegin() const { return stripOffset[0]; }
    constexpr uint16_t planeEnd(const size_t stripCount) const { return stripOffset[stripCount]; }
    constexpr size_t encodedBytes(const size_t stripCount) const {
      return static_cast<size_t>(stripOffset[stripCount]) - static_cast<size_t>(stripOffset[0]);
    }
  };
  static_assert(sizeof(PlaneStripIndex) == 26, "PlaneStripIndex must stay 26 bytes");

  static inline void invertBytesInPlace(uint8_t* buf, const size_t len) {
    if (!buf) return;
    for (size_t i = 0; i < len; ++i) {
      buf[i] = static_cast<uint8_t>(~buf[i]);
    }
  }

  // Stateful stack-only strip encoder that records per-strip offsets in `PlaneStripIndex`
  // while preserving atomic plane rollback: if any strip overflows `dstCap`, caller's
  // `cursor` is left untouched so any earlier plane in `dst[0..cursor)` stays valid.
  class PlaneEncoder {
   public:
    PlaneEncoder(uint8_t* dst, const size_t dstCap, size_t& cursor, PlaneStripIndex& index)
        : _dst(dst), _dstCap(dstCap), _committedCursor(cursor), _workingCursor(cursor), _index(index) {
      if (!dst || cursor > dstCap || cursor > UINT16_MAX) {
        _failed = true;
      } else {
        _index.stripOffset[0] = static_cast<uint16_t>(cursor);
      }
    }

    bool appendStrip(const uint8_t* stripSrc, const size_t rows, const size_t widthBytes) {
      if (_failed || _stripCount >= MAX_INDEXED_STRIPS) {
        _failed = true;
        return false;
      }
      const size_t cappedMax = _dstCap <= UINT16_MAX ? _dstCap : UINT16_MAX;
      if (!encodeStrip(stripSrc, rows, widthBytes, _dst, cappedMax, _workingCursor)) {
        _failed = true;
        return false;
      }
      ++_stripCount;
      _index.stripOffset[_stripCount] = static_cast<uint16_t>(_workingCursor);
      return true;
    }

    // Inverts a `0xFF`-background strip (e.g. `GRAY16_CLEANUP_DTM1`) in place so white paper runs
    // become `0x00` zero-runs before appending; pair with `GateStreamReader(..., /*invertOutput=*/true)`.
    bool appendStripInverted(uint8_t* stripBuf, const size_t rows, const size_t widthBytes) {
      if (!stripBuf) {
        _failed = true;
        return false;
      }
      invertBytesInPlace(stripBuf, rows * widthBytes);
      return appendStrip(stripBuf, rows, widthBytes);
    }

    bool finish() {
      if (_failed) return false;
      _committedCursor = _workingCursor;
      return true;
    }

    size_t stripCount() const { return _stripCount; }
    bool ok() const { return !_failed; }

   private:
    uint8_t* _dst;
    size_t _dstCap;
    size_t& _committedCursor;
    size_t _workingCursor;
    PlaneStripIndex& _index;
    size_t _stripCount = 0;
    bool _failed = false;
  };

  // Losslessly appends a 2D strip `src` (`rows * widthBytes` bytes) into `dst[cursor..dstCap)`
  // in column-major 8-byte groups using zero-packed RLE:
  //   - `{0x00, runMinus1}` encodes `1..256` consecutive all-zero 8-byte groups (up to 2,048 zero bytes).
  //   - `tag != 0x00` followed by `popcount(tag)` literal bytes places non-zero bytes at the set bit lanes.
  // Scanning column-major walks along horizontal text lines in Portrait X3 orientation while never
  // expanding isolated 1-byte zero gaps between glyph stems. On overflow (`false`), `cursor` is
  // left unchanged so any complete plane already written in `dst[0..cursor)` remains valid.
  static bool encodeStrip(const uint8_t* src, const size_t rows, const size_t widthBytes, uint8_t* dst,
                          const size_t dstCap, size_t& cursor) {
    const size_t totalBytes = rows * widthBytes;
    if (totalBytes == 0) return true;
    if (!src || !dst || cursor > dstCap) return false;

    size_t out = cursor;
    size_t zeroGroups = 0;
    const auto flushZeroGroups = [&]() -> bool {
      if (zeroGroups == 0) return true;
      if (dstCap - out < 2u) return false;
      dst[out++] = 0x00u;
      dst[out++] = static_cast<uint8_t>(zeroGroups - 1u);
      zeroGroups = 0;
      return true;
    };

    if ((rows & 7u) == 0u) {
      const size_t wb = widthBytes;
      const size_t stride8 = wb * 8u;
      for (size_t col = 0; col < wb; ++col) {
        const uint8_t* p = src + col;
        for (size_t r = 0; r < rows; r += 8u, p += stride8) {
          const uint8_t b0 = p[0];
          const uint8_t b1 = p[wb];
          const uint8_t b2 = p[wb * 2u];
          const uint8_t b3 = p[wb * 3u];
          const uint8_t b4 = p[wb * 4u];
          const uint8_t b5 = p[wb * 5u];
          const uint8_t b6 = p[wb * 6u];
          const uint8_t b7 = p[wb * 7u];
          if ((b0 | b1 | b2 | b3 | b4 | b5 | b6 | b7) == 0u) {
            if (++zeroGroups == 256u && !flushZeroGroups()) return false;
            continue;
          }
          if (!flushZeroGroups()) return false;
          if (dstCap - out < 9u) {
            const size_t nz = static_cast<size_t>(b0 != 0u) + static_cast<size_t>(b1 != 0u) +
                              static_cast<size_t>(b2 != 0u) + static_cast<size_t>(b3 != 0u) +
                              static_cast<size_t>(b4 != 0u) + static_cast<size_t>(b5 != 0u) +
                              static_cast<size_t>(b6 != 0u) + static_cast<size_t>(b7 != 0u);
            if (dstCap - out < 1u + nz) return false;
          }
          const size_t tagPos = out++;
          uint8_t tag = 0u;
          if (b0 != 0u) {
            tag |= 0x01u;
            dst[out++] = b0;
          }
          if (b1 != 0u) {
            tag |= 0x02u;
            dst[out++] = b1;
          }
          if (b2 != 0u) {
            tag |= 0x04u;
            dst[out++] = b2;
          }
          if (b3 != 0u) {
            tag |= 0x08u;
            dst[out++] = b3;
          }
          if (b4 != 0u) {
            tag |= 0x10u;
            dst[out++] = b4;
          }
          if (b5 != 0u) {
            tag |= 0x20u;
            dst[out++] = b5;
          }
          if (b6 != 0u) {
            tag |= 0x40u;
            dst[out++] = b6;
          }
          if (b7 != 0u) {
            tag |= 0x80u;
            dst[out++] = b7;
          }
          dst[tagPos] = tag;
        }
      }
      if (!flushZeroGroups()) return false;
      cursor = out;
      return true;
    }

    const size_t totalGroups = (totalBytes + 7u) >> 3;
    size_t col = 0;
    size_t row = 0;
    size_t rem = totalBytes;
    for (size_t g = 0; g < totalGroups; ++g) {
      const size_t laneCount = rem < 8u ? rem : 8u;
      rem -= laneCount;
      uint8_t lanes[8] = {0, 0, 0, 0, 0, 0, 0, 0};
      uint8_t tag = 0u;
      size_t nz = 0;
      for (size_t k = 0; k < laneCount; ++k) {
        const uint8_t v = src[row * widthBytes + col];
        if (++row == rows) {
          row = 0;
          ++col;
        }
        if (v != 0u) {
          tag |= static_cast<uint8_t>(1u << k);
          lanes[nz++] = v;
        }
      }
      if (tag == 0u) {
        if (++zeroGroups == 256u && !flushZeroGroups()) return false;
        continue;
      }
      if (!flushZeroGroups()) return false;
      if (dstCap - out < 1u + nz) return false;
      dst[out++] = tag;
      std::memcpy(dst + out, lanes, nz);
      out += nz;
    }
    if (!flushZeroGroups()) return false;
    cursor = out;
    return true;
  }

  // Decodes a 2D strip (`rows * widthBytes` bytes) from `src[cursor..srcEnd)` into `dst`.
  // Returns `false` without advancing `cursor` on truncated or out-of-range streams.
  static bool decodeStrip(const uint8_t* src, const size_t srcEnd, size_t& cursor, uint8_t* dst, const size_t rows,
                          const size_t widthBytes) {
    const size_t totalBytes = rows * widthBytes;
    if (totalBytes == 0) return true;
    if (!src || !dst || cursor > srcEnd) return false;

    std::memset(dst, 0x00, totalBytes);
    size_t in = cursor;

    if ((rows & 7u) == 0u) {
      const size_t groupsPerCol = rows >> 3;
      const size_t totalGroups = groupsPerCol * widthBytes;
      size_t g = 0;
      size_t col = 0;
      size_t r = 0;
      while (g < totalGroups) {
        if (in >= srcEnd) return false;
        const uint8_t tag = src[in++];
        if (tag == 0x00u) {
          if (in >= srcEnd) return false;
          const size_t run = static_cast<size_t>(src[in++]) + 1u;
          if (run > totalGroups - g) return false;
          g += run;
          col = g / groupsPerCol;
          r = (g % groupsPerCol) << 3;
          continue;
        }
        uint8_t* p = dst + r * widthBytes + col;
        uint8_t t = tag;
        while (t != 0u) {
          if (in >= srcEnd) return false;
          const size_t bit = ctz8(t);
          p[bit * widthBytes] = src[in++];
          t &= static_cast<uint8_t>(t - 1u);
        }
        ++g;
        r += 8u;
        if (r == rows) {
          r = 0;
          ++col;
        }
      }
      cursor = in;
      return true;
    }

    const size_t totalGroups = (totalBytes + 7u) >> 3;
    size_t g = 0;
    while (g < totalGroups) {
      if (in >= srcEnd) return false;
      const uint8_t tag = src[in++];
      if (tag == 0x00u) {
        if (in >= srcEnd) return false;
        const size_t run = static_cast<size_t>(src[in++]) + 1u;
        if (run > totalGroups - g) return false;
        g += run;
        continue;
      }
      const size_t baseIdx = g << 3;
      uint8_t t = tag;
      while (t != 0u) {
        if (in >= srcEnd) return false;
        const size_t bit = ctz8(t);
        const size_t idx = baseIdx + bit;
        if (idx >= totalBytes) return false;
        const size_t col = idx / rows;
        const size_t row = idx % rows;
        dst[row * widthBytes + col] = src[in++];
        t &= static_cast<uint8_t>(t - 1u);
      }
      ++g;
    }
    cursor = in;
    return true;
  }

  // Decodes an inverted 0xFF-background 2D strip (`appendStripInverted` output) directly into
  // top-down `dst` (`rows * widthBytes` bytes): initializes `dst` to `0xFF` white paper and writes
  // `~literal` only for non-zero encoded ink bytes. Returns `false` without advancing `cursor` on
  // truncated or out-of-range streams.
  static bool decodeStripInverted(const uint8_t* src, const size_t srcEnd, size_t& cursor, uint8_t* dst,
                                  const size_t rows, const size_t widthBytes) {
    const size_t totalBytes = rows * widthBytes;
    if (totalBytes == 0) return true;
    if (!src || !dst || cursor > srcEnd) return false;

    std::memset(dst, 0xFF, totalBytes);
    size_t in = cursor;

    if ((rows & 7u) == 0u) {
      const size_t groupsPerCol = rows >> 3;
      const size_t totalGroups = groupsPerCol * widthBytes;
      size_t g = 0;
      size_t col = 0;
      size_t r = 0;
      while (g < totalGroups) {
        if (in >= srcEnd) return false;
        const uint8_t tag = src[in++];
        if (tag == 0x00u) {
          if (in >= srcEnd) return false;
          const size_t run = static_cast<size_t>(src[in++]) + 1u;
          if (run > totalGroups - g) return false;
          g += run;
          col = g / groupsPerCol;
          r = (g % groupsPerCol) << 3;
          continue;
        }
        uint8_t* p = dst + r * widthBytes + col;
        uint8_t t = tag;
        while (t != 0u) {
          if (in >= srcEnd) return false;
          const size_t bit = ctz8(t);
          p[bit * widthBytes] = static_cast<uint8_t>(~src[in++]);
          t &= static_cast<uint8_t>(t - 1u);
        }
        ++g;
        r += 8u;
        if (r == rows) {
          r = 0;
          ++col;
        }
      }
      cursor = in;
      return true;
    }

    const size_t totalGroups = (totalBytes + 7u) >> 3;
    size_t g = 0;
    while (g < totalGroups) {
      if (in >= srcEnd) return false;
      const uint8_t tag = src[in++];
      if (tag == 0x00u) {
        if (in >= srcEnd) return false;
        const size_t run = static_cast<size_t>(src[in++]) + 1u;
        if (run > totalGroups - g) return false;
        g += run;
        continue;
      }
      const size_t baseIdx = g << 3;
      uint8_t t = tag;
      while (t != 0u) {
        if (in >= srcEnd) return false;
        const size_t bit = ctz8(t);
        const size_t idx = baseIdx + bit;
        if (idx >= totalBytes) return false;
        const size_t col = idx / rows;
        const size_t row = idx % rows;
        dst[row * widthBytes + col] = static_cast<uint8_t>(~src[in++]);
        t &= static_cast<uint8_t>(t - 1u);
      }
      ++g;
    }
    cursor = in;
    return true;
  }

  // Unpacks a full `planeHeight x widthBytes` 0xFF-background plane encoded via `appendStripInverted`
  // directly into top-down `dstPlane` (`frameBuffer`) with zero intermediate scratch buffer.
  static bool decodePlaneTopDownInverted(const uint8_t* src, const size_t srcCap, const PlaneStripIndex& index,
                                         uint8_t* dstPlane, const size_t planeHeight, const size_t stripRows,
                                         const size_t widthBytes) {
    if (planeHeight == 0 || widthBytes == 0) return true;
    if (!src || !dstPlane || stripRows == 0) return false;
    const size_t stripCount = (planeHeight + stripRows - 1u) / stripRows;
    if (stripCount > MAX_INDEXED_STRIPS) return false;
    for (size_t stripIdx = 0; stripIdx < stripCount; ++stripIdx) {
      const size_t beginOff = index.beginOffset(stripIdx);
      const size_t endOff = index.endOffset(stripIdx);
      if (beginOff > endOff || endOff > srcCap) return false;
      const size_t yStart = stripIdx * stripRows;
      const size_t rows = (planeHeight - yStart < stripRows) ? (planeHeight - yStart) : stripRows;
      size_t cursor = beginOff;
      if (!decodeStripInverted(src, endOff, cursor, dstPlane + yStart * widthBytes, rows, widthBytes)) return false;
      if (cursor != endOff) return false;
    }
    return true;
  }

  // Decodes a top-down encoded 2D strip (`rows * widthBytes` bytes) directly into `dst`
  // with rows reversed into UC8253 gate order (`dst` row 0 = logical row `rows - 1`,
  // `dst` row `rows - 1` = logical row 0). Returns `false` without advancing `cursor`
  // on truncated or out-of-range streams.
  static bool decodeStripGateOrder(const uint8_t* src, const size_t srcEnd, size_t& cursor, uint8_t* dst,
                                   const size_t rows, const size_t widthBytes) {
    const size_t totalBytes = rows * widthBytes;
    if (totalBytes == 0) return true;
    if (!src || !dst || cursor > srcEnd) return false;

    std::memset(dst, 0x00, totalBytes);
    size_t in = cursor;

    if ((rows & 7u) == 0u) {
      const size_t groupsPerCol = rows >> 3;
      const size_t totalGroups = groupsPerCol * widthBytes;
      size_t g = 0;
      size_t col = 0;
      size_t r = 0;
      while (g < totalGroups) {
        if (in >= srcEnd) return false;
        const uint8_t tag = src[in++];
        if (tag == 0x00u) {
          if (in >= srcEnd) return false;
          const size_t run = static_cast<size_t>(src[in++]) + 1u;
          if (run > totalGroups - g) return false;
          g += run;
          col = g / groupsPerCol;
          r = (g % groupsPerCol) << 3;
          continue;
        }
        uint8_t* p = dst + (rows - 1u - r) * widthBytes + col;
        uint8_t t = tag;
        while (t != 0u) {
          if (in >= srcEnd) return false;
          const size_t bit = ctz8(t);
          *(p - bit * widthBytes) = src[in++];
          t &= static_cast<uint8_t>(t - 1u);
        }
        ++g;
        r += 8u;
        if (r == rows) {
          r = 0;
          ++col;
        }
      }
      cursor = in;
      return true;
    }

    const size_t totalGroups = (totalBytes + 7u) >> 3;
    size_t g = 0;
    while (g < totalGroups) {
      if (in >= srcEnd) return false;
      const uint8_t tag = src[in++];
      if (tag == 0x00u) {
        if (in >= srcEnd) return false;
        const size_t run = static_cast<size_t>(src[in++]) + 1u;
        if (run > totalGroups - g) return false;
        g += run;
        continue;
      }
      const size_t baseIdx = g << 3;
      uint8_t t = tag;
      while (t != 0u) {
        if (in >= srcEnd) return false;
        const size_t bit = ctz8(t);
        const size_t idx = baseIdx + bit;
        if (idx >= totalBytes) return false;
        const size_t col = idx / rows;
        const size_t gateRow = (rows - 1u) - (idx % rows);
        dst[gateRow * widthBytes + col] = src[in++];
        t &= static_cast<uint8_t>(t - 1u);
      }
      ++g;
    }
    cursor = in;
    return true;
  }

  // Random-access gate-order decode of a single strip `stripIdx` recorded in `index`.
  // Verifies that `[beginOffset, endOffset)` lies within `srcCap` and is consumed in full.
  static bool decodeIndexedStripGateOrder(const uint8_t* src, const size_t srcCap, const PlaneStripIndex& index,
                                          const size_t stripIdx, uint8_t* dst, const size_t rows,
                                          const size_t widthBytes) {
    if (stripIdx >= MAX_INDEXED_STRIPS) return false;
    const size_t beginOff = index.beginOffset(stripIdx);
    const size_t endOff = index.endOffset(stripIdx);
    if (beginOff > endOff || endOff > srcCap) return false;
    size_t cursor = beginOff;
    if (!decodeStripGateOrder(src, endOff, cursor, dst, rows, widthBytes)) return false;
    return cursor == endOff;
  }

  // Stack-only pull cursor that decodes encoded strips in reverse (`stripCount - 1` down
  // to `0`) into `scratch` in bottom-up UC8253 gate order (`y = planeHeight - 1 .. 0`),
  // suitable for a single scoped SPI stream callback (`nextCallback`) with zero storage I/O.
  class GateStreamReader {
   public:
    GateStreamReader(const uint8_t* src, const size_t srcCap, const PlaneStripIndex& index, uint8_t* scratch,
                     const size_t scratchCap, const size_t planeHeight, const size_t stripRows, const size_t widthBytes,
                     const bool invertOutput = false)
        : _src(src),
          _srcCap(srcCap),
          _index(index),
          _scratch(scratch),
          _planeHeight(planeHeight),
          _stripRows(stripRows),
          _widthBytes(widthBytes),
          _invertOutput(invertOutput) {
      if (planeHeight == 0 || widthBytes == 0) return;
      if (!src || !scratch || stripRows == 0) {
        _failed = true;
        return;
      }
      const size_t stripCount = (planeHeight + stripRows - 1u) / stripRows;
      const size_t maxRows = planeHeight < stripRows ? planeHeight : stripRows;
      if (stripCount > MAX_INDEXED_STRIPS || maxRows > scratchCap / widthBytes) {
        _failed = true;
        return;
      }
      _remainingStrips = stripCount;
    }

    size_t next(const uint8_t** outChunk) {
      if (!outChunk) {
        _failed = true;
        return 0;
      }
      if (_failed || _remainingStrips == 0) return 0;
      const size_t stripIdx = --_remainingStrips;
      const size_t yStart = stripIdx * _stripRows;
      const size_t rows = (_planeHeight - yStart < _stripRows) ? (_planeHeight - yStart) : _stripRows;
      if (!decodeIndexedStripGateOrder(_src, _srcCap, _index, stripIdx, _scratch, rows, _widthBytes)) {
        _failed = true;
        return 0;
      }
      const size_t chunkBytes = rows * _widthBytes;
      if (_invertOutput) {
        invertBytesInPlace(_scratch, chunkBytes);
      }
      *outChunk = _scratch;
      return chunkBytes;
    }

    static size_t nextCallback(void* ctx, const uint8_t** outChunk) {
      return static_cast<GateStreamReader*>(ctx)->next(outChunk);
    }

    bool ok() const { return !_failed && _remainingStrips == 0; }

   private:
    const uint8_t* _src;
    size_t _srcCap;
    const PlaneStripIndex& _index;
    uint8_t* _scratch;
    size_t _planeHeight;
    size_t _stripRows;
    size_t _widthBytes;
    size_t _remainingStrips = 0;
    bool _invertOutput = false;
    bool _failed = false;
  };

  // Walks all encoded strips of a full `planeHeight x widthBytes` plane in reverse
  // (`stripCount - 1` down to `0`), decoding each strip directly into `scratch` in
  // bottom-up gate order (`y = planeHeight - 1 .. 0`) and invoking `emitChunk(scratch, stripBytes)`
  // once per contiguous strip block.
  template <typename EmitChunkFn>
  static bool decodePlaneGateOrder(const uint8_t* src, const size_t srcCap, const PlaneStripIndex& index,
                                   uint8_t* scratch, const size_t scratchCap, const size_t planeHeight,
                                   const size_t stripRows, const size_t widthBytes, EmitChunkFn&& emitChunk) {
    GateStreamReader reader(src, srcCap, index, scratch, scratchCap, planeHeight, stripRows, widthBytes);
    const uint8_t* chunk = nullptr;
    while (const size_t bytes = reader.next(&chunk)) {
      emitChunk(chunk, bytes);
    }
    return reader.ok();
  }

  // 1D convenience wrappers (`rows = len`, `widthBytes = 1`).
  static bool encodeAppend(const uint8_t* src, const size_t srcLen, uint8_t* dst, const size_t dstCap, size_t& cursor) {
    return encodeStrip(src, srcLen, 1u, dst, dstCap, cursor);
  }

  static bool decodeChunk(const uint8_t* src, const size_t srcEnd, size_t& cursor, uint8_t* dst, const size_t dstLen) {
    return decodeStrip(src, srcEnd, cursor, dst, dstLen, 1u);
  }
};

using Gray16ZeroRleEncoder = Gray16ZeroRle::PlaneEncoder;
