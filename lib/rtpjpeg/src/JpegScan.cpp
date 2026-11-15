#include "JpegScan.h"

#include <cstring>

#include "HuffmanStd.h"

namespace rtpjpeg {
namespace {

// JPEG markers (ITU-T T.81 Table B.1).
constexpr uint8_t kSoi = 0xD8;
constexpr uint8_t kEoi = 0xD9;
constexpr uint8_t kSos = 0xDA;
constexpr uint8_t kDqt = 0xDB;
constexpr uint8_t kDri = 0xDD;
constexpr uint8_t kDht = 0xC4;
constexpr uint8_t kSof0 = 0xC0;  // baseline DCT
constexpr uint8_t kSof1 = 0xC1;  // extended sequential, Huffman (8-bit only accepted)
constexpr uint8_t kTem = 0x01;

constexpr uint16_t kMaxDimension = 2040;  // RFC 2435 carries width/8 and height/8 in one byte

inline uint16_t be16(const uint8_t *p) { return uint16_t((p[0] << 8) | p[1]); }

inline bool isRst(uint8_t m) { return m >= 0xD0 && m <= 0xD7; }

// SOF2..SOF15 (progressive, lossless, hierarchical, arithmetic) and DAC:
// none of these processes can be carried by RFC 2435.
inline bool isUnsupportedSof(uint8_t m) {
  return m >= 0xC2 && m <= 0xCF && m != kDht && m != 0xC8 /* JPG, reserved */;
}

struct Component {
  uint8_t id;
  uint8_t h;
  uint8_t v;
  uint8_t tq;
};

struct ParseState {
  const uint8_t *quant[4] = {nullptr, nullptr, nullptr, nullptr};
  Component comp[3] = {};
  bool haveSof = false;
};

// All segment parsers get `seg` = first byte after the length field and
// `len` = segment length minus the 2 length bytes.

JpegError parseSof(const uint8_t *seg, uint16_t len, ParseState &st, JpegInfo &info) {
  if (st.haveSof || len < 6) return JpegError::kBadFrameHeader;
  const uint8_t precision = seg[0];
  const uint16_t height = be16(seg + 1);
  const uint16_t width = be16(seg + 3);
  const uint8_t count = seg[5];
  if (precision != 8) return JpegError::kUnsupportedProcess;
  if (count != 3) return JpegError::kUnsupportedSampling;
  if (len != 6 + 3 * count) return JpegError::kBadFrameHeader;
  if (width == 0 || height == 0 || width > kMaxDimension || height > kMaxDimension ||
      (width & 7) != 0 || (height & 7) != 0) {
    return JpegError::kBadDimensions;
  }
  for (int i = 0; i < 3; ++i) {
    const uint8_t *c = seg + 6 + 3 * i;
    st.comp[i] = Component{c[0], uint8_t(c[1] >> 4), uint8_t(c[1] & 0x0F), c[2]};
    if (st.comp[i].tq > 3) return JpegError::kBadFrameHeader;
  }
  // RFC 2435 4.1: chroma is 1x1, luma is 2x1 (type 0, 4:2:2) or 2x2 (type 1,
  // 4:2:0). Announcing the wrong type yields a silently garbled picture.
  const Component &y = st.comp[0];
  if (st.comp[1].h != 1 || st.comp[1].v != 1 || st.comp[2].h != 1 || st.comp[2].v != 1) {
    return JpegError::kUnsupportedSampling;
  }
  if (y.h == 2 && y.v == 1) {
    info.baseType = 0;
  } else if (y.h == 2 && y.v == 2) {
    info.baseType = 1;
  } else {
    return JpegError::kUnsupportedSampling;
  }
  // Receivers rebuild two tables: luma, and one shared by both chroma planes.
  if (st.comp[1].tq != st.comp[2].tq) return JpegError::kBadQuantTable;
  info.width = width;
  info.height = height;
  st.haveSof = true;
  return JpegError::kOk;
}

JpegError parseDqt(const uint8_t *seg, uint16_t len, ParseState &st) {
  if (len == 0) return JpegError::kBadQuantTable;
  size_t pos = 0;
  while (pos < len) {
    const uint8_t pq = seg[pos] >> 4;
    const uint8_t tq = seg[pos] & 0x0F;
    if (pq != 0 || tq > 3) return JpegError::kBadQuantTable;  // 8-bit tables only
    if (pos + 1 + 64 > len) return JpegError::kBadQuantTable;
    st.quant[tq] = seg + pos + 1;
    pos += 1 + 64;
  }
  return JpegError::kOk;
}

JpegError parseDht(const uint8_t *seg, uint16_t len) {
  if (len == 0) return JpegError::kNonStandardHuffman;
  size_t pos = 0;
  while (pos < len) {
    const uint8_t tc = seg[pos] >> 4;
    const uint8_t th = seg[pos] & 0x0F;
    if (tc > 1 || th > 1) return JpegError::kNonStandardHuffman;
    if (pos + 1 + 16 > len) return JpegError::kNonStandardHuffman;
    const uint8_t *bits = seg + pos + 1;
    size_t valueCount = 0;
    for (int i = 0; i < 16; ++i) valueCount += bits[i];
    if (pos + 1 + 16 + valueCount > len) return JpegError::kNonStandardHuffman;
    const huffstd::Table &ref = huffstd::kTables[(tc << 1) | th];
    if (valueCount != ref.valueCount || std::memcmp(bits, ref.bits, 16) != 0 ||
        std::memcmp(bits + 16, ref.values, valueCount) != 0) {
      return JpegError::kNonStandardHuffman;
    }
    pos += 1 + 16 + valueCount;
  }
  return JpegError::kOk;
}

JpegError parseSos(const uint8_t *seg, uint16_t len, const ParseState &st, JpegInfo &info) {
  if (!st.haveSof || len < 1) return JpegError::kBadScanHeader;
  const uint8_t count = seg[0];
  if (count != 3 || len != 4 + 2 * count) return JpegError::kBadScanHeader;  // one interleaved scan
  for (int i = 0; i < 3; ++i) {
    const uint8_t cs = seg[1 + 2 * i];
    const uint8_t td = seg[2 + 2 * i] >> 4;
    const uint8_t ta = seg[2 + 2 * i] & 0x0F;
    if (cs != st.comp[i].id) return JpegError::kBadScanHeader;
    // Receivers map luma to Huffman tables 0 and chroma to tables 1.
    const uint8_t expected = (i == 0) ? 0 : 1;
    if (td != expected || ta != expected) return JpegError::kNonStandardHuffman;
  }
  const uint8_t ss = seg[7], se = seg[8], ahal = seg[9];
  if (ss != 0 || se != 63 || ahal != 0) return JpegError::kUnsupportedProcess;

  info.lumaQuant = st.quant[st.comp[0].tq];
  info.chromaQuant = st.quant[st.comp[1].tq];
  if (info.lumaQuant == nullptr || info.chromaQuant == nullptr) return JpegError::kMissingQuantTable;
  return JpegError::kOk;
}

// Locates EOI inside entropy-coded data. In a scan, 0xFF is only followed by
// 0x00 (stuffing), RSTn, further 0xFF fill bytes, or a real marker.
JpegError findEoi(const uint8_t *data, size_t begin, size_t length, size_t &eoiPos) {
  size_t pos = begin;
  while (pos < length) {
    const void *hit = std::memchr(data + pos, 0xFF, length - pos);
    if (hit == nullptr) return JpegError::kNoEoi;
    pos = size_t(static_cast<const uint8_t *>(hit) - data);
    size_t next = pos + 1;
    while (next < length && data[next] == 0xFF) ++next;
    if (next >= length) return JpegError::kNoEoi;
    const uint8_t m = data[next];
    if (m == 0x00 || isRst(m)) {
      pos = next + 1;
      continue;
    }
    if (m == kEoi) {
      eoiPos = next - 1;  // the 0xFF of FFD9
      return JpegError::kOk;
    }
    return JpegError::kUnsupportedProcess;  // another marker: multi-scan image
  }
  return JpegError::kNoEoi;
}

}  // namespace

const char *toString(JpegError error) {
  switch (error) {
    case JpegError::kOk: return "ok";
    case JpegError::kTooShort: return "frame too short";
    case JpegError::kNoSoi: return "missing SOI";
    case JpegError::kTruncated: return "truncated segment";
    case JpegError::kBadMarker: return "invalid marker";
    case JpegError::kUnsupportedProcess: return "not a single-scan 8-bit baseline JPEG";
    case JpegError::kBadFrameHeader: return "invalid SOF";
    case JpegError::kBadDimensions: return "dimensions not multiple of 8 or > 2040";
    case JpegError::kUnsupportedSampling: return "sampling is neither 4:2:2 nor 4:2:0";
    case JpegError::kBadQuantTable: return "invalid DQT";
    case JpegError::kMissingQuantTable: return "quantization table not defined";
    case JpegError::kNonStandardHuffman: return "non-standard Huffman tables";
    case JpegError::kBadScanHeader: return "invalid SOS";
    case JpegError::kNoScan: return "no scan";
    case JpegError::kNoEoi: return "missing EOI (incomplete frame)";
  }
  return "unknown";
}

JpegError parseJpeg(const uint8_t *data, size_t length, bool keepEoi, JpegInfo &info) {
  info = JpegInfo{};
  if (data == nullptr || length < 4) return JpegError::kTooShort;
  if (data[0] != 0xFF || data[1] != kSoi) return JpegError::kNoSoi;

  ParseState st;
  size_t pos = 2;
  while (pos < length) {
    if (data[pos] != 0xFF) return JpegError::kBadMarker;
    while (pos < length && data[pos] == 0xFF) ++pos;  // optional fill bytes
    if (pos >= length) return JpegError::kTruncated;
    const uint8_t marker = data[pos++];

    if (marker == kEoi) return JpegError::kNoScan;
    if (marker == kSoi || marker == kTem || isRst(marker) || marker == 0x00) {
      return JpegError::kBadMarker;
    }
    if (pos + 2 > length) return JpegError::kTruncated;
    const uint16_t segLen = be16(data + pos);
    if (segLen < 2 || pos + segLen > length) return JpegError::kTruncated;
    const uint8_t *seg = data + pos + 2;
    const uint16_t bodyLen = uint16_t(segLen - 2);

    JpegError err = JpegError::kOk;
    if (marker == kSof0 || marker == kSof1) {
      err = parseSof(seg, bodyLen, st, info);
    } else if (isUnsupportedSof(marker)) {
      err = JpegError::kUnsupportedProcess;
    } else if (marker == kDqt) {
      err = parseDqt(seg, bodyLen, st);
    } else if (marker == kDht) {
      err = parseDht(seg, bodyLen);
    } else if (marker == kDri) {
      if (bodyLen != 2) return JpegError::kBadMarker;
      info.restartInterval = be16(seg);
    } else if (marker == kSos) {
      err = parseSos(seg, bodyLen, st, info);
      if (err != JpegError::kOk) return err;
      const size_t scanBegin = pos + segLen;
      size_t eoiPos = 0;
      err = findEoi(data, scanBegin, length, eoiPos);
      if (err != JpegError::kOk) return err;
      info.scan = data + scanBegin;
      info.scanLength = (eoiPos - scanBegin) + (keepEoi ? 2 : 0);
      return JpegError::kOk;
    }
    // APPn, COM and other informative segments are skipped.
    if (err != JpegError::kOk) return err;
    pos += segLen;
  }
  return JpegError::kNoScan;
}

}  // namespace rtpjpeg

