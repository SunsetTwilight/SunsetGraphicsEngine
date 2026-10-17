#pragma once
// ============================================================
//  backends/dx12/DX12CommandBuffer.h
//  コマンドバッファ (Direct / Bundle) と CB 単位のリソース状態追跡
// ============================================================

#include "DX12Utils.h"
#include "DX12Descriptors.h"

// ============================================================
//  DX12CommandBufferImpl
//
//  Direct … GPU キューに直接サブミットできるコマンドバッファ。
//  Bundle … Direct の ExecuteBundle で繰り返し再利用できるバンドル。
// ============================================================
// 前方宣言
class DX12PipelineImpl;
struct DX12BufRes;

class DX12CommandBufferImpl
{
public:
	DX12CommandBufferImpl();

	// コマンドバッファを作成する
	bool create(
		DX12DeviceImpl*   dev,
		CommandBufferType type);

	// 記録を開始する
	void begin();

	// 記録を終了する
	void end();

	// Bundle を Direct に埋め込む
	void executeBundle(DX12CommandBufferImpl* bundle);

	// DLL 内部でオブジェクトを解放する (Direct はプールへ返却)
	void release();

	// 描画ターゲットをセットする
	void setRenderTargets(
		uint32_t              rtCount,
		IRenderTarget* const* rts,
		IDepthStencil*        ds,
		bool                  clearRT,
		bool                  clearDS);

	// パイプラインをバインドする (PSO + ルート署名を SetPipelineState)
	void bindPipeline(IPipeline* p);

	// シェーダーをバインドする (後方互換のため残留。ABI からは呼ばれない)
	void bindShader(ShaderHandle h);

	// テクスチャをバインドする
	void bindTexture(
		TextureHandle h,
		uint32_t      slot);

	// サンプラーをバインドする
	void bindSampler(
		SamplerHandle h,
		uint32_t      slot);

	void bindUniformBuffer(
		BufferHandle h,
		uint32_t     slot);

	// draw 直前に SRV / Sampler を一時リングの連続領域にコピーして SetDescriptorTable を発行する
	void flushSRVBindings();
	void flushSamplerBindings();

	// インデックス付き描画
	void draw(const GfxDrawCmd* cmd);

	// フルスクリーントライアングル描画
	void drawFullscreen();

	void setTopology(Topology t)
	{
		m_topology = (t == Topology::LineList) ? D3D_PRIMITIVE_TOPOLOGY_LINELIST : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	}
	void bindUAV(TextureHandle h, uint32_t slot);
	void dispatchRays(RTPipelineHandle pipe, TLASHandle tlas, TextureHandle output, TextureHandle depthTex, BufferHandle rtCB, uint32_t w, uint32_t h);
	void transitionToUAV(TextureHandle h);

	// ---- ABI v2 ----
	void setRootConstants  (uint32_t rootIndex, uint32_t num32, const void* data, uint32_t destOffset);
	void setRootDescriptor (uint32_t rootIndex, BufferHandle buffer, uint64_t offset);
	void setDescriptorTable(uint32_t rootIndex, const GfxDescriptor* descs, uint32_t count);
	void setVertexBuffers  (uint32_t startSlot, uint32_t count, const GfxVertexBufferBinding* bindings);
	void setIndexBuffer    (const GfxIndexBufferBinding* binding);
	void drawInstanced       (uint32_t vtx, uint32_t inst, uint32_t firstVtx, uint32_t firstInst);
	void drawIndexedInstanced(uint32_t idx, uint32_t inst, uint32_t firstIdx, int32_t baseVtx, uint32_t firstInst);
	void executeIndirect     (ID3D12CommandSignature* sig, const GfxIndirectDesc* desc, bool compute);
	void dispatch            (uint32_t x, uint32_t y, uint32_t z);
	void barrier       (const GfxBarrierDesc* barriers, uint32_t count);
	void setViewports  (uint32_t count, const GfxViewport* viewports);
	void setScissors   (uint32_t count, const GfxRect* rects);
	void setStencilRef (uint32_t ref);
	void setBlendFactor(const float* rgba);
	void copyBuffer (BufferHandle dst, uint64_t dstOffset, BufferHandle src, uint64_t srcOffset, uint64_t size);
	void copyTexture(GfxResourceRef dst, uint32_t dstSub, uint32_t dstX, uint32_t dstY, uint32_t dstZ,
	                 GfxResourceRef src, uint32_t srcSub, const GfxBox* srcBox);
	void clearRenderTarget(IRenderTarget* rt, const float* rgba, uint32_t rectCount, const GfxRect* rects);
	void clearDepthStencil(IDepthStencil* ds, GfxClearFlags flags, float depth, uint8_t stencil,
	                       uint32_t rectCount, const GfxRect* rects);
	void beginEvent    (const char* name);
	void endEvent      ();
	void writeTimestamp(QueryPoolHandle pool, uint32_t index);
	void beginQuery    (QueryPoolHandle pool, uint32_t index);
	void endQuery      (QueryPoolHandle pool, uint32_t index);
	void resolveQueries(QueryPoolHandle pool, uint32_t first, uint32_t count);
	void setRenderTargetViews (uint32_t count, const ViewHandle* rtvs, ViewHandle dsv);
	void clearRenderTargetView(ViewHandle rtv, const float* rgba, uint32_t rectCount, const GfxRect* rects);
	void clearDepthStencilView(ViewHandle dsv, GfxClearFlags flags, float depth, uint8_t stencil,
	                           uint32_t rectCount, const GfxRect* rects);
	void resolve(GfxResourceRef dst, uint32_t dstSub, GfxResourceRef src, uint32_t srcSub, GfxFormat format);

	// submit 時に参照する: この CB が記録したクエリの resolve
	struct PendingResolve { uint64_t pool; uint32_t first; uint32_t count; };
	const std::vector<PendingResolve>& pendingResolves() const { return m_resolves; }
	// submit 時に参照する: この CB がコピー先にした Readback のバッファ
	const std::vector<uint64_t>& pendingReadbacks() const { return m_readbacks; }

	bool isDirect() const { return m_type == D3D12_COMMAND_LIST_TYPE_DIRECT; }
	DX12DeviceImpl*             device()         { return m_dev; }
	ID3D12GraphicsCommandList4* cmdList()        { return m_cmdList.Get(); }
	ICommandBuffer*             iface()          { return &m_iface; }

	// ---- リソース状態の追跡 (CB ローカル) ----
	// この CB 内で res を state として使うことを宣言する。
	// CB 内で初めて使うリソースはバリアを記録せず、submit 時にグローバル状態と照合して
	// 修正バリアを先行実行する。2 回目以降の状態変化はこの CB にバリアを記録する。
	void requireState(ID3D12Resource* res, D3D12_RESOURCE_STATES state);

	struct LocalState
	{
		ID3D12Resource*       res;
		D3D12_RESOURCE_STATES first;     // CB 実行開始時に必要な状態
		D3D12_RESOURCE_STATES current;   // CB 実行終了時の状態
	};
	const std::vector<LocalState>& localStates() const { return m_localStates; }

	// 一時ディスクリプタリング上で、この CB が最初に確保した位置 (未確保なら UINT64_MAX)
	uint64_t firstSrvRingPos()     const { return m_firstSrvRingPos;     }
	uint64_t firstSamplerRingPos() const { return m_firstSamplerRingPos; }

	// submit 時に呼ばれる: この CB の実行完了を示すフェンス値を記録する
	void markSubmitted(uint64_t fence) { m_lastSubmitFence = fence; }

	static const ICommandBufferVTable* getVTable();

private:
	ICommandBuffer                    m_iface;   // アプリに返すラッパー (この impl が所有)
	ComPtr<ID3D12CommandAllocator>    m_alloc;
	ComPtr<ID3D12GraphicsCommandList4> m_cmdList;
	DX12DeviceImpl*                   m_dev           = nullptr;
	D3D12_COMMAND_LIST_TYPE           m_type          = D3D12_COMMAND_LIST_TYPE_DIRECT;
	DXGI_FORMAT                       m_currentRTVFmts[8] = {};
	uint32_t                          m_currentRTVCount   = 0;
	DXGI_FORMAT                       m_currentDSVFmt     = DXGI_FORMAT_UNKNOWN;

	// 現在バインド中のパイプライン (bindPipeline で更新)
	DX12PipelineImpl*                 m_currentPipeline     = nullptr;
	D3D_PRIMITIVE_TOPOLOGY            m_topology            = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;

	// bindShader で蓄積する VS / PS の ID
	// 両方揃ったとき PSO を確定する
	uint64_t                          m_boundVS             = 0;
	uint64_t                          m_boundPS             = 0;

	// bindTexture / bindSampler で蓄積するスロットリスト
	// draw/drawFullscreen の直前に連続領域にコピーして SetDescriptorTable を呼ぶ
	static constexpr uint32_t MAX_SRV_BINDINGS = 8;
	uint32_t                          m_srvBindings[MAX_SRV_BINDINGS]     = {};
	uint32_t                          m_srvBindingCount                   = 0;
	uint32_t                          m_samplerBindings[MAX_SRV_BINDINGS] = {};
	uint32_t                          m_samplerBindingCount               = 0;

	std::vector<LocalState>           m_localStates;
	uint64_t                          m_firstSrvRingPos     = UINT64_MAX;
	uint64_t                          m_firstSamplerRingPos = UINT64_MAX;
	uint64_t                          m_lastSubmitFence     = 0;

	void transitionNow(ID3D12Resource* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);

	// ---- ABI v2 ----
	// Compute パイプラインがバインドされているか (ルートの設定先を切り替える)
	bool     m_computeBound      = false;
	// setDescriptorTable で明示的に設定したテーブル (draw 時の自動 flush で上書きしない)
	uint32_t m_explicitTableMask = 0;

	// バッファの状態 (CB の中だけで追跡する。開始時は COMMON、end() で COMMON に戻す)
	struct BufferState { ID3D12Resource* res; D3D12_RESOURCE_STATES current; };
	std::vector<BufferState>    m_bufferStates;
	std::vector<PendingResolve> m_resolves;
	std::vector<uint64_t>       m_readbacks;

	void requireBufferState(const DX12BufRes* buf, D3D12_RESOURCE_STATES state);
	void overrideLocalState(ID3D12Resource* res, D3D12_RESOURCE_STATES state);
	ID3D12Resource* resolveTextureRef(const GfxResourceRef& r) const;
	bool requireDirect(const char* fn);
	void setRootTable(uint32_t rootIndex, D3D12_GPU_DESCRIPTOR_HANDLE h);
	void flushBindings();
};
