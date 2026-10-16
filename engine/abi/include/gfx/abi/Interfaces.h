#pragma once
// ============================================================
//  gfx/abi/Interfaces.h
//  DLL 境界のインターフェース (IPipeline / IRenderTarget / IDepthStencil / ISwapChain / ICommandBuffer)
//  ※ GfxBackendABI.h の一部。単独で include してもよい (前のパートを読み込む)
// ============================================================

#include "ShaderReflection.h"
// ============================================================
//  所有権のルール (全インターフェース共通)
//
//  IPipeline / ICommandBuffer / IRenderTarget / IDepthStencil / ISwapChain の
//  ラッパー構造体はバックエンド DLL 内の実装オブジェクトが所有する。
//  アプリ (EXE) 側で delete してはいけない。
//  解放は必ず destroy() / release() (= DLL 内の vtable 関数) を経由する。
//  これにより EXE と DLL の CRT が異なっても安全に解放できる。
//
//  GPU が使用中のリソースはバックエンドが遅延解放するため、
//  destroy() は記録済み・実行中のコマンドがあっても呼んでよい。
// ============================================================

// IPipeline vtable
struct IPipelineVTable
{
	void (*destroy)(void* self);
};

struct IPipeline
{
	const IPipelineVTable* vtable = nullptr;
	void*                  self   = nullptr;

	// 実装とこのラッパーを DLL 内で解放する。呼び出し後このポインタは無効。
	void destroy() { vtable->destroy(self); }
};

// ============================================================
//  シェーダー
//  HLSL ソースは DXC で DXIL にコンパイルする (既定のプロファイルは <stage>_6_0)。
//  コンパイルのエラーと警告はデバッグメッセージ (GfxDeviceConfig::debugCallback) で通知する。
// ============================================================

// マクロ定義 (#define name value)。value が nullptr なら "1"
struct GfxShaderDefine {
	const char* name  = nullptr;
	const char* value = nullptr;
};

enum class GfxShaderCompileFlags : uint32_t {
	None             = 0,
	Debug            = 0x1,   // デバッグ情報を埋め込む (-Zi -Qembed_debug)。PIX / RenderDoc でソースを表示できる
	SkipOptimization = 0x2,   // 最適化しない (-Od)
	WarningsAsErrors = 0x4,   // 警告をエラーにする (-WX)
};
constexpr GfxShaderCompileFlags operator|(GfxShaderCompileFlags a, GfxShaderCompileFlags b)
{
	return static_cast<GfxShaderCompileFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
constexpr bool gfxHasFlag(GfxShaderCompileFlags v, GfxShaderCompileFlags f)
{
	return (static_cast<uint32_t>(v) & static_cast<uint32_t>(f)) != 0;
}

struct GfxShaderDesc {
	ShaderStage      stage;
	const char*      source         = nullptr;  // HLSL ソース (優先)
	const char*      entryPoint     = "main";
	const void*      bytecode       = nullptr;  // コンパイル済みの DXIL (source が nullptr のとき)
	size_t           bytecodeSize   = 0;

	// ---- ABI 7 ----
	const char*            sourceName      = nullptr;  // ファイルのパス (エラーメッセージと、相対 #include の基準)
	const char*            targetProfile   = nullptr;  // "ps_6_6" など。nullptr なら <stage>_6_0
	const GfxShaderDefine* defines         = nullptr;
	uint32_t               defineCount     = 0;
	const char* const*     includeDirs     = nullptr;  // #include の検索パス (-I)
	uint32_t               includeDirCount = 0;
	GfxShaderCompileFlags  flags           = GfxShaderCompileFlags::None;
};
struct GfxDrawCmd {
	BufferHandle vertexBuffer;
	BufferHandle indexBuffer;
	uint32_t     indexCount    = 0;
	uint32_t     vertexCount   = 0;   // indexBuffer なし時に使用
	uint32_t     instanceCount = 1;
	uint32_t     firstIndex    = 0;
	int32_t      vertexOffset  = 0;
};

// ============================================================
//  IRenderTargetVTable
// ============================================================
struct IRenderTargetVTable {
	void     (*destroy)    (void* self);
	bool     (*resize)     (void* self, uint32_t w, uint32_t h);
	uint32_t (*getWidth)   (const void* self);
	uint32_t (*getHeight)  (const void* self);
	PixelFormat (*getFormat)(const void* self);
	// SRV としてシェーダーから読み取るためのテクスチャハンドルを返す
	// (ポストエフェクト、Deferred Lighting パスで使用)
	TextureHandle (*asTexture)(void* self);
};

struct IRenderTarget {
	const IRenderTargetVTable* vtable = nullptr;
	void*                      self   = nullptr;

	void          destroy()                      { vtable->destroy(self); }
	bool          resize(uint32_t w, uint32_t h) { return vtable->resize(self, w, h); }
	uint32_t      getWidth()  const              { return vtable->getWidth(self);  }
	uint32_t      getHeight() const              { return vtable->getHeight(self); }
	PixelFormat   getFormat() const              { return vtable->getFormat(self); }
	TextureHandle asTexture()                    { return vtable->asTexture(self); }
};

// ============================================================
//  IDepthStencilVTable
// ============================================================
struct IDepthStencilVTable {
	void     (*destroy)   (void* self);
	bool     (*resize)    (void* self, uint32_t w, uint32_t h);
	uint32_t (*getWidth)  (const void* self);
	uint32_t (*getHeight) (const void* self);
	PixelFormat (*getFormat)(const void* self);
	// デプス値をシェーダーから読むためのテクスチャハンドル (SSAO 等)
	TextureHandle (*asTexture)(void* self);
};

struct IDepthStencil {
	const IDepthStencilVTable* vtable = nullptr;
	void*                      self   = nullptr;

	void          destroy()                      { vtable->destroy(self); }
	bool          resize(uint32_t w, uint32_t h) { return vtable->resize(self, w, h); }
	uint32_t      getWidth()  const              { return vtable->getWidth(self);  }
	uint32_t      getHeight() const              { return vtable->getHeight(self); }
	PixelFormat   getFormat() const              { return vtable->getFormat(self); }
	TextureHandle asTexture()                    { return vtable->asTexture(self); }
};

// ============================================================
//  ISwapChainVTable  (Present 専用。RT/DS を持たない)
// ============================================================
struct ISwapChainVTable {
	void (*destroy)(void* self);
	bool (*resize) (void* self, uint32_t w, uint32_t h);

	// acquireNextBuffer: 次フレームのバックバッファ index を確定し
	//                    同期オブジェクト (semaphore / fence) を待機する。
	//                    beginFrame 相当。
	bool (*acquireNextBuffer)(void* self);

	// getRenderTarget: 現フレームのバックバッファを IRenderTarget* として返す。
	// 返した IRenderTarget* の寿命は次の acquireNextBuffer まで有効。
	// アプリはこれを setRenderTargets() に渡す。
	IRenderTarget* (*getRenderTarget)(void* self);

	// present: バックバッファを画面に表示する。
	void (*present)(void* self);

	uint32_t    (*getWidth) (const void* self);
	uint32_t    (*getHeight)(const void* self);
	PixelFormat (*getFormat)(const void* self);
};

struct ISwapChain {
	const ISwapChainVTable* vtable = nullptr;
	void*                   self   = nullptr;

	void           destroy()                      { vtable->destroy(self); }
	bool           resize(uint32_t w, uint32_t h) { return vtable->resize(self, w, h); }
	bool           acquireNextBuffer()             { return vtable->acquireNextBuffer(self); }
	IRenderTarget* getRenderTarget()               { return vtable->getRenderTarget(self); }
	void           present()                       { vtable->present(self); }
	uint32_t       getWidth()  const               { return vtable->getWidth(self);  }
	uint32_t       getHeight() const               { return vtable->getHeight(self); }
	PixelFormat    getFormat() const               { return vtable->getFormat(self); }
};

// ============================================================
//  ICommandBuffer — 外部から保持・記録できるコマンドバッファ
//
//  ライフサイクル:
//    ICommandBuffer* cb = device->allocCommandBuffer(type);
//    cb->begin();
//    ... draw calls ...
//    cb->end();
//    device->submitCommandBuffers(&cb, 1);
//    // フレーム終了後に解放
//    cb->release();
//
//  用途別に type を使い分ける:
//    Direct   … GPU に直接サブミットできる主コマンドバッファ
//    Bundle   … Direct に記録して繰り返し再利用 (DX12 Bundle / Vk Secondary)
// ============================================================
enum class CommandBufferType : uint32_t {
	Direct = 0,   // 主コマンドバッファ。サブミット可能。
	Bundle = 1,   // セカンダリ / バンドル。Direct から実行する。
};

struct ICommandBufferVTable {
	// 記録開始 / 終了
	void (*begin) (void* self);
	void (*end)   (void* self);

	// このコマンドバッファを Direct に埋め込む (Bundle/Secondary 用)
	void (*executeBundle) (void* self, void* parentSelf);

	// DLL 内部で確保されたオブジェクトを解放する
	void (*release) (void* self);

	// ---- コマンドバッファへの描画コマンド記録 ----
	// setRenderTargets
	void (*setRenderTargets)(void* self,
							 uint32_t       rtCount,
							 IRenderTarget* const* rts,
							 IDepthStencil* ds,
							 bool clearRT, bool clearDS);
	void (*bindPipeline)      (void* self, IPipeline*    p);
	void (*bindTexture)       (void* self, TextureHandle h, uint32_t slot);
	void (*bindSampler)       (void* self, SamplerHandle h, uint32_t slot);
	void (*bindUniformBuffer) (void* self, BufferHandle  h, uint32_t slot);
	void (*setTopology)       (void* self, Topology t);
	void (*bindUAV)           (void* self, TextureHandle h, uint32_t slot);
	void (*dispatchRays)      (void* self, RTPipelineHandle pipe, TLASHandle tlas, TextureHandle output, TextureHandle depthTex, BufferHandle rtCB, uint32_t w, uint32_t h);
	void (*transitionToUAV)   (void* self, TextureHandle h);
	void (*draw)              (void* self, const GfxDrawCmd* cmd);
	void (*drawFullscreen)    (void* self);

	// クエリ
	CommandBufferType (*getType)(const void* self);

	// ---- ABI v2: 低レベルなバインド ----
	// rootIndex はバインディングレイアウトのパラメータの位置。
	// bindPipeline で Compute パイプラインを選んでいれば Compute 側のルートに設定する。
	void (*setRootConstants)  (void* self, uint32_t rootIndex, uint32_t num32BitValues, const void* data, uint32_t destOffsetIn32BitValues);
	// ルート CBV / SRV / UAV (種類はレイアウトのパラメータから決まる)。offset はバイト数
	void (*setRootDescriptor) (void* self, uint32_t rootIndex, BufferHandle buffer, uint64_t offset);
	// ディスクリプタを一時領域へ連続して書き込み、テーブルとして設定する
	void (*setDescriptorTable)(void* self, uint32_t rootIndex, const GfxDescriptor* descriptors, uint32_t count);
	void (*setVertexBuffers)  (void* self, uint32_t startSlot, uint32_t count, const GfxVertexBufferBinding* bindings);
	void (*setIndexBuffer)    (void* self, const GfxIndexBufferBinding* binding);

	// ---- ABI v2: 描画 / Compute ----
	void (*drawInstanced)       (void* self, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance);
	void (*drawIndexedInstanced)(void* self, uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t baseVertex, uint32_t firstInstance);
	void (*drawIndirect)        (void* self, const GfxIndirectDesc* desc);
	void (*drawIndexedIndirect) (void* self, const GfxIndirectDesc* desc);
	void (*dispatch)            (void* self, uint32_t x, uint32_t y, uint32_t z);
	void (*dispatchIndirect)    (void* self, const GfxIndirectDesc* desc);

	// ---- ABI v2: バリア / 動的ステート ----
	void (*barrier)       (void* self, const GfxBarrierDesc* barriers, uint32_t count);
	void (*setViewports)  (void* self, uint32_t count, const GfxViewport* viewports);
	void (*setScissors)   (void* self, uint32_t count, const GfxRect* rects);
	void (*setStencilRef) (void* self, uint32_t ref);
	void (*setBlendFactor)(void* self, const float* rgba);

	// ---- ABI v2: 転送 / クリア ----
	void (*copyBuffer) (void* self, BufferHandle dst, uint64_t dstOffset, BufferHandle src, uint64_t srcOffset, uint64_t size);
	// srcBox = nullptr ならサブリソース全体
	void (*copyTexture)(void* self, GfxResourceRef dst, uint32_t dstSubresource, uint32_t dstX, uint32_t dstY, uint32_t dstZ,
	                    GfxResourceRef src, uint32_t srcSubresource, const GfxBox* srcBox);
	// rectCount = 0 なら全体
	void (*clearRenderTarget)(void* self, IRenderTarget* rt, const float* rgba, uint32_t rectCount, const GfxRect* rects);
	void (*clearDepthStencil)(void* self, IDepthStencil* ds, GfxClearFlags flags, float depth, uint8_t stencil,
	                          uint32_t rectCount, const GfxRect* rects);

	// ---- ABI v2: マーカー / クエリ ----
	void (*beginEvent)    (void* self, const char* name);
	void (*endEvent)      (void* self);
	void (*writeTimestamp)(void* self, QueryPoolHandle pool, uint32_t index);
	void (*beginQuery)    (void* self, QueryPoolHandle pool, uint32_t index);
	void (*endQuery)      (void* self, QueryPoolHandle pool, uint32_t index);
	// [first, first + count) の結果を読み出し用の領域へ書き出す。readQueryResults で読む
	void (*resolveQueries)(void* self, QueryPoolHandle pool, uint32_t first, uint32_t count);

	// ---- ABI v2 (Phase 2b-2): ビュー / MSAA ----
	// RTV / DSV のビューを設定する (クリアはしない)。ビューポートとシザーは最初のビューの mip のサイズに合わせる
	void (*setRenderTargetViews) (void* self, uint32_t count, const ViewHandle* rtvs, ViewHandle dsv);
	void (*clearRenderTargetView)(void* self, ViewHandle rtv, const float* rgba, uint32_t rectCount, const GfxRect* rects);
	void (*clearDepthStencilView)(void* self, ViewHandle dsv, GfxClearFlags flags, float depth, uint8_t stencil,
	                              uint32_t rectCount, const GfxRect* rects);
	// MSAA のサブリソースを解決する (ResolveSubresource)。format = Unknown なら dst のフォーマット
	void (*resolve)(void* self, GfxResourceRef dst, uint32_t dstSubresource, GfxResourceRef src, uint32_t srcSubresource, GfxFormat format);
};

struct ICommandBuffer {
	const ICommandBufferVTable* vtable = nullptr;
	void*                       self   = nullptr;

	void begin()  { vtable->begin(self);  }
	void end()    { vtable->end(self);    }
	// impl とこのラッパーを DLL 内で解放 (またはプールへ返却) する。
	// 呼び出し後このポインタは無効。
	void release() { vtable->release(self); }

	void executeBundle(ICommandBuffer* parent) {
		vtable->executeBundle(self, parent ? parent->self : nullptr);
	}

	void setRenderTargets(uint32_t count, IRenderTarget* const* rts,
						  IDepthStencil* ds,
						  bool clearRT = true, bool clearDS = true) {
		vtable->setRenderTargets(self, count, rts, ds, clearRT, clearDS);
	}
	void setTopology       (Topology t)                   { vtable->setTopology(self, t);                 }
	void bindUAV           (TextureHandle h, uint32_t s)  { vtable->bindUAV(self, h, s);                  }
	void dispatchRays      (RTPipelineHandle pipe, TLASHandle tlas, TextureHandle output,
	                        TextureHandle depthTex, BufferHandle rtCB, uint32_t w, uint32_t h)
	                       { vtable->dispatchRays(self, pipe, tlas, output, depthTex, rtCB, w, h); }
	void transitionToUAV   (TextureHandle h) { vtable->transitionToUAV(self, h); }
	void bindPipeline      (IPipeline*    p)              { vtable->bindPipeline(self, p);                }
	void bindPipeline      (PipelineHandle h)
	{
		// PipelineHandle.id には IPipeline* が直接格納されている
		if (h.valid())
			vtable->bindPipeline(self, reinterpret_cast<IPipeline*>(h.id));
	}
	void bindTexture       (TextureHandle h, uint32_t s)  { vtable->bindTexture(self, h, s);              }
	void bindSampler       (SamplerHandle h, uint32_t s)  { vtable->bindSampler(self, h, s);              }
	void bindUniformBuffer (BufferHandle  h, uint32_t s)  { vtable->bindUniformBuffer(self, h, s);        }
	void draw              (const GfxDrawCmd& cmd)        { vtable->draw(self, &cmd);                     }
	void drawFullscreen    ()                             { vtable->drawFullscreen(self);                  }

	CommandBufferType getType() const { return vtable->getType(self); }

	// ---- ABI v2 ----
	void setRootConstants  (uint32_t rootIndex, uint32_t num32, const void* data, uint32_t destOffset = 0)
	                       { vtable->setRootConstants(self, rootIndex, num32, data, destOffset); }
	void setRootDescriptor (uint32_t rootIndex, BufferHandle buf, uint64_t offset = 0) { vtable->setRootDescriptor(self, rootIndex, buf, offset); }
	void setDescriptorTable(uint32_t rootIndex, const GfxDescriptor* descs, uint32_t count) { vtable->setDescriptorTable(self, rootIndex, descs, count); }
	void setVertexBuffers  (uint32_t startSlot, uint32_t count, const GfxVertexBufferBinding* b) { vtable->setVertexBuffers(self, startSlot, count, b); }
	void setIndexBuffer    (const GfxIndexBufferBinding& b) { vtable->setIndexBuffer(self, &b); }

	void drawInstanced       (uint32_t vtx, uint32_t inst = 1, uint32_t firstVtx = 0, uint32_t firstInst = 0)
	                         { vtable->drawInstanced(self, vtx, inst, firstVtx, firstInst); }
	void drawIndexedInstanced(uint32_t idx, uint32_t inst = 1, uint32_t firstIdx = 0, int32_t baseVtx = 0, uint32_t firstInst = 0)
	                         { vtable->drawIndexedInstanced(self, idx, inst, firstIdx, baseVtx, firstInst); }
	void drawIndirect        (const GfxIndirectDesc& d) { vtable->drawIndirect(self, &d);        }
	void drawIndexedIndirect (const GfxIndirectDesc& d) { vtable->drawIndexedIndirect(self, &d); }
	void dispatch            (uint32_t x, uint32_t y = 1, uint32_t z = 1) { vtable->dispatch(self, x, y, z); }
	void dispatchIndirect    (const GfxIndirectDesc& d) { vtable->dispatchIndirect(self, &d);    }

	void barrier       (const GfxBarrierDesc* b, uint32_t count) { vtable->barrier(self, b, count); }
	void barrier       (const GfxBarrierDesc& b)                 { vtable->barrier(self, &b, 1);    }
	void setViewports  (uint32_t count, const GfxViewport* v)    { vtable->setViewports(self, count, v); }
	void setScissors   (uint32_t count, const GfxRect* r)        { vtable->setScissors(self, count, r);  }
	void setStencilRef (uint32_t ref)                            { vtable->setStencilRef(self, ref);     }
	void setBlendFactor(const float rgba[4])                     { vtable->setBlendFactor(self, rgba);   }

	void copyBuffer (BufferHandle dst, uint64_t dstOffset, BufferHandle src, uint64_t srcOffset, uint64_t size)
	                { vtable->copyBuffer(self, dst, dstOffset, src, srcOffset, size); }
	void copyTexture(GfxResourceRef dst, uint32_t dstSub, uint32_t dstX, uint32_t dstY, uint32_t dstZ,
	                 GfxResourceRef src, uint32_t srcSub, const GfxBox* srcBox = nullptr)
	                { vtable->copyTexture(self, dst, dstSub, dstX, dstY, dstZ, src, srcSub, srcBox); }
	void clearRenderTarget(IRenderTarget* rt, const float rgba[4], uint32_t rectCount = 0, const GfxRect* rects = nullptr)
	                { vtable->clearRenderTarget(self, rt, rgba, rectCount, rects); }
	void clearDepthStencil(IDepthStencil* ds, GfxClearFlags flags, float depth, uint8_t stencil,
	                       uint32_t rectCount = 0, const GfxRect* rects = nullptr)
	                { vtable->clearDepthStencil(self, ds, flags, depth, stencil, rectCount, rects); }

	void beginEvent    (const char* name)                    { vtable->beginEvent(self, name);          }
	void endEvent      ()                                    { vtable->endEvent(self);                  }
	void writeTimestamp(QueryPoolHandle pool, uint32_t index) { vtable->writeTimestamp(self, pool, index); }
	void beginQuery    (QueryPoolHandle pool, uint32_t index) { vtable->beginQuery(self, pool, index);     }
	void endQuery      (QueryPoolHandle pool, uint32_t index) { vtable->endQuery(self, pool, index);       }
	void resolveQueries(QueryPoolHandle pool, uint32_t first, uint32_t count) { vtable->resolveQueries(self, pool, first, count); }

	void setRenderTargetViews (uint32_t count, const ViewHandle* rtvs, ViewHandle dsv = {}) { vtable->setRenderTargetViews(self, count, rtvs, dsv); }
	void clearRenderTargetView(ViewHandle rtv, const float rgba[4], uint32_t rectCount = 0, const GfxRect* rects = nullptr)
	                          { vtable->clearRenderTargetView(self, rtv, rgba, rectCount, rects); }
	void clearDepthStencilView(ViewHandle dsv, GfxClearFlags flags, float depth, uint8_t stencil,
	                           uint32_t rectCount = 0, const GfxRect* rects = nullptr)
	                          { vtable->clearDepthStencilView(self, dsv, flags, depth, stencil, rectCount, rects); }
	void resolve(GfxResourceRef dst, uint32_t dstSub, GfxResourceRef src, uint32_t srcSub, GfxFormat format = GfxFormat::Unknown)
	            { vtable->resolve(self, dst, dstSub, src, srcSub, format); }
};
