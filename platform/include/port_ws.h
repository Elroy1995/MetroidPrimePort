#ifndef METROID_PRIME_PORT_PORT_WS_H
#define METROID_PRIME_PORT_PORT_WS_H
#include <cstdint>
#include <string>
#include <vector>

// Minimal RFC 6455 WebSocket client: text frames over TCP, optionally TLS.
//
// Archipelago servers speak WebSocket, not raw TCP, so this is the transport
// under the Archipelago client. It deliberately covers only what that protocol
// needs: an HTTP upgrade handshake, masked client text frames, unmasked server
// frames, ping/pong, and close. No extensions and no per-message compression
// (servers still accept uncompressed connections, but it is deprecated on their
// side).
//
// wss:// uses OpenSSL when the build defines MP_HAVE_OPENSSL. The server
// certificate is always verified against the host name; there is no way to
// turn that off. Without OpenSSL a wss:// connect fails rather than falling
// back to plaintext.
//
// The socket is blocking with explicit timeouts; the Archipelago client runs it
// on its own thread.
struct ssl_st;
struct ssl_ctx_st;

namespace PortWs {

// "ws://host", "ws://host:port", "ws://host:port/path". Returns false for any
// other scheme (including wss://) or an empty host. Defaults: port 80, path "/".
bool ParseUrl(const std::string& url, std::string& host, uint16_t& port, std::string& path);
// Same, but also accepts "wss://..." (default port 443) and reports which
// scheme was used in `secure`. ws:// results match the overload above.
bool ParseUrl(const std::string& url, std::string& host, uint16_t& port, std::string& path,
              bool& secure);

// Settings for wss:// connections. Verification is not optional.
struct TlsOptions {
  // PEM CA bundle to verify the server against. It takes precedence over
  // caDirs; with both empty the platform default is used.
  std::string caFile;
  // Directories of PEM certificate files, one or more per file under any name.
  // Every file of the first directory that yields a certificate is loaded; the
  // rest are not consulted, so a later directory cannot add to an earlier one.
  // No certificate in any of them is an error naming each directory. Empty on
  // Android means Conscrypt's store, then /system's; elsewhere the default is
  // OpenSSL's own.
  std::vector<std::string> caDirs;
};

// Whether this build can connect to wss:// servers.
bool TlsAvailable();

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
  // timeoutMs; false fills Error(). `host`/`port`/`path`/`secure` come from
  // ParseUrl. With `secure`, a TLS handshake that verifies the certificate
  // for `host` runs before the upgrade.
  bool Connect(const std::string& host, uint16_t port, const std::string& path, int timeoutMs,
               bool secure = false, const TlsOptions& tls = TlsOptions());
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
  // Writes everything through TLS or the plain socket; false fills `error`.
  bool SendBytes(const std::string& data, int timeoutMs, std::string& error);
  // One read. True with `out` possibly empty when nothing was ready; false
  // with `closed` set when the peer closed, otherwise `error` has the detail.
  bool ReadRaw(std::string& out, bool& closed, std::string& error);
  // Sends TLS close_notify if the session is still healthy; no-op otherwise.
  void ShutdownTls();
  // Waits until a read can make progress: TLS may already hold buffered data,
  // or need the socket writable first. Returns 1 ready, 0 timeout, -1 error.
  int WaitIo(int timeoutMs);
  // Releases the TLS state and the socket without any shutdown exchange.
  void DropConnection();

  int mSocket = -1;
  ssl_ctx_st* mSslContext = nullptr;
  ssl_st* mSsl = nullptr;
  // Set after a fatal TLS error, when SSL_shutdown must not be attempted.
  bool mTlsFailed = false;
  // The last SSL_read wanted the socket writable (renegotiation, key update).
  bool mTlsReadWantsWrite = false;
  int mTimeoutMs = 10000;
  std::string mError = "not connected";
  std::string mReceiveBuffer;
  FrameDecoder mDecoder;
};

} // namespace PortWs

#endif // METROID_PRIME_PORT_PORT_WS_H
