#include "port_log_file.h"

#include "port_paths.h"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <system_error>

#if !defined(_WIN32) && !defined(__ANDROID__)
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace PortLogFile {
namespace {
bool sActive = false;

#if !defined(_WIN32) && !defined(__ANDROID__)
void WriteAll(int fd, const char* data, ssize_t size) {
  while (size > 0) {
    const ssize_t n = write(fd, data, static_cast< size_t >(size));
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return;
    }
    data += n;
    size -= n;
  }
}

// The copying process: pipe -> terminal + file until every writer has gone. Only
// async-signal-safe calls, since the game may already have threads when it forks.
[[noreturn]] void Copy(int in, int terminal, int file, int maxFd) {
  for (int fd = 3; fd < maxFd; ++fd) {
    if (fd != in && fd != terminal && fd != file) {
      close(fd);
    }
  }
  // Ctrl+C in the terminal reaches this process too; it must keep draining until
  // the game has printed its last line.
  signal(SIGINT, SIG_IGN);
  signal(SIGQUIT, SIG_IGN);
  signal(SIGPIPE, SIG_IGN);
  char buffer[8192];
  for (;;) {
    const ssize_t n = read(in, buffer, sizeof(buffer));
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      break;
    }
    WriteAll(terminal, buffer, n);
    WriteAll(file, buffer, n);
  }
  _exit(0);
}
#endif
} // namespace

std::string Path() {
  const std::string& folder = PortPaths::UserFolder();
  return folder.empty() ? std::string() : folder + "metroid_prime_port.log";
}

bool Active() { return sActive; }

bool Start() {
#if defined(__ANDROID__)
  return false;
#else
  if (sActive) {
    return true;
  }
  const std::string path = Path();
  if (path.empty()) {
    return false;
  }
  std::error_code ec;
  const std::filesystem::path file(path);
  if (std::filesystem::exists(file, ec)) {
    std::filesystem::path old = file;
    old.replace_extension(".old.log");
    std::filesystem::rename(file, old, ec);
  }
  std::fflush(stdout);
  std::fflush(stderr);
  char started[64] = "";
  const std::time_t now = std::time(nullptr);
  if (const std::tm* local = std::localtime(&now)) {
    std::strftime(started, sizeof(started), "%Y-%m-%d %H:%M:%S", local);
  }
#if defined(_WIN32)
  // A GUI program may start without a console, so reopen the streams themselves
  // rather than their descriptors.
  if (_wfreopen(file.c_str(), L"w", stdout) == nullptr) {
    return false;
  }
  std::fprintf(stdout, "metroid_prime_port log, started %s\n", started);
  std::fflush(stdout);
  if (_wfreopen(file.c_str(), L"a", stderr) == nullptr) {
    return false;
  }
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::setvbuf(stderr, nullptr, _IONBF, 0);
#else
  const int out = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (out < 0) {
    return false;
  }
  char header[128];
  const int headerSize = std::snprintf(header, sizeof(header), "metroid_prime_port log, started %s\n", started);
  WriteAll(out, header, headerSize);
  int fds[2];
  const int terminal = dup(STDERR_FILENO);
  if (terminal < 0 || pipe(fds) != 0) {
    close(out);
    if (terminal >= 0) {
      close(terminal);
    }
    return false;
  }
  long maxFd = sysconf(_SC_OPEN_MAX);
  maxFd = maxFd < 256 ? 256 : maxFd > 65536 ? 65536 : maxFd;
  const pid_t child = fork();
  if (child == 0) {
    close(fds[1]);
    Copy(fds[0], terminal, out, static_cast< int >(maxFd));
  }
  close(fds[0]);
  close(out);
  close(terminal);
  if (child < 0) {
    close(fds[1]);
    return false;
  }
  dup2(fds[1], STDOUT_FILENO);
  dup2(fds[1], STDERR_FILENO);
  close(fds[1]);
  // stdout to a pipe is fully buffered, and abort() flushes nothing: one line at a
  // time keeps the last messages before a crash.
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
#endif
  sActive = true;
  std::fprintf(stderr, "port: writing the log to %s\n", path.c_str());
  return true;
#endif
}

} // namespace PortLogFile
