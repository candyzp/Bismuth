#pragma once

#ifdef GEODE_IS_IOS

#include <array>
#include <string>

namespace IOSGPUSelfTest {

struct Result {
    bool ok = false;
    int vertexTextureUnits = 0;
    std::string stage = "not-run";
    unsigned int glError = 0;
    std::array<unsigned char, 4> pixel = {0, 0, 0, 0};
};

Result run();

} // namespace IOSGPUSelfTest

#endif
