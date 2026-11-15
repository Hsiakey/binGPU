/* bingpu_backend.cpp —— 对象构造函数 + 两个后端共享的命令状态机
 *
 * 0.1.0 里 Null 与 CPU 各写了一套状态机（当时是**故意**的对照）；对照的价值已经被
 * tests/test_abi.c 固定住了，C++ 版把它们收成一份 —— 于是"同样的误用在两个后端
 * 得到同样的结果码"不再是靠人肉保持一致，而是结构上不可能不一致。
 *
 * 这里只放"与像素无关"的东西：状态字段、状态迁移、参数校验、统计计数。
 * 真正要画/要存/要读的几段在 BackendBase 的钩子里，由 Null/Cpu 覆盖。
 */
#include "bingpu_backend.h"

namespace bgpu {

/* ---------------------------------------------------------------
 * 对象构造
 * --------------------------------------------------------------- */
Device::Device(Backend& b, BgpuDevice owner, uint32_t flags, uint32_t width, uint32_t height)
    : ObjectBase(kObjDevice, b, owner), dev_flags(flags), w(width), h(height) {}

Texture::Texture(Backend& b, BgpuDevice owner) : ObjectBase(kObjTexture, b, owner) {}

Buffer::Buffer(Backend& b, BgpuDevice owner) : ObjectBase(kObjBuffer, b, owner) {}

Sampler::Sampler(Backend& b, BgpuDevice owner) : ObjectBase(kObjSampler, b, owner) {}

Pipeline::Pipeline(Backend& b, BgpuDevice owner) : ObjectBase(kObjPipeline, b, owner) {}

Swapchain::Swapchain(Backend& b, BgpuDevice owner) : ObjectBase(kObjSwapchain, b, owner) {}

Cmd::Cmd(Backend& b, BgpuDevice owner, Device& d)
    : ObjectBase(kObjCmd, b, owner), dev(&d) {
    /* 与 0.1.0 一致：cmd 建出来时 viewport 覆盖整个设备面；begin_render 会按目标重置。 */
    vp[0] = 0.0f;
    vp[1] = 0.0f;
    vp[2] = static_cast<float>(d.w);
    vp[3] = static_cast<float>(d.h);
}

void Cmd::onPeerDestroyed(ObjectBase& dying) {
    /* 把指向即将消失对象的裸指针清掉 —— 之后的使用退化成"没有 X"的 STATE 错误，
     * 而不是 use-after-free（0.1.0 的 C 版在这里是 UB）。 */
    if (static_cast<ObjectBase*>(target) == &dying) target = nullptr;
    if (static_cast<ObjectBase*>(depth) == &dying) depth = nullptr;
    if (static_cast<ObjectBase*>(pipe) == &dying) pipe = nullptr;
    if (static_cast<ObjectBase*>(tex) == &dying) tex = nullptr;
    if (static_cast<ObjectBase*>(samp) == &dying) samp = nullptr;
    if (static_cast<ObjectBase*>(frame) == &dying) {
        frame = nullptr;
        inFrame = false;
    }
    if (inRender && !target) inRender = false;   /* 目标没了，这一趟渲染作废 */
}

/* ---------------------------------------------------------------
 * 共享状态机
 * --------------------------------------------------------------- */
BgpuResult BackendBase::createCmd(Device& dev, BgpuCmd* out) {
    const uint64_t h = makeObject<Cmd>(*this, dev.self, dev);
    if (h == BGPU_INVALID) {
        setReason("对象表分配失败（内存不足）");
        return BGPU_ERR_OUT_OF_MEMORY;
    }
    *out = h;
    return BGPU_OK;
}

BgpuResult BackendBase::frameBegin(Swapchain& sc, Cmd& c, BgpuTexture* out) {
    if (c.inFrame) {
        return fail(BGPU_ERR_STATE, __FILE__, __LINE__,
                    "cmd 已经在一个帧里（先 frame_end）");
    }
    c.inFrame = true;
    c.frame = &sc;
    *out = sc.target;
    return BGPU_OK;
}

BgpuResult BackendBase::frameEnd(Swapchain& sc, Cmd& c) {
    if (!c.inFrame || c.frame != &sc) {
        return fail(BGPU_ERR_STATE, __FILE__, __LINE__,
                    "这个 cmd 没抓着这个交换链的帧（frame_begin 呢？）");
    }
    if (c.inRender) {
        return fail(BGPU_ERR_STATE, __FILE__, __LINE__,
                    "还在 begin_render/end_render 之间，先 end_render");
    }
    c.inFrame = false;
    c.frame = nullptr;
    presentFrame(sc);
    sc.frames++;
    sc.dev->frames_presented++;
    sc.dev->frame_index++;
    return BGPU_OK;
}

BgpuResult BackendBase::beginRender(Cmd& c, Texture& target, Texture* depth,
                                    const BgpuRenderBeginDesc& desc) {
    if (c.inRender) {
        return fail(BGPU_ERR_STATE, __FILE__, __LINE__, "已经在渲染里（重复 begin_render）");
    }
    if (!(target.tex_flags & BGPU_TEXTURE_RENDER_TARGET) && !target.borrowed) {
        return fail(BGPU_ERR_STATE, __FILE__, __LINE__,
                    "target 纹理没有 BGPU_TEXTURE_RENDER_TARGET 标志，不能当渲染目标");
    }
    if (depth) {
        BgpuResult r = prepareDepth(*depth, target, desc);
        if (r != BGPU_OK) return r;
    }
    if (desc.flags & BGPU_RENDER_CLEAR_COLOR) {
        BgpuResult r = clearColor(target, desc.clear_color);
        if (r != BGPU_OK) return r;
    }
    c.inRender = true;
    c.target = &target;
    c.depth = depth;
    /* 每次 begin_render 把状态复位到"整目标 + 单位仿射 + 无裁剪"：
     * 与 GPU 后端的 pass 起点一致，免得上一趟的 scissor 漏到下一趟。 */
    c.vp[0] = 0.0f;
    c.vp[1] = 0.0f;
    c.vp[2] = static_cast<float>(target.w);
    c.vp[3] = static_cast<float>(target.h);
    c.scissorOn = false;
    c.sc[0] = 0;
    c.sc[1] = 0;
    c.sc[2] = static_cast<int32_t>(target.w);
    c.sc[3] = static_cast<int32_t>(target.h);
    c.affineOn = false;
    return BGPU_OK;
}

BgpuResult BackendBase::endRender(Cmd& c) {
    if (!c.inRender) {
        return fail(BGPU_ERR_STATE, __FILE__, __LINE__,
                    "没有正在进行的渲染（end_render 前没有 begin_render）");
    }
    c.inRender = false;
    c.target = nullptr;
    c.depth = nullptr;
    return BGPU_OK;
}

BgpuResult BackendBase::setViewport(Cmd& c, float x, float y, float w, float h) {
    c.vp[0] = x;
    c.vp[1] = y;
    c.vp[2] = w;
    c.vp[3] = h;
    return BGPU_OK;
}

BgpuResult BackendBase::setScissor(Cmd& c, int32_t x, int32_t y, int32_t w, int32_t h) {
    if (w < 0 || h < 0) {
        c.scissorOn = false;   /* 负数 = 关掉裁剪（整目标） */
        return BGPU_OK;
    }
    c.scissorOn = true;
    c.sc[0] = x;
    c.sc[1] = y;
    c.sc[2] = w;
    c.sc[3] = h;
    return BGPU_OK;
}

BgpuResult BackendBase::setPipeline(Cmd& c, Pipeline& pipe) {
    c.pipe = &pipe;
    return BGPU_OK;
}

BgpuResult BackendBase::setTexture(Cmd& c, Texture* tex, Sampler* samp) {
    c.tex = tex;
    c.samp = samp;
    return BGPU_OK;
}

BgpuResult BackendBase::setAffine(Cmd& c, const float* xf) {
    if (!xf) {
        c.affineOn = false;
        return BGPU_OK;
    }
    for (int i = 0; i < 6; i++) c.xf[i] = xf[i];
    c.affineOn = true;
    return BGPU_OK;
}

BgpuResult BackendBase::drawQuads(Cmd& c, const BgpuVertex* verts, uint32_t quad_count) {
    if (!c.inRender) {
        return fail(BGPU_ERR_STATE, __FILE__, __LINE__, "draw 之前没有 begin_render");
    }
    if (!c.pipe) {
        return fail(BGPU_ERR_STATE, __FILE__, __LINE__, "draw 之前没有 set_pipeline");
    }
    if (c.pipe->layout != BGPU_VERTEX_2D) {
        return fail(BGPU_ERR_STATE, __FILE__, __LINE__,
                    "draw_quads 需要 BGPU_VERTEX_2D 管线的顶点布局");
    }
    if (!verts || quad_count == 0) return BGPU_OK;
    c.dev->draws++;
    c.dev->quads += quad_count;
    c.dev->tris += static_cast<uint64_t>(quad_count) * 2u;
    return rasterQuads(c, verts, quad_count);
}

BgpuResult BackendBase::drawIndexed(Cmd& c, Buffer& vb, uint32_t vertex_offset, Buffer& ib,
                                    uint32_t index_count, uint32_t index_offset) {
    if (!c.inRender) {
        return fail(BGPU_ERR_STATE, __FILE__, __LINE__,
                    "draw_indexed 之前没有 begin_render");
    }
    if (!c.pipe) {
        return fail(BGPU_ERR_STATE, __FILE__, __LINE__,
                    "draw_indexed 之前没有 set_pipeline");
    }
    if (c.pipe->layout != BGPU_VERTEX_MESH) {
        return fail(BGPU_ERR_STATE, __FILE__, __LINE__,
                    "draw_indexed 需要 BGPU_VERTEX_MESH 管线的顶点布局");
    }
    /* 偏移是**字节**（与 buffer_update 一致），见 docs/raster_rules.md §11 */
    const uint32_t vsize = static_cast<uint32_t>(sizeof(BgpuMeshVertex));
    if (vertex_offset % vsize != 0) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__,
                    cat("vertex_offset=", vertex_offset, " 不是 ", vsize,
                        " 的倍数（按字节偏移）"));
    }
    if (index_offset % 4u != 0) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__,
                    cat("index_offset=", index_offset, " 不是 4 的倍数（32 位索引，按字节偏移）"));
    }
    if (static_cast<uint64_t>(index_offset) + static_cast<uint64_t>(index_count) * 4u >
        ib.size) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__,
                    cat("索引区间超出索引缓冲（offset=", index_offset, " count=", index_count,
                        " size=", ib.size, "）"));
    }
    if (vertex_offset > vb.size) {
        return fail(BGPU_ERR_INVALID_ARGUMENT, __FILE__, __LINE__,
                    cat("vertex_offset=", vertex_offset, " 超出顶点缓冲 ", vb.size, " 字节"));
    }
    c.dev->draws++;
    c.dev->tris += index_count / 3u;
    return rasterIndexed(c, vb, vertex_offset, ib, index_count, index_offset);
}

}  // namespace bgpu
