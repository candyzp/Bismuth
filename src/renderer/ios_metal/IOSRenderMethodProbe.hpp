#pragma once
#ifdef GEODE_IS_IOS

#include <cstdint>
#include <string>

namespace IOSUpstream {

// Runs one isolated Metal/OpenGL ES interoperability probe while Geometry Dash
// keeps ownership of its normal renderer. This function never hides Cocos batches.
bool runIOSRenderMethodProbe(std::uint32_t method, std::string& report);

} // namespace IOSUpstream
#endif
