// Line splitting for importer output (port_importers.h), apart from the process
// code so the tests can build it without SDL.

#include "port_importers.h"

namespace PortImporters {

void SplitLines(const char* text, size_t size, std::string& partial, std::vector<std::string>& lines) {
  for (size_t i = 0; i < size; ++i) {
    const char c = text[i];
    if (c == '\n') {
      lines.push_back(partial);
      partial.clear();
    } else if (c == '\r') {
      // "\r\n" ends the line; a bare "\r" rewrites it.
      if (i + 1 < size && text[i + 1] == '\n') {
        continue;
      }
      partial.clear();
    } else {
      partial += c;
    }
  }
  if (lines.size() > kMaxLines) {
    lines.erase(lines.begin(), lines.end() - kMaxLines);
  }
}

} // namespace PortImporters
