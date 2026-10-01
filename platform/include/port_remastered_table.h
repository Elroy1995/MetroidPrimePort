#pragma once

// The list of Remastered models the importer converts, and the retail model
// each one replaces. Generated; see port_remastered_table.cpp.

#include <cstddef>
#include <cstdint>

namespace PortRemastered {

// What the few models that need more than the defaults get.
struct TableOptions {
  int material;             // retail material every surface is put on, -1 to vote
  int maxTexture;           // largest edge of a TEV path texture
  const char* squeezeRole;  // null for no squeeze; see ConvertOptions
  double squeeze[4];        // from u0, u1 onto lo, hi
};

struct TableEntry {
  uint32_t retail;      // CMDL id
  uint8_t rem[16];      // Remastered model id, in a pak's byte order
  int8_t orient[9];     // gc = orient * remastered + offset, row major
  double offset[3];
  uint16_t firstSkin;   // into TableSkins
  uint16_t skinCount;
  int16_t options;      // into the option sets, -1 for the defaults
  bool pbr;
};

const TableEntry* Table(size_t& count);
const uint32_t* TableSkins(const TableEntry& entry);
const TableOptions* TableExtra(const TableEntry& entry);

} // namespace PortRemastered
