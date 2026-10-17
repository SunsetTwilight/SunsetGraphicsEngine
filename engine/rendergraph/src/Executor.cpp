// ============================================================
//  engine/rendergraph/src/Executor.cpp
// ============================================================
#include "gfx/rendergraph/Executor.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gfx::rg
{
namespace
{
constexpr uint32_t kTimestampRing = 4;     // 読み出しを待つフレーム数
constexpr uint32_t kMaxTimestamps = 256;   // 1 フレームのタイムスタンプの上限 (パスの数 + 1)
constexpr uint64_t kKeepFrames    = 3;     // 使わなくなった一時リソースを残すフレーム数

bool hasStencil(GfxFormat f)
{
	return f == GfxFormat::D24_Unorm_S8_Uint || f == GfxFormat::D32_Float_S8X24_Uint;
}

// ディスクリプタテーブルの大きさと、offset の位置のディスクリプタの種類
struct TableInfo
{
	uint64_t                            size = 0;
	std::vector<GfxDescriptorRangeType> types;   // 先頭からの種類 (size が大きすぎるときは空)
};

TableInfo tableInfo(const GfxBindingLayoutDesc& layout, const GfxRootParameter& p)
{
	TableInfo info;
	struct R { uint64_t start, count; GfxDescriptorRangeType type; };
	std::vector<R> ranges;
	uint64_t cursor = 0;
	for (uint32_t i = 0; i < p.numRanges; ++i)
	{
		const uint32_t ri = p.firstRange + i;
		if (ri >= layout.numRanges || ri >= GFX_MAX_DESCRIPTOR_RANGES)
			break;
		const GfxDescriptorRange& r = layout.ranges[ri];
		const uint64_t start = r.offsetInDescriptorsFromTableStart == GFX_DESCRIPTOR_RANGE_OFFSET_APPEND
			? cursor : r.offsetInDescriptorsFromTableStart;
		const uint64_t count = r.numDescriptors == GFX_UNBOUNDED_DESCRIPTOR_COUNT ? 0xFFFFFFFFull : r.numDescriptors;
		ranges.push_back({ start, count, r.rangeType });
		cursor    = start + count;
		info.size = std::max(info.size, cursor);
	}
	if (info.size <= 1024)
	{
		info.types.assign(static_cast<size_t>(info.size), GfxDescriptorRangeType::SRV);
		for (const auto& r : ranges)
			for (uint64_t k = r.start; k < r.start + r.count; ++k)
				info.types[static_cast<size_t>(k)] = r.type;
	}
	return info;
}

GfxDescriptor nullDescriptor(GfxDescriptorRangeType t)
{
	GfxDescriptor d;
	d.type = t == GfxDescriptorRangeType::UAV ? GfxDescriptorType::NullUAV
	       : t == GfxDescriptorRangeType::CBV ? GfxDescriptorType::NullCBV
	       :                                    GfxDescriptorType::NullSRV;
	return d;
}

// 参照のサブリソースの範囲
struct Range
{
	uint32_t m0 = 0, mc = 1, s0 = 0, sc = 1;
	bool whole(uint32_t mips, uint32_t layers) const { return m0 == 0 && mc == mips && s0 == 0 && sc == layers; }
};

Range rangeOf(const PassResourceRef& r, uint32_t mips, uint32_t layers)
{
	Range g;
	g.m0 = r.mip.all() ? 0 : static_cast<uint32_t>(r.mip.value);
	g.mc = r.mip.all() ? mips : r.mipCount;
	g.s0 = r.slice.all() ? 0 : static_cast<uint32_t>(r.slice.value);
	g.sc = r.slice.all() ? layers : r.sliceCount;
	return g;
}

GfxTextureDimension toTextureDimension(TextureDimension d)
{
	switch (d)
	{
	case TextureDimension::Cube:  return GfxTextureDimension::TextureCube;
	case TextureDimension::Tex3D: return GfxTextureDimension::Texture3D;
	default:                      return GfxTextureDimension::Texture2D;
	}
}
} // namespace

// ============================================================
//  内部の状態
// ============================================================
struct Executor::Impl
{
	// ---- 一時リソースのプール ----
	struct TexKey
	{
		GfxTextureDimension dimension = GfxTextureDimension::Texture2D;
		uint32_t            width = 0, height = 0, depthOrArraySize = 1, mips = 1, samples = 1;
		GfxFormat           format = GfxFormat::Unknown;
		GfxTextureUsage     usage  = GfxTextureUsage::None;
		bool                useClear = false;
		float               clearColor[4] = {};
		float               clearDepth    = 1.0f;
		uint8_t             clearStencil  = 0;
		bool operator==(const TexKey&) const = default;
	};
	struct ViewKey
	{
		GfxViewType      type = GfxViewType::SRV;
		GfxViewDimension dimension = GfxViewDimension::Texture2D;
		uint32_t         m0 = 0, mc = 1, s0 = 0, sc = 1;
		GfxFormat        format = GfxFormat::Unknown;
		bool             readOnly = false;
		bool operator==(const ViewKey&) const = default;
	};
	struct PooledTexture
	{
		TexKey                                    key;
		TextureHandle                             handle;
		std::string                               name;
		uint64_t                                  lastUsed = 0;
		bool                                      inUse    = false;
		std::vector<std::pair<ViewKey, ViewHandle>> views;
	};
	struct PooledBuffer
	{
		uint64_t     size = 0;
		BufferHandle handle;
		uint64_t     lastUsed = 0;
		bool         inUse    = false;
		ViewHandle   rawSrv, rawUav;
	};

	// ---- このフレームのリソース ----
	struct Res
	{
		const ResourceInfo* info = nullptr;
		const ResourceDesc* desc = nullptr;     // 宣言 (インポートは nullptr のことがある)
		bool                imported = false;
		ImportedResource    imp;
		PooledTexture*      tex = nullptr;
		PooledBuffer*       buf = nullptr;
		uint32_t            width = 0, height = 0;   // mip 0
		GfxResourceRef      ref;
		bool                isTexture() const { return tex || imp.renderTarget || imp.depthStencil || imp.texture.valid(); }
		BufferHandle        buffer() const { return buf ? buf->handle : imp.buffer; }
	};

	Impl(GraphicsEngine& e, PipelineLibrary& p, Executor& s) : engine(e), pipelines(p), self(s) {}

	GraphicsEngine&  engine;
	PipelineLibrary& pipelines;
	Executor&        self;
	uint64_t         frame = 0;

	std::vector<std::unique_ptr<PooledTexture>> textures;
	std::vector<std::unique_ptr<PooledBuffer>>  buffers;
	std::vector<ViewHandle>                     frameViews;   // インポートのビュー (記録の最後に破棄する)

	// ---- タイムスタンプ ----
	struct Slot
	{
		bool                     pending = false;
		uint64_t                 frame   = 0;
		uint32_t                 count   = 0;    // タイムスタンプの数
		std::vector<std::string> names;          // パスの名前 (count - 1 個)
	};
	QueryPoolHandle queries;
	bool            queriesFailed = false;
	Slot            slots[kTimestampRing];

	// ========================================================
	void releaseUnused()
	{
		for (auto it = textures.begin(); it != textures.end();)
		{
			if (!(*it)->inUse && frame - (*it)->lastUsed > kKeepFrames)
			{
				destroyTexture(**it);
				it = textures.erase(it);
			}
			else
				++it;
		}
		for (auto it = buffers.begin(); it != buffers.end();)
		{
			if (!(*it)->inUse && frame - (*it)->lastUsed > kKeepFrames)
			{
				destroyBuffer(**it);
				it = buffers.erase(it);
			}
			else
				++it;
		}
	}
	void destroyTexture(PooledTexture& t)
	{
		for (auto& [k, v] : t.views) engine.destroyView(v);
		engine.destroyTexture(t.handle);
	}
	void destroyBuffer(PooledBuffer& b)
	{
		if (b.rawSrv.valid()) engine.destroyView(b.rawSrv);
		if (b.rawUav.valid()) engine.destroyView(b.rawUav);
		engine.destroyBuffer(b.handle);
	}
	void clear()
	{
		for (auto& t : textures) destroyTexture(*t);
		for (auto& b : buffers) destroyBuffer(*b);
		textures.clear();
		buffers.clear();
		for (auto v : frameViews) engine.destroyView(v);
		frameViews.clear();
		if (queries.valid()) engine.destroyQueryPool(queries);
		queries = {};
		for (auto& s : slots) s = {};
	}

	PooledTexture* acquireTexture(const TexKey& key, const std::string& name)
	{
		for (auto& t : textures)
			if (!t->inUse && t->key == key)
			{
				t->inUse    = true;
				t->lastUsed = frame;
				return t.get();
			}
		auto t = std::make_unique<PooledTexture>();
		t->key  = key;
		t->name = "rg:" + name;
		GfxTextureResourceDesc d;
		d.dimension        = key.dimension;
		d.width            = key.width;
		d.height           = key.height;
		d.depthOrArraySize = key.depthOrArraySize;
		d.mipLevels        = key.mips;
		d.format           = key.format;
		d.sampleCount      = key.samples;
		d.usage            = key.usage;
		d.useClearValue    = key.useClear;
		d.clearValue.format = key.format;
		std::memcpy(d.clearValue.color, key.clearColor, sizeof(key.clearColor));
		d.clearValue.depth   = key.clearDepth;
		d.clearValue.stencil = key.clearStencil;
		d.debugName = t->name.c_str();
		t->handle = engine.createTexture(d);
		if (!t->handle.valid())
			return nullptr;
		t->inUse    = true;
		t->lastUsed = frame;
		textures.push_back(std::move(t));
		return textures.back().get();
	}

	PooledBuffer* acquireBuffer(uint64_t size)
	{
		for (auto& b : buffers)
			if (!b->inUse && b->size == size)
			{
				b->inUse    = true;
				b->lastUsed = frame;
				return b.get();
			}
		auto b = std::make_unique<PooledBuffer>();
		b->size   = size;
		b->handle = engine.createBuffer({ size, BufferUsage::Storage, nullptr, 0 });
		if (!b->handle.valid())
			return nullptr;
		b->inUse    = true;
		b->lastUsed = frame;
		buffers.push_back(std::move(b));
		return buffers.back().get();
	}

	ViewHandle textureView(PooledTexture& t, const ViewKey& k)
	{
		for (const auto& [key, v] : t.views)
			if (key == k)
				return v;
		const ViewHandle v = createTextureView(gfxRef(t.handle), k, t.name.c_str());
		if (v.valid())
			t.views.emplace_back(k, v);
		return v;
	}

	ViewHandle createTextureView(GfxResourceRef ref, const ViewKey& k, const char* name)
	{
		GfxViewDesc d;
		d.type            = k.type;
		d.resource        = ref;
		d.dimension       = k.dimension;
		d.format          = k.format;
		d.mostDetailedMip = k.m0;
		d.mipLevels       = k.type == GfxViewType::SRV ? k.mc : GFX_ALL_MIPS;
		d.firstArraySlice = k.s0;
		d.arraySize       = k.sc;
		if (k.readOnly)
			d.dsvFlags = hasStencil(k.format) ? GfxDSVFlags::ReadOnlyDepth | GfxDSVFlags::ReadOnlyStencil : GfxDSVFlags::ReadOnlyDepth;
		d.debugName = name;
		return engine.createView(d);
	}

	ViewHandle bufferView(Res& r, bool uav)
	{
		const BufferHandle bh = r.buffer();
		uint64_t size = r.buf ? r.buf->size : 0;
		if (r.imported && r.desc) size = r.desc->byteSize;
		if (!bh.valid() || size < 4)
			return {};
		if (r.buf)
		{
			ViewHandle& cached = uav ? r.buf->rawUav : r.buf->rawSrv;
			if (cached.valid())
				return cached;
		}
		GfxViewDesc d;
		d.type        = uav ? GfxViewType::UAV : GfxViewType::SRV;
		d.resource    = gfxRef(bh);
		d.dimension   = GfxViewDimension::Buffer;
		d.rawBuffer   = true;
		d.numElements = static_cast<uint32_t>(size / 4);
		d.debugName   = "rg:buffer";
		const ViewHandle v = engine.createView(d);
		if (r.buf)
			(uav ? r.buf->rawUav : r.buf->rawSrv) = v;
		else if (v.valid())
			frameViews.push_back(v);
		return v;
	}

	// テクスチャのビューの次元
	static GfxViewDimension viewDimension(const ResourceDesc* rd, GfxViewType type, const Range& g, uint32_t layers)
	{
		if (!rd) return GfxViewDimension::Texture2D;
		if (rd->dimension == TextureDimension::Tex3D) return GfxViewDimension::Texture3D;
		if (rd->sampleCount > 1) return layers > 1 ? GfxViewDimension::Texture2DMSArray : GfxViewDimension::Texture2DMS;
		if (rd->dimension == TextureDimension::Cube && type == GfxViewType::SRV && g.s0 == 0 && g.sc == layers)
			return rd->arraySize > 1 ? GfxViewDimension::TextureCubeArray : GfxViewDimension::TextureCube;
		return layers > 1 ? GfxViewDimension::Texture2DArray : GfxViewDimension::Texture2D;
	}

	// SRV / UAV のディスクリプタ。作れなければ type = NullSRV で valid = false
	bool shaderDescriptor(Res& r, const PassResourceRef& ref, bool uav, GfxDescriptor& out, std::string& why)
	{
		if (!r.isTexture())
		{
			const ViewHandle v = bufferView(r, uav);
			if (!v.valid()) { why = "バッファのビューを作れません"; return false; }
			out = gfxView(v);
			return true;
		}
		const uint32_t mips   = r.info->mips;
		const uint32_t layers = r.info->layers;
		const Range    g      = rangeOf(ref, mips, layers);
		const bool     plain  = g.whole(mips, layers) && ref.viewFormat == GfxFormat::Unknown && (!uav || mips == 1);
		if (r.imported)
		{
			if (plain && !uav && r.imp.renderTarget) { out = gfxSRV(r.imp.renderTarget->asTexture()); return true; }
			if (plain && !uav && r.imp.depthStencil) { out = gfxSRV(r.imp.depthStencil->asTexture()); return true; }
			if (!r.imp.texture.valid()) { why = "SRV / UAV には ImportedResource::texture が必要です"; return false; }
			if (plain) { out = uav ? gfxUAV(r.imp.texture) : gfxSRV(r.imp.texture); return true; }
			ViewKey k{ uav ? GfxViewType::UAV : GfxViewType::SRV, GfxViewDimension::Texture2D, g.m0, g.mc, g.s0, g.sc, ref.viewFormat, false };
			const ViewHandle v = createTextureView(gfxRef(r.imp.texture), k, ref.res.c_str());
			if (!v.valid()) { why = "ビューを作れません"; return false; }
			frameViews.push_back(v);
			out = gfxView(v);
			return true;
		}
		if (plain) { out = uav ? gfxUAV(r.tex->handle) : gfxSRV(r.tex->handle); return true; }
		const GfxViewType type = uav ? GfxViewType::UAV : GfxViewType::SRV;
		ViewKey k{ type, viewDimension(r.desc, type, g, layers), g.m0, g.mc, g.s0, g.sc, ref.viewFormat, false };
		const ViewHandle v = textureView(*r.tex, k);
		if (!v.valid()) { why = "ビューを作れません"; return false; }
		out = gfxView(v);
		return true;
	}

	// ========================================================
	//  記録
	// ========================================================
	bool record(ICommandBuffer* cb, const GraphDesc& graph, const CompiledGraph& c, const FrameInputs& in);
	bool prepare(const GraphDesc& graph, const CompiledGraph& c, const FrameInputs& in, std::vector<Res>& res);
	void recordPass(ICommandBuffer* cb, const CompiledPass& pass, std::vector<Res>& res, const FrameInputs& in, const std::vector<std::vector<uint32_t>>& firstUse);
	bool setTargets(ICommandBuffer* cb, const CompiledPass& pass, std::vector<Res>& res, const CompiledGraph& c, PipelineTargets& targets, uint32_t& w, uint32_t& h);
	void bindResources(ICommandBuffer* cb, const CompiledPass& pass, std::vector<Res>& res, const CompiledGraph& c, const DrawContext& ctx, IDrawListProvider* provider);
	void recordCopy(ICommandBuffer* cb, const CompiledPass& pass, std::vector<Res>& res, const CompiledGraph& c);
	void harvestTimings();

	const CompiledGraph* current = nullptr;
	Res* find(std::vector<Res>& res, const std::string& name)
	{
		const ResourceInfo* info = current->findResource(name);
		return info ? &res[static_cast<size_t>(info - current->resources.data())] : nullptr;
	}
};

// ============================================================
//  準備: 検証と一時リソースの確保
// ============================================================
bool Executor::Impl::prepare(const GraphDesc& graph, const CompiledGraph& c, const FrameInputs& in, std::vector<Res>& res)
{
	bool ok = true;
	if (!c.ok)
	{
		self.error("コンパイルのエラーがあるグラフは実行できません");
		return false;
	}
	if (in.width == 0 || in.height == 0)
	{
		self.error("FrameInputs の width / height が 0 です");
		return false;
	}
	for (const auto& p : c.passes)
		for (const auto& b : p.barriers)
			if (b.subresource != GFX_ALL_SUBRESOURCES)
			{
				self.error("passes[" + p.name + "]: '" + c.resources[b.resource].name
				           + "' はサブリソースごとに状態が変わります。M3b の実行はサブリソース単位のバリアに対応していません");
				ok = false;
			}

	res.assign(c.resources.size(), {});
	for (size_t i = 0; i < c.resources.size(); ++i)
	{
		const ResourceInfo& info = c.resources[i];
		Res& r = res[i];
		r.info     = &info;
		r.desc     = info.descIndex >= 0 ? &graph.resources[static_cast<size_t>(info.descIndex)] : nullptr;
		r.imported = info.imported();
		if (!info.used())
			continue;
		if (info.isPrev() || (r.desc && r.desc->history))
		{
			self.error("resources." + info.name + ": history / @prev は M3b の実行では使えません");
			ok = false;
			continue;
		}
		if (r.imported)
		{
			auto it = in.imports.find(info.name);
			if (it == in.imports.end())
			{
				self.error("resources." + info.name + ": アプリが渡していません (FrameInputs::imports)");
				ok = false;
				continue;
			}
			r.imp = it->second;
			if (r.imp.renderTarget)          { r.ref = gfxRef(r.imp.renderTarget); r.width = r.imp.renderTarget->getWidth(); r.height = r.imp.renderTarget->getHeight(); }
			else if (r.imp.depthStencil)     { r.ref = gfxRef(r.imp.depthStencil); r.width = r.imp.depthStencil->getWidth(); r.height = r.imp.depthStencil->getHeight(); }
			else if (r.imp.texture.valid())  { r.ref = gfxRef(r.imp.texture); r.width = in.width; r.height = in.height; }
			else if (r.imp.buffer.valid())   { r.ref = gfxRef(r.imp.buffer); }
			else
			{
				self.error("resources." + info.name + ": ImportedResource が空です");
				ok = false;
			}
			continue;
		}
		if (!r.desc)
			continue;
		const ResourceDesc& rd = *r.desc;
		if (rd.kind == ResourceKind::Buffer)
		{
			r.buf = acquireBuffer(rd.byteSize);
			if (!r.buf) { self.error("resources." + info.name + ": バッファを作れません"); ok = false; continue; }
			r.ref = gfxRef(r.buf->handle);
			continue;
		}

		// テクスチャ: 使い方から作成のフラグを決める。SRV は常に付ける
		// (DSVRead の DepthRead はシェーダーから読める状態を含むので、DENY_SHADER_RESOURCE だと遷移できない。
		//  グラフの編集でパスが減っても、同じ大きさなら同じテクスチャを使い続けられる)
		GfxTextureUsage usage = GfxTextureUsage::ShaderResource;
		for (const auto& p : c.passes)
			for (const auto* refs : { &p.desc.inputs, &p.desc.outputs })
				for (const auto& ref : *refs)
				{
					if (ref.res != info.name) continue;
					switch (ref.as)
					{
					case Access::SRV:     usage = usage | GfxTextureUsage::ShaderResource;  break;
					case Access::UAV:     usage = usage | GfxTextureUsage::UnorderedAccess; break;
					case Access::RTV:     usage = usage | GfxTextureUsage::RenderTarget;    break;
					case Access::DSV:
					case Access::DSVRead: usage = usage | GfxTextureUsage::DepthStencil;    break;
					default: break;
					}
				}
		TexKey k;
		k.dimension = toTextureDimension(rd.dimension);
		if (rd.size.relative)
		{
			k.width  = std::max(1u, static_cast<uint32_t>(std::lround(in.width * rd.size.scale)));
			k.height = std::max(1u, static_cast<uint32_t>(std::lround(in.height * rd.size.scale)));
		}
		else
		{
			k.width  = rd.size.width;
			k.height = rd.size.height;
		}
		k.depthOrArraySize = rd.dimension == TextureDimension::Tex3D ? rd.depth : info.layers;
		k.mips    = rd.mipLevels;
		k.samples = rd.sampleCount;
		k.format  = rd.format;
		k.usage   = usage;
		const bool rt = (usage & GfxTextureUsage::RenderTarget) != GfxTextureUsage::None;
		const bool ds = (usage & GfxTextureUsage::DepthStencil) != GfxTextureUsage::None;
		if (rt || ds)
		{
			k.useClear = true;
			if (rd.clear.hasColor) std::memcpy(k.clearColor, rd.clear.color, sizeof(k.clearColor));
			k.clearDepth   = rd.clear.depth;
			k.clearStencil = rd.clear.stencil;
		}
		r.tex = acquireTexture(k, info.name);
		if (!r.tex) { self.error("resources." + info.name + ": テクスチャを作れません (詳しくはデバッグメッセージ)"); ok = false; continue; }
		r.ref    = gfxRef(r.tex->handle);
		r.width  = k.width;
		r.height = k.height;
	}
	return ok;
}

// ============================================================
//  出力の設定 (Raster)
// ============================================================
bool Executor::Impl::setTargets(ICommandBuffer* cb, const CompiledPass& pass, std::vector<Res>& res, const CompiledGraph& c,
                                PipelineTargets& targets, uint32_t& w, uint32_t& h)
{
	(void)c;
	const PassDesc& p = pass.desc;
	struct Target { const PassResourceRef* ref; Res* r; };
	std::vector<Target> rtvs;
	Target dsv{ nullptr, nullptr };
	for (const auto* refs : { &p.outputs, &p.inputs })
		for (const auto& ref : *refs)
		{
			if (ref.as == Access::RTV)                                 rtvs.push_back({ &ref, find(res, ref.res) });
			else if (ref.as == Access::DSV || ref.as == Access::DSVRead) dsv = { &ref, find(res, ref.res) };
		}
	std::sort(rtvs.begin(), rtvs.end(), [](const Target& a, const Target& b) { return a.ref->rtvIndex < b.ref->rtvIndex; });
	for (size_t i = 0; i < rtvs.size(); ++i)
		if (rtvs[i].ref->rtvIndex != i)
		{
			self.error("passes[" + pass.name + "]: RTV の番号は 0 から順に使ってください (M3b の実行の制限)");
			return false;
		}

	auto legacy = [](const Res* r) { return r && r->imported && (r->imp.renderTarget || r->imp.depthStencil) && !r->imp.texture.valid(); };
	bool anyLegacy = false, allLegacy = true;
	for (const auto& t : rtvs) { anyLegacy |= legacy(t.r); allLegacy &= legacy(t.r); }
	if (dsv.r) { anyLegacy |= legacy(dsv.r); allLegacy &= legacy(dsv.r); }

	targets = {};
	targets.numRenderTargets = static_cast<uint32_t>(rtvs.size());
	w = h = 0;

	if (anyLegacy)
	{
		// アプリが IRenderTarget / IDepthStencil で渡した出力 (バックバッファなど)
		if (!allLegacy)
		{
			self.error("passes[" + pass.name + "]: アプリが IRenderTarget / IDepthStencil で渡した出力と、グラフのテクスチャを同じパスの出力に混ぜられません (M3b の実行の制限)");
			return false;
		}
		if (dsv.ref && dsv.ref->as == Access::DSVRead)
		{
			self.error("passes[" + pass.name + "]: IDepthStencil で渡した深度は DSVRead に使えません (ImportedResource::texture で渡してください)");
			return false;
		}
		std::vector<IRenderTarget*> rts;
		for (const auto& t : rtvs)
		{
			if (!t.r->imp.renderTarget) { self.error("passes[" + pass.name + "]: '" + t.ref->res + "' は IRenderTarget ではありません"); return false; }
			rts.push_back(t.r->imp.renderTarget);
			targets.rtvFormats[t.ref->rtvIndex] = t.ref->viewFormat != GfxFormat::Unknown ? t.ref->viewFormat : gfxToFormat(t.r->imp.renderTarget->getFormat());
		}
		IDepthStencil* ds = nullptr;
		if (dsv.r)
		{
			if (!dsv.r->imp.depthStencil) { self.error("passes[" + pass.name + "]: '" + dsv.ref->res + "' は IDepthStencil ではありません"); return false; }
			ds = dsv.r->imp.depthStencil;
			targets.dsvFormat = gfxToFormat(ds->getFormat());
		}
		cb->setRenderTargets(static_cast<uint32_t>(rts.size()), rts.data(), ds, false, false);
		const float black[4] = { 0, 0, 0, 1 };
		for (const auto& t : rtvs)
			if (t.ref->load == LoadOp::Clear)
				cb->clearRenderTarget(t.r->imp.renderTarget, black);
		if (ds && dsv.ref->load == LoadOp::Clear)
			cb->clearDepthStencil(ds, hasStencil(targets.dsvFormat) ? GfxClearFlags::Depth | GfxClearFlags::Stencil : GfxClearFlags::Depth, 1.0f, 0);
		if (!rts.empty())    { w = rts[0]->getWidth(); h = rts[0]->getHeight(); }
		else if (ds)         { w = ds->getWidth();     h = ds->getHeight(); }
		return true;
	}

	// グラフのテクスチャ (または ImportedResource::texture) のビュー
	auto makeView = [&](const Target& t, GfxViewType type, bool readOnly, GfxFormat& fmt) -> ViewHandle {
		Res& r = *t.r;
		const Range g = rangeOf(*t.ref, r.info->mips, r.info->layers);
		if (r.imported)
		{
			if (!r.imp.texture.valid() || t.ref->viewFormat == GfxFormat::Unknown)
			{
				self.error("passes[" + pass.name + "]: '" + t.ref->res + "' を出力にするには ImportedResource::texture と、参照の format が必要です");
				return {};
			}
			fmt = t.ref->viewFormat;
			ViewKey k{ type, r.info->layers > 1 ? GfxViewDimension::Texture2DArray : GfxViewDimension::Texture2D, g.m0, 1, g.s0, g.sc, fmt, readOnly };
			const ViewHandle v = createTextureView(r.ref, k, t.ref->res.c_str());
			if (v.valid()) frameViews.push_back(v);
			return v;
		}
		fmt = t.ref->viewFormat != GfxFormat::Unknown ? t.ref->viewFormat : r.desc->format;
		ViewKey k{ type, viewDimension(r.desc, type, g, r.info->layers), g.m0, 1, g.s0, g.sc, t.ref->viewFormat, readOnly };
		if (w == 0) { w = std::max(1u, r.width >> g.m0); h = std::max(1u, r.height >> g.m0); }
		return textureView(*r.tex, k);
	};

	std::vector<ViewHandle> rtvViews;
	for (const auto& t : rtvs)
	{
		GfxFormat fmt = GfxFormat::Unknown;
		const ViewHandle v = makeView(t, GfxViewType::RTV, false, fmt);
		if (!v.valid()) { self.error("passes[" + pass.name + "]: '" + t.ref->res + "' の RTV を作れません"); return false; }
		rtvViews.push_back(v);
		targets.rtvFormats[t.ref->rtvIndex] = fmt;
		if (t.r->desc) targets.sampleCount = t.r->desc->sampleCount;
	}
	ViewHandle dsvView;
	if (dsv.r)
	{
		GfxFormat fmt = GfxFormat::Unknown;
		dsvView = makeView(dsv, GfxViewType::DSV, dsv.ref->as == Access::DSVRead, fmt);
		if (!dsvView.valid()) { self.error("passes[" + pass.name + "]: '" + dsv.ref->res + "' の DSV を作れません"); return false; }
		targets.dsvFormat = fmt;
		if (dsv.r->desc) targets.sampleCount = dsv.r->desc->sampleCount;
	}
	cb->setRenderTargetViews(static_cast<uint32_t>(rtvViews.size()), rtvViews.data(), dsvView);
	for (size_t i = 0; i < rtvs.size(); ++i)
		if (rtvs[i].ref->load == LoadOp::Clear)
		{
			const float zero[4] = { 0, 0, 0, 0 };
			const ResourceDesc* rd = rtvs[i].r->desc;
			cb->clearRenderTargetView(rtvViews[i], rd && rd->clear.hasColor ? rd->clear.color : zero);
		}
	if (dsv.r && dsv.ref->as == Access::DSV && dsv.ref->load == LoadOp::Clear)
	{
		const ResourceDesc* rd = dsv.r->desc;
		cb->clearDepthStencilView(dsvView, hasStencil(targets.dsvFormat) ? GfxClearFlags::Depth | GfxClearFlags::Stencil : GfxClearFlags::Depth,
		                          rd ? rd->clear.depth : 1.0f, rd ? rd->clear.stencil : 0);
	}
	return true;
}

// ============================================================
//  bind の指定どおりにつなぐ
// ============================================================
void Executor::Impl::bindResources(ICommandBuffer* cb, const CompiledPass& pass, std::vector<Res>& res, const CompiledGraph& c,
                                   const DrawContext& ctx, IDrawListProvider* provider)
{
	(void)c;
	const PassDesc& p = pass.desc;
	const GfxBindingLayoutDesc* layout = pipelines.bindingLayout(p.pipeline);
	if (!layout)
		return;
	const std::string where = "passes[" + pass.name + "]";

	std::map<int32_t, std::vector<GfxDescriptor>> tables;
	auto table = [&](int32_t param) -> std::vector<GfxDescriptor>* {
		auto it = tables.find(param);
		if (it != tables.end()) return &it->second;
		const TableInfo info = tableInfo(*layout, layout->parameters[param]);
		std::vector<GfxDescriptor> d;
		for (auto t : info.types)
		{
			if (t == GfxDescriptorRangeType::Sampler)
			{
				self.error(where + ": param " + std::to_string(param) + " は Sampler のテーブルです。M3b の実行では静的サンプラーを使ってください");
				return nullptr;
			}
			d.push_back(nullDescriptor(t));
		}
		return &(tables[param] = std::move(d));
	};
	auto put = [&](const Binding& b, const GfxDescriptor& desc, const std::string& path) {
		if (b.param < 0 || static_cast<uint32_t>(b.param) >= layout->numParameters) return;
		const GfxRootParameter& rp = layout->parameters[b.param];
		if (rp.parameterType != GfxRootParameterType::DescriptorTable) return;
		auto* t = table(b.param);
		if (!t) return;
		if (b.offset >= t->size())
		{
			if (b.offset >= 1024) { self.error(path + ": offset が大きすぎます"); return; }
			t->resize(b.offset + 1, nullDescriptor(GfxDescriptorRangeType::SRV));
		}
		(*t)[b.offset] = desc;
	};

	// ---- 入力と出力の SRV / UAV ----
	for (const auto* refs : { &p.inputs, &p.outputs })
		for (size_t i = 0; i < refs->size(); ++i)
		{
			const PassResourceRef& ref = (*refs)[i];
			if ((ref.as != Access::SRV && ref.as != Access::UAV) || !ref.bind.valid())
				continue;
			Res* r = find(res, ref.res);
			if (!r) continue;
			const std::string path = where + (refs == &p.inputs ? ".inputs[" : ".outputs[") + std::to_string(i) + "]";
			const bool uav = ref.as == Access::UAV;
			const uint32_t param = static_cast<uint32_t>(ref.bind.param);
			if (param >= layout->numParameters) continue;
			const GfxRootParameter& rp = layout->parameters[param];
			if (rp.parameterType == GfxRootParameterType::SRV || rp.parameterType == GfxRootParameterType::UAV)
			{
				if (!r->buffer().valid()) { self.error(path + ": ルートの SRV / UAV にはバッファが必要です"); continue; }
				cb->setRootDescriptor(param, r->buffer(), 0);
				continue;
			}
			GfxDescriptor desc;
			std::string why;
			if (!shaderDescriptor(*r, ref, uav, desc, why))
			{
				self.error(path + ": " + why);
				continue;
			}
			put(ref.bind, desc, path);
		}

	// ---- 定数 ----
	for (size_t i = 0; i < p.constants.size(); ++i)
	{
		const ConstantRef& cr = p.constants[i];
		const std::string path = where + ".constants[" + std::to_string(i) + "]";
		if (!cr.bind.valid() || static_cast<uint32_t>(cr.bind.param) >= layout->numParameters)
			continue;
		const uint32_t param = static_cast<uint32_t>(cr.bind.param);
		const GfxRootParameter& rp = layout->parameters[param];
		if (cr.source == "inline")
		{
			if (rp.parameterType == GfxRootParameterType::Constants32Bit)
				cb->setRootConstants(param, static_cast<uint32_t>(cr.values.size()), cr.values.data(), cr.bind.offset);
			else
				self.error(path + ": inline の値は、M3b の実行ではルート定数にだけつなげます");
			continue;
		}
		if (rp.parameterType == GfxRootParameterType::Constants32Bit)
			continue;   // アプリが draw の中で設定する
		const BufferHandle buf = provider ? provider->constants(cr.source, ctx) : BufferHandle{};
		if (!buf.valid())
		{
			if (rp.parameterType == GfxRootParameterType::DescriptorTable)
				self.error(path + ": テーブルの CBV には、アプリが constants('" + cr.source + "') で定数バッファを返す必要があります");
			continue;   // ルート CBV はアプリが draw の中で設定してよい
		}
		if (rp.parameterType == GfxRootParameterType::CBV)
			cb->setRootDescriptor(param, buf, 0);
		else if (rp.parameterType == GfxRootParameterType::DescriptorTable)
			put(cr.bind, gfxCBV(buf), path);
	}

	for (auto& [param, descs] : tables)
		if (!descs.empty())
			cb->setDescriptorTable(static_cast<uint32_t>(param), descs.data(), static_cast<uint32_t>(descs.size()));
}

// ============================================================
//  Copy のパス (コピーと MSAA の解決)
// ============================================================
void Executor::Impl::recordCopy(ICommandBuffer* cb, const CompiledPass& pass, std::vector<Res>& res, const CompiledGraph& c)
{
	(void)c;
	const PassDesc& p = pass.desc;
	std::vector<const PassResourceRef*> srcs, dsts;
	for (const auto& r : p.inputs)  if (r.as == Access::CopySrc || r.as == Access::ResolveSrc) srcs.push_back(&r);
	for (const auto& r : p.outputs) if (r.as == Access::CopyDst || r.as == Access::ResolveDst) dsts.push_back(&r);
	if (srcs.size() != dsts.size())
	{
		self.error("passes[" + pass.name + "]: コピー元とコピー先の数が違います (入力と出力を同じ順に並べてください)");
		return;
	}
	for (size_t i = 0; i < srcs.size(); ++i)
	{
		Res* s = find(res, srcs[i]->res);
		Res* d = find(res, dsts[i]->res);
		if (!s || !d) continue;
		if (srcs[i]->as == Access::ResolveSrc)
		{
			const uint32_t layers = std::min(s->info->layers, d->info->layers);
			for (uint32_t l = 0; l < layers; ++l)
				cb->resolve(d->ref, l * d->info->mips, s->ref, l * s->info->mips, dsts[i]->viewFormat);
			continue;
		}
		if (!s->isTexture() && !d->isTexture())
		{
			const uint64_t size = std::min(s->buf ? s->buf->size : (s->desc ? s->desc->byteSize : 0),
			                               d->buf ? d->buf->size : (d->desc ? d->desc->byteSize : 0));
			if (size == 0) { self.error("passes[" + pass.name + "]: コピーするバッファの大きさが分かりません"); continue; }
			cb->copyBuffer(d->buffer(), 0, s->buffer(), 0, size);
			continue;
		}
		const uint32_t subs = std::min(s->info->mips * s->info->layers, d->info->mips * d->info->layers);
		for (uint32_t k = 0; k < subs; ++k)
			cb->copyTexture(d->ref, k, 0, 0, 0, s->ref, k);
	}
}

// ============================================================
//  1 つのパス
// ============================================================
void Executor::Impl::recordPass(ICommandBuffer* cb, const CompiledPass& pass, std::vector<Res>& res, const FrameInputs& in,
                                const std::vector<std::vector<uint32_t>>& firstUse)
{
	const CompiledGraph& c = *current;
	const PassDesc&      p = pass.desc;
	const size_t         k = static_cast<size_t>(&pass - c.passes.data());
	cb->beginEvent(pass.name.c_str());

	// ---- バリア ----
	std::vector<GfxBarrierDesc> barriers;
	for (uint32_t ri : firstUse[k])   // 一時リソース: 前のフレームの最後の状態から、作ったときの状態へ
		barriers.push_back(gfxTransition(res[ri].ref, c.resources[ri].initialState));
	for (const auto& b : pass.barriers)
	{
		const Res& r = res[b.resource];
		if (b.type == GfxBarrierType::UAV)
			barriers.push_back(gfxUAVBarrier(r.ref));
		else
			barriers.push_back(gfxTransition(r.ref, b.after));   // 状態追跡の値から (計画と同じ状態のはず)
	}
	if (!barriers.empty())
		cb->barrier(barriers.data(), static_cast<uint32_t>(barriers.size()));

	DrawContext ctx;
	ctx.cb     = cb;
	ctx.pass   = &pass;
	ctx.width  = in.width;
	ctx.height = in.height;

	if (p.type == PassType::Copy)
	{
		recordCopy(cb, pass, res, c);
		cb->endEvent();
		return;
	}

	// ---- 出力とパイプライン ----
	PipelineHandle pipe;
	if (p.type == PassType::Raster)
	{
		PipelineTargets targets;
		uint32_t w = 0, h = 0;
		if (!setTargets(cb, pass, res, c, targets, w, h))
		{
			cb->endEvent();
			return;
		}
		if (w > 0) { ctx.width = w; ctx.height = h; }
		GfxViewport vp;
		vp.width  = static_cast<float>(ctx.width);
		vp.height = static_cast<float>(ctx.height);
		GfxRect sc;
		sc.right  = static_cast<int32_t>(ctx.width);
		sc.bottom = static_cast<int32_t>(ctx.height);
		auto region = [&](const Region& r, float& x, float& y, float& rw, float& rh) {
			const float sx = r.relative ? static_cast<float>(ctx.width) : 1.0f;
			const float sy = r.relative ? static_cast<float>(ctx.height) : 1.0f;
			x = r.x * sx; y = r.y * sy; rw = r.width * sx; rh = r.height * sy;
		};
		if (p.viewport.set)
			region(p.viewport, vp.x, vp.y, vp.width, vp.height);
		if (p.scissor.set)
		{
			float x, y, rw, rh;
			region(p.scissor, x, y, rw, rh);
			sc.left = static_cast<int32_t>(x); sc.top = static_cast<int32_t>(y);
			sc.right = static_cast<int32_t>(x + rw); sc.bottom = static_cast<int32_t>(y + rh);
		}
		else if (p.viewport.set)
		{
			sc.left = static_cast<int32_t>(vp.x); sc.top = static_cast<int32_t>(vp.y);
			sc.right = static_cast<int32_t>(vp.x + vp.width); sc.bottom = static_cast<int32_t>(vp.y + vp.height);
		}
		cb->setViewports(1, &vp);
		cb->setScissors(1, &sc);
		pipe = pipelines.graphics(p.pipeline, targets);
	}
	else
		pipe = pipelines.compute(p.pipeline);

	if (!pipe.valid())
	{
		self.error("passes[" + pass.name + "]: パイプライン '" + p.pipeline + "' を使えないので、このパスを飛ばします (PipelineLibrary::errors())");
		cb->endEvent();
		return;
	}
	cb->bindPipeline(pipe);
	bindResources(cb, pass, res, c, ctx, in.provider);

	// ---- 内容 ----
	auto indirect = [&](GfxIndirectDesc& d) -> bool {
		Res* args = find(res, p.content.indirect.args);
		if (!args || !args->buffer().valid()) { self.error("passes[" + pass.name + "]: 引数のバッファがありません"); return false; }
		d.argumentBuffer  = args->buffer();
		d.argumentOffset  = p.content.indirect.offset;
		d.maxCommandCount = p.content.indirect.maxCount;
		if (!p.content.indirect.count.empty())
			if (Res* cnt = find(res, p.content.indirect.count))
				d.countBuffer = cnt->buffer();
		return true;
	};
	switch (p.content.kind)
	{
	case ContentKind::DrawList:
		if (in.provider) in.provider->draw(p.content.drawList, ctx);
		else self.error("passes[" + pass.name + "]: drawList には IDrawListProvider が必要です");
		break;
	case ContentKind::Fullscreen:
		cb->drawInstanced(3, 1, 0, 0);
		break;
	case ContentKind::Dispatch:
		cb->dispatch(p.content.dispatch[0], p.content.dispatch[1], p.content.dispatch[2]);
		break;
	case ContentKind::DrawIndirect:
	{
		GfxIndirectDesc d;
		if (indirect(d)) cb->drawIndirect(d);
		break;
	}
	case ContentKind::DispatchIndirect:
	{
		GfxIndirectDesc d;
		if (indirect(d)) cb->dispatchIndirect(d);
		break;
	}
	case ContentKind::Custom:
		if (in.provider) in.provider->custom(p.content.custom, ctx);
		break;
	case ContentKind::None:
		break;
	}
	cb->endEvent();
}

// ============================================================
//  タイムスタンプの読み出し
// ============================================================
void Executor::Impl::harvestTimings()
{
	if (!queries.valid())
		return;
	const double freq = static_cast<double>(engine.getTimestampFrequency());
	uint64_t newest = 0;
	for (uint32_t s = 0; s < kTimestampRing; ++s)
	{
		Slot& slot = slots[s];
		if (!slot.pending)
			continue;
		std::vector<uint64_t> ts(slot.count);
		if (!engine.readQueryResults(queries, s * kMaxTimestamps, slot.count, ts.data()))
			continue;
		slot.pending = false;
		if (slot.frame < newest || freq <= 0)
			continue;
		newest = slot.frame;
		self.m_timings.clear();
		for (uint32_t i = 0; i + 1 < slot.count; ++i)
			self.m_timings.push_back({ slot.names[i], ts[i + 1] >= ts[i] ? (ts[i + 1] - ts[i]) * 1000.0 / freq : 0.0 });
		self.m_totalMs = slot.count > 1 && ts[slot.count - 1] >= ts[0] ? (ts[slot.count - 1] - ts[0]) * 1000.0 / freq : 0.0;
	}
}

// ============================================================
//  記録の全体
// ============================================================
bool Executor::Impl::record(ICommandBuffer* cb, const GraphDesc& graph, const CompiledGraph& c, const FrameInputs& in)
{
	if (!cb)
	{
		self.error("コマンドバッファがありません");
		return false;
	}
	++frame;
	harvestTimings();
	for (auto& t : textures) t->inUse = false;
	for (auto& b : buffers) b->inUse = false;
	current = &c;

	std::vector<Res> res;
	if (!prepare(graph, c, in, res))
	{
		releaseUnused();
		return false;
	}
	releaseUnused();

	// 一時リソースを最初に使うパス
	std::vector<std::vector<uint32_t>> firstUse(c.passes.size());
	for (uint32_t ri = 0; ri < c.resources.size(); ++ri)
	{
		const ResourceInfo& info = c.resources[ri];
		if (info.used() && !info.imported() && !info.isPrev() && (res[ri].tex || res[ri].buf))
			firstUse[static_cast<size_t>(info.firstPass)].push_back(ri);
	}

	// タイムスタンプ
	if (!queries.valid() && !queriesFailed)
	{
		GfxQueryPoolDesc qd;
		qd.type      = GfxQueryType::Timestamp;
		qd.count     = kTimestampRing * kMaxTimestamps;
		qd.debugName = "rg:timestamps";
		queries = engine.createQueryPool(qd);
		queriesFailed = !queries.valid();
	}
	const uint32_t slotIndex = static_cast<uint32_t>(frame % kTimestampRing);
	Slot& slot = slots[slotIndex];
	slot = {};
	const uint32_t base  = slotIndex * kMaxTimestamps;
	const uint32_t timed = static_cast<uint32_t>(std::min<size_t>(c.passes.size(), kMaxTimestamps - 1));
	if (queries.valid())
		cb->writeTimestamp(queries, base);

	for (size_t k = 0; k < c.passes.size(); ++k)
	{
		recordPass(cb, c.passes[k], res, in, firstUse);
		if (queries.valid() && k < timed)
		{
			cb->writeTimestamp(queries, base + 1 + static_cast<uint32_t>(k));
			slot.names.push_back(c.passes[k].name);
		}
	}
	if (queries.valid())
	{
		slot.count   = timed + 1;
		slot.frame   = frame;
		slot.pending = true;
		cb->resolveQueries(queries, base, slot.count);
	}

	for (auto v : frameViews) engine.destroyView(v);   // GPU が使い終わるまでエンジンが解放を遅らせる
	frameViews.clear();
	current = nullptr;
	return true;
}

// ============================================================
//  Executor
// ============================================================
Executor::Executor(GraphicsEngine& engine, PipelineLibrary& pipelines)
	: m_impl(std::make_unique<Impl>(engine, pipelines, *this))
{
}

Executor::~Executor()
{
	m_impl->clear();
}

bool Executor::record(ICommandBuffer* cb, const GraphDesc& graph, const CompiledGraph& compiled, const FrameInputs& in)
{
	return m_impl->record(cb, graph, compiled, in);
}

void Executor::clear()
{
	m_impl->clear();
}

size_t Executor::pooledTextures() const { return m_impl->textures.size(); }
size_t Executor::pooledBuffers() const  { return m_impl->buffers.size(); }

void Executor::error(const std::string& text)
{
	if (std::find(m_errorSet.begin(), m_errorSet.end(), text) != m_errorSet.end())
		return;
	m_errorSet.push_back(text);
	m_errors.push_back(text);
}
} // namespace gfx::rg
