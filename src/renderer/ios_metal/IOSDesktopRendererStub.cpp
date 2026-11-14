#if defined(GEODE_IS_IOS)

#include "../Renderer.hpp"

Ref<Renderer> Renderer::get() {
    return {};
}

void Renderer::update(float) {}

void Renderer::reset() {}

bool Renderer::useOptimizations() {
    return false;
}

void GroupManager::storeCheckpoint(CheckpointObject*) {}
void GroupManager::restoreCheckpoint(CheckpointObject*) {}
void GroupManager::removeCheckpoint(CheckpointObject*) {}

void GroupManager::moveGroup(GroupID, float, float) {}

void GroupManager::rotateGroup(
    GroupID,
    float,
    bool,
    std::optional<glm::vec2>
) {}

void GroupManager::toggleGroup(GroupID, bool) {}

#endif
