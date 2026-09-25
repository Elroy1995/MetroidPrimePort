#include "port_ws.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#if defined(MP_HAVE_OPENSSL) && !defined(_WIN32)
#include <arpa/inet.h>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#endif

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

#if defined(MP_HAVE_OPENSSL) && !defined(_WIN32)
bool Run(const std::string& command) { return std::system(command.c_str()) == 0; }

bool Contains(const std::string& text, const char* wanted) {
  return text.find(wanted) != std::string::npos;
}

// Asks the kernel for a free loopback port. The fake server binds it a moment
// later; nothing else on a test machine races for it in practice.
uint16_t FreeLoopbackPort() {
  const int probe = ::socket(AF_INET, SOCK_STREAM, 0);
  if (probe < 0)
    return 0;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t length = sizeof(address);
  uint16_t port = 0;
  if (::bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
      ::getsockname(probe, reinterpret_cast<sockaddr*>(&address), &length) == 0)
    port = ntohs(address.sin_port);
  ::close(probe);
  return port;
}

// wss:// against tools/ap_fake_server.py --tls with a throwaway CA: a verified
// round trip, then the rejections that make verification mean something.
void CheckTlsEndToEnd() {
  if (!Run("command -v openssl >/dev/null 2>&1") || !Run("command -v python3 >/dev/null 2>&1")) {
    std::puts("[ws-tests] tls skipped (no openssl/python3)");
    return;
  }
  std::error_code ignored;
  char dirTemplate[] = "/tmp/mp-ws-tls-XXXXXX";
  if (mkdtemp(dirTemplate) == nullptr) {
    Check(false, "TLS test temp directory");
    return;
  }
  const std::string dir = dirTemplate;
  {
    std::ofstream extensions(dir + "/server.ext");
    extensions << "subjectAltName=IP:127.0.0.1\nbasicConstraints=CA:FALSE\n";
  }
  // EC keys keep generation fast. `wrong-ca` never signs anything the server
  // presents.
  const std::string quiet = " >/dev/null 2>&1";
  const std::string newKey = "openssl req -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes ";
  const bool generated =
      Run("cd '" + dir + "' && " + newKey + "-x509 -days 1 -subj /CN=mp-test-ca -keyout ca.key -out ca.pem" +
          quiet) &&
      Run("cd '" + dir + "' && " + newKey +
          "-x509 -days 1 -subj /CN=mp-wrong-ca -keyout wrong-ca.key -out wrong-ca.pem" + quiet) &&
      Run("cd '" + dir + "' && " + newKey + "-subj /CN=127.0.0.1 -keyout server.key -out server.csr" + quiet) &&
      Run("cd '" + dir + "' && openssl x509 -req -days 1 -in server.csr -CA ca.pem -CAkey ca.key " +
          "-CAcreateserial -extfile server.ext -out server.pem" + quiet);
  Check(generated, "openssl CLI generates the test CA and server certificate");
  const uint16_t port = FreeLoopbackPort();
  Check(port != 0, "a free loopback port for the TLS server");
  if (!generated || port == 0) {
    std::filesystem::remove_all(dir, ignored);
    return;
  }

  const std::string script = std::string(MP_SOURCE_DIR) + "/tools/ap_fake_server.py";
  const std::string cert = dir + "/server.pem";
  const std::string key = dir + "/server.key";
  const std::string portText = std::to_string(port);
  const std::string logPath = dir + "/server.log";
  const pid_t child = fork();
  if (child == 0) {
    const int log = ::open(logPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (log >= 0) {
      dup2(log, STDOUT_FILENO);
      dup2(log, STDERR_FILENO);
    }
    execlp("python3", "python3", script.c_str(), "--tls", "--cert", cert.c_str(), "--key", key.c_str(),
           "--port", portText.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  Check(child > 0, "fork the TLS fake server");
  if (child <= 0) {
    std::filesystem::remove_all(dir, ignored);
    return;
  }

  PortWs::TlsOptions goodCa;
  goodCa.caFile = dir + "/ca.pem";
  PortWs::Client client;
  bool connected = false;
  // The server needs a moment to start listening; retry until it does.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
  while (!connected && std::chrono::steady_clock::now() < deadline) {
    connected = client.Connect("127.0.0.1", port, "/", 2000, true, goodCa);
    if (!connected)
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (!connected)
    std::fprintf(stderr, "[ws-tests] wss connect error: %s\n", client.Error());
  Check(connected, "wss:// connects and verifies against the test CA");

  std::string roomInfo;
  std::string reply;
  if (connected) {
    Check(client.ReceiveText(roomInfo, 3000) && Contains(roomInfo, "\"RoomInfo\""),
          "RoomInfo arrives over TLS");
    Check(client.SendText(R"([{"cmd":"Connect","name":"Player1","password":"","game":"Metroid Prime",)"
                          R"("uuid":"test","version":{"major":0,"minor":6,"build":0,"class":"Version"},)"
                          R"("items_handling":7,"tags":[],"slot_data":false}])"),
          "text frame sends over TLS");
    Check(client.ReceiveText(reply, 3000) && Contains(reply, "\"Connected\""),
          "the server's Connected reply arrives over TLS");
    std::string items;
    Check(client.ReceiveText(items, 3000) && Contains(items, "\"ReceivedItems\""),
          "ReceivedItems follows over TLS");
    // Nothing else is queued now, so the receive timeout must fire rather
    // than block on a TLS read.
    std::string nothing;
    const auto before = std::chrono::steady_clock::now();
    const bool idle = client.ReceiveText(nothing, 300);
    const auto waited = std::chrono::steady_clock::now() - before;
    Check(!idle && client.IsOpen() && Contains(client.Error(), "timed out") &&
              waited < std::chrono::seconds(2),
          "an idle TLS receive times out and keeps the connection");
    client.Close();
    Check(!client.IsOpen(), "Close releases the TLS connection");
  }
  std::printf("[ws-tests] tls round trip: %s\n", reply.empty() ? "(none)" : reply.c_str());

  struct Rejection {
    const char* label;
    const char* host;
    std::string caFile;
    const char* wanted;
  };
  const Rejection rejections[] = {
      {"wrong CA", "127.0.0.1", dir + "/wrong-ca.pem", "certificate verification failed"},
      {"certificate for another host", "localhost", dir + "/ca.pem", "certificate verification failed"},
      {"system trust store", "127.0.0.1", "", "certificate verification failed"},
      {"missing CA file", "127.0.0.1", dir + "/missing.pem", "could not load TLS CA file"},
  };
  for (const Rejection& rejection : rejections) {
    PortWs::TlsOptions options;
    options.caFile = rejection.caFile;
    PortWs::Client rejected;
    const bool accepted = rejected.Connect(rejection.host, port, "/", 3000, true, options);
    const std::string error = rejected.Error();
    std::printf("[ws-tests] tls %s rejected: %s\n", rejection.label, error.c_str());
    Check(!accepted && !rejected.IsOpen() && Contains(error, rejection.wanted),
          "TLS rejects an unverifiable server with a readable error");
  }

  // The server is still healthy after turning away bad handshakes.
  PortWs::Client again;
  Check(again.Connect("127.0.0.1", port, "/", 3000, true, goodCa) && again.ReceiveText(roomInfo, 3000),
        "the TLS server still serves a verified client after rejections");
  again.Close();

  kill(child, SIGTERM);
  int status = 0;
  waitpid(child, &status, 0);
  if (!sPassed) {
    std::ifstream log(logPath);
    std::fprintf(stderr, "[ws-tests] fake server log:\n%s\n",
                 std::string(std::istreambuf_iterator<char>(log), {}).c_str());
  }
  std::filesystem::remove_all(dir, ignored);
}
#endif

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
    // The scheme-reporting overload agrees on every ws:// form.
    if (std::string(test.url).rfind("ws://", 0) == 0) {
      std::string secureHost;
      uint16_t securePort = 0;
      std::string securePath;
      bool secure = true;
      const bool secureAccepted = PortWs::ParseUrl(test.url, secureHost, securePort, securePath, secure);
      Check(secureAccepted == test.accepted && (!secureAccepted ||
                (!secure && secureHost == host && securePort == port && securePath == path)),
            "ParseUrl secure overload matches the ws:// table");
    }
  }

  struct SecureUrlCase {
    const char* url;
    bool accepted;
    bool secure;
    const char* host;
    uint16_t port;
    const char* path;
  };
  const SecureUrlCase secureUrls[] = {
      {"ws://example.com", true, false, "example.com", 80, "/"},
      {"wss://example.com", true, true, "example.com", 443, "/"},
      {"wss://example.com:38281", true, true, "example.com", 38281, "/"},
      {"wss://archipelago.gg:38281/room/abc?x=1", true, true, "archipelago.gg", 38281, "/room/abc?x=1"},
      {"wss://[::1]:9000/chat", true, true, "::1", 9000, "/chat"},
      {"wss://127.0.0.1:443/", true, true, "127.0.0.1", 443, "/"},
      {"wss://", false, false, "", 0, ""},
      {"wss://:443/path", false, false, "", 0, ""},
      {"wss://example.com:", false, false, "", 0, ""},
      {"wss://example.com:0", false, false, "", 0, ""},
      {"wss://example.com:65536", false, false, "", 0, ""},
      {"wss://user@example.com", false, false, "", 0, ""},
      {"wss://example.com/a#frag", false, false, "", 0, ""},
      {"wsss://example.com", false, false, "", 0, ""},
      {"https://example.com", false, false, "", 0, ""},
      {"ftp://example.com", false, false, "", 0, ""},
      {"wss:/example.com", false, false, "", 0, ""},
  };
  for (const SecureUrlCase& test : secureUrls) {
    std::string host = "unchanged";
    uint16_t port = 444;
    std::string path = "unchanged";
    bool secure = !test.secure;
    const bool accepted = PortWs::ParseUrl(test.url, host, port, path, secure);
    Check(accepted == test.accepted, "ParseUrl wss:// acceptance table");
    if (accepted)
      Check(secure == test.secure && host == test.host && port == test.port && path == test.path,
            "ParseUrl wss:// scheme, host, port, and path values");
    else
      Check(host == "unchanged" && port == 444 && path == "unchanged",
            "rejected URLs leave the outputs alone");
  }

  if (!PortWs::TlsAvailable()) {
    PortWs::Client client;
    Check(!client.Connect("127.0.0.1", 9, "/", 500, true) && !client.IsOpen() &&
              std::string(client.Error()).find("no TLS") != std::string::npos,
          "a build without OpenSSL refuses wss:// instead of connecting in plaintext");
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

#if defined(MP_HAVE_OPENSSL) && !defined(_WIN32)
  CheckTlsEndToEnd();
#endif

  if (!sPassed)
    return 1;
  std::puts("[ws-tests] passed");
  return 0;
}
