// Host helper for tools/ffmpeg_loopback.py: packetizes a JPEG file with the
// firmware's own lib/rtpjpeg code and writes the RTP packets to stdout, each
// prefixed by a 2-byte big-endian length.
//
// Built automatically by tools/ffmpeg_loopback.py.
// Usage: host_packetize <file.jpg> <frames> <keepEoi 0|1>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "JpegScan.h"
#include "RtpJpegPacketizer.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

int main(int argc, char **argv) {
  if (argc != 4) {
    std::fprintf(stderr, "usage: %s <file.jpg> <frames> <keepEoi>\n", argv[0]);
    return 2;
  }
#ifdef _WIN32
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  FILE *f = std::fopen(argv[1], "rb");
  if (!f) {
    std::perror("fopen");
    return 1;
  }
  std::vector<uint8_t> jpeg;
  uint8_t chunk[4096];
  size_t n;
  while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) jpeg.insert(jpeg.end(), chunk, chunk + n);
  std::fclose(f);

  rtpjpeg::JpegInfo info;
  const rtpjpeg::JpegError err = rtpjpeg::parseJpeg(jpeg.data(), jpeg.size(), argv[3][0] == '1', info);
  if (err != rtpjpeg::JpegError::kOk) {
    std::fprintf(stderr, "parse error: %s\n", rtpjpeg::toString(err));
    return 1;
  }
  std::fprintf(stderr, "%ux%u type=%u dri=%u scan=%zu\n", info.width, info.height, info.rtpType(),
               info.restartInterval, info.scanLength);

  rtpjpeg::RtpJpegPacketizer pk(0x5EED1234u, 1000, 1400);
  std::vector<uint8_t> pkt(pk.maxPacketSize());
  const int frames = std::atoi(argv[2]);
  for (int i = 0; i < frames; ++i) {
    pk.begin(info, uint32_t(i) * 9000u);  // 10 fps at 90 kHz
    while (pk.hasNext()) {
      const size_t len = pk.next(pkt.data(), pkt.size());
      const uint8_t hdr[2] = {uint8_t(len >> 8), uint8_t(len)};
      std::fwrite(hdr, 1, 2, stdout);
      std::fwrite(pkt.data(), 1, len, stdout);
    }
  }
  return 0;
}
