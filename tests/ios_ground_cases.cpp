static void checkRestored(){
    assert(fixture::vao==9 && fixture::arrayBuffer==8 && fixture::elements[9]==6);
    assert(fixture::program==7 && fixture::texture==5 && fixture::activeTexture==GL_TEXTURE0);
    assert(!fixture::blending && (fixture::blend==std::array<GLint,4>{1,2,3,4}));
}
int main(){
    Renderer renderer;Renderer::current=&renderer;
    GJGroundLayer ground;ground.parent=&renderer.layer;
    RendererGroundOwnedCCSprite g1,g2,tile1,tile2,line,shadow,unrelated;
    cocos2d::CCNode nested;
    g1.parent=&ground;g2.parent=&ground;line.parent=&ground;shadow.parent=&ground;
    ground.m_ground1Sprite=&g1;ground.m_ground2Sprite=&g2;ground.m_lineSprite=&line;
    tile1.parent=&g1;nested.parent=&g2;tile2.parent=&nested;
    g1.dontDraw=true;g2.rect.size.width=0;
    static_cast<cocos2d::CCSprite&>(g1).draw();static_cast<cocos2d::CCSprite&>(g2).draw();assert(fixture::gpuDraws==0 && fixture::failures==0);
    assert(groundOwner(&tile1)==&ground && groundOwner(&tile2)==&ground);
    assert(GroundOwnership::part(&ground,&tile1)==GroundOwnership::Part::Ground1);
    assert(GroundOwnership::part(&ground,&tile2)==GroundOwnership::Part::Ground2);
    tile1.quad.bl.colors={40,30,20,80};tile1.quad.tl.colors={0,0,0,0};
    tile1.quad.br.texCoords={0.75f,0.125f};
    static_cast<cocos2d::CCSprite&>(tile1).draw();checkRestored();
    assert((fixture::uniforms.at(fixture::locations.at("u_colorBL"))==std::array<float,4>{40/255.f,30/255.f,20/255.f,80/255.f}));
    assert(fixture::uniforms.at(fixture::locations.at("u_colorTL"))[3]==0.f);
    assert(fixture::uniforms.at(fixture::locations.at("u_uvBR"))[0]==0.75f);
    assert((fixture::indices==std::vector<u16>{0,1,2,1,3,2}));
    static_cast<cocos2d::CCSprite&>(tile2).draw();static_cast<cocos2d::CCSprite&>(line).draw();static_cast<cocos2d::CCSprite&>(shadow).draw();checkRestored();
    assert(fixture::proofs==4 && fixture::stockDraws==0);
    assert(groundGPU().ground1Proven && groundGPU().ground2Proven && groundGPU().lineProven);
    // Current texture/quad changes are consumed each frame, never cached as a
    // new floor shape; child membership survives tile recycling and scrolling.
    for(int frame=0;frame<120;++frame){
        tile1.quad.bl.vertices.x=frame*4.f;tile1.texture.id=100+frame;static_cast<cocos2d::CCSprite&>(tile1).draw();
        assert(fixture::uniforms.at(fixture::locations.at("u_posBL"))[0]==frame*4.f);
        checkRestored();
    }
    fixture::failDraw=true;auto proof=fixture::proofs;static_cast<cocos2d::CCSprite&>(tile1).draw();fixture::failDraw=false;
    assert(fixture::proofs==proof && fixture::failures==1 && fixture::stockDraws==0);checkRestored();
    static_cast<cocos2d::CCSprite&>(unrelated).draw();assert(fixture::stockDraws==1);
    PlayLayer menu;ground.parent=&menu;static_cast<cocos2d::CCSprite&>(tile1).draw();assert(fixture::stockDraws==2);ground.parent=&renderer.layer;
    renderer.enabled=false;static_cast<cocos2d::CCSprite&>(tile1).draw();assert(fixture::stockDraws==3);renderer.enabled=true;
    groundGPU()={};fixture::failSetup=true;
    assert(!initGroundGPU());fixture::failSetup=false;
    assert(!initGroundGPU()); // Allocated IDs alone do not prove successful setup.
    checkRestored();
    std::cout<<"PASS: both ground tile trees, line/shadows, empty containers, exact corner color/UV, scrolling, strict failures and GL restoration\n";
}
