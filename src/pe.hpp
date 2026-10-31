#pragma once

#include "codegen.hpp"

#include <string>
#include <vector>

namespace ron {

enum class Subsystem { Console, Windows };

// Writes a Windows PE32+ executable: headers, .text, .rdata, .data and .idata
// (import tables). All code references are RIP-relative, so the image needs no
// base relocations and is loaded at its preferred base address.
std::vector<uint8_t> writePE(const Image& image, Subsystem subsystem);

} // namespace ron
