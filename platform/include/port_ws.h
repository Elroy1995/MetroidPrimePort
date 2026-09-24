#ifndef METROID_PRIME_PORT_PORT_WS_H
#define METROID_PRIME_PORT_PORT_WS_H
#include <cstdint>
#include <string>
#include <vector>

// Minimal RFC 6455 WebSocket client: text frames over plain TCP.
//
// Archipelago servers speak WebSocket, not raw TCP, so this is the transport
// under the Archipelago client. It deliberately covers only what that protocol
// needs: an HTTP upgrade handshake, masked client text frames, unmasked server
// frames, ping/pong, and close. No TLS (ws:// only), no extensions, and no
// per-message compression (servers still accept uncompressed connections, but
// it is deprecated on their side).
//
// The socket is blocking with explicit timeouts; the Archipelago client runs it
// on its own thread.
namespace PortWs {

// "ws://host", "ws://host:port", "ws://host:port/path". Returns false for any
// other scheme (including wss://) or an empty host. Defaults: port 80, path "/".
bool ParseUrl(const std::string& url, std::string& host, uint16_t& port, std::string& path);

// A received frame. Opcodes follow RFC 6455: 0x1 text, 0x2 binary, 0x8 close,
// 0x9 ping, 0xA pong. Continuation frames are never surfaced: the decoder
// joins them, so text arrives whole.
struct Frame {
  uint8_t opcode = 0;
  std::string payload;
};

// Client frames must be masked; server frames must not be. Both directions are
// handled so the codec can round-trip in tests.
std::string EncodeFrame(uint8_t opcode, const std::string& payload, uint32_t maskSeed);
// Incremental decoder. Feed() consumes bytes and appends whole frames to `out`,
// returning the number appended. Failed() latches a protocol error (bad length,
// a fragmented control frame, a message over the size limit).
class FrameDecoder {
public:
  static const size_t kMaxMessageSize = 16u * 1024u * 1024u;

  size_t Feed(const char* data, size_t size, std::vector<Frame>& out);
  bool Failed() const { return mFailed; }
  void Reset();

private:
  std::string mBuffer;
  std::string mMessage;
  uint8_t mMessageOpcode = 0;
  bool mInMessage = false;
  bool mFailed = false;
};

// Exposed because they are the fiddly parts; the tests pin them to published
// vectors.
std::string Base64Encode(const void* data, size_t size);
void Sha1(const void* data, size_t size, uint8_t out[20]);
// The Sec-WebSocket-Accept value for a client key, per RFC 6455.
std::string AcceptKey(const std::string& clientKey);

class Client {
public:
  Client();
  ~Client();
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  // Resolves and connects, then performs the upgrade handshake. Blocks up to
  // timeoutMs; false fills Error(). `host`/`port`/`path` come from ParseUrl.
  bool Connect(const std::string& host, uint16_t port, const std::string& path, int timeoutMs);
  // Sends a close frame if open and releases the socket. Idempotent.
  void Close();
  bool IsOpen() const { return mSocket >= 0; }

  // Sends one text message. Blocking with the configured timeout.
  bool SendText(const std::string& message);
  // Waits for a text message, transparently answering pings and ignoring pongs.
  // Returns false on timeout, close, or error (check Error/IsOpen). timeoutMs 0
  // waits indefinitely; a close frame returns false with IsOpen() false.
  bool ReceiveText(std::string& message, int timeoutMs);

  // Wait for up to timeoutMs for the socket to become readable; -1 timeout.
  bool WaitReadable(int timeoutMs);

  // Last error text, never null.
  const char* Error() const { return mError.c_str(); }
  void SetTimeoutMs(int timeoutMs) { mTimeoutMs = timeoutMs; }

private:
  bool SendRaw(const std::string& data);
  bool ReadRaw(std::string& out);

  int mSocket = -1;
  int mTimeoutMs = 10000;
  std::string mError = "not connected";
  std::string mReceiveBuffer;
  FrameDecoder mDecoder;
};

} // namespace PortWs

#endif // METROID_PRIME_PORT_PORT_WS_H
