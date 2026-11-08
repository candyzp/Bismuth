int main() {
    GameObject object;
    object.parent=&object;
    ResolvedStateLayer state;
    state.objects.push_back({});
    state.objects[0].object=&object;
    state.objects[0].safety=ResolvedStateLayer::SafetyClass::StaticSafe;
    state.sprites.push_back({}); state.sprites[0].sprite=&object;
    state.sprites[0].geometry=state.captureSpriteState(&object);
    state.spriteIndexByPointer.emplace(&object,0);
    state.objectTexels.resize(3); state.spriteTexels.resize(3);
    assert(state.canDrawSprite(&object));
    {
        GameObject solid;
        cocos2d::CCSprite child, grandchild, outside;
        child.parent = &solid;
        grandchild.parent = &child;
        solid.children.size = 1;
        solid.unpackedSprites = {&child, &grandchild};
        std::vector<cocos2d::CCSprite*> collected;
        ResolvedStateLayer::CollectionDiagnostics diagnostics;
        assert(state.classifyObject(&solid, collected, diagnostics) ==
            ResolvedStateLayer::SafetyClass::StaticSafe);
        assert(collected.size() == 3);
        assert(collected[0] == &solid && collected[1] == &child && collected[2] == &grandchild);

        ResolvedStateLayer treeState;
        treeState.objects.push_back({});
        treeState.objects[0].object = &solid;
        treeState.objects[0].safety = ResolvedStateLayer::SafetyClass::StaticSafe;
        for (auto sprite : collected) {
            const usize index = treeState.sprites.size();
            treeState.sprites.push_back({});
            treeState.sprites.back().sprite = sprite;
            treeState.sprites.back().objectIndex = 0;
            treeState.spriteIndexByPointer.emplace(sprite, index);
        }
        assert(treeState.canDrawSprite(&solid));
        assert(treeState.canDrawSprite(&child));
        assert(treeState.canDrawSprite(&grandchild));

        child.parent = nullptr;
        treeState.beginFrameValidation();
        assert(!treeState.canDrawSprite(&child));
        child.parent = &solid;

        solid.m_glowSprite = &outside;
        assert(state.classifyObject(&solid, collected, diagnostics) ==
            ResolvedStateLayer::SafetyClass::StockOnly);
        assert(collected.empty());
    }
    {
        GameObject decoration;
        decoration.m_objectType = GameObjectType::Decoration;
        cocos2d::CCNode outerContainer, innerContainer;
        cocos2d::CCSprite firstPiece, secondPiece;
        outerContainer.parent = &decoration;
        innerContainer.parent = &outerContainer;
        firstPiece.parent = &outerContainer;
        secondPiece.parent = &innerContainer;
        decoration.children.nodes = {&outerContainer};
        outerContainer.children.nodes = {&firstPiece, &innerContainer};
        innerContainer.children.nodes = {&secondPiece};

        std::vector<cocos2d::CCSprite*> collected;
        ResolvedStateLayer::CollectionDiagnostics diagnostics;
        // The nested visual tree is GPU-owned, but a fixed root does not
        // need a per-frame Cocos root-transform capture.
        assert(state.classifyObject(&decoration, collected, diagnostics) ==
            ResolvedStateLayer::SafetyClass::StaticSafe);
        assert(collected.size() == 3);
        assert(diagnostics.invalidChildNodes == 2);

        decoration.m_groupCount = 1;
        assert(state.classifyObject(&decoration, collected, diagnostics) ==
            ResolvedStateLayer::SafetyClass::StaticSafe);
        decoration.m_groupCount = 0;
        decoration.synced = true;
        assert(state.classifyObject(&decoration, collected, diagnostics) ==
            ResolvedStateLayer::SafetyClass::DynamicSafe);
        decoration.synced = false;
        assert(collected[0] == &decoration);
        assert(collected[1] == &firstPiece);
        assert(collected[2] == &secondPiece);
    }
    {
        // Dynamic roots nested below plain CCNode containers must upload their
        // complete transform to the stock atlas, not only the GameObject-local
        // matrix. Moving the container changes the composed GPU transform too.
        GameObject moving;
        cocos2d::CCNode container;
        cocos2d::CCSpriteBatchNode batch;
        moving.parent = &container;
        container.parent = &batch;
        moving.batch = &batch;
        moving.transform = {1.f, 0.f, 0.f, 1.f, 10.f, 20.f};
        container.transform = {0.f, -1.f, 1.f, 0.f, 100.f, 50.f};

        std::vector<cocos2d::CCSprite*> accepted;
        ResolvedStateLayer::CollectionDiagnostics diagnostics;
        assert(state.classifyObject(&moving, accepted, diagnostics) ==
            ResolvedStateLayer::SafetyClass::DynamicSafe);
        auto initial = state.captureObjectState(&moving);
        auto expected = cocos2d::CCAffineTransformConcat(
            moving.transform, container.transform
        );
        assert(initial.transform.a == expected.a && initial.transform.b == expected.b);
        assert(initial.transform.c == expected.c && initial.transform.d == expected.d);
        assert(initial.transform.tx == expected.tx && initial.transform.ty == expected.ty);

        container.transform.tx += 40.f;
        auto moved = state.captureFrameObjectState(
            &moving, ResolvedStateLayer::SafetyClass::DynamicSafe, initial
        );
        expected = cocos2d::CCAffineTransformConcat(
            moving.transform, container.transform
        );
        assert(moved.transform.tx == expected.tx && moved.transform.ty == expected.ty);
        assert(moved.transform.tx != initial.transform.tx);
    }
    {
        GameObject glow, detail;
        object.m_objectType=GameObjectType::Hazard;
        object.m_glowSprite=&glow; object.m_colorSprite=&detail;
        std::vector<cocos2d::CCSprite*> accepted;
        ResolvedStateLayer::CollectionDiagnostics diagnostics;
        auto classify=[&]{accepted.clear();return state.classifyObject(&object,accepted,diagnostics);};
        assert(classify()==ResolvedStateLayer::SafetyClass::DynamicSafe);
        assert(accepted.size()==1 && accepted.front()==&object);
        assert(state.canDrawSprite(&object));
        object.m_groupCount=1;
        assert(classify()==ResolvedStateLayer::SafetyClass::DynamicSafe);
        object.m_groupCount=0;
        object.m_classType=GameObjectClassType::Animated;
        assert(classify()==ResolvedStateLayer::SafetyClass::StockOnly && state.canDrawSprite(&object));
        object.m_classType=GameObjectClassType::Normal;
        object.synced=true; assert(classify()==ResolvedStateLayer::SafetyClass::StockOnly);
        object.synced=false;
        object.children.size=1;
        assert(classify()==ResolvedStateLayer::SafetyClass::StockOnly && state.canDrawSprite(&object));
        object.children.size=0;
        object.dontDraw=true;
        assert(state.canDrawSprite(&object));
        assert(!state.captureFrameObjectState(
            &object, ResolvedStateLayer::SafetyClass::DynamicSafe,
            state.captureObjectState(&object)
        ).visible);
        object.dontDraw=false;
        object.flipX=true; assert(state.canDrawSprite(&object)); object.flipX=false;
        object.m_glowSprite=nullptr; object.m_colorSprite=nullptr;
        object.m_objectType=GameObjectType::Solid;
    }
    object.transform={0.7f,1.3f,-0.2f,-2.f,84000.f,700.f};
    object.vertexZ=9;

    // Static-safe roots reuse their previously resolved affine transform while
    // still mirroring stock lifecycle visibility. Dynamic-safe roots continue to
    // capture the live matrix each frame.
    auto staticBaseline=state.captureObjectState(&object);
    object.transform.tx+=37.f;
    auto staticFrame=state.captureFrameObjectState(
        &object,
        ResolvedStateLayer::SafetyClass::StaticSafe,
        staticBaseline
    );
    assert(staticFrame.transform.tx==staticBaseline.transform.tx);
    auto dynamicFrame=state.captureFrameObjectState(
        &object,
        ResolvedStateLayer::SafetyClass::DynamicSafe,
        staticBaseline
    );
    assert(dynamicFrame.transform.tx==object.transform.tx);
    object.parent=nullptr;
    staticFrame=state.captureFrameObjectState(
        &object,
        ResolvedStateLayer::SafetyClass::StaticSafe,
        staticBaseline
    );
    assert(!staticFrame.visible);
    object.parent=&object;
    object.transform=staticBaseline.transform;

    state.objects[0].startPosition = {123.f, 456.f};
    state.objects[0].groupCombinationIndex = 17;
    state.objects[0].groupFlags = 3.f;
    auto next=state.captureObjectState(&object);
    state.packObjectState(0,next,ResolvedStateLayer::SafetyClass::DynamicSafe);
    auto a=state.objectTexels[0], b=state.objectTexels[1], c=state.objectTexels[2];
    assert(c.x == 123.f && c.y == 456.f && c.z == 17.f && c.w == 3.f);
    assert(a.x==0.7f && a.y==1.3f && a.z==-0.2f && a.w==-2.f);
    assert(b.x==84000.f && b.y==700.f && b.z==9 && b.w==1);
    auto old=next; next.transform.c+=0.000001f;
    assert(state.transformChanged(old,next));
    object.parent=nullptr;
    next=state.captureObjectState(&object);
    state.packObjectState(0,next,ResolvedStateLayer::SafetyClass::DynamicSafe);
    assert(state.objectTexels[1].w==0);
    object.parent=&object;
    object.m_isInvisible=true;
    next=state.captureObjectState(&object);
    state.packObjectState(0,next,ResolvedStateLayer::SafetyClass::DynamicSafe);
    assert(state.objectTexels[1].w==0);
    object.m_isInvisible=false;
    object.children.size=1; assert(state.canDrawSprite(&object)); object.children.size=0;
    object.m_glowSprite=&object; assert(state.canDrawSprite(&object)); object.m_glowSprite=nullptr;
    object.rect.size.width=60; assert(state.canDrawSprite(&object)); object.rect.size.width=30;
    object.flipX=true; assert(state.canDrawSprite(&object)); object.flipX=false;
    object.offset.x=1; assert(state.canDrawSprite(&object)); object.offset.x=0;
    object.tex.id=5; assert(state.canDrawSprite(&object)); object.tex.id=4;
    object.tex.width=2048; assert(state.canDrawSprite(&object)); object.tex.width=1024;
    assert(state.canDrawSprite(&object));
    auto sprite=state.captureSpriteState(&object);
    sprite.opacityModifyRGB=true;
    for(int color=0;color<256;++color) for(int opacity=0;opacity<256;++opacity) {
        sprite.color={static_cast<u8>(color),static_cast<u8>(color),static_cast<u8>(color)};
        sprite.opacity=opacity;
        state.packSpriteState(0,sprite,0);
        u8 stockByte=color;
        stockByte*=opacity/255.0f;
        assert(state.spriteTexels[0].x==stockByte/255.f);
        assert(state.spriteTexels[0].w==opacity/255.f);
    }
    sprite.opacityModifyRGB=false; sprite.color={100,150,200}; sprite.opacity=3;
    state.packSpriteState(0,sprite,0);
    assert(state.spriteTexels[0].x==100/255.f && state.spriteTexels[0].w==3/255.f);
    {
        ResolvedStateLayer temporary;
        assert(ResolvedStateLayer::getCurrent()==&temporary);
    }
    assert(ResolvedStateLayer::getCurrent()==nullptr);
    state.setCurrent(true);
    {
        ResolvedStateLayer newer;
        state.setCurrent(false);
        assert(ResolvedStateLayer::getCurrent()==&newer);
        newer.setCurrent(false);
        assert(ResolvedStateLayer::getCurrent()==nullptr);
        state.setCurrent(true);
    }
    assert(ResolvedStateLayer::getCurrent()==&state);

    // A live geometry check is paid once per displayed frame. Repeated atlas
    // ownership queries in that frame reuse the exact result, while a new frame
    // invalidates the memoized answer and observes later stock geometry changes.
    state.beginFrameValidation();
    assert(state.canDrawSprite(&object));
    assert(state.canDrawSprite(&object));
    assert(state.getStats().spriteValidations==1);
    assert(state.getStats().spriteValidationReuses==1);
    object.flipX=true;
    state.beginFrameValidation();
    assert(state.canDrawSprite(&object));
    assert(state.getStats().spriteValidations==1);
    assert(state.getStats().spriteValidationReuses==0);
    object.flipX=false;
    state.beginFrameValidation();
    assert(state.canDrawSprite(&object));

    {
        // A deactivated sprite can leave the active set before a failed hide
        // upload retries. Empty active vectors must not strand that dirty state.
        ResolvedStateLayer pending;
        DataTexture objects, sprites, groups;
        pending.objectStateTexture=&objects; pending.spriteStateTexture=&sprites;
        pending.groupStateTexture=&groups; pending.groupStateUploadCurrent=true;
        pending.objectTexels.resize(3); pending.spriteTexels.resize(2);
        pending.dirtyObjectRecords={0}; pending.uploadsCurrent=true;
        objects.fail=true; pending.update(true);
        assert(!pending.isGPUStateReady() && pending.dirtyObjectRecords.size()==1);
        objects.fail=false; pending.update(true);
        assert(pending.isGPUStateReady() && pending.dirtyObjectRecords.empty());
        assert(objects.uploads==2);
    }
    std::cout << "PASS: exact affine state, inherited CCNode transforms, static transform reuse, detached visibility, small transform changes, live geometry eligibility, per-frame validation cache, 65,536 stock opacity/color pairs, current-state teardown\n";
}
