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
        renderer.preparedState=&resolved;
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
        for(int i:indices) {
            owner.ownedSprites.push_back(&sprites.at(i));
            renderer.owned.insert(&sprites.at(i));
            renderer.persistentOwned.insert(&sprites.at(i));
            fixture::vaoSpriteIDs[owner.vao].push_back(i);
        }
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
    assert(fixture::vao==fixture::cachedVAO && fixture::program==77 && fixture::arrayBuffer==88);
    assert(fixture::activeTexture==GL_TEXTURE0);
    assert(fixture::textures==fixture::cachedTextures);
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
        // Readiness regression: selection must not require uploadsCurrent before
        // Cocos has produced this frame's authoritative transforms. Production
        // prepareGPUFrame() refreshes the state after selection and before draw.
        Scene s(4);
        s.claim({0,1,2,3});
        s.resolved.ready=false;
        s.draw();
        assert(s.resolved.ready);
        assert(fixture::gpuDraws==1);
    }
    {
        // 32-bit deferred registry regression. Sprite 16,385 begins at vertex
        // 65,536, one vertex beyond the old u16-addressable range.
        Scene s(16385);
        std::vector<int> all(16385);
        std::iota(all.begin(), all.end(), 0);
        s.claim(all);
        auto it=registry().spriteOwners.find(&s.sprites[16384]);
        assert(it!=registry().spriteOwners.end());
        assert(it->second.baseVertex==65536u);
        assert(!registry().invalidRenderers.contains(&s.renderer));
    }

    {
        // Custom-level fragmentation regression: ten useful 6-sprite islands
        // separated by stock-only slots. No run reaches the historical 48-sprite
        // profitability floor. The scheduler must still keep the GPU working
        // instead of reporting GPU IDLE like dense custom levels did.
        Scene s(69);
        std::vector<int> claimed;
        for (int group = 0; group < 10; ++group) {
            const int start = group * 7;
            for (int j = 0; j < 6; ++j)
                claimed.push_back(start + j);
        }
        s.claim(claimed);
        s.draw();

        std::vector<int> expected(69);
        std::iota(expected.begin(), expected.end(), 0);
        assert(fixture::pixels == expected);
        assert(fixture::gpuDraws == 6);
        assert(fixture::stockTransforms == 69 - 35);
    }

    {
        // Dense runs must win scarce draw-call slots over tiny early islands.
        // Eight 1-sprite islands used to exhaust the per-batch call budget
        // before this later 100-sprite run was even considered.
        Scene s(500);
        std::vector<int> claimed;
        for (int i = 0; i < 8; ++i)
            claimed.push_back(i * 2);
        for (int i = 200; i < 300; ++i)
            claimed.push_back(i);
        s.claim(claimed);
        s.draw();
        std::vector<int> expected(500);
        std::iota(expected.begin(), expected.end(), 0);
        assert(fixture::pixels == expected);
        assert(fixture::gpuDraws == 8);
        assert(fixture::stockTransforms == 500 - (100 + 7));
    }
    {
        // CCSpriteBatchNode binds one batch texture/blend for the whole atlas.
        // Per-sprite metadata differences must not fragment an otherwise
        // contiguous GPU-safe run.
        Scene s(96);
        std::vector<int> claimed(96);
        std::iota(claimed.begin(), claimed.end(), 0);
        s.claim(claimed);

        for (int i = 1; i < 96; i += 2) {
            s.sprites[i].blend.src = GL_ONE;
            s.sprites[i].blend.dst = GL_ONE;
        }

        s.draw();
        std::vector<int> expected(96);
        std::iota(expected.begin(), expected.end(), 0);
        assert(fixture::pixels == expected);
        assert(fixture::gpuDraws == 1);
    }

    {
        // Orbit-style lifecycle pattern: every other slot is currently inactive
        // but remains persistently safe and hidden in GPU state. Those hidden
        // owned slots must bridge the active sprites into one useful submission
        // instead of consuming eight calls on eight one-sprite islands.
        Scene s(200);
        std::vector<int> all(200);
        std::iota(all.begin(), all.end(), 0);
        s.claim(all);
        for (int i = 1; i < 200; i += 2)
            s.renderer.owned.erase(&s.sprites[i]);

        s.draw();
        std::vector<int> expected(200);
        std::iota(expected.begin(), expected.end(), 0);
        assert(fixture::pixels == expected);
        assert(fixture::gpuDraws == 1);
        // Fifty active GPU sprites require 49 inactive bridge slots between
        // them, so the selected prefix is 99 atlas slots and stock visits 101.
        assert(fixture::stockTransforms == 101);
    }
    {
        Scene s(10000);
        std::vector<int> order(10000); std::iota(order.begin(),order.end(),0);
        auto shuffled=order; std::mt19937 random(13); std::shuffle(shuffled.begin(),shuffled.end(),random);
        s.claim(shuffled); s.draw();
        assert(fixture::pixels==order && fixture::gpuDraws==1);
        assert(fixture::stockTransforms==10000-HYBRID_GPU_SPRITE_BUDGET);
        auto uploads=fixture::uploads;
        const auto* cachedRuns=registry().indexCaches.at(&s.batch).runs.data();
        s.draw(); assert(fixture::uploads==uploads && fixture::pixels==order);
        assert(registry().indexCaches.at(&s.batch).runs.data()==cachedRuns);
        for(int i=0;i<20;++i) {
            std::shuffle(order.begin(),order.end(),random); s.reorder(order); s.draw();
            assert(fixture::pixels==order && fixture::gpuDraws==1);
            assert(fixture::stockTransforms==10000-HYBRID_GPU_SPRITE_BUDGET);
            checkStateRestored();
        }
        std::cout << "10,000 reordered sprites: GPU budget + CPU overflow preserve order; unchanged order: 0 index uploads\n";
    }
    {
        Scene s(3); s.claim({0,2});
        fixture::failUpload=true; s.draw(); fixture::failUpload=false;
        assert((fixture::pixels==std::vector<int>{0,1,2}));
        assert(fixture::gpuDraws==0 && fixture::stockTransforms==3);
        s.batch.atlas.dirty=true; fixture::failAtlasSync=true; s.draw(); fixture::failAtlasSync=false;
        assert((fixture::pixels==std::vector<int>{0,1,2}));
        // The hybrid attempt updates the one stock-owned sprite, explicitly
        // restores the two suppressed GPU-owned quads, then this fixture's stock
        // draw simulates a full child-transform visit. The dedicated raw-draw
        // case below verifies recovery when that final visit does not exist.
        assert(fixture::gpuDraws==0 && fixture::stockTransforms==7);
        checkStateRestored();
        s.draw(); assert(fixture::gpuDraws==2);
        s.resolved.ready=false; s.draw();
        assert(fixture::gpuDraws==0 && (fixture::pixels==std::vector<int>{0,1,2}));
        assert(fixture::stockTransforms==3);
    }
    {
        // A failure after hybrid transform suppression must restore the selected
        // stock quads before the draw hook falls back to raw CCSpriteBatchNode::draw().
        // Model the real draw path here without a second child-transform visit.
        Scene s(3); s.claim({0,2});
        fixture::stockDrawUpdatesTransforms=false;
        fixture::failGeometryFlush=true;
        s.draw();
        fixture::failGeometryFlush=false;
        fixture::stockDrawUpdatesTransforms=true;
        assert((fixture::pixels==std::vector<int>{0,1,2}));
        assert(fixture::gpuDraws==0 && fixture::stockTransforms==4);
        checkStateRestored();
        s.draw(); assert(fixture::gpuDraws==2);
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
        Buffer otherBuffer; otherBuffer.id=124;
        other.resolvedState=&s.resolved; other.shader=&s.shader; other.indexBuffer=&otherBuffer;
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
        assert(fixture::gpuDraws==2 && s.owner.stats.indicesLastFrame==12);
    }
    {
        Scene s(3); s.claim({0,2});
        fixture::colorMask={0,1,0,1}; s.draw();
        assert((fixture::colorMask==std::array<GLboolean,4>{0,1,0,1}));
        checkStateRestored();
        fixture::colorMask={0,0,0,0}; s.draw(); assert(fixture::pixels.empty());
        fixture::colorMask={1,1,1,1};
        // Last quad in a 65,536-sprite unified registry. This crosses the
        // old u16 vertex wall by a full 196,608 vertices.
        std::vector<SpriteOwner> owners{{&s.renderer,nullptr,&s.owner,262140}};
        std::vector<AtlasDrawRun> runs; std::vector<u32> indices;
        buildAtlasDrawPlan(owners,runs,indices);
        assert((indices==std::vector<u32>{262140,262142,262143,262140,262143,262141}));
    }
    {
        // A single sprite mapped to two different vertices in one owner is not a
        // valid GPU identity. Reject the renderer instead of silently using the
        // first mapping and drawing arbitrary geometry.
        Scene s(2);
        s.owner.ownedSprites={&s.sprites[0],&s.sprites[0]};
        s.renderer.owned.insert(&s.sprites[0]);
        fixture::vaoSpriteIDs[s.owner.vao]={0,0};
        AtlasInterleaveRegistry::registerDeferred(&s.owner);
        s.draw();
        assert(fixture::gpuDraws==0 && (fixture::pixels==std::vector<int>{0,1}));
        assert(fixture::stockTransforms==2);
        AtlasInterleaveRegistry::unregisterDeferred(&s.owner);
        s.owner.ownedSprites.clear();
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
        assert(fixture::stockTransforms==3);
        AtlasInterleaveRegistry::unregisterImmediate(&immediate);
    }
    {
        Scene s(3); s.claim({0,1,2}); s.draw();
        s.reorder({2,1,0}); fixture::failUpload=true;
        s.draw(); assert((fixture::pixels==std::vector<int>{2,1,0}));
        assert(fixture::gpuDraws==0 && fixture::stockTransforms==3);
        fixture::failUpload=false; s.reorder({0,1,2});
        const auto uploads=fixture::uploads;
        s.draw(); assert((fixture::pixels==std::vector<int>{0,1,2}));
        assert(fixture::uploads>uploads); // A failed replacement cannot reuse old cache metadata.
    }
    {
        Scene s(3);
        AssistShadowBatch immediate;
        immediate.stockBatch=&s.batch; immediate.resolvedState=&s.resolved;
        immediate.shader=&s.shader; immediate.indexBuffer=&s.buffer; immediate.vao=40;
        immediate.ownedSprites={&s.sprites[0]};
        fixture::vaoSpriteIDs[40]={0}; s.renderer.owned.insert(&s.sprites[0]);
        AtlasInterleaveRegistry::registerImmediate(&immediate);
        s.draw(); assert(fixture::gpuDraws==1);
        // Temporarily unready GPU geometry stays visible through the CPU lane,
        // then returns to GPU ownership without rejoining.
        s.renderer.owned.clear(); s.draw();
        assert((fixture::pixels==std::vector<int>{0,1,2}) && fixture::gpuDraws==0);
        assert(fixture::stockTransforms==3);
        s.renderer.owned.insert(&s.sprites[0]); s.draw();
        assert((fixture::pixels==std::vector<int>{0,1,2}) && fixture::gpuDraws==1);
        AtlasInterleaveRegistry::unregisterImmediate(&immediate);
    }
    {
        Scene s(5); s.claim({1,3});
        s.draw(); // Dirty warmup establishes this atlas's VAO.
        assert((fixture::pixels==std::vector<int>{0,1,2,3,4}));
        // The floor or an earlier batch draws before the next frame. This atlas
        // is now clean, so the first stock run changes the cached VAO after the
        // old code took its batch-wide snapshot. Exercise many later frames.
        for(int frame=0;frame<120;++frame) {
            ccGLBindVAO(99);
            ccGLBindTexture2D(11);
            fixture::vaoSpriteIDs[99]={-10,-11,-12,-13,-14};
            s.draw();
            assert((fixture::pixels==std::vector<int>{0,1,2,3,4}));
            checkStateRestored();
        }
        std::cout << "PASS: 120 clean frames after unrelated floor/atlas draws; Cocos cache stays synchronized\n";
    }
    {
        Scene s(3); s.claim({0,1});
        s.sprites[1].parent=&s.sprites[0]; s.sprites[2].parent=&s.sprites[0];
        s.sprites[0].children.nodes={&s.sprites[1],&s.sprites[2]};
        s.batch.children.nodes={&s.sprites[0]};
        s.sprites[0].transform.tx=17;
        s.draw();
        assert((fixture::pixels==std::vector<int>{0,1,2}));
        assert(fixture::stockTransforms==1 && fixture::gpuDraws==1);
        assert(s.sprites[0].m_transformToBatch.tx==17);
        // One unready owned sprite becomes stock work for this frame while the
        // ready owner stays GPU drawn. No atlas flash or full-batch rejection.
        s.renderer.owned.erase(&s.sprites[1]); s.draw();
        assert((fixture::pixels==std::vector<int>{0,1,2}));
        assert(fixture::gpuDraws==1 && fixture::stockTransforms==2);
    }
    assert(registry().spriteOwners.empty() && registry().indexCaches.empty());
    std::cout << "PASS: mixed stock/GPU order, hybrid recovery, GPU budgeting, batch migration, masks, VAO/EBO state, teardown, 65k/u32 limits\n";
}
