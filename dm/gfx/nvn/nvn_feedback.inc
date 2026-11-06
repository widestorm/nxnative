// Transform feedback uses a void vertex entry. For GS output the generated
// reader gathers packed primitive indices; its draw count stays on the GPU.
enum { TF_SLOT = 8 };
static id<MTLBuffer> g_tf_state;
static id<MTLComputePipelineState> g_tf_prepare;
// ---- Batched captures (DM_NVN_TF_BATCH: default on, 0 off, ab... alternates with the DM_AB phases) ----
// Pokémon Scarlet / Violet (Trinity) skin every character on the GPU with a capture of its own: begin, one point draw with the rasterizer off, end. Each capture took four encoders, each with full
// barriers: the state copy at the begin, the 1x1x1 tf_prepare dispatch, a 1x1 capture pass and the state copy at the end. A busy Violet 4.0.0 scene has 138 captures a frame (57,717 vertices in all):
// about 550 encoders and 15-21 ms of GPU idle between them on an M4, so 30 fps was missed. After a begin the state is known on the CPU (zero offsets), so a capture computes tf_prepare's job itself,
// consecutive captures share one capture pass (one ends it first when it reads or writes what an earlier one in the pass wrote), and the end states go to the game's control buffers in one copy
// encoder after the pass, in command order. A resume, a geometry capture or the switch turned off writes the known state to g_tf_state first and takes the GPU path.
static int g_tfb_mode = -1, g_tfb_known, g_tfb_nw;
static uint32_t g_tfb_st[8];
enum { TFB_ENDS = 256, TFB_W = 64 };
static uint64_t g_tfb_ctl[TFB_ENDS]; static uint32_t g_tfb_ends[TFB_ENDS][8];
static uint64_t g_tfb_wlo[TFB_W], g_tfb_whi[TFB_W];
static uint64_t g_tfb_caps, g_tfb_passes, g_tfb_log = 1000;
static int tfb_on(void) {
    if (g_tfb_mode < 0) { g_tfb_mode = sw_mode("DM_NVN_TF_BATCH", 1); if (g_tfb_mode == 1) MLOG("transform feedback: captures after a begin are prepared on the CPU and batched into shared capture passes (DM_NVN_TF_BATCH=0 turns it off)"); }
    return sw_on(g_tfb_mode);
}
static void tfb_end_pass(void) { if (g_tfb_en) { id<MTL4RenderCommandEncoder> e = g_tfb_en; g_tfb_en = nil; g_tfb_nw = 0; m4_end(e); } }
// Ends the capture pass, then writes the end states to the control buffers (copy encoder, in command order)
static void tfb_flush(void) {
    tfb_end_pass();
    if (!g_tfb_nend) return;
    int n = g_tfb_nend; g_tfb_nend = 0;
    id<MTLBuffer> buf; size_t off = arena_alloc(32 * (size_t)n, &buf); uint8_t *src = (uint8_t *)buf.contents + off;
    for (int i = 0; i < n; i++) {
        memcpy(src + 32 * i, g_tfb_ends[i], 32);
        void *cb; uint64_t co, gpu;
        if (!mtl_gpu_addr(g_tfb_ctl[i], 32, &cb, &co, &gpu)) { static int once; if (!once++) MLOG("[RED] transform feedback control buffer %#llx is outside a memory pool", (unsigned long long)g_tfb_ctl[i]); continue; }
        enc_b_copy_buffer(blit_buf(0, 0, g_tfb_ctl[i], g_tfb_ctl[i] + 32), buf, off + 32 * (size_t)i, (__bridge id<MTLBuffer>)cb, co, 32); pend_add(g_tfb_ctl[i], 32, 4);
    }
    g_wr_epoch++; g_mdx_epoch++;
}
static void tf_state_gpu_(const p_tf_state *b);
// The GPU path needs the state in g_tf_state: write the CPU-known one there (in command order, after the captures that made it)
static void tfb_materialize(void) {
    tfb_flush();
    if (!g_tfb_known) return;
    g_tfb_known = 0;
    if (!g_tf_state) { g_tf_state = [g_dev newBufferWithLength:32 options:MTLResourceStorageModeShared]; res_add(g_tf_state); }
    id<MTLBuffer> buf; size_t off = arena_alloc(32, &buf); memcpy((uint8_t *)buf.contents + off, g_tfb_st, 32);
    enc_b_copy_buffer(blit(), buf, off, g_tf_state, 0, 32);
    g_wr_epoch++; g_mdx_epoch++;
}
static void tf_state_gpu(const p_tf_state *b) {
    if (tfb_on() && b->what != 3) {
        if (b->what == 0) {   // begin: offsets and count start at zero (tf_state_gpu_'s initial state)
            rp_end(); ubd_land();
            memset(g_tfb_st, 0, sizeof g_tfb_st); g_tfb_st[5] = 1; g_tfb_known = 1;
            g_wr_epoch++; g_mdx_epoch++; return;
        }
        if (g_tfb_known) {   // end / pause: the state goes to the control buffer when the batch is flushed
            if (b->ctl) { if (g_tfb_nend == TFB_ENDS) tfb_flush(); g_tfb_ctl[g_tfb_nend] = b->ctl; memcpy(g_tfb_ends[g_tfb_nend++], g_tfb_st, 32); }
            return;
        }
    }
    tfb_materialize();
    tf_state_gpu_(b);
}
static void tf_state_gpu_(const p_tf_state *b) {
    rp_end(); ubd_land();
    if (!g_tf_state) { g_tf_state = [g_dev newBufferWithLength:32 options:MTLResourceStorageModeShared]; res_add(g_tf_state); }
    if (b->what == 0) {
        id<MTLBuffer> buf; size_t off = arena_alloc(32, &buf);
        uint32_t initial[8] = {0,0,0,0,0,1,0,0}; memcpy((uint8_t *)buf.contents + off, initial, 32);
        enc_b_copy_buffer(blit(), buf, off, g_tf_state, 0, 32);
    } else if (b->ctl) {
        void *buffer; uint64_t off, gpu;
        if (!mtl_gpu_addr(b->ctl, 32, &buffer, &off, &gpu)) { RED_ONCE(103, "transform feedback control buffer is outside a memory pool"); return; }
        if (b->what == 3) enc_b_copy_buffer(blit_buf(b->ctl,b->ctl+32,0,0), (__bridge id<MTLBuffer>)buffer, off, g_tf_state, 0, 32);
        else { enc_b_copy_buffer(blit_buf(0,0,b->ctl,b->ctl+32), g_tf_state, 0, (__bridge id<MTLBuffer>)buffer, off, 32); pend_add(b->ctl,32,4); }
    }
    g_wr_epoch++; g_mdx_epoch++;
}
static id<MTLRenderPipelineState> tf_pso(NvShader *vs) {
    pso_key k; static const uint32_t cf[8]; pso_key_fill(&k, vs, nil, cf, 0, 0, 1, 0); k.fsfn = TF_MARK;
    id<MTLRenderPipelineState> cached = hm_get_id(&g_pso, &k);
    if (cached || hm_get_id(&g_pso_fail, &k)) return cached;
    NSError *e = nil; id<MTLRenderPipelineState> ps = pso_build(pso_desc(&k, vs, nil, nil, 0), &e);
    if (!ps) { MLOG("[RED] transform feedback pipeline %s: %s", vs->key, e.localizedDescription.UTF8String); hm_put_id(&g_pso_fail, &k, NSNull.null); }
    else hm_put_id(&g_pso, &k, ps);
    return ps;
}
static uint64_t tf_prepare(NvShader *vs, uint64_t source_count, uint64_t order, uint32_t count, uint32_t primitive,
                           uint32_t first, uint32_t per_instance, uint32_t base_instance, uint64_t dst[4]) {
    tfb_materialize();
    if (!g_tf_state || !vs || !vs->tf_fn) { RED_ONCE(100, "transform feedback shader %s has no capture entry", vs ? vs->key : "-"); return 0; }
    nvn_tf_job j = {.order=order,.first=first,.count=per_instance,.base_instance=base_instance,
                    .state=g_tf_state.gpuAddress,.source_count=source_count,.requested=count,.primitive=primitive};
    for (int b = 0; b < 4; b++) if (vs->tf_stride[b]) {
        void *buf; uint64_t off;
        if (!S.tf[b].addr || S.tf[b].size > UINT32_MAX || !mtl_gpu_addr(S.tf[b].addr,S.tf[b].size,&buf,&off,&dst[b])) {
            RED_ONCE(102, "transform feedback buffer %d of %s is unbound or outside its memory pool", b, vs->key); return 0; }
        j.stride[b]=vs->tf_stride[b];j.capacity[b]=(uint32_t)S.tf[b].size;
    }
    if (!g_tf_prepare) {
        NSError *e=nil; id<MTLLibrary> lib=host_lib("tf",&e);
        if (lib) g_tf_prepare=[g_dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"tf_prepare"] error:&e];
        if (!g_tf_prepare) { MLOG("[RED] transform feedback prepare: %s",e.localizedDescription.UTF8String); return 0; }
    }
    rp_end(); ubd_land();
    if (g_blit) { enc_b_end(g_blit);g_blit=nil; }
    id<MTLBuffer> buf;size_t off=arena_alloc(sizeof j,&buf);memcpy((uint8_t *)buf.contents+off,&j,sizeof j);
    uint64_t job=buf.gpuAddress+off;
    id<MTL4ComputeCommandEncoder> ce=enc_c_begin(NULL);enc_c_pso(ce,g_tf_prepare);enc_c_args(ce,job,0);
    enc_c_dispatch(ce,MTLSizeMake(1,1,1),MTLSizeMake(1,1,1));enc_c_end(ce);
    return job;
}
static void tf_note(NvShader *vs) {
    for(int b=0;b<4;b++)if(vs->tf_stride[b])pend_add(S.tf[b].addr,S.tf[b].size,4);
    args_dirty_all();g_enc_pso=nil;g_enc_dss=nil;g_ed.ok=0;g_wr_epoch++;g_mdx_epoch++;
}
static int tfb_hits(uint64_t lo, uint64_t hi) { for (int i = 0; i < g_tfb_nw; i++) if (lo < g_tfb_whi[i] && g_tfb_wlo[i] < hi) return 1; return 0; }
// A capture after a begin (state known on the CPU): tf_prepare's job computed here (the same arithmetic as dm/gfx/msl/tf.metal), drawn in the shared capture pass
static void tfb_draw(NvProg *pr, NvShader *vs, id<MTLRenderPipelineState> ps, const p_draw *dw, unsigned instances, uint64_t count) {
    rp_end(); ubd_land();   // a capture of a rasterized draw comes inside the game's pass: end it, as tf_prepare does
    uint32_t prim = dw->prim == 0 ? 1 : dw->prim == 1 ? 2 : 3, n = (uint32_t)count;
    uint64_t dst[4] = {0}, wlo[4] = {0}, whi[4] = {0};
    for (int b = 0; b < 4; b++) if (vs->tf_stride[b]) {
        void *buf; uint64_t off;
        if (!S.tf[b].addr || S.tf[b].size > UINT32_MAX || !mtl_gpu_addr(S.tf[b].addr, S.tf[b].size, &buf, &off, &dst[b])) {
            RED_ONCE(102, "transform feedback buffer %d of %s is unbound or outside its memory pool", b, vs->key); return; }
        uint32_t cap = (uint32_t)S.tf[b].size, room = (cap - MIN(cap, g_tfb_st[b])) / vs->tf_stride[b];
        if (room < n) n = room;
    }
    n -= n % prim;
    nvn_tf_job j; memset(&j, 0, sizeof j);
    for (int b = 0; b < 4; b++) j.offset[b] = g_tfb_st[b];
    j.args[0] = n; j.args[1] = 1; j.args[2] = 0; j.args[3] = (uint32_t)dw->base_instance;
    j.first = (uint32_t)dw->first; j.count = (uint32_t)dw->count; j.base_instance = (uint32_t)dw->base_instance; j.requested = (uint32_t)count; j.primitive = prim;
    for (int b = 0; b < 4; b++) if (vs->tf_stride[b]) {
        wlo[b] = S.tf[b].addr + g_tfb_st[b]; whi[b] = wlo[b] + (uint64_t)n * vs->tf_stride[b];
        g_tfb_st[b] += n * vs->tf_stride[b];
        j.stride[b] = vs->tf_stride[b]; j.capacity[b] = (uint32_t)S.tf[b].size;
    }
    g_tfb_st[4] += n; g_tfb_st[5] = 1; g_tfb_st[6] = g_tfb_st[7] = 0;
    // GPU order within the pass: a capture that reads or writes what an earlier one in it wrote starts a new pass (vertex streams, stage 0 uniform / storage buffers, its own outputs)
    int dep = g_tfb_nw >= TFB_W - 4;
    for (int b = 0; b < 4 && !dep; b++) if (whi[b] > wlo[b] && tfb_hits(wlo[b], whi[b])) dep = 1;
    for (int i = 0; i < 16 && !dep; i++) if (S.vbuf[i].size && tfb_hits(S.vbuf[i].address, S.vbuf[i].address + S.vbuf[i].size)) dep = 1;
    for (int i = 0; i < 16 && !dep; i++) {
        if (S.ubo[0][i].size && tfb_hits(S.ubo[0][i].addr, S.ubo[0][i].addr + S.ubo[0][i].size)) dep = 1;
        if (S.ssbo[0][i].size && tfb_hits(S.ssbo[0][i].addr, S.ssbo[0][i].addr + S.ssbo[0][i].size)) dep = 1;
    }
    if (dep) tfb_end_pass();
    vbuf_prepare(vs ? &vs->rf : NULL, dw, NULL); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    if (!g_tfb_en) {
        MTL4RenderPassDescriptor *rd = [MTL4RenderPassDescriptor new]; rd.renderTargetWidth = rd.renderTargetHeight = rd.defaultRasterSampleCount = 1;
        g_tfb_en = enc_r_begin(rd); g_tfb_passes++;
    }
    id<MTL4RenderCommandEncoder> en = g_tfb_en;
    enc_r_pso(en, ps); bind_vbufs(en); vconv_bind(en, vs, 1, dw, NULL);
    args_dirty_all(); encode_args(en, vs, 0, 1);
    id<MTLBuffer> jb; size_t jo = arena_alloc(sizeof j, &jb); memcpy((uint8_t *)jb.contents + jo, &j, sizeof j); enc_r_vbuf(en, 14, jb.gpuAddress + jo);
    for (int b = 0; b < 4; b++) if (vs->tf_stride[b]) enc_r_vbuf(en, TF_SLOT + b, dst[b]);
    enc_r_draw_instanced(en, MTLPrimitiveTypePoint, dw->first, dw->count, instances, dw->base_instance);
    for (int b = 0; b < 4; b++) if (whi[b] > wlo[b] && g_tfb_nw < TFB_W) { g_tfb_wlo[g_tfb_nw] = wlo[b]; g_tfb_whi[g_tfb_nw++] = whi[b]; }
    swrite_note(pr); tf_note(vs);
    if (++g_tfb_caps == g_tfb_log) { g_tfb_log *= 10; MLOG("transform feedback batching: %llu captures in %llu capture passes so far", (unsigned long long)g_tfb_caps, (unsigned long long)g_tfb_passes); }
    PD("T %llu tf capture (batched) vs=%s first=%d count=%d instances=%u n=%u", g_stat_pass, vs->key, dw->first, dw->count, instances, n);
}
static void tf_draw(NvProg *pr,const p_draw *dw,const p_draw_idx *di) {
    NvShader *vs=pr?pr->sh[0]:nil;
    if(!dw||dw->ind||dw->mdi_cnt||!(dw->prim==0||dw->prim==1||dw->prim==4)) {
        RED_ONCE(101,"transform feedback array capture requires a point, line or triangle list (vertex %s)",vs?vs->key:"-");return; }
    if(dw->count<=0||dw->instances<0)return;
    unsigned instances=MAX(dw->instances,1);uint64_t count=(uint64_t)dw->count*instances;
    if(count>UINT32_MAX)return;
    id<MTLRenderPipelineState> ps=vs&&vs->tf_fn?tf_pso(vs):nil;if(!ps)return;
    if (g_tfb_known && tfb_on()) { tfb_draw(pr, vs, ps, dw, instances, count); return; }
    uint64_t dst[4]={0};uint64_t job=tf_prepare(vs,0,0,(uint32_t)count,dw->prim==0?1:dw->prim==1?2:3,
                                              dw->first,dw->count,dw->base_instance,dst);if(!job)return;
    vbuf_prepare(vs?&vs->rf:NULL,dw,NULL);if(g_blit){enc_b_end(g_blit);g_blit=nil;}
    MTL4RenderPassDescriptor *rd=[MTL4RenderPassDescriptor new];rd.renderTargetWidth=rd.renderTargetHeight=rd.defaultRasterSampleCount=1;
    id<MTL4RenderCommandEncoder> en=enc_r_begin(rd);enc_r_pso(en,ps);bind_vbufs(en);vconv_bind(en,vs,1,dw,NULL);
    args_dirty_all();encode_args(en,vs,0,1);enc_r_vbuf(en,14,job);
    for(int b=0;b<4;b++)if(vs->tf_stride[b])enc_r_vbuf(en,TF_SLOT+b,dst[b]);
    enc_r_draw_instanced(en,MTLPrimitiveTypePoint,dw->first,dw->count,instances,dw->base_instance);enc_r_end(en);
    swrite_note(pr);tf_note(vs);
    PD("T %llu tf capture vs=%s first=%d count=%d instances=%u",g_stat_pass,vs->key,dw->first,dw->count,instances);
}
static void tf_geometry(NvShader *reader,uint64_t indices,uint64_t args,uint32_t primitive,uint32_t base_instance) {
    if(!reader->tf_fn){RED_ONCE(104,"geometry feedback reader %s has no capture entry",reader->key);return;}
    id<MTLRenderPipelineState> ps=tf_pso(reader);if(!ps)return;
    uint64_t dst[4]={0};uint64_t job=tf_prepare(reader,args,indices,0,primitive,0,0,base_instance,dst);if(!job)return;
    MTL4RenderPassDescriptor *rd=[MTL4RenderPassDescriptor new];rd.renderTargetWidth=rd.renderTargetHeight=rd.defaultRasterSampleCount=1;
    id<MTL4RenderCommandEncoder> en=enc_r_begin(rd);enc_r_pso(en,ps);args_dirty_all();encode_args(en,reader,0,1);enc_r_vbuf(en,14,job);
    for(int b=0;b<4;b++)if(reader->tf_stride[b])enc_r_vbuf(en,TF_SLOT+b,dst[b]);
    enc_r_draw_indirect(en,MTLPrimitiveTypePoint,job+16);enc_r_end(en);tf_note(reader);
}
