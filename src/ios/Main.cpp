#include <Geode/Geode.hpp>

using namespace geode::prelude;

$on_mod(Loaded) {
    log::info(
        "Kyanite iOS Metal port loaded. The Metal diagnostic probe is experimental; "
        "Geometry Dash's stock level renderer remains active until Metal object "
        "rendering is implemented and validated."
    );
}
