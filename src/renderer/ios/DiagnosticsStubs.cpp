#ifdef GEODE_IS_IOS

#include "../DifferenceMode.hpp"
#include "../OverdrawView.hpp"

OverdrawView::OverdrawView(Renderer& renderer)
    : renderer(renderer) {}
OverdrawView::~OverdrawView() = default;
void OverdrawView::init() { initialized = true; enabled = false; }
void OverdrawView::predraw() {}
void OverdrawView::postdraw() {}
void OverdrawView::clear() {}
void OverdrawView::finish() {}
std::string OverdrawView::getDebugText() { return ""; }

DifferenceMode::~DifferenceMode() = default;
void DifferenceMode::drawSceneHook() {}
void DifferenceMode::prepare(u32, u32) {}
void DifferenceMode::destroyFramebuffers() {}

#endif
