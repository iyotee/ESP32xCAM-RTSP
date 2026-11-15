// Parsing and validation of a baseline JFIF/JPEG frame for RTP transport
// (RFC 2435). Pure C++, no allocation, no platform dependency.
//
// RFC 2435 does not carry JPEG headers on the wire: the receiver rebuilds them
// from the RTP/JPEG header (type, width, height, quantization tables) and
// assumes the standard Huffman tables of ITU-T T.81 Annex K. Only the entropy-
// coded scan data may be sent. Sending the whole file (SOI, APP0, DQT, ...) is
// what made FFmpeg print "overread 8" for every frame.
#pragma once

#include <cstddef>
#include <cstdint>

namespace rtpjpeg {

enum class JpegError : uint8_t {
  kOk = 0,
  kTooShort,
  kNoSoi,
  kTruncated,           // a segment runs past the end of the buffer
  kBadMarker,           // garbage where a marker was expected
  kUnsupportedProcess,  // progressive, lossless, arithmetic, 12-bit, ...
  kBadFrameHeader,
  kBadDimensions,       // zero, > 2040 or not a multiple of 8
  kUnsupportedSampling, // not YUV 4:2:2 (type 0) or 4:2:0 (type 1)
  kBadQuantTable,
  kMissingQuantTable,
  kNonStandardHuffman,  // RFC 2435 receivers always use the Annex K tables
  kBadScanHeader,
  kNoScan,
  kNoEoi,               // incomplete frame (sensor/DMA overrun)
};

const char *toString(JpegError error);

struct JpegInfo {
  uint16_t width = 0;
  uint16_t height = 0;
  // RFC 2435 type: 0 = 4:2:2, 1 = 4:2:0. Restart markers add 64 (see rtpType()).
  uint8_t baseType = 0;
  uint16_t restartInterval = 0;  // 0 = no DRI
  // Luma table then chroma table, 64 bytes each, 8-bit precision, zig-zag order
  // (as stored in DQT, which is also the order RFC 2435 uses).
  const uint8_t *lumaQuant = nullptr;
  const uint8_t *chromaQuant = nullptr;
  // Entropy-coded data, starting right after the SOS header. Ends either at
  // EOI (excluded) or just after it, depending on parseJpeg(keepEoi).
  const uint8_t *scan = nullptr;
  size_t scanLength = 0;

  uint8_t rtpType() const { return restartInterval ? uint8_t(baseType + 64) : baseType; }
};

// Parses `data[0..length)` and fills `info` with pointers into `data`.
// `keepEoi` keeps the trailing FFD9 in the scan payload; RFC 2435 receivers
// tolerate it and FFmpeg then never has to emulate a missing EOI.
JpegError parseJpeg(const uint8_t *data, size_t length, bool keepEoi, JpegInfo &info);

}  // namespace rtpjpeg
