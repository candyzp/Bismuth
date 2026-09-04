int main() {
    GameObject object;
    object.parent=&object;
    ResolvedStateLayer state;
    state.objects.push_back({}); state.objects[0].object=&object;
    state.sprites.push_back({}); state.sprites[0].sprite=&object;
    state.sprites[0].geometry=state.captureSpriteState(&object);
    state.spriteIndexByPointer.emplace(&object,0);
    state.objectTexels.resize(2); state.spriteTexels.resize(3);
    assert(state.canDrawSprite(&object));
    object.transform={0.7f,1.3f,-0.2f,-2.f,84000.f,700.f};
    object.vertexZ=9;
    auto next=state.captureObjectState(&object);
    state.packObjectState(0,next,ResolvedStateLayer::SafetyClass::DynamicSafe);
    auto a=state.objectTexels[0], b=state.objectTexels[1];
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
    object.children.size=1; assert(!state.canDrawSprite(&object)); object.children.size=0;
    object.m_glowSprite=&object; assert(!state.canDrawSprite(&object)); object.m_glowSprite=nullptr;
    object.rect.size.width=60; assert(!state.canDrawSprite(&object)); object.rect.size.width=30;
    object.flipX=true; assert(!state.canDrawSprite(&object)); object.flipX=false;
    object.offset.x=1; assert(!state.canDrawSprite(&object)); object.offset.x=0;
    object.tex.id=5; assert(!state.canDrawSprite(&object)); object.tex.id=4;
    object.tex.width=2048; assert(!state.canDrawSprite(&object)); object.tex.width=1024;
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
    std::cout << "PASS: exact affine state, detached visibility, small transform changes, mutable geometry rejection, 65,536 stock opacity/color pairs, current-state teardown\n";
}