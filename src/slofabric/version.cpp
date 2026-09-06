#include "slofabric/version.hpp"

namespace slofabric {

// The library version, exposed as a single canonical symbol.
const char* library_version() noexcept { return kVersionString; }
int library_version_major() noexcept { return kVersionMajor; }
int library_version_minor() noexcept { return kVersionMinor; }
int library_version_patch() noexcept { return kVersionPatch; }

}  // namespace slofabric
