#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/binding/EffectGameObject.hpp>

#include "MetalBackend.hpp"
#include "MetalDiagnosticsOverlay.hpp"

using namespace geode::prelude;

class $modify(KyaniteIOSDiagnosticsPlayLayer, PlayLayer) {
    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();

        auto overlay = MetalDiagnosticsOverlay::create(this);
        if (!overlay)
            return;

        overlay->setID("iliashdz.kyanite/ios-metal-diagnostics");
        addChild(overlay, 100000);
        log::info(
            "Kyanite iOS: diagnostics attached; Metal object metadata and GPU culling "
            "are active while Geometry Dash keeps drawing the sprites."
        );
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        MetalBackend::get().resetGroupStates();
    }
};

class $modify(KyaniteIOSDiagnosticsGameLayer, GJBaseGameLayer) {
    void toggleGroup(int id, bool activate) {
        GJBaseGameLayer::toggleGroup(id, activate);
        MetalBackend::get().toggleGroup(id, activate);
    }

    void processMoveActions() {
        GJBaseGameLayer::processMoveActions();

        if (!m_effectManager)
            return;

        // Mirror the game's already-computed movement deltas into compact
        // group state. The stock game still owns its object positions and
        // physics; this state is consumed only by the Metal visibility pass.
        for (auto node : m_effectManager->m_unkVector6c0) {
            if (!node || node->m_unk0d0)
                continue;

            const int groupId = node->getTag();
            MetalBackend::get().moveGroup(
                groupId,
                node->m_unk038,
                node->m_unk040
            );
        }
    }

    void processRotationActions() {
        GJBaseGameLayer::processRotationActions();

        if (!m_effectManager)
            return;

        for (auto cmdObj : m_effectManager->m_unkVector5b0) {
            if (!cmdObj || cmdObj->m_someInterpValue1RelatedFalse)
                continue;

            const int targetId = cmdObj->m_targetGroupID;
            const int centerId = cmdObj->m_centerGroupID;
            auto centerObject = tryGetMainObject(centerId);
            auto staticGroup = getStaticGroup(targetId);

            float rotation;
            if (staticGroup && staticGroup->count() != 0) {
                rotation =
                    cmdObj->m_someInterpValue1RelatedOne -
                    cmdObj->m_someInterpValue1RelatedZero;
            } else {
                rotation =
                    cmdObj->m_someInterpValue2RelatedOne -
                    cmdObj->m_someInterpValue2RelatedZero;
            }

            if (rotation == 0.0f && !cmdObj->m_finishRelated)
                continue;

            if (!centerObject) {
                MetalBackend::get().rotateGroup(
                    targetId,
                    rotation,
                    cmdObj->m_lockObjectRotation
                );
            } else {
                const MetalPoint center{
                    static_cast<float>(centerObject->m_positionX),
                    static_cast<float>(centerObject->m_positionY)
                };
                MetalBackend::get().rotateGroup(
                    targetId,
                    rotation,
                    cmdObj->m_lockObjectRotation,
                    center
                );
            }
        }
    }

    void processFollowActions() {
        if (m_effectManager) {
            for (auto node : m_effectManager->m_unkVector6d8) {
                if (!node)
                    continue;

                const int targetGroupId = node->getTag();
                const int followGroupId = node->m_unk074;
                auto mainObject = tryGetMainObject(followGroupId);
                if (!mainObject)
                    continue;

                double moveX = 0.0;
                double moveY = 0.0;
                if (mainObject->m_unk4C4 == m_gameState.m_commandIndex) {
                    moveX =
                        (mainObject->m_positionX - mainObject->m_lastPosition.x) *
                        node->m_unk080;
                    moveY =
                        (mainObject->m_positionY - mainObject->m_lastPosition.y) *
                        node->m_unk088;
                }

                MetalBackend::get().moveGroup(
                    targetGroupId,
                    static_cast<float>(moveX),
                    static_cast<float>(moveY)
                );
            }
        }

        GJBaseGameLayer::processFollowActions();
    }
};
