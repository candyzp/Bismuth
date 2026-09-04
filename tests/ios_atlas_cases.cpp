struct Scene {
    Renderer renderer;
    RendererInterleavedSpriteBatchNode batch;
    cocos2d::CCArray batches;
    std::vector<RendererOwnedCCSprite> sprites;
    ResolvedStateLayer resolved;
    Shader shader;
    Buffer buffer;
    StandaloneAssistBatch owner;
    explicit Scene(int count) : sprites(count) {
        Renderer::current=&renderer;
        batches.nodes={&batch}; renderer.layer.m_batchNodes=&batches;
        owner.resolvedState=&resolved; owner.shader=&shader; owner.indexBuffer=&buffer; owner.vao=40;
        fixture::elements[owner.vao]=123;
        fixture::vaoSpriteIDs[owner.vao].clear();
        for(int i=0;i<count;++i) {
            auto& s=sprites[i]; s.id=i; s.slot=i; s.batch=&batch; s.parent=&batch; s.texture=&batch.texture;
            batch.children.nodes.push_back(&s); batch.descendants.nodes.push_back(&s); batch.atlas.quads.push_back(-100);
        }
    }
    void claim(const std::vector<int>& indices) {
        for(int i:indices) { owner.ownedSprites.push_back(&sprites.at(i)); renderer.owned.insert(&sprites.at(i)); fixture::vaoSpriteIDs[owner.vao].push_back(i); }
        AtlasInterleaveRegistry::registerDeferred(&owner);
    }
    void reorder(const std::vector<int>& indices) {
        batch.children.nodes.clear(); batch.descendants.nodes.clear();
        for(usize i=0;i<indices.size();++i) {
            auto& s=sprites.at(indices[i]); s.slot=i; s.dirty=true;
            batch.children.nodes.push_back(&s); batch.descendants.nodes.push_back(&s);
        }
        batch.atlas.dirty=true;
    }
    void draw() { fixture::clearFrame(); AtlasInterleaveRegistry::beginFrame(); static_cast<cocos2d::CCSpriteBatchNode&>(batch).draw(); }
    ~Scene() { AtlasInterleaveRegistry::unregisterDeferred(&owner); Renderer::current=nullptr; }
};
static void checkStateRestored() {
    assert(fixture::vao==99 && fixture::program==77 && fixture::arrayBuffer==88);
    assert(fixture::activeTexture==GL_TEXTURE0);
    assert((fixture::textures==std::array<GLint,3>{11,22,33}));
    assert(fixture::frontMask==7 && fixture::backMask==13 && fixture::depthMask==1);
    assert(fixture::elements.at(40)==123);
}
int main() {
    {
        Scene s(5); s.claim({4,0,2});
        s.draw();
        assert((fixture::pixels==std::vector<int>{0,1,2,3,4}));
        assert(fixture::gpuDraws==3 && fixture::stockTransforms==2);
        checkStateRestored();
        s.sprites[1].m_classType=GameObjectClassType::Animated;
        s.draw();
        assert((fixture::pixels==std::vector<int>{0,1,2,3,4}));
        assert(fixture::gpuDraws==3 && fixture::stockTransforms==2);
        s.sprites[1].m_classType=GameObjectClassType::Normal;
        s.draw();
        assert(fixture::gpuDraws==3);
        s.renderer.enabled=false; s.renderer.layer.m_batchNodes=nullptr;
        s.draw();
        assert((fixture::pixels==std::vector<int>{0,1,2,3,4}));
        assert(fixture::gpuDraws==0);
    }
    {
        Scene s(10000);
        std::vector<int> order(10000); std::iota(order.begin(),order.end(),0);
        auto shuffled=order; std::mt19937 random(13); std::shuffle(shuffled.begin(),shuffled.end(),random);
        s.claim(shuffled); s.draw();
        assert(fixture::pixels==order && fixture::gpuDraws==1 && fixture::stockTransforms==0);
        auto uploads=fixture::uploads;
        s.draw(); assert(fixture::uploads==uploads && fixture::pixels==order);
        for(int i=0;i<20;++i) {
            std::shuffle(order.begin(),order.end(),random); s.reorder(order); s.draw();
            assert(fixture::pixels==order && fixture::gpuDraws==1 && fixture::stockTransforms==0);
            checkStateRestored();
        }
        std::cout << "10,000 reordered GPU sprites: 1 ordered draw; unchanged order: 0 index uploads\n";
    }
    {
        Scene s(3); s.claim({0,2});
        fixture::failUpload=true; s.draw(); fixture::failUpload=false;
        assert(fixture::pixels.empty());
        assert(fixture::gpuDraws==0 && fixture::stockTransforms==0);
        s.batch.atlas.dirty=true; fixture::failAtlasSync=true; s.draw(); fixture::failAtlasSync=false;
        assert(fixture::pixels.empty());
        assert(fixture::gpuDraws==0);
        checkStateRestored();
        s.draw(); assert(fixture::gpuDraws==2);
        s.resolved.ready=false; s.draw();
        assert(fixture::gpuDraws==0 && fixture::pixels.empty());
    }
    {
        Scene s(2); s.claim({0,1});
        s.sprites[1].slot=0;
        s.draw(); assert(fixture::gpuDraws==0);
        s.sprites[1].slot=1;
        s.batch.transform.tx=4;
        s.draw(); assert(fixture::gpuDraws==0);
        s.batch.transform.tx=0;
        s.batches.nodes.clear();
        s.draw(); assert(fixture::gpuDraws==0);
    }
    {
        Scene s(4); s.claim({3,1});
        StandaloneAssistBatch other;
        other.resolvedState=&s.resolved; other.shader=&s.shader; other.indexBuffer=&s.buffer;
        other.vao=41; other.ownedSprites={&s.sprites[2],&s.sprites[0]};
        fixture::elements[41]=124; fixture::vaoSpriteIDs[41]={2,0};
        s.renderer.owned.insert(&s.sprites[0]); s.renderer.owned.insert(&s.sprites[2]);
        AtlasInterleaveRegistry::registerDeferred(&other);
        s.draw();
        assert((fixture::pixels==std::vector<int>{0,1,2,3}));
        assert(fixture::elements.at(41)==124);
        AtlasInterleaveRegistry::unregisterDeferred(&other);
    }
    {
        Scene s(4); s.claim({3,1,0,2});
        RendererInterleavedSpriteBatchNode second;
        s.batches.nodes.push_back(&second);
        s.batch.children.nodes.resize(2); s.batch.descendants.nodes.resize(2); s.batch.atlas.quads.resize(2);
        for(int i=2;i<4;++i) {
            auto& sprite=s.sprites[i]; sprite.batch=&second; sprite.parent=&second; sprite.slot=i-2; sprite.texture=&second.texture;
            second.children.nodes.push_back(&sprite); second.descendants.nodes.push_back(&sprite); second.atlas.quads.push_back(-100);
        }
        s.draw(); static_cast<cocos2d::CCSpriteBatchNode&>(second).draw();
        assert((fixture::pixels==std::vector<int>{0,1,2,3}));
        assert(fixture::gpuDraws==2 && s.owner.stats.indicesLastFrame==24);
    }
    {
        Scene s(3); s.claim({0,2});
        fixture::colorMask={0,1,0,1}; s.draw();
        assert((fixture::colorMask==std::array<GLboolean,4>{0,1,0,1}));
        checkStateRestored();
        fixture::colorMask={0,0,0,0}; s.draw(); assert(fixture::pixels.empty());
        fixture::colorMask={1,1,1,1};
        std::vector<SpriteOwner> owners{{&s.renderer,nullptr,&s.owner,65532}};
        std::vector<AtlasDrawRun> runs; std::vector<u16> indices;
        buildAtlasDrawPlan(owners,runs,indices);
        assert((indices==std::vector<u16>{65532,65534,65535,65532,65535,65533}));
    }
    {
        Scene s(3);
        AssistShadowBatch immediate;
        immediate.stockBatch=&s.batch; immediate.resolvedState=&s.resolved;
        immediate.shader=&s.shader; immediate.indexBuffer=&s.buffer; immediate.vao=40;
        immediate.ownedSprites={&s.sprites[0],&s.sprites[2]};
        fixture::vaoSpriteIDs[40]={0,2};
        s.renderer.owned.insert(&s.sprites[0]); s.renderer.owned.insert(&s.sprites[2]);
        AtlasInterleaveRegistry::registerImmediate(&immediate);
        s.draw();
        assert((fixture::pixels==std::vector<int>{0,1,2}) && fixture::gpuDraws==2);
        s.claim({0}); s.draw();
        assert((fixture::pixels==std::vector<int>{0,1,2}) && fixture::gpuDraws==0);
        AtlasInterleaveRegistry::unregisterImmediate(&immediate);
    }
    assert(registry().spriteOwners.empty() && registry().indexCaches.empty());
    std::cout << "PASS: mixed stock/GPU order, strict owned-batch failures, batch migration, masks, VAO/EBO state, teardown, u16 limits\n";
}