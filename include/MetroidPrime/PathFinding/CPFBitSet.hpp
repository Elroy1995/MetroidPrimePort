#ifndef _CPFBITSET
#define _CPFBITSET

#include "types.h"
#ifdef TARGET_PC
#include "rstl/vector.hpp"
#endif

class CPFBitSet {
public:
  CPFBitSet() { Clear(); }
#ifdef TARGET_PC
  void Clear() {
    for (int i = 0; i < mBits.size(); ++i)
      mBits[i] = 0;
  }
  void Add(int bit) {
    if (bit < 0) return;
    if (bit / 32 >= mBits.size()) mBits.resize(bit / 32 + 1, 0u);
    mBits[bit / 32] |= 1u << (bit & 31);
  }
  bool Test(int bit) {
    return bit >= 0 && bit / 32 < mBits.size() && (mBits[bit / 32] & (1u << (bit & 31))) != 0;
  }
  void Rmv(int bit) {
    if (bit >= 0 && bit / 32 < mBits.size()) mBits[bit / 32] &= ~(1u << (bit & 31));
  }
#else
  void Clear() {
    for (int i = 0; i < 16; ++i) {
      mBits[i] = 0;
    }
  }
  void Add(int bit) { mBits[bit / 32] |= 1 << (bit & 31); }
  bool Test(int bit) { return (mBits[bit / 32] & (1 << (bit & 31))) != 0; }
  void Rmv(int bit) { mBits[bit / 32] &= ~(1 << (bit & 31)); }
#endif

private:
#ifdef TARGET_PC
  // Grow to the native resource's indices instead of writing through adjacent
  // CPFArea state if an index exceeds the console's fixed 512-bit buffer.
  rstl::vector<uint> mBits;
#else
  uint mBits[16];
#endif
};
CHECK_SIZEOF(CPFBitSet, 0x40)

#endif // _CPFBITSET
