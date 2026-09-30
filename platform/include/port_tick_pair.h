#pragma once

// Frame interpolation helper (docs/FRAME_INTERPOLATION.md): keeps the value an
// effect had at the last two simulation ticks, recorded when it is drawn.
// Effects that are only written at draw time or whose state lives in many
// places (swooshes, beams) snapshot what they draw instead of hooking the sim.
//
// Note() records `now` the first time it is called in tick generation `gen`
// and returns true when `prev` holds the value from generation gen - 1, so a
// blend from prev to cur is valid. A tick without a draw (or several ticks per
// frame) breaks the chain and the next frames draw the current value.
template < typename T >
struct PortTickPair {
  T prev;
  T cur;
  unsigned prevGen = 0;
  unsigned curGen = 0;

  // Types without a default constructor (CTransform4f) pass a start value.
  explicit PortTickPair(const T& init = T()) : prev(init), cur(init) {}

  bool Note(const T& now, unsigned gen) {
    if (curGen != gen) {
      if (curGen != 0 && curGen + 1 == gen) {
        prev = cur;
        prevGen = curGen;
      } else {
        prevGen = 0;
      }
      cur = now;
      curGen = gen;
    }
    return Valid(gen);
  }

  bool Valid(unsigned gen) const {
    return gen != 0 && prevGen != 0 && prevGen + 1 == gen && curGen == gen;
  }
  void Reset() { prevGen = curGen = 0; }
};
