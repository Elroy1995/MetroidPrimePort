#include "port_ws.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace {

bool sPassed = true;

void Check(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "[ws-tests] FAILED: %s\n", message);
    sPassed = false;
  }
}

std::string Hex(const uint8_t* bytes, size_t size) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(size * 2);
  for (size_t i = 0; i < size; ++i) {
    result.push_back(digits[bytes[i] >> 4]);
    result.push_back(digits[bytes[i] & 0x0f]);
  }
  return result;
}

std::string ServerFrame(uint8_t opcode, bool final, const std::string& payload) {
  std::string frame;
  frame.push_back(static_cast<char>((final ? 0x80 : 0) | opcode));
  if (payload.size() < 126) {
    frame.push_back(static_cast<char>(payload.size()));
  } else if (payload.size() <= 0xffff) {
    frame.push_back(static_cast<char>(126));
    frame.push_back(static_cast<char>((payload.size() >> 8) & 0xff));
    frame.push_back(static_cast<char>(payload.size() & 0xff));
  } else {
    frame.push_back(static_cast<char>(127));
    const uint64_t size = payload.size();
    for (int i = 7; i >= 0; --i)
      frame.push_back(static_cast<char>((size >> (i * 8)) & 0xff));
  }
  frame.append(payload);
  return frame;
}

void CheckRoundTrip(size_t size) {
  const std::string payload(size, 'x');
  const std::string encoded = PortWs::EncodeFrame(1, payload, 0x12345678);
  PortWs::FrameDecoder decoder;
  std::vector<PortWs::Frame> frames;
  size_t consumed = 0;
  const size_t chunks[] = {1, 2, 7, 3, 19, 5};
  size_t chunkIndex = 0;
  while (consumed < encoded.size()) {
    const size_t count = std::min(chunks[chunkIndex++ % 6], encoded.size() - consumed);
    decoder.Feed(encoded.data() + consumed, count, frames);
    consumed += count;
  }
  Check(!decoder.Failed() && frames.size() == 1 && frames[0].opcode == 1 &&
            frames[0].payload == payload,
        "masked encode/decode round trip at each length encoding boundary");
}

} // namespace

int main() {
  uint8_t digest[20];
  PortWs::Sha1("", 0, digest);
  Check(Hex(digest, sizeof(digest)) == "da39a3ee5e6b4b0d3255bfef95601890afd80709",
        "SHA-1 empty-string vector");
  PortWs::Sha1("abc", 3, digest);
  Check(Hex(digest, sizeof(digest)) == "a9993e364706816aba3e25717850c26c9cd0d89d",
        "SHA-1 abc vector");
  const std::string rfc448 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  PortWs::Sha1(rfc448.data(), rfc448.size(), digest);
  Check(Hex(digest, sizeof(digest)) == "84983e441c3bd26ebaae4aa1f95129e5e54670f1",
        "SHA-1 RFC 3174 448-bit padding vector");

  Check(PortWs::Base64Encode("", 0).empty(), "base64 empty vector");
  Check(PortWs::Base64Encode("f", 1) == "Zg==" && PortWs::Base64Encode("fo", 2) == "Zm8=" &&
            PortWs::Base64Encode("foo", 3) == "Zm9v" &&
            PortWs::Base64Encode("foob", 4) == "Zm9vYg==" &&
            PortWs::Base64Encode("fooba", 5) == "Zm9vYmE=" &&
            PortWs::Base64Encode("foobar", 6) == "Zm9vYmFy",
        "base64 standard vectors and padding");
  Check(PortWs::AcceptKey("dGhlIHNhbXBsZSBub25jZQ==") ==
            "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=",
        "RFC 6455 Sec-WebSocket-Accept example");

  struct UrlCase {
    const char* url;
    bool accepted;
    const char* host;
    uint16_t port;
    const char* path;
  };
  const UrlCase urls[] = {
      {"ws://example.com", true, "example.com", 80, "/"},
      {"ws://example.com:8080", true, "example.com", 8080, "/"},
      {"ws://example.com:1234/path?q=1", true, "example.com", 1234, "/path?q=1"},
      {"ws://[::1]:9000/chat", true, "::1", 9000, "/chat"},
      {"wss://example.com", false, "", 0, ""},
      {"http://example.com", false, "", 0, ""},
      {"ws://", false, "", 0, ""},
      {"ws://:80/path", false, "", 0, ""},
      {"ws://example.com:abc", false, "", 0, ""},
      {"ws://example.com:0", false, "", 0, ""},
      {"ws://example.com:65536", false, "", 0, ""},
      {"ws://example.com:", false, "", 0, ""},
      {"ws://user@example.com", false, "", 0, ""},
      {"ws://example.com?x=1", false, "", 0, ""},
      {"ws://example.com/a#frag", false, "", 0, ""},
  };
  for (const UrlCase& test : urls) {
    std::string host = "unchanged";
    uint16_t port = 444;
    std::string path = "unchanged";
    const bool accepted = PortWs::ParseUrl(test.url, host, port, path);
    Check(accepted == test.accepted, "ParseUrl acceptance table");
    if (accepted)
      Check(host == test.host && port == test.port && path == test.path,
            "ParseUrl host, port, and path values");
  }

  const std::string encodedSmall = PortWs::EncodeFrame(1, "HI", 0x04030201);
  const std::string handComputed("\x81\x82\x01\x02\x03\x04\x49\x4b", 8);
  Check(encodedSmall == handComputed &&
            encodedSmall == PortWs::EncodeFrame(1, "HI", 0x04030201),
        "deterministic masked frame matches hand-computed bytes");

  CheckRoundTrip(0);
  CheckRoundTrip(125);
  CheckRoundTrip(126);
  CheckRoundTrip(65535);
  CheckRoundTrip(65536);

  PortWs::FrameDecoder fragmented;
  std::string fragments;
  fragments += ServerFrame(1, false, "hel");
  fragments += ServerFrame(9, true, "p");
  fragments += ServerFrame(0, true, "lo");
  std::vector<PortWs::Frame> fragmentFrames;
  for (size_t i = 0; i < fragments.size(); ++i)
    fragmented.Feed(fragments.data() + i, 1, fragmentFrames);
  Check(!fragmented.Failed() && fragmentFrames.size() == 2 &&
            fragmentFrames[0].opcode == 9 && fragmentFrames[0].payload == "p" &&
            fragmentFrames[1].opcode == 1 && fragmentFrames[1].payload == "hello",
        "fragment reassembly with interleaved ping frame");

  const std::string maskedServer = PortWs::EncodeFrame(1, "masked", 0xdeadbeef);
  PortWs::FrameDecoder maskedDecoder;
  std::vector<PortWs::Frame> maskedFrames;
  maskedDecoder.Feed(maskedServer.data(), maskedServer.size(), maskedFrames);
  Check(!maskedDecoder.Failed() && maskedFrames.size() == 1 &&
            maskedFrames[0].opcode == 1 && maskedFrames[0].payload == "masked",
        "decoder accepts masked server frames");

  std::string oversized("\x81\x7f", 2);
  const uint64_t overLimit = PortWs::FrameDecoder::kMaxMessageSize + 1;
  for (int i = 7; i >= 0; --i)
    oversized.push_back(static_cast<char>((overLimit >> (i * 8)) & 0xff));
  PortWs::FrameDecoder oversizedDecoder;
  std::vector<PortWs::Frame> oversizedFrames;
  oversizedDecoder.Feed(oversized.data(), oversized.size(), oversizedFrames);
  Check(oversizedDecoder.Failed(), "oversized message latches decoder failure");

  if (!sPassed)
    return 1;
  std::puts("[ws-tests] passed");
  return 0;
}
