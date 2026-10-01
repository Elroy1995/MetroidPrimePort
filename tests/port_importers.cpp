#include "port_importers.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
int sFailures = 0;

void Check(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++sFailures;
  }
}

void Feed(const char* text, std::string& partial, std::vector<std::string>& lines) {
  PortImporters::SplitLines(text, std::strlen(text), partial, lines);
}

void TestLines() {
  std::string partial;
  std::vector<std::string> lines;
  Feed("one\ntw", partial, lines);
  Check(lines.size() == 1 && lines[0] == "one" && partial == "tw", "a line split across reads waits for its end");
  Feed("o\r\nthree\n", partial, lines);
  Check(lines.size() == 3 && lines[1] == "two" && lines[2] == "three" && partial.empty(), "CRLF ends one line");
  Feed("10%\r20%\r30%", partial, lines);
  Check(lines.size() == 3 && partial == "30%", "a carriage return rewrites the line");
  Feed("\n\n", partial, lines);
  Check(lines.size() == 5 && lines[3] == "30%" && lines[4].empty(), "an empty line is kept");
}

void TestCap() {
  std::string partial;
  std::vector<std::string> lines;
  std::string text;
  for (size_t i = 0; i < PortImporters::kMaxLines + 50; ++i) {
    text += std::to_string(i) + "\n";
  }
  PortImporters::SplitLines(text.data(), text.size(), partial, lines);
  Check(lines.size() == PortImporters::kMaxLines, "the oldest lines are dropped");
  Check(lines.front() == "50" && lines.back() == std::to_string(PortImporters::kMaxLines + 49),
        "the newest lines are the ones kept");
}
} // namespace

int main() {
  TestLines();
  TestCap();
  if (sFailures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", sFailures);
    return 1;
  }
  std::puts("port_importers_tests: ok");
  return 0;
}
