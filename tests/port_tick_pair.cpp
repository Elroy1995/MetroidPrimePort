#include "port_tick_pair.h"

#include <cstdio>
#include <cstdlib>

namespace {
int sLine = 0;
void Check(bool condition) {
  if (!condition) {
    std::fprintf(stderr, "tick pair regression failed at line %d\n", sLine);
    std::abort();
  }
}
#define CHECK(cond)                                                                                \
  do {                                                                                             \
    sLine = __LINE__;                                                                              \
    Check(cond);                                                                                   \
  } while (0)
} // namespace

int main() {
  // The first value drawn in each generation is kept; a blend needs the
  // previous generation's value.
  {
    PortTickPair< int > p;
    CHECK(!p.Note(10, 0));
    CHECK(!p.Note(10, 1));
    CHECK(!p.Valid(1));
    CHECK(p.Note(20, 2));
    CHECK(p.prev == 10 && p.cur == 20);
    // Later draws in the same generation don't overwrite it.
    CHECK(p.Note(99, 2));
    CHECK(p.prev == 10 && p.cur == 20);
    CHECK(p.Note(30, 3));
    CHECK(p.prev == 20 && p.cur == 30);
  }
  // A skipped generation (not drawn for a tick) snaps instead of blending
  // across two ticks.
  {
    PortTickPair< int > p;
    p.Note(1, 5);
    CHECK(p.Note(2, 6));
    CHECK(!p.Note(3, 8));
    CHECK(p.cur == 3);
    CHECK(p.Note(4, 9));
    CHECK(p.prev == 3 && p.cur == 4);
  }
  // Valid() only answers for the generation the pair was last noted in.
  {
    PortTickPair< int > p;
    p.Note(1, 1);
    p.Note(2, 2);
    CHECK(p.Valid(2));
    CHECK(!p.Valid(3));
    p.Reset();
    CHECK(!p.Valid(2));
    CHECK(!p.Note(5, 3));
  }
  // Types without a default constructor pass a start value.
  {
    struct NoDefault {
      explicit NoDefault(int v) : value(v) {}
      int value;
    };
    PortTickPair< NoDefault > p(NoDefault(7));
    CHECK(p.prev.value == 7 && p.cur.value == 7);
    p.Note(NoDefault(1), 1);
    CHECK(p.Note(NoDefault(2), 2));
    CHECK(p.prev.value == 1 && p.cur.value == 2);
  }
  std::puts("port_tick_pair_tests passed");
  return 0;
}
