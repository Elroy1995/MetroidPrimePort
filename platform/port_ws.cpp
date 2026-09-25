#include "port_ws.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cctype>
#include <climits>
#include <cstring>
#include <mutex>
#include <random>
#include <string_view>
#include <utility>

#ifdef _WIN32
// winsock2.h drags in windows.h, whose min/max macros would break every
// std::min and std::max below. NOMINMAX has to be defined before it is read.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <fcntl.h>
// Bionic declares IPPROTO_TCP here rather than in <netdb.h>, where glibc and
// Winsock get it, so getaddrinfo's ai_protocol below needs this on Android.
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#ifdef MP_HAVE_OPENSSL
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#if !defined(_WIN32)
#include <csignal>
#include <ctime>
#include <pthread.h>
#endif
#endif

namespace PortWs {
namespace {

using Clock = std::chrono::steady_clock;

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;

bool EnsureWinsock() {
  static std::once_flag once;
  static int result = WSASYSNOTREADY;
  std::call_once(once, [] {
    WSADATA data{};
    result = WSAStartup(MAKEWORD(2, 2), &data);
  });
  return result == 0;
}

int SocketError() { return WSAGetLastError(); }
bool IsInterrupted(int error) { return error == WSAEINTR; }
bool IsWouldBlock(int error) {
  return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS || error == WSAEALREADY;
}
void CloseNative(NativeSocket socket) {
  if (socket != kInvalidSocket)
    closesocket(socket);
}
bool SetNonBlocking(NativeSocket socket) {
  u_long enabled = 1;
  return ioctlsocket(socket, FIONBIO, &enabled) == 0;
}
int StoreSocket(NativeSocket socket) {
  if (socket == kInvalidSocket || socket > static_cast<NativeSocket>(INT_MAX))
    return -1;
  return static_cast<int>(socket);
}
NativeSocket NativeFromStored(int socket) {
  return static_cast<NativeSocket>(static_cast<unsigned int>(socket));
}
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;
bool EnsureWinsock() { return true; }
int SocketError() { return errno; }
bool IsInterrupted(int error) { return error == EINTR; }
bool IsWouldBlock(int error) {
  return error == EAGAIN || error == EWOULDBLOCK || error == EINPROGRESS || error == EALREADY;
}
void CloseNative(NativeSocket socket) {
  if (socket != kInvalidSocket)
    close(socket);
}
bool SetNonBlocking(NativeSocket socket) {
  const int flags = fcntl(socket, F_GETFL, 0);
  return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
}
int StoreSocket(NativeSocket socket) { return socket; }
NativeSocket NativeFromStored(int socket) { return socket; }
#endif

std::string SystemError(int error) {
#ifdef _WIN32
  return "socket error " + std::to_string(error);
#else
  return std::strerror(error);
#endif
}

// A return value of 1 means ready, 0 means timed out, and -1 means error.
int WaitReady(int storedSocket, bool readable, int timeoutMs) {
  const NativeSocket socket = NativeFromStored(storedSocket);
  fd_set descriptors;
  FD_ZERO(&descriptors);
  FD_SET(socket, &descriptors);
  timeval timeout{};
  timeval* timeoutPointer = nullptr;
  if (timeoutMs > 0) {
    timeout.tv_sec = timeoutMs / 1000;
    timeout.tv_usec = (timeoutMs % 1000) * 1000;
    timeoutPointer = &timeout;
  }
#ifdef _WIN32
  const int result = select(0, readable ? &descriptors : nullptr,
                            readable ? nullptr : &descriptors, nullptr, timeoutPointer);
#else
  const int result = select(socket + 1, readable ? &descriptors : nullptr,
                            readable ? nullptr : &descriptors, nullptr, timeoutPointer);
#endif
  if (result > 0)
    return 1;
  if (result == 0)
    return 0;
  return -1;
}

int RemainingMs(const Clock::time_point& deadline, bool infinite) {
  if (infinite)
    return 0;
  const auto remaining = deadline - Clock::now();
  if (remaining <= Clock::duration::zero())
    return -1;
  auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
  if (std::chrono::milliseconds(milliseconds) < remaining)
    ++milliseconds;
  return static_cast<int>(std::min<int64_t>(milliseconds, INT_MAX));
}

bool SendAll(int storedSocket, const std::string& data, int timeoutMs, std::string& error) {
  const bool infinite = timeoutMs <= 0;
  const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(std::max(timeoutMs, 0));
  size_t sent = 0;
  while (sent < data.size()) {
    const int remaining = RemainingMs(deadline, infinite);
    if (remaining < 0) {
      error = "send timed out";
      return false;
    }
    const int ready = WaitReady(storedSocket, false, remaining);
    if (ready == 0) {
      error = "send timed out";
      return false;
    }
    if (ready < 0) {
      const int socketError = SocketError();
      if (IsInterrupted(socketError))
        continue;
      error = "send wait failed: " + SystemError(socketError);
      return false;
    }
    const size_t chunk = std::min<size_t>(data.size() - sent, static_cast<size_t>(INT_MAX));
#ifdef _WIN32
    const int count = ::send(NativeFromStored(storedSocket), data.data() + sent,
                             static_cast<int>(chunk), 0);
#else
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
    const ssize_t count = ::send(NativeFromStored(storedSocket), data.data() + sent, chunk, flags);
#endif
    if (count > 0) {
      sent += static_cast<size_t>(count);
      continue;
    }
    if (count == 0) {
      error = "socket closed during send";
      return false;
    }
    const int socketError = SocketError();
    if (IsInterrupted(socketError) || IsWouldBlock(socketError))
      continue;
    error = "send failed: " + SystemError(socketError);
    return false;
  }
  return true;
}

bool RandomSeed(uint32_t& seed) {
  try {
    std::random_device random;
    seed = 0;
    for (unsigned i = 0; i < 4; ++i)
      seed |= static_cast<uint32_t>(random() & 0xffu) << (8 * i);
    return true;
  } catch (...) {
    return false;
  }
}

std::string Lower(std::string value) {
  for (char& c : value)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return value;
}

std::string_view Trim(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
    value.remove_prefix(1);
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
    value.remove_suffix(1);
  return value;
}

bool HasToken(std::string value, const std::string& wanted) {
  size_t start = 0;
  while (start <= value.size()) {
    const size_t comma = value.find(',', start);
    const size_t end = comma == std::string::npos ? value.size() : comma;
    if (Lower(std::string(Trim(std::string_view(value).substr(start, end - start)))) == wanted)
      return true;
    if (comma == std::string::npos)
      break;
    start = comma + 1;
  }
  return false;
}

bool ValidUrlCharacter(const std::string& text, bool allowPathSpace) {
  for (unsigned char c : text) {
    if (c <= 0x20 || c == 0x7f || (!allowPathSpace && std::isspace(c)))
      return false;
  }
  return true;
}

bool SetSocketErrorText(std::string& error, const char* operation) {
  error = operation;
  error += ": ";
  error += SystemError(SocketError());
  return false;
}

void AppendUnmaskedFrame(std::string& destination, uint8_t opcode, const std::string& payload) {
  destination.push_back(static_cast<char>(0x80 | (opcode & 0x0f)));
  const uint64_t length = payload.size();
  if (length < 126) {
    destination.push_back(static_cast<char>(length));
  } else if (length <= 0xffff) {
    destination.push_back(static_cast<char>(126));
    destination.push_back(static_cast<char>((length >> 8) & 0xff));
    destination.push_back(static_cast<char>(length & 0xff));
  } else {
    destination.push_back(static_cast<char>(127));
    for (int i = 7; i >= 0; --i)
      destination.push_back(static_cast<char>((length >> (i * 8)) & 0xff));
  }
  destination.append(payload);
}

#ifdef MP_HAVE_OPENSSL
#if !defined(_WIN32) && !defined(SO_NOSIGPIPE)
// OpenSSL writes to the socket with write(), which raises SIGPIPE when the
// server has gone away. Block it on this thread around each TLS call and
// swallow one that arrives, so a dead server is an error instead of killing
// the game. (Windows has no SIGPIPE; Apple sockets get SO_NOSIGPIPE.)
class SigpipeGuard {
public:
  SigpipeGuard() {
    sigemptyset(&mPipe);
    sigaddset(&mPipe, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &mPipe, &mPrevious);
    sigset_t pending;
    sigemptyset(&pending);
    mWasPending = sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1;
  }
  ~SigpipeGuard() {
    const int savedErrno = errno;
    sigset_t pending;
    sigemptyset(&pending);
    if (!mWasPending && sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1) {
      const timespec zero{};
      sigtimedwait(&mPipe, nullptr, &zero);
    }
    pthread_sigmask(SIG_SETMASK, &mPrevious, nullptr);
    errno = savedErrno;
  }
  SigpipeGuard(const SigpipeGuard&) = delete;
  SigpipeGuard& operator=(const SigpipeGuard&) = delete;

private:
  sigset_t mPipe;
  sigset_t mPrevious;
  bool mWasPending = false;
};
#else
struct SigpipeGuard {
  SigpipeGuard() {}
};
#endif

// Takes the oldest queued OpenSSL error as text and clears the queue.
std::string TlsQueueText() {
  const unsigned long code = ERR_get_error();
  ERR_clear_error();
  if (code == 0)
    return "unknown TLS error";
  if (const char* reason = ERR_reason_error_string(code))
    return reason;
  char text[256];
  ERR_error_string_n(code, text, sizeof(text));
  return text;
}

// Describes a fatal SSL_get_error result. `closed` reports that the peer
// simply went away (clean close_notify or a bare TCP close).
std::string TlsFailureText(int sslError, bool& closed) {
  closed = false;
  if (sslError == SSL_ERROR_ZERO_RETURN) {
    closed = true;
    ERR_clear_error();
    return "connection closed";
  }
  if (sslError == SSL_ERROR_SYSCALL) {
    const int socketError = SocketError();
    if (ERR_peek_error() == 0 && socketError == 0) {
      closed = true;
      return "connection closed";
    }
    if (ERR_peek_error() == 0)
      return SystemError(socketError);
  }
#ifdef SSL_R_UNEXPECTED_EOF_WHILE_READING
  // OpenSSL 3 reports a server that closes without close_notify this way.
  if (sslError == SSL_ERROR_SSL && ERR_GET_REASON(ERR_peek_error()) == SSL_R_UNEXPECTED_EOF_WHILE_READING) {
    closed = true;
    ERR_clear_error();
    return "connection closed";
  }
#endif
  return TlsQueueText();
}

// SNI carries DNS names only (RFC 6066); IP literals are still checked by
// SSL_set1_host against the certificate's IP addresses.
bool IsIpLiteral(const std::string& host) {
  addrinfo hints{};
  hints.ai_flags = AI_NUMERICHOST;
  addrinfo* result = nullptr;
  const bool numeric = getaddrinfo(host.c_str(), nullptr, &hints, &result) == 0;
  if (result != nullptr)
    freeaddrinfo(result);
  return numeric;
}

bool SendAllTls(SSL* ssl, int storedSocket, const std::string& data, int timeoutMs, std::string& error,
                bool& fatal) {
  const bool infinite = timeoutMs <= 0;
  const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(std::max(timeoutMs, 0));
  size_t sent = 0;
  while (sent < data.size()) {
    // A retry after WANT_READ/WANT_WRITE must repeat the same buffer and
    // length, which it does because `sent` has not moved.
    const int chunk = static_cast<int>(std::min<size_t>(data.size() - sent, static_cast<size_t>(INT_MAX)));
    int count = 0;
    int sslError = SSL_ERROR_NONE;
    {
      SigpipeGuard guard;
      ERR_clear_error();
      count = SSL_write(ssl, data.data() + sent, chunk);
      if (count <= 0)
        sslError = SSL_get_error(ssl, count);
    }
    if (count > 0) {
      sent += static_cast<size_t>(count);
      continue;
    }
    if (sslError != SSL_ERROR_WANT_READ && sslError != SSL_ERROR_WANT_WRITE) {
      bool closed = false;
      const std::string detail = TlsFailureText(sslError, closed);
      fatal = true;
      error = closed ? "socket closed during send" : "send failed: " + detail;
      return false;
    }
    const int remaining = RemainingMs(deadline, infinite);
    if (remaining < 0) {
      error = "send timed out";
      return false;
    }
    const int ready = WaitReady(storedSocket, sslError == SSL_ERROR_WANT_READ, remaining);
    if (ready == 0) {
      error = "send timed out";
      return false;
    }
    if (ready < 0) {
      const int socketError = SocketError();
      if (IsInterrupted(socketError))
        continue;
      error = "send wait failed: " + SystemError(socketError);
      return false;
    }
  }
  return true;
}
#endif

} // namespace

bool TlsAvailable() {
#ifdef MP_HAVE_OPENSSL
  return true;
#else
  return false;
#endif
}

bool ParseUrl(const std::string& url, std::string& host, uint16_t& port, std::string& path) {
  std::string parsedHost;
  uint16_t parsedPort = 0;
  std::string parsedPath;
  bool secure = false;
  if (!ParseUrl(url, parsedHost, parsedPort, parsedPath, secure) || secure)
    return false;
  host = std::move(parsedHost);
  port = parsedPort;
  path = std::move(parsedPath);
  return true;
}

bool ParseUrl(const std::string& url, std::string& host, uint16_t& port, std::string& path,
              bool& secure) {
  constexpr std::string_view plainScheme = "ws://";
  constexpr std::string_view secureScheme = "wss://";
  bool parsedSecure = false;
  size_t authorityStart = 0;
  if (url.compare(0, plainScheme.size(), plainScheme) == 0) {
    authorityStart = plainScheme.size();
  } else if (url.compare(0, secureScheme.size(), secureScheme) == 0) {
    authorityStart = secureScheme.size();
    parsedSecure = true;
  } else {
    return false;
  }
  const size_t pathStart = url.find('/', authorityStart);
  const size_t authorityEnd = pathStart == std::string::npos ? url.size() : pathStart;
  const std::string authority = url.substr(authorityStart, authorityEnd - authorityStart);
  if (authority.empty() || authority.find('@') != std::string::npos ||
      !ValidUrlCharacter(authority, false))
    return false;

  std::string parsedHost;
  uint16_t parsedPort = parsedSecure ? 443 : 80;
  std::string_view portText;
  if (authority.front() == '[') {
    const size_t close = authority.find(']');
    if (close == std::string::npos || close == 1)
      return false;
    parsedHost = authority.substr(1, close - 1);
    const std::string_view suffix(authority.data() + close + 1, authority.size() - close - 1);
    if (!suffix.empty()) {
      if (suffix.front() != ':' || suffix.size() == 1)
        return false;
      portText = suffix.substr(1);
    }
  } else {
    if (authority.find('[') != std::string::npos || authority.find(']') != std::string::npos)
      return false;
    const size_t colon = authority.find(':');
    if (colon == std::string::npos) {
      parsedHost = authority;
    } else {
      if (authority.find(':', colon + 1) != std::string::npos || colon == 0 || colon + 1 == authority.size())
        return false;
      parsedHost = authority.substr(0, colon);
      portText = std::string_view(authority).substr(colon + 1);
    }
  }
  if (parsedHost.empty() || parsedHost.find_first_of("/?#\\") != std::string::npos)
    return false;
  if (!portText.empty()) {
    unsigned parsed = 0;
    const auto result = std::from_chars(portText.data(), portText.data() + portText.size(), parsed);
    if (result.ec != std::errc() || result.ptr != portText.data() + portText.size() || parsed == 0 ||
        parsed > 65535)
      return false;
    parsedPort = static_cast<uint16_t>(parsed);
  } else if ((!authority.empty() && authority.back() == ':') ||
             (authority.front() == '[' && authority.back() == ':' )) {
    return false;
  }

  std::string parsedPath = pathStart == std::string::npos ? "/" : url.substr(pathStart);
  if (parsedPath.find('#') != std::string::npos || !ValidUrlCharacter(parsedPath, true))
    return false;
  host = std::move(parsedHost);
  port = parsedPort;
  path = std::move(parsedPath);
  secure = parsedSecure;
  return true;
}

std::string Base64Encode(const void* data, size_t size) {
  static constexpr char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const auto* bytes = static_cast<const uint8_t*>(data);
  std::string result;
  result.reserve(((size + 2) / 3) * 4);
  for (size_t i = 0; i < size; i += 3) {
    const size_t remaining = size - i;
    const uint32_t value = (static_cast<uint32_t>(bytes[i]) << 16) |
                           (remaining > 1 ? static_cast<uint32_t>(bytes[i + 1]) << 8 : 0) |
                           (remaining > 2 ? static_cast<uint32_t>(bytes[i + 2]) : 0);
    result.push_back(alphabet[(value >> 18) & 0x3f]);
    result.push_back(alphabet[(value >> 12) & 0x3f]);
    result.push_back(remaining > 1 ? alphabet[(value >> 6) & 0x3f] : '=');
    result.push_back(remaining > 2 ? alphabet[value & 0x3f] : '=');
  }
  return result;
}

void Sha1(const void* data, size_t size, uint8_t out[20]) {
  std::vector<uint8_t> message;
  const auto* bytes = static_cast<const uint8_t*>(data);
  if (size != 0)
    message.assign(bytes, bytes + size);
  const uint64_t bitLength = static_cast<uint64_t>(size) * 8u;
  message.push_back(0x80);
  while ((message.size() & 63u) != 56u)
    message.push_back(0);
  for (int i = 7; i >= 0; --i)
    message.push_back(static_cast<uint8_t>(bitLength >> (i * 8)));

  uint32_t h0 = 0x67452301;
  uint32_t h1 = 0xefcdab89;
  uint32_t h2 = 0x98badcfe;
  uint32_t h3 = 0x10325476;
  uint32_t h4 = 0xc3d2e1f0;
  for (size_t offset = 0; offset < message.size(); offset += 64) {
    uint32_t words[80]{};
    for (size_t i = 0; i < 16; ++i) {
      const size_t index = offset + i * 4;
      words[i] = (static_cast<uint32_t>(message[index]) << 24) |
                 (static_cast<uint32_t>(message[index + 1]) << 16) |
                 (static_cast<uint32_t>(message[index + 2]) << 8) |
                 static_cast<uint32_t>(message[index + 3]);
    }
    for (size_t i = 16; i < 80; ++i) {
      const uint32_t value = words[i - 3] ^ words[i - 8] ^ words[i - 14] ^ words[i - 16];
      words[i] = (value << 1) | (value >> 31);
    }
    uint32_t a = h0;
    uint32_t b = h1;
    uint32_t c = h2;
    uint32_t d = h3;
    uint32_t e = h4;
    for (uint32_t i = 0; i < 80; ++i) {
      uint32_t function;
      uint32_t constant;
      if (i < 20) {
        function = (b & c) | ((~b) & d);
        constant = 0x5a827999;
      } else if (i < 40) {
        function = b ^ c ^ d;
        constant = 0x6ed9eba1;
      } else if (i < 60) {
        function = (b & c) | (b & d) | (c & d);
        constant = 0x8f1bbcdc;
      } else {
        function = b ^ c ^ d;
        constant = 0xca62c1d6;
      }
      const uint32_t rotatedA = (a << 5) | (a >> 27);
      const uint32_t temporary = rotatedA + function + e + constant + words[i];
      e = d;
      d = c;
      c = (b << 30) | (b >> 2);
      b = a;
      a = temporary;
    }
    h0 += a;
    h1 += b;
    h2 += c;
    h3 += d;
    h4 += e;
  }
  const uint32_t digest[5] = {h0, h1, h2, h3, h4};
  for (size_t i = 0; i < 5; ++i) {
    out[i * 4] = static_cast<uint8_t>(digest[i] >> 24);
    out[i * 4 + 1] = static_cast<uint8_t>(digest[i] >> 16);
    out[i * 4 + 2] = static_cast<uint8_t>(digest[i] >> 8);
    out[i * 4 + 3] = static_cast<uint8_t>(digest[i]);
  }
}

std::string AcceptKey(const std::string& clientKey) {
  const std::string input = clientKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  uint8_t digest[20];
  Sha1(input.data(), input.size(), digest);
  return Base64Encode(digest, sizeof(digest));
}

std::string EncodeFrame(uint8_t opcode, const std::string& payload, uint32_t maskSeed) {
  std::string frame;
  const uint64_t length = payload.size();
  frame.reserve(payload.size() + 14);
  frame.push_back(static_cast<char>(0x80 | (opcode & 0x0f)));
  if (length < 126) {
    frame.push_back(static_cast<char>(0x80 | length));
  } else if (length <= 0xffff) {
    frame.push_back(static_cast<char>(0x80 | 126));
    frame.push_back(static_cast<char>((length >> 8) & 0xff));
    frame.push_back(static_cast<char>(length & 0xff));
  } else {
    frame.push_back(static_cast<char>(0x80 | 127));
    for (int i = 7; i >= 0; --i)
      frame.push_back(static_cast<char>((length >> (i * 8)) & 0xff));
  }
  // The mask key is the four little-endian bytes of maskSeed; payload byte i
  // is XORed with key[i mod 4].
  const uint8_t mask[4] = {static_cast<uint8_t>(maskSeed), static_cast<uint8_t>(maskSeed >> 8),
                           static_cast<uint8_t>(maskSeed >> 16), static_cast<uint8_t>(maskSeed >> 24)};
  for (uint8_t byte : mask)
    frame.push_back(static_cast<char>(byte));
  for (size_t i = 0; i < payload.size(); ++i)
    frame.push_back(static_cast<char>(static_cast<uint8_t>(payload[i]) ^ mask[i & 3]));
  return frame;
}

size_t FrameDecoder::Feed(const char* data, size_t size, std::vector<Frame>& out) {
  const size_t initialCount = out.size();
  if (mFailed)
    return 0;
  if (size != 0 && data == nullptr) {
    mFailed = true;
    return 0;
  }
  if (size != 0)
    mBuffer.append(data, size);

  size_t cursor = 0;
  while (mBuffer.size() - cursor >= 2) {
    const uint8_t first = static_cast<uint8_t>(mBuffer[cursor]);
    const uint8_t second = static_cast<uint8_t>(mBuffer[cursor + 1]);
    const bool final = (first & 0x80) != 0;
    const uint8_t opcode = first & 0x0f;
    const bool masked = (second & 0x80) != 0;
    if ((first & 0x70) != 0) {
      mFailed = true;
      break;
    }
    const bool control = opcode >= 0x8;
    if (control) {
      if ((opcode != 0x8 && opcode != 0x9 && opcode != 0xa) || !final || (second & 0x7f) > 125) {
        mFailed = true;
        break;
      }
    } else if (opcode != 0x0 && opcode != 0x1 && opcode != 0x2) {
      mFailed = true;
      break;
    }

    uint64_t length = second & 0x7f;
    size_t headerSize = 2;
    if (length == 126) {
      if (mBuffer.size() - cursor < 4)
        break;
      length = (static_cast<uint8_t>(mBuffer[cursor + 2]) << 8) |
               static_cast<uint8_t>(mBuffer[cursor + 3]);
      headerSize += 2;
      if (length < 126) {
        mFailed = true;
        break;
      }
    } else if (length == 127) {
      if (mBuffer.size() - cursor < 10)
        break;
      if ((static_cast<uint8_t>(mBuffer[cursor + 2]) & 0x80) != 0) {
        mFailed = true;
        break;
      }
      length = 0;
      for (size_t i = 0; i < 8; ++i)
        length = (length << 8) | static_cast<uint8_t>(mBuffer[cursor + 2 + i]);
      headerSize += 8;
      if (length <= 0xffff) {
        mFailed = true;
        break;
      }
    }
    if (control && length > 125) {
      mFailed = true;
      break;
    }
    if (length > kMaxMessageSize) {
      mFailed = true;
      break;
    }
    if (masked)
      headerSize += 4;
    if (mBuffer.size() - cursor < headerSize || length > mBuffer.size() - cursor - headerSize)
      break;
    if (opcode == 0x0) {
      if (!mInMessage || length > kMaxMessageSize - mMessage.size()) {
        mFailed = true;
        break;
      }
    } else if (!control) {
      if (mInMessage || length > kMaxMessageSize) {
        mFailed = true;
        break;
      }
    }

    const size_t payloadStart = cursor + headerSize;
    const size_t maskStart = masked ? payloadStart - 4 : 0;
    std::string payload;
    payload.reserve(static_cast<size_t>(length));
    for (size_t i = 0; i < static_cast<size_t>(length); ++i) {
      uint8_t byte = static_cast<uint8_t>(mBuffer[payloadStart + i]);
      if (masked)
        byte ^= static_cast<uint8_t>(mBuffer[maskStart + (i & 3)]);
      payload.push_back(static_cast<char>(byte));
    }
    cursor = payloadStart + static_cast<size_t>(length);

    if (control) {
      out.push_back(Frame{opcode, std::move(payload)});
      continue;
    }
    if (opcode == 0) {
      mMessage.append(payload);
      if (final) {
        out.push_back(Frame{mMessageOpcode, std::move(mMessage)});
        mMessage.clear();
        mMessageOpcode = 0;
        mInMessage = false;
      }
      continue;
    }
    if (final) {
      out.push_back(Frame{opcode, std::move(payload)});
    } else {
      mMessage = std::move(payload);
      mMessageOpcode = opcode;
      mInMessage = true;
    }
  }
  if (cursor != 0)
    mBuffer.erase(0, cursor);
  return out.size() - initialCount;
}

void FrameDecoder::Reset() {
  mBuffer.clear();
  mMessage.clear();
  mMessageOpcode = 0;
  mInMessage = false;
  mFailed = false;
}

Client::Client() = default;

Client::~Client() { Close(); }

bool Client::Connect(const std::string& host, uint16_t port, const std::string& path, int timeoutMs,
                     bool secure, const TlsOptions& tls) {
  DropConnection();
  mReceiveBuffer.clear();
  mDecoder.Reset();
  mError.clear();
  mTimeoutMs = timeoutMs;
  auto fail = [this](const std::string& reason) {
    mError = reason;
    DropConnection();
    mReceiveBuffer.clear();
    mDecoder.Reset();
    return false;
  };

  if (!EnsureWinsock())
    return fail("WSAStartup failed");
  if (host.empty() || host.find_first_of("\r\n\t /\\") != std::string::npos || port == 0 ||
      path.empty() || path.front() != '/' || path.find_first_of("\r\n") != std::string::npos)
    return fail("invalid WebSocket endpoint");
#ifndef MP_HAVE_OPENSSL
  // Never downgrade a wss:// server to plaintext.
  if (secure)
    return fail("wss:// is not supported: this build has no TLS (built without OpenSSL)");
  (void)tls;
#endif

  const bool infinite = timeoutMs <= 0;
  const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(std::max(timeoutMs, 0));
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* addresses = nullptr;
  const std::string service = std::to_string(port);
  const int resolveResult = getaddrinfo(host.c_str(), service.c_str(), &hints, &addresses);
  if (resolveResult != 0)
    return fail("getaddrinfo failed: " + std::to_string(resolveResult));

  std::string connectError = "connect failed";
  for (addrinfo* address = addresses; address != nullptr; address = address->ai_next) {
    const NativeSocket candidate = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (candidate == kInvalidSocket) {
      connectError = "socket failed: " + SystemError(SocketError());
      continue;
    }
    if (!SetNonBlocking(candidate)) {
      connectError = "could not set nonblocking socket: " + SystemError(SocketError());
      CloseNative(candidate);
      continue;
    }
    int result = ::connect(candidate, address->ai_addr, static_cast<int>(address->ai_addrlen));
    bool connected = result == 0;
    if (!connected) {
      const int socketError = SocketError();
      if (IsWouldBlock(socketError)) {
        const int remaining = RemainingMs(deadline, infinite);
        if (remaining < 0) {
          connectError = "connect timed out";
        } else {
          const int ready = WaitReady(StoreSocket(candidate), false, remaining);
          if (ready > 0) {
            int pendingError = 0;
#ifdef _WIN32
            int optionLength = sizeof(pendingError);
#else
            socklen_t optionLength = sizeof(pendingError);
#endif
            if (getsockopt(candidate, SOL_SOCKET, SO_ERROR,
                           reinterpret_cast<char*>(&pendingError), &optionLength) == 0 && pendingError == 0)
              connected = true;
            else
              connectError = "connect failed: " + SystemError(pendingError == 0 ? SocketError() : pendingError);
          } else if (ready == 0) {
            connectError = "connect timed out";
          } else {
            connectError = "connect wait failed: " + SystemError(SocketError());
          }
        }
      } else {
        connectError = "connect failed: " + SystemError(socketError);
      }
    }
    if (connected) {
      const int stored = StoreSocket(candidate);
      if (stored >= 0) {
        mSocket = stored;
        break;
      }
      connectError = "socket handle cannot be represented by this client";
    }
    CloseNative(candidate);
  }
  freeaddrinfo(addresses);
  if (mSocket < 0)
    return fail(connectError);

#ifdef MP_HAVE_OPENSSL
  if (secure) {
#ifdef SO_NOSIGPIPE
    int noSigpipe = 1;
    setsockopt(NativeFromStored(mSocket), SOL_SOCKET, SO_NOSIGPIPE, &noSigpipe, sizeof(noSigpipe));
#endif
    ERR_clear_error();
    mSslContext = SSL_CTX_new(TLS_client_method());
    if (mSslContext == nullptr)
      return fail("could not create TLS context: " + TlsQueueText());
    SSL_CTX_set_min_proto_version(mSslContext, TLS1_2_VERSION);
    SSL_CTX_set_verify(mSslContext, SSL_VERIFY_PEER, nullptr);
    if (tls.caFile.empty()) {
      if (SSL_CTX_set_default_verify_paths(mSslContext) != 1)
        return fail("could not load the system TLS trust store: " + TlsQueueText());
    } else if (SSL_CTX_load_verify_locations(mSslContext, tls.caFile.c_str(), nullptr) != 1) {
      return fail("could not load TLS CA file " + tls.caFile + ": " + TlsQueueText());
    }
    mSsl = SSL_new(mSslContext);
    if (mSsl == nullptr)
      return fail("could not create TLS session: " + TlsQueueText());
    if (SSL_set_fd(mSsl, mSocket) != 1)
      return fail("could not attach TLS to the socket: " + TlsQueueText());
    // The certificate must name this host; a valid certificate for any other
    // name fails verification.
    if (SSL_set1_host(mSsl, host.c_str()) != 1)
      return fail("could not set the TLS host name: " + TlsQueueText());
    if (!IsIpLiteral(host) && SSL_set_tlsext_host_name(mSsl, host.c_str()) != 1)
      return fail("could not set the TLS server name: " + TlsQueueText());

    for (;;) {
      int result = 0;
      int sslError = SSL_ERROR_NONE;
      {
        SigpipeGuard guard;
        ERR_clear_error();
        result = SSL_connect(mSsl);
        if (result != 1)
          sslError = SSL_get_error(mSsl, result);
      }
      if (result == 1)
        break;
      if (sslError != SSL_ERROR_WANT_READ && sslError != SSL_ERROR_WANT_WRITE) {
        mTlsFailed = true;
        const long verifyResult = SSL_get_verify_result(mSsl);
        if (verifyResult != X509_V_OK) {
          ERR_clear_error();
          return fail(std::string("TLS certificate verification failed: ") +
                      X509_verify_cert_error_string(verifyResult));
        }
        bool closed = false;
        const std::string detail = TlsFailureText(sslError, closed);
        return fail(closed ? "server closed during TLS handshake" : "TLS handshake failed: " + detail);
      }
      const int remaining = RemainingMs(deadline, infinite);
      if (remaining < 0)
        return fail("TLS handshake timed out");
      const int ready = WaitReady(mSocket, sslError == SSL_ERROR_WANT_READ, remaining);
      if (ready == 0)
        return fail("TLS handshake timed out");
      if (ready < 0) {
        const int socketError = SocketError();
        if (IsInterrupted(socketError))
          continue;
        return fail("TLS handshake wait failed: " + SystemError(socketError));
      }
    }
  }
#endif

  uint8_t randomKey[16];
  try {
    std::random_device random;
    for (uint8_t& byte : randomKey)
      byte = static_cast<uint8_t>(random() & 0xffu);
  } catch (...) {
    return fail("could not generate WebSocket key");
  }
  const std::string clientKey = Base64Encode(randomKey, sizeof(randomKey));
  std::string hostHeader = host;
  if (host.find(':') != std::string::npos && (host.empty() || host.front() != '['))
    hostHeader = "[" + host + "]";
  if (port != (secure ? 443 : 80))
    hostHeader += ":" + std::to_string(port);
  const std::string request = "GET " + path + " HTTP/1.1\r\nHost: " + hostHeader +
                              "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " +
                              clientKey + "\r\nSec-WebSocket-Version: 13\r\n\r\n";
  std::string ioError;
  const int remainingForSend = RemainingMs(deadline, infinite);
  if (remainingForSend < 0 || !SendBytes(request, remainingForSend, ioError))
    return fail(remainingForSend < 0 ? "handshake timed out" : ioError);

  size_t headerEnd = std::string::npos;
  while (headerEnd == std::string::npos) {
    if (mReceiveBuffer.size() > 16 * 1024)
      return fail("WebSocket response headers too large");
    const int remaining = RemainingMs(deadline, infinite);
    if (remaining < 0)
      return fail("handshake timed out");
    const int ready = WaitIo(remaining);
    if (ready == 0)
      return fail("handshake timed out");
    if (ready < 0) {
      const int socketError = SocketError();
      if (IsInterrupted(socketError))
        continue;
      return fail("handshake read wait failed: " + SystemError(socketError));
    }
    std::string bytes;
    bool closed = false;
    std::string readError;
    if (!ReadRaw(bytes, closed, readError))
      return fail(closed ? "server closed during handshake" : "handshake read failed: " + readError);
    mReceiveBuffer.append(bytes);
    headerEnd = mReceiveBuffer.find("\r\n\r\n");
    if (headerEnd == std::string::npos && mReceiveBuffer.size() > 16 * 1024)
      return fail("WebSocket response headers too large");
  }
  if (headerEnd + 4 > 16 * 1024)
    return fail("WebSocket response headers too large");

  const std::string responseHeaders = mReceiveBuffer.substr(0, headerEnd);
  mReceiveBuffer.erase(0, headerEnd + 4); // Preserve any first frame bytes received with the headers.
  const size_t firstLineEnd = responseHeaders.find("\r\n");
  const std::string_view statusLine(responseHeaders.data(),
                                    firstLineEnd == std::string::npos ? responseHeaders.size() : firstLineEnd);
  if (statusLine.size() < 12 || statusLine.substr(0, 9) != "HTTP/1.1 " ||
      statusLine.substr(9, 3) != "101" || (statusLine.size() > 12 && statusLine[12] != ' '))
    return fail("WebSocket upgrade did not return HTTP 101");

  std::string accept;
  std::string upgrade;
  std::string connection;
  size_t lineStart = firstLineEnd == std::string::npos ? responseHeaders.size() : firstLineEnd + 2;
  while (lineStart < responseHeaders.size()) {
    const size_t lineEnd = responseHeaders.find("\r\n", lineStart);
    const size_t end = lineEnd == std::string::npos ? responseHeaders.size() : lineEnd;
    const std::string_view line(responseHeaders.data() + lineStart, end - lineStart);
    const size_t colon = line.find(':');
    if (colon == std::string_view::npos)
      return fail("malformed WebSocket response header");
    const std::string name = Lower(std::string(Trim(line.substr(0, colon))));
    const std::string value(Trim(line.substr(colon + 1)));
    if (name == "sec-websocket-accept") {
      if (!accept.empty())
        return fail("duplicate Sec-WebSocket-Accept header");
      accept = value;
    } else if (name == "upgrade") {
      upgrade = value;
    } else if (name == "connection") {
      connection = value;
    }
    lineStart = lineEnd == std::string::npos ? responseHeaders.size() : lineEnd + 2;
  }
  if (accept != AcceptKey(clientKey))
    return fail("invalid Sec-WebSocket-Accept header");
  if (Lower(upgrade) != "websocket" || !HasToken(connection, "upgrade"))
    return fail("invalid WebSocket upgrade headers");
  mError.clear();
  return true;
}

void Client::Close() {
  if (mSocket >= 0) {
    uint32_t seed = 0;
    if (RandomSeed(seed))
      SendRaw(EncodeFrame(0x8, std::string(), seed));
    else
      mError = "could not generate close-frame mask";
    ShutdownTls();
  }
  DropConnection();
  mReceiveBuffer.clear();
  mDecoder.Reset();
}

bool Client::SendText(const std::string& message) {
  if (mSocket < 0) {
    mError = "not connected";
    return false;
  }
  uint32_t seed = 0;
  if (!RandomSeed(seed)) {
    mError = "could not generate frame mask";
    return false;
  }
  return SendRaw(EncodeFrame(0x1, message, seed));
}

bool Client::ReceiveText(std::string& message, int timeoutMs) {
  if (mSocket < 0) {
    mError = "not connected";
    return false;
  }
  const bool infinite = timeoutMs <= 0;
  const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(std::max(timeoutMs, 0));
  for (;;) {
    size_t consumed = 0;
    while (consumed < mReceiveBuffer.size()) {
      std::vector<Frame> frames;
      const size_t appended = mDecoder.Feed(mReceiveBuffer.data() + consumed, 1, frames);
      ++consumed;
      if (mDecoder.Failed()) {
        mError = "invalid WebSocket frame";
        DropConnection();
        mReceiveBuffer.clear();
        return false;
      }
      for (const Frame& frame : frames) {
        if (frame.opcode == 0x1) {
          message = frame.payload;
          mReceiveBuffer.erase(0, consumed);
          return true;
        }
        if (frame.opcode == 0x8) {
          uint32_t seed = 0;
          if (RandomSeed(seed))
            SendRaw(EncodeFrame(0x8, frame.payload, seed));
          ShutdownTls();
          DropConnection();
          mReceiveBuffer.clear();
          mDecoder.Reset();
          mError = "server closed WebSocket";
          return false;
        }
        if (frame.opcode == 0x9) {
          uint32_t seed = 0;
          if (!RandomSeed(seed) || !SendRaw(EncodeFrame(0xa, frame.payload, seed))) {
            DropConnection();
            return false;
          }
        } else if (frame.opcode != 0xa) {
          mError = "unsupported WebSocket data opcode";
          DropConnection();
          mReceiveBuffer.clear();
          return false;
        }
      }
      (void)appended;
    }
    mReceiveBuffer.clear();

    const int remaining = RemainingMs(deadline, infinite);
    if (remaining < 0) {
      mError = "receive timed out";
      return false;
    }
    const int ready = WaitIo(remaining);
    if (ready == 0) {
      mError = "receive timed out";
      return false;
    }
    if (ready < 0) {
      const int socketError = SocketError();
      if (IsInterrupted(socketError))
        continue;
      mError = "receive wait failed: " + SystemError(socketError);
      return false;
    }
    std::string bytes;
    bool closed = false;
    std::string readError;
    if (!ReadRaw(bytes, closed, readError)) {
      mError = closed ? "server closed the connection" : "receive failed: " + readError;
      if (closed)
        ShutdownTls();
      DropConnection();
      return false;
    }
    if (!bytes.empty())
      mReceiveBuffer = std::move(bytes);
  }
}

bool Client::WaitReadable(int timeoutMs) {
  if (mSocket < 0) {
    mError = "not connected";
    return false;
  }
  const int ready = WaitIo(timeoutMs);
  if (ready > 0)
    return true;
  if (ready == 0)
    mError = "receive timed out";
  else
    mError = "receive wait failed: " + SystemError(SocketError());
  return false;
}

bool Client::SendRaw(const std::string& data) {
  if (mSocket < 0) {
    mError = "not connected";
    return false;
  }
  std::string error;
  if (!SendBytes(data, mTimeoutMs, error)) {
    mError = std::move(error);
    return false;
  }
  return true;
}

bool Client::SendBytes(const std::string& data, int timeoutMs, std::string& error) {
#ifdef MP_HAVE_OPENSSL
  if (mSsl != nullptr) {
    bool fatal = false;
    const bool sent = SendAllTls(mSsl, mSocket, data, timeoutMs, error, fatal);
    if (fatal)
      mTlsFailed = true;
    return sent;
  }
#endif
  return SendAll(mSocket, data, timeoutMs, error);
}

bool Client::ReadRaw(std::string& out, bool& closed, std::string& error) {
  closed = false;
  char buffer[4096];
#ifdef MP_HAVE_OPENSSL
  if (mSsl != nullptr) {
    int count = 0;
    int sslError = SSL_ERROR_NONE;
    {
      // Reads can write too (key updates), so they need the guard as well.
      SigpipeGuard guard;
      ERR_clear_error();
      count = SSL_read(mSsl, buffer, sizeof(buffer));
      if (count <= 0)
        sslError = SSL_get_error(mSsl, count);
    }
    mTlsReadWantsWrite = sslError == SSL_ERROR_WANT_WRITE;
    if (count > 0) {
      out.assign(buffer, static_cast<size_t>(count));
      return true;
    }
    if (sslError == SSL_ERROR_WANT_READ || sslError == SSL_ERROR_WANT_WRITE)
      return true;
    error = TlsFailureText(sslError, closed);
    // Only a clean close_notify leaves the session fit for our own shutdown.
    if (sslError != SSL_ERROR_ZERO_RETURN)
      mTlsFailed = true;
    return false;
  }
#endif
#ifdef _WIN32
  const int count = ::recv(NativeFromStored(mSocket), buffer, sizeof(buffer), 0);
#else
  const ssize_t count = ::recv(NativeFromStored(mSocket), buffer, sizeof(buffer), 0);
#endif
  if (count > 0) {
    out.assign(buffer, static_cast<size_t>(count));
    return true;
  }
  if (count == 0) {
    closed = true;
    return false;
  }
  const int socketError = SocketError();
  if (IsInterrupted(socketError) || IsWouldBlock(socketError))
    return true;
  error = SystemError(socketError);
  return false;
}

int Client::WaitIo(int timeoutMs) {
#ifdef MP_HAVE_OPENSSL
  if (mSsl != nullptr) {
    // Decrypted bytes, or a whole record OpenSSL has already pulled off the
    // socket, would never make select() fire.
    if (SSL_has_pending(mSsl) == 1)
      return 1;
    return WaitReady(mSocket, !mTlsReadWantsWrite, timeoutMs);
  }
#endif
  return WaitReady(mSocket, true, timeoutMs);
}

void Client::ShutdownTls() {
#ifdef MP_HAVE_OPENSSL
  if (mSsl != nullptr && !mTlsFailed) {
    SigpipeGuard guard;
    ERR_clear_error();
    // Sends close_notify without waiting for the server's; the socket closes
    // right after.
    SSL_shutdown(mSsl);
    ERR_clear_error();
  }
#endif
}

void Client::DropConnection() {
#ifdef MP_HAVE_OPENSSL
  SSL_free(mSsl);
  mSsl = nullptr;
  SSL_CTX_free(mSslContext);
  mSslContext = nullptr;
#endif
  mTlsFailed = false;
  mTlsReadWantsWrite = false;
  if (mSocket >= 0) {
    CloseNative(NativeFromStored(mSocket));
    mSocket = -1;
  }
}

} // namespace PortWs
