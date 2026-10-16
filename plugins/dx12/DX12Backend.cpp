// ============================================================
//  plugins/dx12/DX12Backend.cpp
//
//  クラス構成:
//    DX12CommandBufferImpl  … コマンドバッファ (Direct / Bundle)
//    DX12RenderTargetImpl   … カラー RT + RTV
//    DX12DepthStencilImpl   … デプス/ステンシル + DSV
//    DX12SwapChainImpl      … スワップチェーン (DSV 不保持)
//    DX12DeviceImpl         … デバイス / コマンドキュー / リソース管理
// ============================================================
#ifdef _WIN32

#include "GfxBackendABI.h"
#include <cassert>
#include <d3d12.h>
#include <string>

// DXC (DirectX Shader Compiler) - GFX_DXC_DIR が設定されている場合のみ有効
#ifdef GFX_HAS_DXC
#include <dxcapi.h>
#endif
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <deque>
#include <algorithm>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;

// ============================================================
//  前方宣言
// ============================================================
class DX12DeviceImpl;

// ============================================================
//  ヘルパー関数
// ============================================================

// PixelFormat → DXGI_FORMAT 変換
static DXGI_FORMAT toDXGI(PixelFormat fmt)
{
	switch (fmt)
	{
	case PixelFormat::RGBA8_UNORM:       return DXGI_FORMAT_R8G8B8A8_UNORM;
	case PixelFormat::BGRA8_UNORM:       return DXGI_FORMAT_B8G8R8A8_UNORM;
	case PixelFormat::RGBA16_FLOAT:      return DXGI_FORMAT_R16G16B16A16_FLOAT;
	case PixelFormat::R32_FLOAT:         return DXGI_FORMAT_R32_FLOAT;
	case PixelFormat::D32_FLOAT:         return DXGI_FORMAT_D32_FLOAT;
	case PixelFormat::D24_UNORM_S8_UINT: return DXGI_FORMAT_D24_UNORM_S8_UINT;
	case PixelFormat::D16_UNORM:         return DXGI_FORMAT_D16_UNORM;
	default:                             return DXGI_FORMAT_UNKNOWN;
	}
}

// デプスフォーマット判定
static bool isDepthFormat(PixelFormat fmt)
{
	return fmt == PixelFormat::D32_FLOAT
		|| fmt == PixelFormat::D24_UNORM_S8_UINT
		|| fmt == PixelFormat::D16_UNORM;
}

// VertexAttribute 列挙型 → HLSL セマンティクス文字列・インデックス変換テーブル
// エンジン外から VertexAttribute 列挙値で指定し、エンジン内でここで文字列に変換する。
struct SemanticInfo { const char* name; uint32_t index; };
static const SemanticInfo k_SemanticTable[] =
{
	{ "POSITION", 0 },  // VertexAttribute::Position
	{ "NORMAL",   0 },  // VertexAttribute::Normal
	{ "TANGENT",  0 },  // VertexAttribute::Tangent
	{ "TEXCOORD", 0 },  // VertexAttribute::TexCoord0
	{ "TEXCOORD", 1 },  // VertexAttribute::TexCoord1
	{ "COLOR",    0 },  // VertexAttribute::Color0
};
static constexpr uint32_t k_SemanticTableCount =
	sizeof(k_SemanticTable) / sizeof(k_SemanticTable[0]);

// VertexAttribute + PixelFormat から D3D12_INPUT_ELEMENT_DESC を生成するヘルパー
static void vertexElementToD3D12(
	const GfxVertexElement&    e,
	D3D12_INPUT_ELEMENT_DESC&  out)
{
	const uint32_t ai = static_cast<uint32_t>(e.attribute);
	const SemanticInfo& sem = (ai < k_SemanticTableCount)
		? k_SemanticTable[ai] : k_SemanticTable[0];

	DXGI_FORMAT fmt = toDXGI(e.format);
	if (fmt == DXGI_FORMAT_R32_FLOAT)
	{
		// R32_FLOAT 指定時はセマンティクスごとのデフォルト幅に展開する
		switch (e.attribute)
		{
		case VertexAttribute::TexCoord0:
		case VertexAttribute::TexCoord1: fmt = DXGI_FORMAT_R32G32_FLOAT;       break;
		case VertexAttribute::Tangent:   fmt = DXGI_FORMAT_R32G32B32A32_FLOAT; break;
		default:                         fmt = DXGI_FORMAT_R32G32B32_FLOAT;    break;
		}
	}

	out = { sem.name, sem.index, fmt, 0, e.offset,
			D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 };
}

// GfxVertexLayout から D3D12_INPUT_LAYOUT_DESC を生成するヘルパー
static uint32_t buildInputLayout(
	const GfxVertexLayout&      layout,
	D3D12_INPUT_ELEMENT_DESC    outElems[GfxVertexLayout::MAX_ELEMENTS])
{
	if (layout.stride == 0) return 0;  // stride=0 は InputLayout なし
	uint32_t count = 0;
	for (uint32_t i = 0; i < layout.elementCount; ++i)
		vertexElementToD3D12(layout.elements[i], outElems[count++]);
	return count;
}


// ============================================================
//  DX12CommandBufferImpl
//
//  Direct … GPU キューに直接サブミットできるコマンドバッファ。
//  Bundle … Direct の ExecuteBundle で繰り返し再利用できるバンドル。
// ============================================================
// ============================================================
//  DX12DescriptorHeap
//  Descriptor Heap のラッパー。型・容量・GPU 可視性をコンストラクタで設定する。
//  alloc() でスロットを確保し cpu/gpu ハンドルを返す。
//  free() でスロットをフリーリストに返却し再利用する。
// ============================================================
class DX12DescriptorHeap
{
public:
	DX12DescriptorHeap() = default;

	bool init(
		ID3D12Device*              dev,
		D3D12_DESCRIPTOR_HEAP_TYPE type,
		uint32_t                   capacity,
		bool                       gpuVisible)
	{
		m_capacity   = capacity;
		m_gpuVisible = gpuVisible;
		m_type       = type;

		D3D12_DESCRIPTOR_HEAP_DESC d = {};
		d.Type           = type;
		d.NumDescriptors = capacity;
		d.Flags          = gpuVisible
			? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
			: D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

		if (FAILED(dev->CreateDescriptorHeap(&d, IID_PPV_ARGS(&m_heap))))
			return false;

		m_descSize = dev->GetDescriptorHandleIncrementSize(type);
		return true;
	}

	// スロットを1つ確保して CPU ハンドルを返す
	// フリーリストに空きがあれば再利用し、なければ nextSlot から新規確保する
	// 容量を使い切った場合は outSlot = UINT32_MAX、ハンドルは {0} を返す
	D3D12_CPU_DESCRIPTOR_HANDLE alloc(uint32_t& outSlot)
	{
		if (!m_freeList.empty())
		{
			outSlot = m_freeList.back();
			m_freeList.pop_back();
		}
		else if (m_nextSlot < m_capacity)
		{
			outSlot = m_nextSlot++;
		}
		else
		{
			outSlot = UINT32_MAX;
			fprintf(stderr, "[GfxEngine] DescriptorHeap exhausted (type=%d capacity=%u)\n",
				static_cast<int>(m_type), m_capacity);
			return { 0 };
		}
		return cpuHandle(outSlot);
	}

	// スロットをフリーリストに返却する
	// 返却されたスロットは次の alloc() で再利用される
	// GPU が参照中の可能性があるスロットは DX12DeviceImpl::deferRelease 経由で返却すること
	void free(uint32_t slot)
	{
		if (slot < m_capacity)
			m_freeList.push_back(slot);
	}

	D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle(uint32_t slot) const
	{
		auto h = m_heap->GetCPUDescriptorHandleForHeapStart();
		h.ptr += (uint64_t)slot * m_descSize;
		return h;
	}

	D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle(uint32_t slot) const
	{
		auto h = m_heap->GetGPUDescriptorHandleForHeapStart();
		h.ptr += (uint64_t)slot * m_descSize;
		return h;
	}

	ID3D12DescriptorHeap* heap()       const { return m_heap.Get(); }
	uint32_t              descSize()   const { return m_descSize;   }
	uint32_t              nextSlot()   const { return m_nextSlot;   }
	uint32_t              capacity()   const { return m_capacity;   }
	bool                  gpuVisible() const { return m_gpuVisible; }
	bool                  valid()      const { return m_heap != nullptr; }

	// 先頭の予約スロット (null SRV 等) を飛ばすために使う
	void resetFrom(uint32_t slot) { m_nextSlot = slot; }

private:
	ComPtr<ID3D12DescriptorHeap> m_heap;
	D3D12_DESCRIPTOR_HEAP_TYPE   m_type       = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	uint32_t                     m_descSize   = 0;
	uint32_t                     m_capacity   = 0;
	uint32_t                     m_nextSlot   = 0;
	bool                         m_gpuVisible = false;
	std::vector<uint32_t>        m_freeList;   // 返却済みスロットの再利用リスト
};

// ============================================================
//  DX12TempDescriptorRing
//  シェーダー可視ヒープの一時領域を、フェンス付きリングとして管理する。
//
//  ・draw ごとに必要な連続スロット (SRV テーブル / Sampler テーブル) を確保する
//  ・submit 時に {GPU フェンス値, 解放可能になる位置} を記録し、
//    GPU がそのフェンスを通過するまで領域を再利用しない
//  ・位置は 64bit の単調増加カウンタで持ち、スロット番号は % capacity で求める
// ============================================================
class DX12TempDescriptorRing
{
public:
	void init(uint32_t baseSlot, uint32_t capacity)
	{
		m_base = baseSlot;
		m_cap  = capacity;
		m_head = m_tail = 0;
		m_retire.clear();
	}

	// count 個の連続スロットを確保する。空きがなければ false
	bool tryAlloc(uint32_t count, uint32_t& outSlot, uint64_t& outPos)
	{
		if (count == 0 || count > m_cap)
			return false;

		const uint32_t off   = static_cast<uint32_t>(m_head % m_cap);
		// 末尾で連続領域が取れない場合は先頭まで読み飛ばす
		const uint64_t start = (off + count > m_cap) ? m_head + (m_cap - off) : m_head;
		if (start + count - m_tail > m_cap)
			return false;

		m_head  = start + count;
		outPos  = start;
		outSlot = m_base + static_cast<uint32_t>(start % m_cap);
		return true;
	}

	// GPU が completed まで完了していれば、そこまでの領域を解放する
	void retire(uint64_t completedFence)
	{
		while (!m_retire.empty() && m_retire.front().fence <= completedFence)
		{
			m_tail = std::max(m_tail, m_retire.front().end);
			m_retire.pop_front();
		}
	}

	// submit 時に呼ぶ: fence 完了後に safeEnd までの領域を解放してよい
	void onSubmit(uint64_t fence, uint64_t safeEnd)
	{
		if (!m_retire.empty() && m_retire.back().end > safeEnd)
			safeEnd = m_retire.back().end;   // 単調性を保つ
		m_retire.push_back({ fence, safeEnd });
	}

	uint64_t head()             const { return m_head; }
	bool     hasPendingRetire() const { return !m_retire.empty(); }
	uint64_t oldestRetireFence() const { return m_retire.front().fence; }

private:
	struct Retire { uint64_t fence; uint64_t end; };

	uint32_t           m_base = 0;
	uint32_t           m_cap  = 0;
	uint64_t           m_head = 0;   // 次に確保する位置
	uint64_t           m_tail = 0;   // ここより前は GPU 完了済み
	std::deque<Retire> m_retire;
};

// フォーマットの 1 ピクセルあたりのバイト数 (テクスチャアップロード用)
static uint32_t bytesPerPixel(PixelFormat fmt)
{
	switch (fmt)
	{
	case PixelFormat::RGBA16_FLOAT: return 8;
	case PixelFormat::D16_UNORM:    return 2;
	default:                        return 4;   // RGBA8 / BGRA8 / R32 / D32 / D24S8
	}
}

static bool hasStencil(PixelFormat fmt)
{
	return fmt == PixelFormat::D24_UNORM_S8_UINT;
}

static bool hasStencilDXGI(DXGI_FORMAT fmt)
{
	return fmt == DXGI_FORMAT_D24_UNORM_S8_UINT || fmt == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
}

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

	void requireBufferState(const DX12BufRes* buf, D3D12_RESOURCE_STATES state);
	void overrideLocalState(ID3D12Resource* res, D3D12_RESOURCE_STATES state);
	ID3D12Resource* resolveTextureRef(const GfxResourceRef& r) const;
	bool requireDirect(const char* fn);
	void setRootTable(uint32_t rootIndex, D3D12_GPU_DESCRIPTOR_HANDLE h);
	void flushBindings();
};

// ============================================================
//  DX12RenderTargetImpl
//
//  カラー RT 1 枚と RTV ディスクリプタを管理する。
//  SwapChain バックバッファ用とオフスクリーン RT 用の両方に使える。
//  リソース状態はデバイスのグローバル状態テーブルで管理する (DX12DeviceImpl::m_globalStates)。
// ============================================================
class DX12RenderTargetImpl
{
public:
	DX12RenderTargetImpl();

	// オフスクリーン RT 用: デバイスが新規リソースを作成する
	bool create(
		DX12DeviceImpl*           dev,
		const GfxRenderTargetDesc* desc);

	// SwapChain バックバッファ用: 外部から Resource を受け取る
	bool createFromBuffer(
		DX12DeviceImpl* dev,
		ID3D12Resource* buffer,
		PixelFormat     fmt,
		uint32_t        w,
		uint32_t        h);

	// リソースを解放する (GPU 使用中の可能性があるため遅延解放)
	void destroy();

	// バックバッファ用: GPU アイドルを保証したうえで即時解放する (ResizeBuffers の前提条件)
	void destroyImmediate();

	// サイズを変更する (オフスクリーン RT 用)
	bool resize(uint32_t w, uint32_t h);

	uint32_t      getWidth()  const { return m_width;  }
	uint32_t      getHeight() const { return m_height; }
	PixelFormat   getFormat() const { return m_format; }
	TextureHandle asTexture();

	// 他クラスから参照するため public に公開
	ID3D12Resource*             resource()     const { return m_resource.Get(); }
	D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle()    const { return m_rtvHandle; }
	const float*                clearColor()   const { return m_desc.clearColor; }
	bool                        isSwapChainBuffer() const { return !m_ownResource; }
	IRenderTarget*              iface()              { return &m_iface; }

	static const IRenderTargetVTable* getVTable();

private:
	IRenderTarget               m_iface;   // アプリに返すラッパー (この impl が所有)
	ComPtr<ID3D12Resource>      m_resource;
	D3D12_CPU_DESCRIPTOR_HANDLE m_rtvHandle    = {};
	uint32_t                    m_rtvSlot      = UINT32_MAX;  // freeResource 時に返却
	uint32_t                    m_width        = 0;
	uint32_t                    m_height       = 0;
	PixelFormat                 m_format       = PixelFormat::RGBA8_UNORM;
	bool                        m_ownResource  = false;

	// SRV スロット (asTexture 用、shaderRead / allowUAV の RT のみ)
	uint32_t                    m_srvSlot      = UINT32_MAX;

	DX12DeviceImpl*     m_dev  = nullptr;
	GfxRenderTargetDesc m_desc = {};

	bool allocateResource();
	void freeResource(bool immediate);
};

// ============================================================
//  DX12DepthStencilImpl
//
//  DS 1 枚と DSV ディスクリプタを管理する。
//  SwapChain と完全に独立して作成・破棄できる。
// ============================================================
class DX12DepthStencilImpl
{
public:
	DX12DepthStencilImpl();

	bool create(
		DX12DeviceImpl*            dev,
		const GfxDepthStencilDesc* desc);

	void destroy();

	bool resize(uint32_t w, uint32_t h);

	uint32_t      getWidth()  const { return m_width;  }
	uint32_t      getHeight() const { return m_height; }
	PixelFormat   getFormat() const { return m_format; }
	TextureHandle asTexture();

	ID3D12Resource*       resource()     const { return m_resource.Get(); }
	float                 clearDepth()   const { return m_desc.clearDepth;   }
	uint8_t               clearStencil() const { return m_desc.clearStencil; }
	IDepthStencil*        iface()              { return &m_iface; }

	D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle() const { return m_dsvHandle; }

	static const IDepthStencilVTable* getVTable();

private:
	IDepthStencil               m_iface;   // アプリに返すラッパー (この impl が所有)
	ComPtr<ID3D12Resource>      m_resource;
	D3D12_CPU_DESCRIPTOR_HANDLE m_dsvHandle = {};
	uint32_t                    m_dsvSlot   = UINT32_MAX;  // freeResource 時に返却
	uint32_t                    m_srvSlot   = UINT32_MAX;
	uint32_t                    m_width     = 0;
	uint32_t                    m_height    = 0;
	PixelFormat                 m_format    = PixelFormat::D32_FLOAT;
	DX12DeviceImpl*             m_dev       = nullptr;
	GfxDepthStencilDesc         m_desc      = {};

	bool allocateResource();
	void freeResource();
};

// ============================================================
//  DX12SwapChainImpl
//
//  IDXGISwapChain4 とフレームごとの DX12RenderTargetImpl を管理する。
//  DSV は持たない。アプリが setRenderTargets() で明示的に渡す。
// ============================================================
class DX12SwapChainImpl
{
public:
	DX12SwapChainImpl();

	bool create(
		DX12DeviceImpl*       dev,
		const GfxSwapChainDesc* desc);

	void destroy();

	bool resize(uint32_t w, uint32_t h);

	// バックバッファを確定し GPU フェンスを待機する
	bool acquireNextBuffer();

	// 現フレームのバックバッファ RT を返す
	IRenderTarget* getRenderTarget();

	// 画面に表示する
	void present();

	uint32_t    getWidth()  const { return m_width;  }
	uint32_t    getHeight() const { return m_height; }
	PixelFormat getFormat() const { return m_format; }
	ISwapChain* iface()           { return &m_iface; }

	static const ISwapChainVTable* getVTable();

private:
	static constexpr uint32_t MAX_BUFFERS = 3;

	ISwapChain                m_iface;     // アプリに返すラッパー (この impl が所有)
	ComPtr<IDXGISwapChain4>   m_swapChain;
	DX12RenderTargetImpl*     m_backBufferRTs[MAX_BUFFERS] = {};

	// Present 専用コマンドリスト
	// バックバッファを PRESENT 状態に遷移させるためだけに使う
	ComPtr<ID3D12CommandAllocator>    m_presentAlloc;
	ComPtr<ID3D12GraphicsCommandList4> m_presentCmdList;
	ComPtr<ID3D12Fence>               m_presentFence;
	HANDLE                            m_presentEvent    = nullptr;
	uint64_t                          m_presentFenceVal = 0;

	// フレーム同期
	ComPtr<ID3D12Fence> m_fence;
	uint64_t            m_fenceValues[MAX_BUFFERS] = {};
	HANDLE              m_fenceEvent  = nullptr;
	uint32_t            m_frameIndex  = 0;
	uint32_t            m_bufferCount = 2;

	uint32_t    m_width  = 0;
	uint32_t    m_height = 0;
	PixelFormat m_format = PixelFormat::BGRA8_UNORM;

	// vsync / ティアリング
	bool        m_vsync        = true;
	bool        m_allowTearing = false;   // DXGI_FEATURE_PRESENT_ALLOW_TEARING 対応時 true
	UINT        m_swapFlags    = 0;       // CreateSwapChain / ResizeBuffers に渡すフラグ

	DX12DeviceImpl* m_dev = nullptr;

	bool createBackBufferRTs();
	void releaseBackBufferRTs();
	bool createPresentCmdList();
	void waitForGPU();
	void moveToNextFrame();
};

// ============================================================
//  DX12DeviceImpl — リソース補助構造体
// ============================================================

// バッファ
// UPLOAD ヒープのバッファは常時 Map しておく。
// Uniform バッファは versionCount 個のバージョンを持つリングにし、
// updateBuffer ごとに GPU が使っていないバージョンへ書き込む (実行中フレームとの競合を防ぐ)。
struct DX12BufRes
{
	static constexpr uint32_t MAX_VERSIONS = 4;

	ComPtr<ID3D12Resource> res;
	uint64_t    size          = 0;   // アプリが要求したサイズ (1 バージョン分)
	uint64_t    versionStride = 0;   // 1 バージョンのバイト数 (Uniform は 256 アライン)
	uint32_t    stride        = 0;   // 頂点ストライド / インデックスサイズ (2 なら R16)
	BufferUsage usage         = BufferUsage::Vertex;
	bool        upload        = false;
	uint8_t*    mapped        = nullptr;
	uint32_t    versionCount  = 1;
	uint32_t    current       = 0;
	uint64_t    versionFence[MAX_VERSIONS] = {};   // そのバージョンを最後に使い得る submit のフェンス値

	D3D12_GPU_VIRTUAL_ADDRESS gpuAddress() const
	{
		return res->GetGPUVirtualAddress() + versionStride * current;
	}
};
struct DX12TexRes  {
	ComPtr<ID3D12Resource>      res;
	uint32_t                    slot         = 0;            // 静的 SRV スロット (既定の SRV がなければ UINT32_MAX)
	uint32_t                    uavSlot      = UINT32_MAX;   // 静的 UAV スロット (allowUAV のみ)
	DXGI_FORMAT                 srvFormat    = DXGI_FORMAT_UNKNOWN;   // createView で format = Unknown のときに使う
	DXGI_FORMAT                 dsvFormat    = DXGI_FORMAT_UNKNOWN;
};

// createView で作ったビュー (リソースへの参照を持つ)
struct DX12ViewRes
{
	GfxViewType                 type   = GfxViewType::SRV;
	ComPtr<ID3D12Resource>      res;
	uint32_t                    slot   = UINT32_MAX;   // SRV / UAV: 静的 SRV スロット、RTV / DSV: 各ヒープのスロット
	D3D12_CPU_DESCRIPTOR_HANDLE cpu    = {};           // RTV / DSV のハンドル
	D3D12_RESOURCE_STATES       state  = D3D12_RESOURCE_STATE_COMMON;   // このビューで使うときの状態
	uint32_t                    width  = 0;            // mip のサイズ (ビューポート用)
	uint32_t                    height = 0;
	DXGI_FORMAT                 format = DXGI_FORMAT_UNKNOWN;
	bool                        hasStencil = false;
};
struct DX12RTPipelineRes
{
	ComPtr<ID3D12StateObject>           pso;
	ComPtr<ID3D12StateObjectProperties> props;
	ComPtr<ID3D12RootSignature>         globalRS;    // グローバルルートシグネチャ
	ComPtr<ID3D12Resource>              shaderTable;
	uint64_t                            tableStride = 0;
	uint64_t                            tableSize   = 0;
};

struct DX12BLASRes {
	ComPtr<ID3D12Resource> result;
	ComPtr<ID3D12Resource> scratch;
};
struct DX12TLASRes {
	ComPtr<ID3D12Resource> result;       // TLAS バッファ
	ComPtr<ID3D12Resource> scratch;      // スクラッチバッファ
	ComPtr<ID3D12Resource> instanceBuf;  // インスタンス記述子バッファ
	uint32_t               maxInstances = 0;
	uint32_t               builtCount   = 0;   // 直近のビルドのインスタンス数 (PERFORM_UPDATE 可否の判定)
};
struct DX12SamplerRes { uint32_t slot = 0; };   // 静的サンプラースロット

// コンパイル済みシェーダーリソース
struct DX12ShadRes
{
	std::vector<uint8_t> bytecode;  // DXBC バイトコード
	ShaderStage          stage      = ShaderStage::Vertex;
};

// ============================================================
//  DX12BindingSlots
//  バインディングレイアウトから求めた、既存のバインドコマンド
//  (bindTexture / bindSampler / bindUniformBuffer) とルートパラメータの対応。
// ============================================================
struct DX12BindingSlots
{
	static constexpr uint32_t MAX_CBV = 8;

	uint32_t srvTableSlot      = UINT32_MAX;   // SRV レンジだけを持つ最初のテーブル
	uint32_t srvTableCount     = 0;            // そのテーブルのディスクリプタ数
	uint32_t samplerTableSlot  = UINT32_MAX;   // Sampler レンジだけを持つ最初のテーブル
	uint32_t samplerTableCount = 0;

	// ルート CBV (space 0) のレジスタ番号 → ルートパラメータの位置
	uint32_t cbvBindRegisters[MAX_CBV] = {};
	uint32_t cbvRootSlots    [MAX_CBV] = {};
	uint32_t cbvCount                  = 0;

	// 各ルートパラメータの種類 (setRootDescriptor / setDescriptorTable の検査用)
	GfxRootParameterType paramTypes[GFX_MAX_ROOT_PARAMETERS] = {};
	uint32_t             numParameters                       = 0;
};

// バインディングレイアウト (ルートシグネチャ)
struct DX12BindingLayoutRes
{
	ComPtr<ID3D12RootSignature> rootSig;
	DX12BindingSlots            slots;
};

// ============================================================
//  DX12PipelineImpl
//  GfxGraphicsPipelineDesc (v2) から PSO を事前作成して保持する。
//  v1 の GfxPipelineDesc は DX12DeviceImpl::createPipeline で v2 に変換してから作る。
//  bindPipeline で SetPipelineState を呼ぶだけになる。
// ============================================================
class DX12PipelineImpl
{
public:
	DX12PipelineImpl();

	bool create(DX12DeviceImpl* dev, const GfxGraphicsPipelineDesc& desc, const DX12BindingLayoutRes& layout);
	bool createCompute(DX12DeviceImpl* dev, const GfxComputePipelineDesc& desc, const DX12BindingLayoutRes& layout);
	bool isCompute() const { return m_isCompute; }
	const DX12BindingSlots& slots() const { return m_slots; }

	ID3D12PipelineState* pso()     const { return m_pso.Get();     }
	ID3D12RootSignature* rootSig() const { return m_rootSig.Get(); }
	bool                 valid()   const { return m_pso != nullptr; }
	IPipeline*           iface()         { return &m_iface; }
	DX12DeviceImpl*      device()  const { return m_dev; }

	uint32_t srvTableSlot()      const { return m_slots.srvTableSlot;      }
	uint32_t samplerTableSlot()  const { return m_slots.samplerTableSlot;  }
	uint32_t srvTableCount()     const { return m_slots.srvTableCount;     }   // SRV テーブルのディスクリプタ数
	uint32_t samplerTableCount() const { return m_slots.samplerTableCount; }   // Sampler テーブルのディスクリプタ数
	D3D_PRIMITIVE_TOPOLOGY topology() const { return m_topology; }

	// bindRegister: b0=0, b1=1, ... のレジスタ番号からルートパラメータスロットを返す
	// 見つからなければ UINT32_MAX
	uint32_t cbvSlot(uint32_t bindRegister) const
	{
		for (uint32_t i = 0; i < m_slots.cbvCount; ++i)
			if (m_slots.cbvBindRegisters[i] == bindRegister)
				return m_slots.cbvRootSlots[i];
		return UINT32_MAX;
	}

	// GPU 参照が終わってから D3D オブジェクトを解放するため、デバイスに引き渡す
	ComPtr<ID3D12PipelineState> takePSO()     { return std::move(m_pso);     }
	ComPtr<ID3D12RootSignature> takeRootSig() { return std::move(m_rootSig); }

	static const IPipelineVTable* getVTable();

private:
	IPipeline                   m_iface;   // アプリに返すラッパー (この impl が所有)
	DX12DeviceImpl*             m_dev = nullptr;
	ComPtr<ID3D12PipelineState> m_pso;
	ComPtr<ID3D12RootSignature> m_rootSig;  // バインディングレイアウトと共有する (参照カウント)

	DX12BindingSlots       m_slots;
	D3D_PRIMITIVE_TOPOLOGY m_topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	bool                   m_isCompute = false;
};

// クエリプール (クエリヒープ + 読み出し用の READBACK バッファ)
struct DX12QueryPoolRes
{
	ComPtr<ID3D12QueryHeap> heap;
	ComPtr<ID3D12Resource>  readback;          // count * 8 バイト、常時 Map
	const uint64_t*         mapped = nullptr;
	GfxQueryType            type   = GfxQueryType::Timestamp;
	uint32_t                count  = 0;
	std::vector<uint64_t>   resolveFence;      // 各クエリを最後に resolve した submit のフェンス値 (0 = 未 resolve)

	D3D12_QUERY_TYPE queryType() const
	{
		switch (type)
		{
		case GfxQueryType::Occlusion:       return D3D12_QUERY_TYPE_OCCLUSION;
		case GfxQueryType::BinaryOcclusion: return D3D12_QUERY_TYPE_BINARY_OCCLUSION;
		default:                            return D3D12_QUERY_TYPE_TIMESTAMP;
		}
	}
};

struct DX12PSOKey
{
	uint64_t    vsId          = 0;
	uint64_t    psId          = 0;
	DXGI_FORMAT rtvFmts[8]   = {};   // MRT 対応 (最大 8 枚)
	uint32_t    rtvCount      = 0;
	DXGI_FORMAT dsvFmt        = DXGI_FORMAT_UNKNOWN;
	uint64_t    layoutHash    = 0;

	bool operator==(const DX12PSOKey& o) const
	{
		if (vsId != o.vsId || psId != o.psId
		 || rtvCount != o.rtvCount || dsvFmt != o.dsvFmt
		 || layoutHash != o.layoutHash)
			return false;
		for (uint32_t i = 0; i < rtvCount; ++i)
			if (rtvFmts[i] != o.rtvFmts[i]) return false;
		return true;
	}
};

struct DX12PSOKeyHash
{
	size_t operator()(const DX12PSOKey& k) const
	{
		size_t h = std::hash<uint64_t>{}(k.vsId);
		h ^= std::hash<uint64_t>{}(k.psId)                                   + 0x9e3779b9 + (h << 6) + (h >> 2);
		for (uint32_t i = 0; i < k.rtvCount; ++i)
			h ^= std::hash<uint32_t>{}(static_cast<uint32_t>(k.rtvFmts[i])) + 0x9e3779b9 + (h << 6) + (h >> 2);
		h ^= std::hash<uint32_t>{}(static_cast<uint32_t>(k.dsvFmt))          + 0x9e3779b9 + (h << 6) + (h >> 2);
		h ^= std::hash<uint64_t>{}(k.layoutHash)                              + 0x9e3779b9 + (h << 6) + (h >> 2);
		return h;
	}
};

// ============================================================
//  DX12DeviceImpl
//
//  デバイス初期化、コマンドキュー、ディスクリプタヒープ、
//  GPU リソース管理を担う。
//
//  同期モデル:
//    ・submitCommandBuffers ごとに m_submitFence を +1 して Signal する
//    ・GPU が参照し得るオブジェクトの解放は deferRelease() で
//      pendingFence() (= 次の submit のフェンス値) 完了まで遅延する
//    ・リソース状態は m_globalStates (submit 順で確定する状態) と
//      CB ローカル状態 (DX12CommandBufferImpl::requireState) の 2 段で管理する
//
//  シェーダー可視 CBV_SRV_UAV ヒープのレイアウト:
//    [0]                         null SRV
//    [1, SRV_STATIC_CAPACITY)    静的ディスクリプタ (SRV / UAV)。CPU ヒープと同じ番号
//    [SRV_STATIC_CAPACITY, end)  一時リング (draw ごとのテーブル用、フェンスで回収)
//  シェーダー可視 Sampler ヒープも同様に「静的領域 + 一時リング」の 2 領域。
// ============================================================
class DX12DeviceImpl
{
public:
	bool init(const GfxDeviceConfig* cfg);
	void shutdown();

	ISwapChain*    createSwapChain   (const GfxSwapChainDesc*    desc);
	IRenderTarget* createRenderTarget(const GfxRenderTargetDesc* desc);
	IDepthStencil* createDepthStencil(const GfxDepthStencilDesc* desc);

	// コマンドバッファを確保する
	ICommandBuffer* allocCommandBuffer(CommandBufferType type);

	// Direct コマンドバッファを GPU キューにサブミットする
	void submitCommandBuffers(
		ICommandBuffer* const* cbs,
		uint32_t               count);

	// 外部コマンドリストに draw コマンドを記録する
	void drawOnCmdList(
		ID3D12GraphicsCommandList4* cl,
		const GfxDrawCmd*          cmd,
		D3D_PRIMITIVE_TOPOLOGY     topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	BufferHandle  createBuffer (const GfxBufferDesc*  d);
	TextureHandle createTexture(const GfxTextureDesc* d);
	ShaderHandle  compileShader(const GfxShaderDesc*  d);
	SamplerHandle createSampler(const GfxSamplerDesc* d);

	void destroyBuffer (BufferHandle  h);
	void destroyTexture(TextureHandle h);
	void destroyShader (ShaderHandle  h);
	void destroySampler   (SamplerHandle  h);
	bool updateBuffer     (BufferHandle   h, const void* data, uint64_t size);
	PipelineHandle createPipeline (const GfxPipelineDesc* desc);           // v1 (v2 に変換して作成する)
	PipelineHandle createGraphicsPipeline(const GfxGraphicsPipelineDesc* desc);   // v2
	BindingLayoutHandle createBindingLayout (const GfxBindingLayoutDesc* desc);
	void                destroyBindingLayout(BindingLayoutHandle h);
	PipelineHandle  createComputePipeline(const GfxComputePipelineDesc* desc);
	QueryPoolHandle createQueryPool      (const GfxQueryPoolDesc* desc);
	void            destroyQueryPool     (QueryPoolHandle h);
	bool            readQueryResults     (QueryPoolHandle h, uint32_t first, uint32_t count, uint64_t* out);
	uint64_t        getTimestampFrequency() const;

	// ---- Phase 2b-2 ----
	TextureHandle createTextureResource(const GfxTextureResourceDesc* desc);
	ViewHandle    createView           (const GfxViewDesc* desc);
	void          destroyView          (ViewHandle h);
	SamplerHandle createSamplerState   (const GfxSamplerStateDesc* desc);
	bool          checkSampleCount     (DXGI_FORMAT fmt, uint32_t count, uint32_t quality, const char* fn);
	bool          uploadSubresource0   (ID3D12Resource* res, DXGI_FORMAT fmt, uint32_t width, uint32_t height,
	                                    const void* data, uint32_t rowPitch, D3D12_RESOURCE_STATES finalState);
	bool          resolveViewResource  (const GfxResourceRef& ref, GfxViewType type,
	                                    ComPtr<ID3D12Resource>& outRes, DXGI_FORMAT& outDefaultFormat, bool& outIsBuffer);
	const DX12ViewRes* getView(uint64_t id) const
	{
		auto it = m_views.find(id);
		return it != m_views.end() ? &it->second : nullptr;
	}

	DX12QueryPoolRes* getQueryPool(uint64_t id)
	{
		auto it = m_queryPools.find(id);
		return it != m_queryPools.end() ? &it->second : nullptr;
	}
	ID3D12CommandSignature* drawSignature()        const { return m_drawSig.Get();        }
	ID3D12CommandSignature* drawIndexedSignature() const { return m_drawIndexedSig.Get(); }
	ID3D12CommandSignature* dispatchSignature()    const { return m_dispatchSig.Get();    }
	void           destroyPipeline(PipelineHandle h);
	void           destroyPipelineImpl(DX12PipelineImpl* impl);
	BLASHandle     createBLAS     (const GfxBLASDesc* desc);
	void           destroyBLAS    (BLASHandle h);
	TLASHandle     createTLAS     (const GfxTLASInstance* instances, uint32_t count);
	void           destroyTLAS    (TLASHandle h);
	void           updateTLAS     (TLASHandle h, const GfxTLASInstance* instances, uint32_t count);
	RTPipelineHandle createRTPipeline  (const GfxRTPipelineDesc* desc);
	void             destroyRTPipeline (RTPipelineHandle h);

	// デバイス側描画コマンド (ICommandBuffer 経由が推奨のためスタブ)
	void bindShader    (ShaderHandle           h);
	void bindTexture   (TextureHandle          h, uint32_t slot);
	void draw          (const GfxDrawCmd*      cmd);
	void drawFullscreen();

	GraphicsAPI getAPI()         const { return GraphicsAPI::DirectX12; }
	const char* getAdapterName() const { return m_adapterName; }
	uint64_t    getVRAMUsageMB() const;

	static const GfxDeviceVTable* getVTable();

	// 他クラスから参照するため public に公開
	ID3D12Device8*        device()         { return m_device.Get();        }
	IDXGIFactory7*        factory()        { return m_factory.Get();       }
	ID3D12CommandQueue*   cmdQueue()       { return m_cmdQueue.Get();      }
	ID3D12DescriptorHeap* srvHeap()        { return m_srvHeap.heap();       }
	ID3D12DescriptorHeap* samplerHeap()    { return m_samplerHeap.heap();   }
	ID3D12RootSignature*                defaultRootSig()        { return m_defaultRootSig.Get(); }
	const ComPtr<ID3D12RootSignature>&  defaultRootSigComPtr()  { return m_defaultRootSig;       }

	// ---------------------------------------------------------
	//  フェンス / 遅延解放
	// ---------------------------------------------------------
	uint64_t submittedFence() const { return m_submitFenceValue; }
	uint64_t pendingFence()   const { return m_submitFenceValue + 1; }   // 次の submit で Signal される値
	uint64_t completedFence() const { return m_submitFence ? m_submitFence->GetCompletedValue() : 0; }
	// submit 済みのフェンス値 v の完了を待つ (未 submit の値は待たない)
	void     waitForFence(uint64_t v);

	// GPU が参照し得るオブジェクトを pendingFence() 完了後に解放する。
	// onRelease はディスクリプタスロットの返却など、解放時に行う追加処理。
	void deferRelease(ComPtr<IUnknown> obj, std::function<void()> onRelease = {});
	// GPU 完了済みの遅延解放を実行する。force=true なら全件 (GPU アイドル前提)
	void processDeferredReleases(bool force = false);

	// ---------------------------------------------------------
	//  リソース状態 (submit 順で確定するグローバル状態)
	// ---------------------------------------------------------
	void registerResourceState(ID3D12Resource* res, D3D12_RESOURCE_STATES s) { m_globalStates[res] = s; }
	void unregisterResourceState(ID3D12Resource* res)                         { m_globalStates.erase(res); }
	D3D12_RESOURCE_STATES globalState(ID3D12Resource* res) const
	{
		auto it = m_globalStates.find(res);
		return it != m_globalStates.end() ? it->second : D3D12_RESOURCE_STATE_COMMON;
	}
	void setGlobalState(ID3D12Resource* res, D3D12_RESOURCE_STATES s) { m_globalStates[res] = s; }

	// ---------------------------------------------------------
	//  コマンドバッファ管理
	// ---------------------------------------------------------
	// CB プールへの返却 (GPU 完了後に再利用する)
	void returnToPool(DX12CommandBufferImpl* impl)
	{
		m_openCBs.erase(impl);
		m_cbPool.push_back({ pendingFence(), impl });
	}
	// begin() 済みで未 submit の CB (一時リングの回収位置の計算に使う)
	void markCBOpen(DX12CommandBufferImpl* impl)   { m_openCBs.insert(impl); }
	void markCBClosed(DX12CommandBufferImpl* impl) { m_openCBs.erase(impl);  }

	// ---------------------------------------------------------
	//  ディスクリプタ
	// ---------------------------------------------------------
	static constexpr uint32_t SRV_STATIC_CAPACITY     = 8192;
	static constexpr uint32_t SRV_TEMP_CAPACITY       = 8192;
	static constexpr uint32_t SAMPLER_STATIC_CAPACITY = 256;
	static constexpr uint32_t SAMPLER_TEMP_CAPACITY   = 1792;  // シェーダー可視 Sampler ヒープの上限は 2048

	// 静的ディスクリプタ: CPU ヒープに作成し、publish でシェーダー可視ヒープの同じ番号へコピーする
	D3D12_CPU_DESCRIPTOR_HANDLE allocStaticSRV(uint32_t& outSlot) { return m_srvHeapCPU.alloc(outSlot); }
	void publishStaticSRV(uint32_t slot);
	void freeStaticSRVDeferred(uint32_t slot);    // GPU 完了後にスロットを返却する
	D3D12_CPU_DESCRIPTOR_HANDLE cpuStaticSRV(uint32_t slot) { return m_srvHeapCPU.cpuHandle(slot); }
	D3D12_GPU_DESCRIPTOR_HANDLE gpuSRV(uint32_t slot)       { return m_srvHeap.gpuHandle(slot);    }

	D3D12_CPU_DESCRIPTOR_HANDLE allocStaticSampler(uint32_t& outSlot) { return m_samplerHeapCPU.alloc(outSlot); }
	void publishStaticSampler(uint32_t slot);
	D3D12_CPU_DESCRIPTOR_HANDLE cpuStaticSampler(uint32_t slot) { return m_samplerHeapCPU.cpuHandle(slot); }
	D3D12_GPU_DESCRIPTOR_HANDLE gpuSampler(uint32_t slot)       { return m_samplerHeap.gpuHandle(slot);    }

	// 一時リングから count 個の連続スロットを確保する (失敗時 false)
	bool allocTempSRV    (uint32_t count, uint32_t& outSlot, uint64_t& outPos);
	bool allocTempSampler(uint32_t count, uint32_t& outSlot, uint64_t& outPos);
	D3D12_CPU_DESCRIPTOR_HANDLE gpuHeapCpuSRV(uint32_t slot)     { return m_srvHeap.cpuHandle(slot);     }
	D3D12_CPU_DESCRIPTOR_HANDLE gpuHeapCpuSampler(uint32_t slot) { return m_samplerHeap.cpuHandle(slot); }

	D3D12_CPU_DESCRIPTOR_HANDLE allocRTV(uint32_t& outSlot) { return m_rtvHeap.alloc(outSlot); }
	D3D12_CPU_DESCRIPTOR_HANDLE allocDSV(uint32_t& outSlot) { return m_dsvHeap.alloc(outSlot); }
	void freeRTVDeferred(uint32_t slot);
	void freeDSVDeferred(uint32_t slot);
	void freeRTVNow(uint32_t slot) { m_rtvHeap.free(slot); }

	uint32_t srvDescSize()     const { return m_srvHeap.descSize();     }
	uint32_t samplerDescSize() const { return m_samplerHeap.descSize(); }

	// ---------------------------------------------------------
	//  リソース参照
	// ---------------------------------------------------------
	const DX12TexRes* getTexRes(uint64_t id) const
	{
		auto it = m_textures.find(id);
		return it != m_textures.end() ? &it->second : nullptr;
	}

	DX12RTPipelineRes* getRTPipelineRes(uint64_t id)
	{
		auto it = m_rtPipelineMap.find(id);
		return it != m_rtPipelineMap.end() ? &it->second : nullptr;
	}

	DX12TLASRes* getTLASRes(uint64_t id)
	{
		auto it = m_tlasMap.find(id);
		return it != m_tlasMap.end() ? &it->second : nullptr;
	}

	// asTexture() が返す id の上位ビットを立てて m_nextId と区別する
	// 下位 32bit: srvSlot、最上位ビット: RT/DS フラグ
	static constexpr uint64_t RT_TEXTURE_FLAG = 0x8000000000000000ULL;
	void registerSRVSlot  (uint32_t slot, DX12RenderTargetImpl* rt) { m_srvSlotToRT[slot] = rt; }
	void unregisterSRVSlot(uint32_t slot)                           { m_srvSlotToRT.erase(slot); }
	void registerDSSRVSlot  (uint32_t slot, DX12DepthStencilImpl* ds) { m_srvSlotToDS[slot] = ds; }
	void unregisterDSSRVSlot(uint32_t slot)                           { m_srvSlotToDS.erase(slot); }

	// テクスチャ id (createTexture / asTexture) から静的 SRV スロット番号を返す。
	// 見つからなければ UINT32_MAX を返す。
	uint32_t getTexSRVSlot(uint64_t id) const;

	// テクスチャ id から ID3D12Resource を返す (状態追跡用)。見つからなければ nullptr
	ID3D12Resource* getTexResource(uint64_t id) const;

	// シェーダーリソースを取得する (存在しなければ nullptr)
	const DX12ShadRes* getShadRes(uint64_t id) const
	{
		auto it = m_shaders.find(id);
		return it != m_shaders.end() ? &it->second : nullptr;
	}

	// サンプラーリソースを取得する (存在しなければ nullptr)
	const DX12SamplerRes* getSamplerRes(uint64_t id) const
	{
		auto it = m_samplers.find(id);
		return it != m_samplers.end() ? &it->second : nullptr;
	}

	const DX12BufRes* getBuffer(uint64_t id) const
	{
		auto it = m_buffers.find(id);
		return it != m_buffers.end() ? &it->second : nullptr;
	}

	// (vsId, psId, rtFormat) に対応する PSO を返す (bindShader 旧経路用)。
	// キャッシュになければ作成してキャッシュに登録する。
	ID3D12PipelineState* getOrCreatePSO(
		uint64_t             vsId,
		uint64_t             psId,
		const DXGI_FORMAT*   rtvFmts,
		uint32_t             rtvCount,
		DXGI_FORMAT          dsvFmt);

	// リソースバリアを指定コマンドリストに記録する
	static void transition(
		ID3D12GraphicsCommandList4* cl,
		ID3D12Resource*            res,
		D3D12_RESOURCE_STATES      before,
		D3D12_RESOURCE_STATES      after);

	D3D12_RAYTRACING_TIER raytracingTier() const { return m_raytracingTier; }
	bool supportsRaytracing() const { return m_raytracingTier >= D3D12_RAYTRACING_TIER_1_0; }

	// GPU の全コマンド完了を待機する
	void waitGPUIdle();

	// フォーマットに合った PSO を返す。
	// BGRA8 以外 (UNKNOWN 含む) はすべて RGBA8 PSO にフォールバックする
	ID3D12PipelineState* defaultPSOForFormat(DXGI_FORMAT fmt)
	{
		if (fmt == DXGI_FORMAT_B8G8R8A8_UNORM && m_defaultPSO_BGRA8)
			return m_defaultPSO_BGRA8.Get();
		return m_defaultPSO_RGBA8.Get();  // UNKNOWN / RGBA8 / その他すべて
	}

	// フレームインフライト数: この数だけ GPU が先行して実行できる
	static constexpr uint64_t FRAME_IN_FLIGHT = 2;

private:
	ComPtr<ID3D12Device8>      m_device;
	ComPtr<IDXGIFactory7>      m_factory;
	ComPtr<ID3D12CommandQueue> m_cmdQueue;

	// ディスクリプタヒープ
	DX12DescriptorHeap m_rtvHeap;         // RTV     · FLAG_NONE      · 1024
	DX12DescriptorHeap m_dsvHeap;         // DSV     · FLAG_NONE      · 256
	DX12DescriptorHeap m_srvHeapCPU;      // SRV/UAV · FLAG_NONE      · SRV_STATIC_CAPACITY (静的ディスクリプタの原本)
	DX12DescriptorHeap m_srvHeap;         // SRV/UAV · SHADER_VISIBLE · 静的 + 一時リング
	DX12DescriptorHeap m_samplerHeapCPU;  // Sampler · FLAG_NONE      · SAMPLER_STATIC_CAPACITY
	DX12DescriptorHeap m_samplerHeap;     // Sampler · SHADER_VISIBLE · 静的 + 一時リング
	DX12TempDescriptorRing m_srvRing;
	DX12TempDescriptorRing m_samplerRing;

	ComPtr<IDXGIAdapter4> m_adapter;
	char m_adapterName[256] = {};

	// submitCommandBuffers 用の同期フェンス
	ComPtr<ID3D12Fence> m_submitFence;
	HANDLE              m_submitEvent      = nullptr;
	uint64_t            m_submitFenceValue = 0;

	// DXR サポートレベル
	D3D12_RAYTRACING_TIER m_raytracingTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;

	// 遅延解放キュー
	struct DeferredRelease
	{
		uint64_t              fence;
		ComPtr<IUnknown>      obj;
		std::function<void()> onRelease;
	};
	std::deque<DeferredRelease> m_deferred;

	// submit 順で確定するリソース状態
	std::unordered_map<ID3D12Resource*, D3D12_RESOURCE_STATES> m_globalStates;

	// submit 時の状態合わせ用バリアを記録するコマンドリストのプール
	struct FixupList
	{
		ComPtr<ID3D12CommandAllocator>    alloc;
		ComPtr<ID3D12GraphicsCommandList> list;
		uint64_t                          fence = 0;
	};
	std::vector<FixupList> m_fixupPool;
	ID3D12GraphicsCommandList* acquireFixupList();

	// begin 済み・未 submit の CB
	std::unordered_set<DX12CommandBufferImpl*> m_openCBs;

	bool allocTemp(DX12TempDescriptorRing& ring, uint32_t count, uint32_t& outSlot, uint64_t& outPos);

public:
	std::unordered_map<uint64_t, DX12BufRes>     m_buffers;
	std::unordered_map<uint64_t, DX12TexRes>     m_textures;
	std::unordered_map<uint64_t, DX12BLASRes>    m_blasMap;
	std::unordered_map<uint64_t, DX12TLASRes>    m_tlasMap;
	std::unordered_map<uint64_t, DX12RTPipelineRes> m_rtPipelineMap;
	std::unordered_map<uint64_t, DX12ShadRes>    m_shaders;
	std::unordered_map<uint64_t, DX12SamplerRes>    m_samplers;
	std::unordered_map<uint64_t, DX12BindingLayoutRes> m_bindingLayouts;
	std::unordered_map<uint64_t, DX12QueryPoolRes>     m_queryPools;
	std::unordered_map<uint64_t, DX12ViewRes>          m_views;
	uint64_t m_nextId = 1;

	// CommandBuffer プール: {フェンス値, CB} のペアで GPU 完了後に再利用する
	struct PoolEntry { uint64_t fenceValue; DX12CommandBufferImpl* impl; };
	std::vector<PoolEntry> m_cbPool;

	// PSO キャッシュ: (vsId, psId, rtFormat) → PSO
	std::unordered_map<DX12PSOKey, ComPtr<ID3D12PipelineState>, DX12PSOKeyHash> m_psoCache;

	// SRV slot → RT / DS の逆引きマップ (bindTexture での状態追跡用)
	std::unordered_map<uint32_t, DX12RenderTargetImpl*> m_srvSlotToRT;
	std::unordered_map<uint32_t, DX12DepthStencilImpl*> m_srvSlotToDS;

private:
	// デフォルト RootSignature / PSO
	bool createCoreDevice(bool validation);
	bool createDescriptorHeaps();
	bool createDefaultPSO();
	bool createSubmitFence();
	bool createCommandSignatures();

	// ExecuteIndirect 用のコマンドシグネチャ (引数のみ、ルート引数の変更なし)
	ComPtr<ID3D12CommandSignature> m_drawSig;
	ComPtr<ID3D12CommandSignature> m_drawIndexedSig;
	ComPtr<ID3D12CommandSignature> m_dispatchSig;

	// RT フォーマットごとに PSO を用意する
	ComPtr<ID3D12RootSignature> m_defaultRootSig;
	ComPtr<ID3D12PipelineState> m_defaultPSO_RGBA8;   // RGBA8_UNORM 用 (G-Buffer 等)
	ComPtr<ID3D12PipelineState> m_defaultPSO_BGRA8;   // BGRA8_UNORM 用 (バックバッファ)
};

// ============================================================
//  vtable 定義 — IRenderTarget
// ============================================================
static const IRenderTargetVTable k_DX12RTVTable =
{
	[](void* s)
	{
		auto* impl = static_cast<DX12RenderTargetImpl*>(s);
		// バックバッファ RT は SwapChain が所有する。アプリからの destroy は無視する
		if (impl->isSwapChainBuffer())
		{
			fprintf(stderr, "[GfxEngine] IRenderTarget::destroy: SwapChain のバックバッファは破棄できません (無視しました)\n");
			return;
		}
		impl->destroy();
		delete impl;   // m_iface (アプリに返したラッパー) も一緒に解放される
	},
	[](void* s, uint32_t w, uint32_t h) -> bool
	{
		return static_cast<DX12RenderTargetImpl*>(s)->resize(w, h);
	},
	[](const void* s) -> uint32_t
	{
		return static_cast<const DX12RenderTargetImpl*>(s)->getWidth();
	},
	[](const void* s) -> uint32_t
	{
		return static_cast<const DX12RenderTargetImpl*>(s)->getHeight();
	},
	[](const void* s) -> PixelFormat
	{
		return static_cast<const DX12RenderTargetImpl*>(s)->getFormat();
	},
	[](void* s) -> TextureHandle
	{
		return static_cast<DX12RenderTargetImpl*>(s)->asTexture();
	},
};
const IRenderTargetVTable* DX12RenderTargetImpl::getVTable() { return &k_DX12RTVTable; }

// ============================================================
//  vtable 定義 — IDepthStencil
// ============================================================
static const IDepthStencilVTable k_DX12DSVTable =
{
	[](void* s)
	{
		static_cast<DX12DepthStencilImpl*>(s)->destroy();
		delete static_cast<DX12DepthStencilImpl*>(s);
	},
	[](void* s, uint32_t w, uint32_t h) -> bool
	{
		return static_cast<DX12DepthStencilImpl*>(s)->resize(w, h);
	},
	[](const void* s) -> uint32_t
	{
		return static_cast<const DX12DepthStencilImpl*>(s)->getWidth();
	},
	[](const void* s) -> uint32_t
	{
		return static_cast<const DX12DepthStencilImpl*>(s)->getHeight();
	},
	[](const void* s) -> PixelFormat
	{
		return static_cast<const DX12DepthStencilImpl*>(s)->getFormat();
	},
	[](void* s) -> TextureHandle
	{
		return static_cast<DX12DepthStencilImpl*>(s)->asTexture();
	},
};
const IDepthStencilVTable* DX12DepthStencilImpl::getVTable() { return &k_DX12DSVTable; }

// ============================================================
//  vtable 定義 — ISwapChain
// ============================================================
static const ISwapChainVTable k_DX12SCVTable =
{
	[](void* s)
	{
		static_cast<DX12SwapChainImpl*>(s)->destroy();
		delete static_cast<DX12SwapChainImpl*>(s);
	},
	[](void* s, uint32_t w, uint32_t h) -> bool
	{
		return static_cast<DX12SwapChainImpl*>(s)->resize(w, h);
	},
	[](void* s) -> bool
	{
		return static_cast<DX12SwapChainImpl*>(s)->acquireNextBuffer();
	},
	[](void* s) -> IRenderTarget*
	{
		return static_cast<DX12SwapChainImpl*>(s)->getRenderTarget();
	},
	[](void* s)
	{
		static_cast<DX12SwapChainImpl*>(s)->present();
	},
	[](const void* s) -> uint32_t
	{
		return static_cast<const DX12SwapChainImpl*>(s)->getWidth();
	},
	[](const void* s) -> uint32_t
	{
		return static_cast<const DX12SwapChainImpl*>(s)->getHeight();
	},
	[](const void* s) -> PixelFormat
	{
		return static_cast<const DX12SwapChainImpl*>(s)->getFormat();
	},
};
const ISwapChainVTable* DX12SwapChainImpl::getVTable() { return &k_DX12SCVTable; }

// ============================================================
//  vtable 定義 — ICommandBuffer
// ============================================================
static const ICommandBufferVTable k_DX12CmdVTable =
{
	[](void* s)
	{
		static_cast<DX12CommandBufferImpl*>(s)->begin();
	},
	[](void* s)
	{
		static_cast<DX12CommandBufferImpl*>(s)->end();
	},
	[](void* s, void* p)
	{
		static_cast<DX12CommandBufferImpl*>(s)->executeBundle(
			static_cast<DX12CommandBufferImpl*>(p));
	},
	[](void* s)
	{
		static_cast<DX12CommandBufferImpl*>(s)->release();
	},
	[](void* s, uint32_t n, IRenderTarget* const* rts, IDepthStencil* ds, bool cr, bool cd)
	{
		static_cast<DX12CommandBufferImpl*>(s)->setRenderTargets(n, rts, ds, cr, cd);
	},
	[](void* s, IPipeline* p)
	{
		static_cast<DX12CommandBufferImpl*>(s)->bindPipeline(p);
	},
	[](void* s, TextureHandle h, uint32_t slot)
	{
		static_cast<DX12CommandBufferImpl*>(s)->bindTexture(h, slot);
	},
	[](void* s, SamplerHandle h, uint32_t slot)
	{
		static_cast<DX12CommandBufferImpl*>(s)->bindSampler(h, slot);
	},
	[](void* s, BufferHandle h, uint32_t slot)
	{
		static_cast<DX12CommandBufferImpl*>(s)->bindUniformBuffer(h, slot);
	},
	[](void* s, Topology t)
	{
		static_cast<DX12CommandBufferImpl*>(s)->setTopology(t);
	},
	[](void* s, TextureHandle h, uint32_t slot)
	{
		static_cast<DX12CommandBufferImpl*>(s)->bindUAV(h, slot);
	},
	[](void* s, RTPipelineHandle pipe, TLASHandle tlas, TextureHandle output,
	   TextureHandle depthTex, BufferHandle rtCB, uint32_t w, uint32_t h)
	{
		static_cast<DX12CommandBufferImpl*>(s)->dispatchRays(pipe, tlas, output, depthTex, rtCB, w, h);
	},
	[](void* s, TextureHandle h)
	{
		static_cast<DX12CommandBufferImpl*>(s)->transitionToUAV(h);
	},
	[](void* s, const GfxDrawCmd* cmd)
	{
		static_cast<DX12CommandBufferImpl*>(s)->draw(cmd);
	},
	[](void* s)
	{
		static_cast<DX12CommandBufferImpl*>(s)->drawFullscreen();
	},
	[](const void* s) -> CommandBufferType
	{
		return static_cast<const DX12CommandBufferImpl*>(s)->isDirect()
			? CommandBufferType::Direct : CommandBufferType::Bundle;
	},
	// ---- ABI v2 ----
	[](void* s, uint32_t i, uint32_t n, const void* d, uint32_t o) { static_cast<DX12CommandBufferImpl*>(s)->setRootConstants(i, n, d, o); },
	[](void* s, uint32_t i, BufferHandle b, uint64_t o)            { static_cast<DX12CommandBufferImpl*>(s)->setRootDescriptor(i, b, o); },
	[](void* s, uint32_t i, const GfxDescriptor* d, uint32_t n)    { static_cast<DX12CommandBufferImpl*>(s)->setDescriptorTable(i, d, n); },
	[](void* s, uint32_t first, uint32_t n, const GfxVertexBufferBinding* b) { static_cast<DX12CommandBufferImpl*>(s)->setVertexBuffers(first, n, b); },
	[](void* s, const GfxIndexBufferBinding* b)                    { static_cast<DX12CommandBufferImpl*>(s)->setIndexBuffer(b); },
	[](void* s, uint32_t v, uint32_t i, uint32_t fv, uint32_t fi)  { static_cast<DX12CommandBufferImpl*>(s)->drawInstanced(v, i, fv, fi); },
	[](void* s, uint32_t n, uint32_t i, uint32_t fi, int32_t bv, uint32_t fin) { static_cast<DX12CommandBufferImpl*>(s)->drawIndexedInstanced(n, i, fi, bv, fin); },
	[](void* s, const GfxIndirectDesc* d) { auto* c = static_cast<DX12CommandBufferImpl*>(s); c->executeIndirect(c->device()->drawSignature(), d, false); },
	[](void* s, const GfxIndirectDesc* d) { auto* c = static_cast<DX12CommandBufferImpl*>(s); c->executeIndirect(c->device()->drawIndexedSignature(), d, false); },
	[](void* s, uint32_t x, uint32_t y, uint32_t z)                { static_cast<DX12CommandBufferImpl*>(s)->dispatch(x, y, z); },
	[](void* s, const GfxIndirectDesc* d) { auto* c = static_cast<DX12CommandBufferImpl*>(s); c->executeIndirect(c->device()->dispatchSignature(), d, true); },
	[](void* s, const GfxBarrierDesc* b, uint32_t n)               { static_cast<DX12CommandBufferImpl*>(s)->barrier(b, n); },
	[](void* s, uint32_t n, const GfxViewport* v)                  { static_cast<DX12CommandBufferImpl*>(s)->setViewports(n, v); },
	[](void* s, uint32_t n, const GfxRect* r)                      { static_cast<DX12CommandBufferImpl*>(s)->setScissors(n, r); },
	[](void* s, uint32_t ref)                                      { static_cast<DX12CommandBufferImpl*>(s)->setStencilRef(ref); },
	[](void* s, const float* rgba)                                 { static_cast<DX12CommandBufferImpl*>(s)->setBlendFactor(rgba); },
	[](void* s, BufferHandle d, uint64_t dof, BufferHandle src, uint64_t sof, uint64_t n) { static_cast<DX12CommandBufferImpl*>(s)->copyBuffer(d, dof, src, sof, n); },
	[](void* s, GfxResourceRef d, uint32_t ds, uint32_t x, uint32_t y, uint32_t z, GfxResourceRef src, uint32_t ss, const GfxBox* box)
	   { static_cast<DX12CommandBufferImpl*>(s)->copyTexture(d, ds, x, y, z, src, ss, box); },
	[](void* s, IRenderTarget* rt, const float* c, uint32_t n, const GfxRect* r) { static_cast<DX12CommandBufferImpl*>(s)->clearRenderTarget(rt, c, n, r); },
	[](void* s, IDepthStencil* ds, GfxClearFlags f, float d, uint8_t st, uint32_t n, const GfxRect* r)
	   { static_cast<DX12CommandBufferImpl*>(s)->clearDepthStencil(ds, f, d, st, n, r); },
	[](void* s, const char* name)                                  { static_cast<DX12CommandBufferImpl*>(s)->beginEvent(name); },
	[](void* s)                                                    { static_cast<DX12CommandBufferImpl*>(s)->endEvent(); },
	[](void* s, QueryPoolHandle p, uint32_t i)                     { static_cast<DX12CommandBufferImpl*>(s)->writeTimestamp(p, i); },
	[](void* s, QueryPoolHandle p, uint32_t i)                     { static_cast<DX12CommandBufferImpl*>(s)->beginQuery(p, i); },
	[](void* s, QueryPoolHandle p, uint32_t i)                     { static_cast<DX12CommandBufferImpl*>(s)->endQuery(p, i); },
	[](void* s, QueryPoolHandle p, uint32_t f, uint32_t n)         { static_cast<DX12CommandBufferImpl*>(s)->resolveQueries(p, f, n); },
	[](void* s, uint32_t n, const ViewHandle* rtvs, ViewHandle dsv) { static_cast<DX12CommandBufferImpl*>(s)->setRenderTargetViews(n, rtvs, dsv); },
	[](void* s, ViewHandle v, const float* c, uint32_t n, const GfxRect* r) { static_cast<DX12CommandBufferImpl*>(s)->clearRenderTargetView(v, c, n, r); },
	[](void* s, ViewHandle v, GfxClearFlags f, float d, uint8_t st, uint32_t n, const GfxRect* r)
	   { static_cast<DX12CommandBufferImpl*>(s)->clearDepthStencilView(v, f, d, st, n, r); },
	[](void* s, GfxResourceRef d, uint32_t ds, GfxResourceRef src, uint32_t ss, GfxFormat f) { static_cast<DX12CommandBufferImpl*>(s)->resolve(d, ds, src, ss, f); },
};
const ICommandBufferVTable* DX12CommandBufferImpl::getVTable() { return &k_DX12CmdVTable; }

// ============================================================
//  vtable 定義 — GfxDevice
// ============================================================
static const GfxDeviceVTable k_DX12DevVTable =
{
	[](void* s, const GfxDeviceConfig* c) -> bool
	{
		return static_cast<DX12DeviceImpl*>(s)->init(c);
	},
	[](void* s)
	{
		static_cast<DX12DeviceImpl*>(s)->shutdown();
	},
	[](void* s, const GfxSwapChainDesc* d) -> ISwapChain*
	{
		return static_cast<DX12DeviceImpl*>(s)->createSwapChain(d);
	},
	[](void* s, const GfxRenderTargetDesc* d) -> IRenderTarget*
	{
		return static_cast<DX12DeviceImpl*>(s)->createRenderTarget(d);
	},
	[](void* s, const GfxDepthStencilDesc* d) -> IDepthStencil*
	{
		return static_cast<DX12DeviceImpl*>(s)->createDepthStencil(d);
	},
	[](void* s, CommandBufferType t) -> ICommandBuffer*
	{
		return static_cast<DX12DeviceImpl*>(s)->allocCommandBuffer(t);
	},
	[](void* s, ICommandBuffer* const* cbs, uint32_t n)
	{
		static_cast<DX12DeviceImpl*>(s)->submitCommandBuffers(cbs, n);
	},
	[](void* s, const GfxBufferDesc* d) -> BufferHandle
	{
		return static_cast<DX12DeviceImpl*>(s)->createBuffer(d);
	},
	[](void* s, const GfxTextureDesc* d) -> TextureHandle
	{
		return static_cast<DX12DeviceImpl*>(s)->createTexture(d);
	},
	[](void* s, const GfxShaderDesc* d) -> ShaderHandle
	{
		return static_cast<DX12DeviceImpl*>(s)->compileShader(d);
	},
	[](void* s, const GfxSamplerDesc* d) -> SamplerHandle
	{
		return static_cast<DX12DeviceImpl*>(s)->createSampler(d);
	},
	[](void* s, BufferHandle h)
	{
		static_cast<DX12DeviceImpl*>(s)->destroyBuffer(h);
	},
	[](void* s, TextureHandle h)
	{
		static_cast<DX12DeviceImpl*>(s)->destroyTexture(h);
	},
	[](void* s, ShaderHandle h)
	{
		static_cast<DX12DeviceImpl*>(s)->destroyShader(h);
	},
	[](void* s, SamplerHandle h)
	{
		static_cast<DX12DeviceImpl*>(s)->destroySampler(h);
	},
	[](void* s, BufferHandle h, const void* data, uint64_t size) -> bool
	{
		return static_cast<DX12DeviceImpl*>(s)->updateBuffer(h, data, size);
	},
	[](void* s, TextureHandle h, uint32_t slot)
	{
		static_cast<DX12DeviceImpl*>(s)->bindTexture(h, slot);
	},
	[](void* s, const GfxDrawCmd* cmd)
	{
		static_cast<DX12DeviceImpl*>(s)->draw(cmd);
	},
	[](void* s)
	{
		static_cast<DX12DeviceImpl*>(s)->drawFullscreen();
	},
	[](const void* s) -> GraphicsAPI
	{
		return static_cast<const DX12DeviceImpl*>(s)->getAPI();
	},
	[](const void* s) -> const char*
	{
		return static_cast<const DX12DeviceImpl*>(s)->getAdapterName();
	},
	[](const void* s) -> uint64_t
	{
		return static_cast<const DX12DeviceImpl*>(s)->getVRAMUsageMB();
	},
	[](void* s, const GfxPipelineDesc* d) -> PipelineHandle
	{
		return static_cast<DX12DeviceImpl*>(s)->createPipeline(d);
	},
	[](void* s, PipelineHandle h)
	{
		static_cast<DX12DeviceImpl*>(s)->destroyPipeline(h);
	},
	[](void* s, const GfxBLASDesc* d) -> BLASHandle
	{
		return static_cast<DX12DeviceImpl*>(s)->createBLAS(d);
	},
	[](void* s, BLASHandle h)
	{
		static_cast<DX12DeviceImpl*>(s)->destroyBLAS(h);
	},
	[](void* s, const GfxTLASInstance* inst, uint32_t n) -> TLASHandle
	{
		return static_cast<DX12DeviceImpl*>(s)->createTLAS(inst, n);
	},
	[](void* s, TLASHandle h)
	{
		static_cast<DX12DeviceImpl*>(s)->destroyTLAS(h);
	},
	[](void* s, TLASHandle h, const GfxTLASInstance* inst, uint32_t n)
	{
		static_cast<DX12DeviceImpl*>(s)->updateTLAS(h, inst, n);
	},
	[](void* s, const GfxRTPipelineDesc* d) -> RTPipelineHandle
	{
		return static_cast<DX12DeviceImpl*>(s)->createRTPipeline(d);
	},
	[](void* s, RTPipelineHandle h)
	{
		static_cast<DX12DeviceImpl*>(s)->destroyRTPipeline(h);
	},
	[](void* s, const GfxBindingLayoutDesc* d) -> BindingLayoutHandle
	{
		return static_cast<DX12DeviceImpl*>(s)->createBindingLayout(d);
	},
	[](void* s, BindingLayoutHandle h)
	{
		static_cast<DX12DeviceImpl*>(s)->destroyBindingLayout(h);
	},
	[](void* s, const GfxGraphicsPipelineDesc* d) -> PipelineHandle
	{
		return static_cast<DX12DeviceImpl*>(s)->createGraphicsPipeline(d);
	},
	[](void* s, const GfxComputePipelineDesc* d) -> PipelineHandle
	{
		return static_cast<DX12DeviceImpl*>(s)->createComputePipeline(d);
	},
	[](void* s, const GfxQueryPoolDesc* d) -> QueryPoolHandle
	{
		return static_cast<DX12DeviceImpl*>(s)->createQueryPool(d);
	},
	[](void* s, QueryPoolHandle h)
	{
		static_cast<DX12DeviceImpl*>(s)->destroyQueryPool(h);
	},
	[](void* s, QueryPoolHandle h, uint32_t first, uint32_t count, uint64_t* out) -> bool
	{
		return static_cast<DX12DeviceImpl*>(s)->readQueryResults(h, first, count, out);
	},
	[](const void* s) -> uint64_t
	{
		return static_cast<const DX12DeviceImpl*>(s)->getTimestampFrequency();
	},
	[](void* s, const GfxTextureResourceDesc* d) -> TextureHandle
	{
		return static_cast<DX12DeviceImpl*>(s)->createTextureResource(d);
	},
	[](void* s, const GfxViewDesc* d) -> ViewHandle
	{
		return static_cast<DX12DeviceImpl*>(s)->createView(d);
	},
	[](void* s, ViewHandle h)
	{
		static_cast<DX12DeviceImpl*>(s)->destroyView(h);
	},
	[](void* s, const GfxSamplerStateDesc* d) -> SamplerHandle
	{
		return static_cast<DX12DeviceImpl*>(s)->createSamplerState(d);
	},
};
const GfxDeviceVTable* DX12DeviceImpl::getVTable() { return &k_DX12DevVTable; }

GFX_PLUGIN_EXPORT_FUNCTIONS(DX12DeviceImpl)

// ============================================================
//  DX12DeviceImpl — 実装
// ============================================================

// デバイスを初期化する
bool DX12DeviceImpl::init(const GfxDeviceConfig* cfg)
{
#ifdef _DEBUG
	if (cfg->validation)
	{
		ComPtr<ID3D12Debug1> dbg;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg))))
		{
			dbg->EnableDebugLayer();
			dbg->SetEnableGPUBasedValidation(TRUE);
		}
	}
#endif
	return createCoreDevice(cfg->validation)
		&& createDescriptorHeaps()
		&& createDefaultPSO()
		&& createSubmitFence()
		&& createCommandSignatures();
}

// デバイス / コマンドキューを作成する
bool DX12DeviceImpl::createCoreDevice(bool validation)
{
	UINT flags = validation ? DXGI_CREATE_FACTORY_DEBUG : 0;

	if (FAILED(CreateDXGIFactory2(flags, IID_PPV_ARGS(&m_factory))))
		return false;

	ComPtr<IDXGIAdapter1> a1;
	for (UINT i = 0; m_factory->EnumAdapters1(i, &a1) != DXGI_ERROR_NOT_FOUND; ++i)
	{
		DXGI_ADAPTER_DESC1 d;
		a1->GetDesc1(&d);

		if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
			continue;

		if (SUCCEEDED(D3D12CreateDevice(a1.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&m_device))))
		{
			a1.As(&m_adapter);
			WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, m_adapterName, 256, nullptr, nullptr);
			break;
		}
	}

	if (!m_device)
		return false;

	// バリデーション有効時はデバッグレイヤーのメッセージを stderr にも出力する
	// (デバッガ未接続でも検証結果を確認できるようにする)
	if (validation)
	{
		ComPtr<ID3D12InfoQueue1> iq;
		if (SUCCEEDED(m_device.As(&iq)))
		{
			DWORD cookie = 0;
			iq->RegisterMessageCallback(
				[](D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY sev,
				   D3D12_MESSAGE_ID id, LPCSTR desc, void*)
				{
					if (sev > D3D12_MESSAGE_SEVERITY_WARNING)
						return;
					static const char* k_sev[] = { "CORRUPTION", "ERROR", "WARNING" };
					fprintf(stderr, "[D3D12 %s #%d] %s\n", k_sev[sev], static_cast<int>(id), desc);
				},
				D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie);
		}
	}

	// DXR (Raytracing) サポートを確認する
	D3D12_FEATURE_DATA_D3D12_OPTIONS5 opts5 = {};
	if (SUCCEEDED(m_device->CheckFeatureSupport(
			D3D12_FEATURE_D3D12_OPTIONS5, &opts5, sizeof(opts5))))
	{
		m_raytracingTier = opts5.RaytracingTier;
		if (m_raytracingTier >= D3D12_RAYTRACING_TIER_1_0)
			OutputDebugStringA("[GfxEngine] DXR Tier 1.0 supported\n");
		else
			OutputDebugStringA("[GfxEngine] DXR not supported on this GPU\n");
	}

	D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
	return SUCCEEDED(m_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_cmdQueue)));
}

// シェーダーから読み取る状態 (PS / 非 PS の両方)
static constexpr D3D12_RESOURCE_STATES k_ShaderReadState =
	D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

// D3D12 オブジェクトにデバッグ名を付ける (PIX / デバッグレイヤーの表示用)
static void setDebugName(ID3D12Object* obj, const char* name)
{
	if (!obj || !name) return;
	wchar_t wname[128] = {};
	MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 128);
	obj->SetName(wname);
}

// バッファリソースを 1 つ作成する
static ComPtr<ID3D12Resource> createBufferResource(
	ID3D12Device*         device,
	uint64_t              size,
	D3D12_HEAP_TYPE       heapType,
	D3D12_RESOURCE_FLAGS  flags = D3D12_RESOURCE_FLAG_NONE)
{
	D3D12_HEAP_PROPERTIES hp = {};
	hp.Type = heapType;

	D3D12_RESOURCE_DESC rd = {};
	rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
	rd.Width            = size;
	rd.Height           = 1;
	rd.DepthOrArraySize = 1;
	rd.MipLevels        = 1;
	rd.SampleDesc.Count = 1;
	rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	rd.Flags            = flags;

	// UPLOAD ヒープは GENERIC_READ 固定、READBACK は COPY_DEST 固定、それ以外のバッファは COMMON で作成される
	const D3D12_RESOURCE_STATES initState =
		(heapType == D3D12_HEAP_TYPE_UPLOAD)   ? D3D12_RESOURCE_STATE_GENERIC_READ :
		(heapType == D3D12_HEAP_TYPE_READBACK) ? D3D12_RESOURCE_STATE_COPY_DEST    :
		                                         D3D12_RESOURCE_STATE_COMMON;

	ComPtr<ID3D12Resource> res;
	if (FAILED(device->CreateCommittedResource(
			&hp, D3D12_HEAP_FLAG_NONE, &rd, initState, nullptr, IID_PPV_ARGS(&res))))
		return nullptr;
	return res;
}

// 一時的なコマンドリストを作成する (同期アップロード / AS ビルド用)
static bool createTransientCmdList(
	ID3D12Device*                       device,
	ComPtr<ID3D12CommandAllocator>&     alloc,
	ComPtr<ID3D12GraphicsCommandList4>& cl)
{
	if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))))
		return false;
	ComPtr<ID3D12GraphicsCommandList> tmp;
	if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
			alloc.Get(), nullptr, IID_PPV_ARGS(&tmp))))
		return false;
	return SUCCEEDED(tmp.As(&cl));
}

// GPU コマンドを実行して完了を待機するヘルパー
// キューは in-order なので、先に submit された描画コマンドの完了後に実行される
static void executeAndWait(
	ID3D12CommandQueue*        queue,
	ID3D12GraphicsCommandList4* cl,
	ID3D12Device*              device)
{
	cl->Close();
	ID3D12CommandList* lists[] = { cl };
	queue->ExecuteCommandLists(1, lists);

	ComPtr<ID3D12Fence> fence;
	HANDLE ev = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
	queue->Signal(fence.Get(), 1);
	fence->SetEventOnCompletion(1, ev);
	WaitForSingleObjectEx(ev, INFINITE, FALSE);
	CloseHandle(ev);
}

// ディスクリプタヒープを作成する
bool DX12DeviceImpl::createDescriptorHeaps()
{
	auto* dev = m_device.Get();

	if (!m_rtvHeap       .init(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV,         1024, false)
	 || !m_dsvHeap       .init(dev, D3D12_DESCRIPTOR_HEAP_TYPE_DSV,          256, false)
	 || !m_srvHeapCPU    .init(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
	                           SRV_STATIC_CAPACITY, false)
	 || !m_srvHeap       .init(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
	                           SRV_STATIC_CAPACITY + SRV_TEMP_CAPACITY, true)
	 || !m_samplerHeapCPU.init(dev, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
	                           SAMPLER_STATIC_CAPACITY, false)
	 || !m_samplerHeap   .init(dev, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
	                           SAMPLER_STATIC_CAPACITY + SAMPLER_TEMP_CAPACITY, true))
		return false;

	// 静的 SRV slot 0 に null SRV を書き込む (未設定スロットのフォールバック用)
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC nullSRV = {};
		nullSRV.Format                    = DXGI_FORMAT_R8G8B8A8_UNORM;
		nullSRV.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
		nullSRV.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		nullSRV.Texture2D.MipLevels       = 1;

		dev->CreateShaderResourceView(nullptr, &nullSRV, m_srvHeapCPU.cpuHandle(0));
		publishStaticSRV(0);

		// slot 0 は null SRV 用に予約するため 1 から確保する
		m_srvHeapCPU.resetFrom(1);
	}

	// 静的サンプラー slot 0 に既定サンプラー (Linear / Wrap) を書き込む (未設定スロットのフォールバック用)
	{
		uint32_t slot = 0;
		D3D12_CPU_DESCRIPTOR_HANDLE h = m_samplerHeapCPU.alloc(slot);   // slot == 0

		D3D12_SAMPLER_DESC sd = {};
		sd.Filter         = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		sd.AddressU       = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		sd.AddressV       = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		sd.AddressW       = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		sd.MaxAnisotropy  = 1;
		sd.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
		sd.MaxLOD         = D3D12_FLOAT32_MAX;
		dev->CreateSampler(&sd, h);
		publishStaticSampler(slot);
	}

	// 一時リングはシェーダー可視ヒープの静的領域の後ろ
	m_srvRing    .init(SRV_STATIC_CAPACITY,     SRV_TEMP_CAPACITY);
	m_samplerRing.init(SAMPLER_STATIC_CAPACITY, SAMPLER_TEMP_CAPACITY);

	return true;
}

// ---- 静的ディスクリプタ ----

void DX12DeviceImpl::publishStaticSRV(uint32_t slot)
{
	m_device->CopyDescriptorsSimple(1, m_srvHeap.cpuHandle(slot), m_srvHeapCPU.cpuHandle(slot),
		D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void DX12DeviceImpl::publishStaticSampler(uint32_t slot)
{
	m_device->CopyDescriptorsSimple(1, m_samplerHeap.cpuHandle(slot), m_samplerHeapCPU.cpuHandle(slot),
		D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
}

// UAV は静的領域を直接参照するため、GPU 完了までスロットを再利用しない
void DX12DeviceImpl::freeStaticSRVDeferred(uint32_t slot)
{
	if (slot == UINT32_MAX || slot == 0) return;
	deferRelease(nullptr, [this, slot] { m_srvHeapCPU.free(slot); });
}

void DX12DeviceImpl::freeRTVDeferred(uint32_t slot)
{
	if (slot == UINT32_MAX) return;
	deferRelease(nullptr, [this, slot] { m_rtvHeap.free(slot); });
}

void DX12DeviceImpl::freeDSVDeferred(uint32_t slot)
{
	if (slot == UINT32_MAX) return;
	deferRelease(nullptr, [this, slot] { m_dsvHeap.free(slot); });
}

// ---- 一時ディスクリプタリング ----

bool DX12DeviceImpl::allocTemp(
	DX12TempDescriptorRing& ring, uint32_t count, uint32_t& outSlot, uint64_t& outPos)
{
	ring.retire(completedFence());
	while (!ring.tryAlloc(count, outSlot, outPos))
	{
		if (!ring.hasPendingRetire())
		{
			// submit 1 回分の描画で一時領域を使い切った (submit を分割するか容量を増やす必要がある)
			fprintf(stderr, "[GfxEngine] 一時ディスクリプタリングが枯渇しました (要求 %u)\n", count);
			return false;
		}
		waitForFence(ring.oldestRetireFence());
		ring.retire(completedFence());
	}
	return true;
}

bool DX12DeviceImpl::allocTempSRV(uint32_t count, uint32_t& outSlot, uint64_t& outPos)
{
	return allocTemp(m_srvRing, count, outSlot, outPos);
}

bool DX12DeviceImpl::allocTempSampler(uint32_t count, uint32_t& outSlot, uint64_t& outPos)
{
	return allocTemp(m_samplerRing, count, outSlot, outPos);
}

// ---- フェンス / 遅延解放 ----

void DX12DeviceImpl::waitForFence(uint64_t v)
{
	if (v == 0 || !m_submitFence || m_submitFence->GetCompletedValue() >= v)
		return;

	if (v > m_submitFenceValue)
	{
		// まだ Signal されていない値を待つとデッドロックするため待たない
		fprintf(stderr, "[GfxEngine] waitForFence: 未 submit のフェンス値 %llu は待機できません\n",
			static_cast<unsigned long long>(v));
		return;
	}

	m_submitFence->SetEventOnCompletion(v, m_submitEvent);
	WaitForSingleObjectEx(m_submitEvent, INFINITE, FALSE);
}

// GPU の全コマンド完了を待機する (submit 済みのもの)
void DX12DeviceImpl::waitGPUIdle()
{
	waitForFence(m_submitFenceValue);
}

void DX12DeviceImpl::deferRelease(ComPtr<IUnknown> obj, std::function<void()> onRelease)
{
	if (!obj && !onRelease)
		return;
	m_deferred.push_back({ pendingFence(), std::move(obj), std::move(onRelease) });
}

void DX12DeviceImpl::processDeferredReleases(bool force)
{
	const uint64_t completed = completedFence();
	// フェンス値は単調増加で積まれるため、先頭から完了済みのものを処理する
	while (!m_deferred.empty() && (force || m_deferred.front().fence <= completed))
	{
		DeferredRelease item = std::move(m_deferred.front());
		m_deferred.pop_front();
		if (item.onRelease)
			item.onRelease();
		// item.obj はここで Release される
	}
}

// ---- テクスチャ id の解決 ----

uint32_t DX12DeviceImpl::getTexSRVSlot(uint64_t id) const
{
	// createTexture で登録したテクスチャを先に探す
	auto it = m_textures.find(id);
	if (it != m_textures.end())
		return it->second.slot;

	// RT / DS の asTexture() は RT_TEXTURE_FLAG | srvSlot の形式
	// 破棄済み (スロット未登録) のハンドルは無効として扱う
	if (id & RT_TEXTURE_FLAG)
	{
		const uint32_t slot = static_cast<uint32_t>(id & ~RT_TEXTURE_FLAG);
		if (m_srvSlotToRT.count(slot) || m_srvSlotToDS.count(slot))
			return slot;
	}

	return UINT32_MAX;
}

ID3D12Resource* DX12DeviceImpl::getTexResource(uint64_t id) const
{
	auto it = m_textures.find(id);
	if (it != m_textures.end())
		return it->second.res.Get();

	if (id & RT_TEXTURE_FLAG)
	{
		const uint32_t slot = static_cast<uint32_t>(id & ~RT_TEXTURE_FLAG);
		auto rt = m_srvSlotToRT.find(slot);
		if (rt != m_srvSlotToRT.end())
			return rt->second->resource();
		auto ds = m_srvSlotToDS.find(slot);
		if (ds != m_srvSlotToDS.end())
			return ds->second->resource();
	}
	return nullptr;
}

// リソースバリアを指定コマンドリストに記録する
/*static*/ void DX12DeviceImpl::transition(
	ID3D12GraphicsCommandList4* cl,
	ID3D12Resource*            res,
	D3D12_RESOURCE_STATES      before,
	D3D12_RESOURCE_STATES      after)
{
	D3D12_RESOURCE_BARRIER b = {};
	b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource   = res;
	b.Transition.StateBefore = before;
	b.Transition.StateAfter  = after;
	b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

	cl->ResourceBarrier(1, &b);
}

// スワップチェーンを作成する
ISwapChain* DX12DeviceImpl::createSwapChain(const GfxSwapChainDesc* desc)
{
	auto* impl = new DX12SwapChainImpl();

	if (!impl->create(this, desc))
	{
		impl->destroy();
		delete impl;
		return nullptr;
	}

	return impl->iface();
}

// レンダーターゲットを作成する
IRenderTarget* DX12DeviceImpl::createRenderTarget(const GfxRenderTargetDesc* desc)
{
	auto* impl = new DX12RenderTargetImpl();

	if (!impl->create(this, desc))
	{
		impl->destroy();
		delete impl;
		return nullptr;
	}

	return impl->iface();
}

// デプスステンシルを作成する
IDepthStencil* DX12DeviceImpl::createDepthStencil(const GfxDepthStencilDesc* desc)
{
	auto* impl = new DX12DepthStencilImpl();

	if (!impl->create(this, desc))
	{
		impl->destroy();
		delete impl;
		return nullptr;
	}

	return impl->iface();
}

// コマンドバッファを確保する
ICommandBuffer* DX12DeviceImpl::allocCommandBuffer(CommandBufferType type)
{
	// プールから GPU 完了済みの CB を再利用する
	DX12CommandBufferImpl* impl = nullptr;
	if (type == CommandBufferType::Direct)
	{
		const uint64_t completed = completedFence();
		for (size_t i = 0; i < m_cbPool.size(); ++i)
		{
			if (m_cbPool[i].fenceValue <= completed)
			{
				impl = m_cbPool[i].impl;
				m_cbPool[i] = m_cbPool.back();
				m_cbPool.pop_back();
				break;
			}
		}
	}

	// プールに再利用可能な CB がなければ新規作成する
	if (!impl)
	{
		impl = new DX12CommandBufferImpl();
		if (!impl->create(this, type))
		{
			delete impl;
			return nullptr;
		}
	}

	return impl->iface();
}

// submit 時の状態合わせバリア用コマンドリストを取得する (記録可能な状態で返す)
ID3D12GraphicsCommandList* DX12DeviceImpl::acquireFixupList()
{
	const uint64_t completed = completedFence();
	for (auto& f : m_fixupPool)
	{
		if (f.fence <= completed)
		{
			f.alloc->Reset();
			f.list->Reset(f.alloc.Get(), nullptr);
			f.fence = pendingFence();
			return f.list.Get();
		}
	}

	FixupList f;
	if (FAILED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.alloc))))
		return nullptr;
	if (FAILED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
			f.alloc.Get(), nullptr, IID_PPV_ARGS(&f.list))))
		return nullptr;
	f.fence = pendingFence();
	m_fixupPool.push_back(f);
	return m_fixupPool.back().list.Get();
}

// Direct コマンドバッファを一括サブミットする
//
// 各 CB の「最初に必要な状態」を、submit 順に確定したグローバル状態と照合し、
// ずれていれば修正バリアを記録したコマンドリストをその CB の直前に挟む。
// これにより CB を提出順と異なる順序・並列で記録しても状態が正しく保たれる。
void DX12DeviceImpl::submitCommandBuffers(
	ICommandBuffer* const* cbs,
	uint32_t               count)
{
	// フレームインフライト: FRAME_IN_FLIGHT 回前の submit の GPU 完了を待ってから submit する
	// これにより CPU と GPU が並列動作し、submit の待機時間をほぼゼロにできる
	if (m_submitFenceValue >= FRAME_IN_FLIGHT)
		waitForFence(m_submitFenceValue - FRAME_IN_FLIGHT + 1);

	processDeferredReleases();

	std::vector<ID3D12CommandList*>     lists;
	std::vector<DX12CommandBufferImpl*> submitted;
	std::vector<D3D12_RESOURCE_BARRIER> fixups;
	lists.reserve(count * 2);
	submitted.reserve(count);

	for (uint32_t i = 0; i < count; ++i)
	{
		if (!cbs[i])
			continue;
		auto* impl = static_cast<DX12CommandBufferImpl*>(cbs[i]->self);
		if (!impl || !impl->isDirect())
			continue;

		fixups.clear();
		for (const auto& ls : impl->localStates())
		{
			const D3D12_RESOURCE_STATES g = globalState(ls.res);
			if (g != ls.first)
			{
				D3D12_RESOURCE_BARRIER b = {};
				b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				b.Transition.pResource   = ls.res;
				b.Transition.StateBefore = g;
				b.Transition.StateAfter  = ls.first;
				b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
				fixups.push_back(b);
			}
			setGlobalState(ls.res, ls.current);
		}

		if (!fixups.empty())
		{
			if (ID3D12GraphicsCommandList* fl = acquireFixupList())
			{
				fl->ResourceBarrier(static_cast<UINT>(fixups.size()), fixups.data());
				fl->Close();
				lists.push_back(fl);
			}
		}

		lists.push_back(impl->cmdList());
		submitted.push_back(impl);
	}

	if (lists.empty())
		return;

	m_cmdQueue->ExecuteCommandLists(static_cast<UINT>(lists.size()), lists.data());

	const uint64_t fence = ++m_submitFenceValue;
	m_cmdQueue->Signal(m_submitFence.Get(), fence);

	for (auto* impl : submitted)
	{
		impl->markSubmitted(fence);
		m_openCBs.erase(impl);
		for (const auto& r : impl->pendingResolves())
			if (DX12QueryPoolRes* qp = getQueryPool(r.pool))
				for (uint32_t q = r.first; q < r.first + r.count; ++q)
					qp->resolveFence[q] = fence;
	}

	// 一時リングの回収位置: まだ submit されていない CB が使っている領域より前まで
	uint64_t srvSafe     = m_srvRing.head();
	uint64_t samplerSafe = m_samplerRing.head();
	for (auto* open : m_openCBs)
	{
		srvSafe     = std::min(srvSafe,     open->firstSrvRingPos());
		samplerSafe = std::min(samplerSafe, open->firstSamplerRingPos());
	}
	m_srvRing    .onSubmit(fence, srvSafe);
	m_samplerRing.onSubmit(fence, samplerSafe);
}

// 外部コマンドリストに draw コマンドを記録する
void DX12DeviceImpl::drawOnCmdList(
	ID3D12GraphicsCommandList4* cl,
	const GfxDrawCmd*          cmd,
	D3D_PRIMITIVE_TOPOLOGY     topology)
{
	if (!cl)
		return;

	const DX12BufRes* vb = getBuffer(cmd->vertexBuffer.id);
	if (!vb || !vb->res)
		return;

	D3D12_VERTEX_BUFFER_VIEW vbv =
	{
		vb->gpuAddress(),
		static_cast<UINT>(vb->size),
		vb->stride > 0 ? vb->stride : static_cast<UINT>(sizeof(float) * 3)
	};

	cl->IASetPrimitiveTopology(topology);
	cl->IASetVertexBuffers(0, 1, &vbv);

	// indexBuffer が有効なら DrawIndexedInstanced、なければ DrawInstanced
	const DX12BufRes* ib = cmd->indexBuffer.valid() ? getBuffer(cmd->indexBuffer.id) : nullptr;
	if (ib && ib->res)
	{
		// インデックスバッファ作成時の stride が 2 なら 16bit、それ以外は 32bit
		D3D12_INDEX_BUFFER_VIEW ibv =
		{
			ib->gpuAddress(),
			static_cast<UINT>(ib->size),
			ib->stride == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT
		};
		cl->IASetIndexBuffer(&ibv);
		cl->DrawIndexedInstanced(
			cmd->indexCount, cmd->instanceCount,
			cmd->firstIndex, cmd->vertexOffset, 0);
	}
	else
	{
		cl->DrawInstanced(
			cmd->vertexCount > 0 ? cmd->vertexCount : cmd->indexCount,
			cmd->instanceCount,
			static_cast<UINT>(cmd->vertexOffset), 0);
	}
}

// バッファリソースを作成する
//   ・initialData / Staging / Uniform → UPLOAD ヒープ (常時 Map)
//   ・それ以外                        → DEFAULT ヒープ (updateBuffer はステージング経由)
//   ・Uniform は MAX_VERSIONS 個のバージョンを持つリング (updateBuffer ごとに切り替え)
BufferHandle DX12DeviceImpl::createBuffer(const GfxBufferDesc* desc)
{
	if (desc->size == 0)
		return {};

	DX12BufRes buf;
	buf.size   = desc->size;
	buf.stride = desc->stride;
	buf.usage  = desc->usage;

	const bool isUniform = hasUsage(desc->usage, BufferUsage::Uniform);
	const bool isStorage = hasUsage(desc->usage, BufferUsage::Storage) && !isUniform;
	buf.upload = !isStorage
			  && ((desc->initialData != nullptr)
			   || hasUsage(desc->usage, BufferUsage::Staging)
			   || isUniform);

	if (isUniform)
	{
		// D3D12 の CBV は 256 バイトアライメントが必要
		buf.versionStride = (desc->size + 255) & ~uint64_t(255);
		buf.versionCount  = DX12BufRes::MAX_VERSIONS;
	}
	else
	{
		buf.versionStride = desc->size;
		buf.versionCount  = 1;
	}

	buf.res = createBufferResource(m_device.Get(), buf.versionStride * buf.versionCount,
		buf.upload ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_DEFAULT,
		isStorage ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE);
	if (!buf.res)
		return {};

	if (buf.upload)
	{
		// UPLOAD ヒープは Map したままでよい (CPU からは書き込みのみ)
		const D3D12_RANGE noRead = { 0, 0 };
		void* p = nullptr;
		if (FAILED(buf.res->Map(0, &noRead, &p)))
			return {};
		buf.mapped = static_cast<uint8_t*>(p);

		if (desc->initialData)
		{
			for (uint32_t v = 0; v < buf.versionCount; ++v)
				memcpy(buf.mapped + buf.versionStride * v, desc->initialData, static_cast<size_t>(desc->size));
		}
	}

	uint64_t id = m_nextId++;
	m_buffers.emplace(id, std::move(buf));

	// Storage (DEFAULT ヒープ) の初期データはステージング経由でコピーする
	if (isStorage && desc->initialData && !updateBuffer({ id }, desc->initialData, desc->size))
	{
		destroyBuffer({ id });
		return {};
	}
	return { id };
}

// テクスチャリソースを作成し、initialData があれば GPU に転送する
TextureHandle DX12DeviceImpl::createTexture(const GfxTextureDesc* desc)
{
	if (desc->width == 0 || desc->height == 0)
		return {};

	DX12TexRes tex;
	const uint32_t mips = desc->mips > 0 ? desc->mips : 1;

	// GPU 側リソース (DEFAULT ヒープ)
	D3D12_HEAP_PROPERTIES hp = {};
	hp.Type = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC rd = {};
	rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	rd.Width            = desc->width;
	rd.Height           = desc->height;
	rd.DepthOrArraySize = 1;
	rd.MipLevels        = static_cast<UINT16>(mips);
	rd.Format           = toDXGI(desc->format);
	rd.SampleDesc.Count = 1;
	if (desc->allowUAV)
		rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

	// 最終状態: UAV テクスチャは UAV、それ以外はシェーダー読み取り
	const D3D12_RESOURCE_STATES finalState = desc->allowUAV
		? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
		: k_ShaderReadState;
	// 初期データがある場合はコピー先として作成し、転送後に最終状態へ遷移させる
	const D3D12_RESOURCE_STATES initState = desc->initialData
		? D3D12_RESOURCE_STATE_COPY_DEST
		: finalState;

	if (FAILED(m_device->CreateCommittedResource(
			&hp, D3D12_HEAP_FLAG_NONE, &rd,
			initState, nullptr,
			IID_PPV_ARGS(&tex.res))))
		return {};

	setDebugName(tex.res.Get(), desc->debugName);

	// initialData があれば Upload ヒープ経由で転送する (mip 0 のみ)
	if (desc->initialData)
	{
		const uint32_t bpp         = bytesPerPixel(desc->format);
		const uint32_t rowBytes    = desc->width * bpp;
		// rowPitch が 0 のとき width * (フォーマットのバイト数) と仮定する
		const uint32_t srcRowPitch = desc->rowPitch > 0 ? desc->rowPitch : rowBytes;

		// D3D12 は行のアライメントが D3D12_TEXTURE_DATA_PITCH_ALIGNMENT (256) 必要
		const UINT dstRowPitch = (rowBytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)
							   & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
		const UINT64 uploadSize = static_cast<UINT64>(dstRowPitch) * desc->height;

		ComPtr<ID3D12Resource> uploadBuf = createBufferResource(m_device.Get(), uploadSize, D3D12_HEAP_TYPE_UPLOAD);
		if (!uploadBuf)
			return {};

		// CPU からステージングバッファへ行単位でコピーする
		{
			uint8_t* dst = nullptr;
			uploadBuf->Map(0, nullptr, reinterpret_cast<void**>(&dst));

			const uint8_t* src = static_cast<const uint8_t*>(desc->initialData);
			for (uint32_t row = 0; row < desc->height; ++row)
			{
				memcpy(dst + static_cast<size_t>(row) * dstRowPitch,
					   src + static_cast<size_t>(row) * srcRowPitch,
					   rowBytes);
			}

			uploadBuf->Unmap(0, nullptr);
		}

		ComPtr<ID3D12CommandAllocator>    copyAlloc;
		ComPtr<ID3D12GraphicsCommandList4> copyCL;
		if (!createTransientCmdList(m_device.Get(), copyAlloc, copyCL))
			return {};

		D3D12_TEXTURE_COPY_LOCATION dst = {};
		dst.pResource        = tex.res.Get();
		dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		dst.SubresourceIndex = 0;

		D3D12_TEXTURE_COPY_LOCATION src_loc = {};
		src_loc.pResource                            = uploadBuf.Get();
		src_loc.Type                                 = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		src_loc.PlacedFootprint.Offset               = 0;
		src_loc.PlacedFootprint.Footprint.Format     = rd.Format;
		src_loc.PlacedFootprint.Footprint.Width      = desc->width;
		src_loc.PlacedFootprint.Footprint.Height     = desc->height;
		src_loc.PlacedFootprint.Footprint.Depth      = 1;
		src_loc.PlacedFootprint.Footprint.RowPitch   = dstRowPitch;

		copyCL->CopyTextureRegion(&dst, 0, 0, 0, &src_loc, nullptr);

		// COPY_DEST → 最終状態に遷移する
		transition(copyCL.Get(), tex.res.Get(), D3D12_RESOURCE_STATE_COPY_DEST, finalState);

		// GPU 完了を待機する (アップロード完了を保証する)
		executeAndWait(m_cmdQueue.Get(), copyCL.Get(), m_device.Get());
	}

	// SRV を作成する (静的領域)
	// 初期データは mip 0 のみなので、mip が複数あっても未初期化の mip は見せない
	UINT srvMips = mips;
	if (desc->initialData && mips > 1)
	{
		fprintf(stderr, "[GfxEngine] createTexture: initialData は mip 0 のみ転送します。SRV の MipLevels を 1 にしました\n");
		srvMips = 1;
	}

	D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
	sv.Format                    = rd.Format;
	sv.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
	sv.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	sv.Texture2D.MipLevels       = srvMips;

	D3D12_CPU_DESCRIPTOR_HANDLE srvCPU = allocStaticSRV(tex.slot);
	if (tex.slot == UINT32_MAX)
		return {};
	m_device->CreateShaderResourceView(tex.res.Get(), &sv, srvCPU);
	publishStaticSRV(tex.slot);

	// allowUAV が true の場合は UAV も作成する (静的領域)
	if (desc->allowUAV)
	{
		D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {};
		uv.Format        = rd.Format;
		uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

		D3D12_CPU_DESCRIPTOR_HANDLE uavCPU = allocStaticSRV(tex.uavSlot);
		if (tex.uavSlot != UINT32_MAX)
		{
			m_device->CreateUnorderedAccessView(tex.res.Get(), nullptr, &uv, uavCPU);
			publishStaticSRV(tex.uavSlot);
		}
	}

	registerResourceState(tex.res.Get(), finalState);

	uint64_t id = m_nextId++;
	m_textures.emplace(id, std::move(tex));
	return { id };
}

// サンプラーを作成して静的サンプラー領域に登録する
SamplerHandle DX12DeviceImpl::createSampler(const GfxSamplerDesc* desc)
{
	// FilterMode → D3D12_FILTER に変換する
	auto toFilter = [](FilterMode f) -> D3D12_FILTER
	{
		switch (f)
		{
		case FilterMode::Nearest:    return D3D12_FILTER_MIN_MAG_MIP_POINT;
		case FilterMode::Aniso:      return D3D12_FILTER_ANISOTROPIC;
		case FilterMode::Comparison: return D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
		default:                     return D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		}
	};

	// AddressMode → D3D12_TEXTURE_ADDRESS_MODE に変換する
	auto toAddr = [](AddressMode a) -> D3D12_TEXTURE_ADDRESS_MODE
	{
		switch (a)
		{
		case AddressMode::Clamp:  return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		case AddressMode::Mirror: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
		case AddressMode::Border: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
		default:                  return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		}
	};

	DX12SamplerRes sam;
	D3D12_CPU_DESCRIPTOR_HANDLE cpu = allocStaticSampler(sam.slot);
	if (sam.slot == UINT32_MAX)
		return {};

	D3D12_SAMPLER_DESC sd = {};
	sd.Filter         = toFilter(desc->filter);
	sd.AddressU       = toAddr(desc->addressU);
	sd.AddressV       = toAddr(desc->addressV);
	sd.AddressW       = toAddr(desc->addressW);
	sd.MipLODBias     = desc->mipLodBias;
	sd.MaxAnisotropy  = std::clamp<uint32_t>(desc->maxAniso, 1u, 16u);
	sd.ComparisonFunc = (desc->filter == FilterMode::Comparison)
		? D3D12_COMPARISON_FUNC_LESS_EQUAL
		: D3D12_COMPARISON_FUNC_NEVER;
	sd.MinLOD         = desc->minLod;
	sd.MaxLOD         = desc->maxLod;

	m_device->CreateSampler(&sd, cpu);
	publishStaticSampler(sam.slot);

	uint64_t id = m_nextId++;
	m_samplers.emplace(id, sam);
	return { id };
}

// サンプラーを削除する
// 描画時に一時領域へ値コピーしているため GPU 参照はないが、他の静的ディスクリプタと同じく遅延返却する
void DX12DeviceImpl::destroySampler(SamplerHandle h)
{
	auto it = m_samplers.find(h.id);
	if (it == m_samplers.end())
		return;

	const uint32_t slot = it->second.slot;
	if (slot != 0)
		deferRelease(nullptr, [this, slot] { m_samplerHeapCPU.free(slot); });
	m_samplers.erase(it);
}
// シェーダーをコンパイルする (DXC 実装は別途)
// HLSL ソースまたはバイトコードからシェーダーをコンパイル・登録する
ShaderHandle DX12DeviceImpl::compileShader(const GfxShaderDesc* desc)
{
	DX12ShadRes sh;
	sh.stage  = desc->stage;

	static const char* k_profiles[] = { "vs_5_1", "ps_5_1", "cs_5_1" };
	const char* profile = k_profiles[static_cast<uint32_t>(desc->stage)];

	if (desc->source)
	{
		// HLSL ソースを D3DCompile でコンパイルする
		ComPtr<ID3DBlob> code;
		ComPtr<ID3DBlob> err;
		HRESULT hr = D3DCompile(
			desc->source,
			strlen(desc->source),
			nullptr,
			nullptr,
			nullptr,
			desc->entryPoint ? desc->entryPoint : "main",
			profile,
			D3DCOMPILE_OPTIMIZATION_LEVEL1,
			0,
			&code,
			&err);

		if (FAILED(hr))
		{
			if (err)
			{
				OutputDebugStringA("[GfxEngine] compileShader failed:\n");
				OutputDebugStringA(static_cast<const char*>(err->GetBufferPointer()));
				// stdout にも出力してコンソールで確認できるようにする
				fprintf(stderr, "[GfxEngine] compileShader failed:\n%s\n",
					static_cast<const char*>(err->GetBufferPointer()));
			}
			return {};  // invalid handle
		}

		const uint8_t* p = static_cast<const uint8_t*>(code->GetBufferPointer());
		sh.bytecode.assign(p, p + code->GetBufferSize());
	}
	else if (desc->bytecode && desc->bytecodeSize > 0)
	{
		// コンパイル済みバイトコードをそのまま格納する
		const uint8_t* p = static_cast<const uint8_t*>(desc->bytecode);
		sh.bytecode.assign(p, p + desc->bytecodeSize);
	}
	else
	{
		// ソースもバイトコードもない場合は無効
		return {};
	}

	uint64_t id = m_nextId++;
	m_shaders.emplace(id, std::move(sh));
	return { id };
}

// 頂点 / インデックス / 定数バッファとして実行中のコマンドが参照している可能性があるため遅延解放する
void DX12DeviceImpl::destroyBuffer(BufferHandle h)
{
	auto it = m_buffers.find(h.id);
	if (it == m_buffers.end())
		return;

	deferRelease(it->second.res);
	m_buffers.erase(it);
}

void DX12DeviceImpl::destroyTexture(TextureHandle h)
{
	auto it = m_textures.find(h.id);
	if (it == m_textures.end())
		return;

	// リソースと SRV / UAV スロットは GPU 完了後に返却する
	freeStaticSRVDeferred(it->second.slot);
	freeStaticSRVDeferred(it->second.uavSlot);
	if (ID3D12Resource* raw = it->second.res.Get())
		deferRelease(it->second.res, [this, raw] { unregisterResourceState(raw); });
	m_textures.erase(it);
}

// シェーダーは CPU 側のバイトコードのみ (PSO 作成後は参照されない) なので即時解放してよい
void DX12DeviceImpl::destroyShader (ShaderHandle  h) { m_shaders.erase(h.id);  }

// バッファに CPU からデータを書き込む
//   ・Uniform        : 次のバージョンに書き込み、以降の bindUniformBuffer はそのバージョンを参照する。
//                      実行中のフレームが読んでいるバージョンは上書きしない
//   ・その他 UPLOAD   : submit 済みの GPU 処理の完了を待ってから書き込む (同期)
//   ・DEFAULT ヒープ  : ステージングバッファ経由でコピーし完了を待つ (同期)
bool DX12DeviceImpl::updateBuffer(BufferHandle h, const void* data, uint64_t size)
{
	auto it = m_buffers.find(h.id);
	if (it == m_buffers.end() || !it->second.res || !data)
		return false;

	DX12BufRes& buf = it->second;
	if (size > buf.size)
	{
		fprintf(stderr, "[GfxEngine] updateBuffer: サイズ超過 (要求 %llu > バッファ %llu)\n",
			static_cast<unsigned long long>(size), static_cast<unsigned long long>(buf.size));
		return false;
	}

	if (buf.upload && buf.versionCount > 1)
	{
		// 現在のバージョンは、ここまでに記録されたコマンドから参照されている可能性がある
		buf.versionFence[buf.current] = pendingFence();

		const uint32_t next = (buf.current + 1) % buf.versionCount;
		const uint64_t f    = buf.versionFence[next];
		if (f > completedFence())
		{
			if (f <= submittedFence())
			{
				waitForFence(f);
			}
			else
			{
				// 1 回の submit までに versionCount 回以上更新された
				static bool s_warned = false;
				if (!s_warned)
				{
					fprintf(stderr, "[GfxEngine] updateBuffer: submit 前に同じ Uniform バッファを %u 回以上更新しました。"
						"未実行のコマンドが参照するデータを上書きします\n", buf.versionCount);
					s_warned = true;
				}
			}
		}

		memcpy(buf.mapped + buf.versionStride * next, data, static_cast<size_t>(size));
		buf.current = next;
		return true;
	}

	if (buf.upload)
	{
		// 単一バージョンの UPLOAD バッファ (初期データ付き頂点バッファ等)
		waitForFence(submittedFence());
		memcpy(buf.mapped, data, static_cast<size_t>(size));
		return true;
	}

	// DEFAULT ヒープ: ステージングバッファ経由でコピーする
	ComPtr<ID3D12Resource> staging = createBufferResource(m_device.Get(), size, D3D12_HEAP_TYPE_UPLOAD);
	if (!staging)
		return false;
	{
		void* p = nullptr;
		if (FAILED(staging->Map(0, nullptr, &p)))
			return false;
		memcpy(p, data, static_cast<size_t>(size));
		staging->Unmap(0, nullptr);
	}

	ComPtr<ID3D12CommandAllocator>    alloc;
	ComPtr<ID3D12GraphicsCommandList4> cl;
	if (!createTransientCmdList(m_device.Get(), alloc, cl))
		return false;

	// バッファは COMMON から COPY_DEST へ暗黙に昇格し、実行後に COMMON へ戻る
	cl->CopyBufferRegion(buf.res.Get(), 0, staging.Get(), 0, size);
	executeAndWait(m_cmdQueue.Get(), cl.Get(), m_device.Get());
	return true;
}

// ICommandBuffer 経由が推奨のためスタブ
void DX12DeviceImpl::bindShader    (ShaderHandle      /*h*/)            {}
void DX12DeviceImpl::bindTexture   (TextureHandle     /*h*/, uint32_t /*slot*/) {}
void DX12DeviceImpl::draw          (const GfxDrawCmd* /*cmd*/)          {}
void DX12DeviceImpl::drawFullscreen()                                   {}

// VRAM 使用量を MB 単位で返す
uint64_t DX12DeviceImpl::getVRAMUsageMB() const
{
	if (!m_adapter)
		return 0;

	DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
	m_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info);
	return info.CurrentUsage / (1024 * 1024);
}


// ============================================================
//  DX12PipelineImpl — 実装
// ============================================================

// ---- ABI v2 の列挙値と D3D12 の値が一致することを確認する ----
#define GFX_CHECK_D3D(gfx, d3d) static_assert(static_cast<uint32_t>(gfx) == static_cast<uint32_t>(d3d), #gfx " != " #d3d);
GFX_CHECK_D3D(GfxFormat::R32G32B32A32_Float,          DXGI_FORMAT_R32G32B32A32_FLOAT)
GFX_CHECK_D3D(GfxFormat::R32G32B32_Float,             DXGI_FORMAT_R32G32B32_FLOAT)
GFX_CHECK_D3D(GfxFormat::R16G16B16A16_Float,          DXGI_FORMAT_R16G16B16A16_FLOAT)
GFX_CHECK_D3D(GfxFormat::R32G32_Float,                DXGI_FORMAT_R32G32_FLOAT)
GFX_CHECK_D3D(GfxFormat::D32_Float_S8X24_Uint,        DXGI_FORMAT_D32_FLOAT_S8X24_UINT)
GFX_CHECK_D3D(GfxFormat::R10G10B10A2_Unorm,           DXGI_FORMAT_R10G10B10A2_UNORM)
GFX_CHECK_D3D(GfxFormat::R11G11B10_Float,             DXGI_FORMAT_R11G11B10_FLOAT)
GFX_CHECK_D3D(GfxFormat::R8G8B8A8_Unorm,              DXGI_FORMAT_R8G8B8A8_UNORM)
GFX_CHECK_D3D(GfxFormat::R8G8B8A8_Sint,               DXGI_FORMAT_R8G8B8A8_SINT)
GFX_CHECK_D3D(GfxFormat::R16G16_Sint,                 DXGI_FORMAT_R16G16_SINT)
GFX_CHECK_D3D(GfxFormat::D32_Float,                   DXGI_FORMAT_D32_FLOAT)
GFX_CHECK_D3D(GfxFormat::R32_Sint,                    DXGI_FORMAT_R32_SINT)
GFX_CHECK_D3D(GfxFormat::D24_Unorm_S8_Uint,           DXGI_FORMAT_D24_UNORM_S8_UINT)
GFX_CHECK_D3D(GfxFormat::R8G8_Sint,                   DXGI_FORMAT_R8G8_SINT)
GFX_CHECK_D3D(GfxFormat::D16_Unorm,                   DXGI_FORMAT_D16_UNORM)
GFX_CHECK_D3D(GfxFormat::R16_Sint,                    DXGI_FORMAT_R16_SINT)
GFX_CHECK_D3D(GfxFormat::R8_Sint,                     DXGI_FORMAT_R8_SINT)
GFX_CHECK_D3D(GfxFormat::B8G8R8A8_Unorm,              DXGI_FORMAT_B8G8R8A8_UNORM)
GFX_CHECK_D3D(GfxFormat::B8G8R8A8_Unorm_sRGB,         DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
GFX_CHECK_D3D(GfxBlend::Zero,                         D3D12_BLEND_ZERO)
GFX_CHECK_D3D(GfxBlend::SrcAlphaSat,                  D3D12_BLEND_SRC_ALPHA_SAT)
GFX_CHECK_D3D(GfxBlend::BlendFactor,                  D3D12_BLEND_BLEND_FACTOR)
GFX_CHECK_D3D(GfxBlend::InvSrc1Alpha,                 D3D12_BLEND_INV_SRC1_ALPHA)
GFX_CHECK_D3D(GfxBlendOp::Add,                        D3D12_BLEND_OP_ADD)
GFX_CHECK_D3D(GfxBlendOp::Max,                        D3D12_BLEND_OP_MAX)
GFX_CHECK_D3D(GfxLogicOp::Clear,                      D3D12_LOGIC_OP_CLEAR)
GFX_CHECK_D3D(GfxLogicOp::Noop,                       D3D12_LOGIC_OP_NOOP)
GFX_CHECK_D3D(GfxLogicOp::OrInverted,                 D3D12_LOGIC_OP_OR_INVERTED)
GFX_CHECK_D3D(GfxColorWriteMask::Red,                 D3D12_COLOR_WRITE_ENABLE_RED)
GFX_CHECK_D3D(GfxColorWriteMask::Alpha,               D3D12_COLOR_WRITE_ENABLE_ALPHA)
GFX_CHECK_D3D(GfxColorWriteMask::All,                 D3D12_COLOR_WRITE_ENABLE_ALL)
GFX_CHECK_D3D(GfxFillMode::Wireframe,                 D3D12_FILL_MODE_WIREFRAME)
GFX_CHECK_D3D(GfxFillMode::Solid,                     D3D12_FILL_MODE_SOLID)
GFX_CHECK_D3D(GfxCullMode::None,                      D3D12_CULL_MODE_NONE)
GFX_CHECK_D3D(GfxCullMode::Back,                      D3D12_CULL_MODE_BACK)
GFX_CHECK_D3D(GfxConservativeRaster::On,              D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON)
GFX_CHECK_D3D(GfxCompareFunc::Never,                  D3D12_COMPARISON_FUNC_NEVER)
GFX_CHECK_D3D(GfxCompareFunc::LessEqual,              D3D12_COMPARISON_FUNC_LESS_EQUAL)
GFX_CHECK_D3D(GfxCompareFunc::Always,                 D3D12_COMPARISON_FUNC_ALWAYS)
GFX_CHECK_D3D(GfxDepthWriteMask::All,                 D3D12_DEPTH_WRITE_MASK_ALL)
GFX_CHECK_D3D(GfxStencilOp::Keep,                     D3D12_STENCIL_OP_KEEP)
GFX_CHECK_D3D(GfxStencilOp::Decr,                     D3D12_STENCIL_OP_DECR)
GFX_CHECK_D3D(GfxInputClassification::PerInstance,    D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA)
GFX_CHECK_D3D(GfxIndexBufferStripCut::Value0xFFFFFFFF, D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF)
GFX_CHECK_D3D(GfxPrimitiveTopologyType::Line,         D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE)
GFX_CHECK_D3D(GfxPrimitiveTopologyType::Patch,        D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH)
GFX_CHECK_D3D(GfxPrimitiveTopology::LineList,         D3D_PRIMITIVE_TOPOLOGY_LINELIST)
GFX_CHECK_D3D(GfxPrimitiveTopology::TriangleStrip,    D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP)
GFX_CHECK_D3D(GfxPrimitiveTopology::TriangleStripAdj, D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ)
GFX_CHECK_D3D(GfxPrimitiveTopology::PatchList1,       D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST)
GFX_CHECK_D3D(GfxPrimitiveTopology::PatchList32,      D3D_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST)
GFX_CHECK_D3D(GfxPipelineStateFlags::ToolDebug,       D3D12_PIPELINE_STATE_FLAG_TOOL_DEBUG)
GFX_CHECK_D3D(GfxDescriptorRangeType::Sampler,        D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER)
GFX_CHECK_D3D(GfxDescriptorRangeType::CBV,            D3D12_DESCRIPTOR_RANGE_TYPE_CBV)
GFX_CHECK_D3D(GfxDescriptorRangeFlags::DescriptorsVolatile, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE)
GFX_CHECK_D3D(GfxDescriptorRangeFlags::DataStatic,    D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC)
GFX_CHECK_D3D(GfxDescriptorRangeFlags::DescriptorsStaticKeepingBufferBoundsChecks, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_STATIC_KEEPING_BUFFER_BOUNDS_CHECKS)
GFX_CHECK_D3D(GfxRootParameterType::Constants32Bit,   D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS)
GFX_CHECK_D3D(GfxRootParameterType::UAV,              D3D12_ROOT_PARAMETER_TYPE_UAV)
GFX_CHECK_D3D(GfxShaderVisibility::Pixel,             D3D12_SHADER_VISIBILITY_PIXEL)
GFX_CHECK_D3D(GfxShaderVisibility::Mesh,              D3D12_SHADER_VISIBILITY_MESH)
GFX_CHECK_D3D(GfxRootDescriptorFlags::DataStatic,     D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC)
GFX_CHECK_D3D(GfxRootSignatureFlags::AllowInputAssemblerInputLayout, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT)
GFX_CHECK_D3D(GfxRootSignatureFlags::DenyMeshShaderRootAccess,       D3D12_ROOT_SIGNATURE_FLAG_DENY_MESH_SHADER_ROOT_ACCESS)
GFX_CHECK_D3D(GfxRootSignatureFlags::SamplerHeapDirectlyIndexed,     D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED)
GFX_CHECK_D3D(GfxFilter::MinMagMipLinear,             D3D12_FILTER_MIN_MAG_MIP_LINEAR)
GFX_CHECK_D3D(GfxFilter::Anisotropic,                 D3D12_FILTER_ANISOTROPIC)
GFX_CHECK_D3D(GfxFilter::ComparisonMinMagMipLinear,   D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR)
GFX_CHECK_D3D(GfxFilter::ComparisonAnisotropic,       D3D12_FILTER_COMPARISON_ANISOTROPIC)
GFX_CHECK_D3D(GfxFilter::MinimumMinLinearMagPointMipLinear, D3D12_FILTER_MINIMUM_MIN_LINEAR_MAG_POINT_MIP_LINEAR)
GFX_CHECK_D3D(GfxFilter::MaximumAnisotropic,          D3D12_FILTER_MAXIMUM_ANISOTROPIC)
GFX_CHECK_D3D(GfxTextureAddressMode::MirrorOnce,      D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE)
GFX_CHECK_D3D(GfxStaticBorderColor::OpaqueWhite,      D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE)
#undef GFX_CHECK_D3D

static_assert(GFX_MAX_RENDER_TARGETS == D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT, "RT count");
static_assert(GFX_APPEND_ALIGNED_ELEMENT == D3D12_APPEND_ALIGNED_ELEMENT, "append aligned");
static_assert(GFX_DESCRIPTOR_RANGE_OFFSET_APPEND == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND, "range append");

// structSize / version 付きの記述子を読む。
// 呼び出し側の structSize が小さければ (古いヘッダでビルドされたアプリ) 足りない分は既定値のまま、
// 大きければ (新しいヘッダ) 知らない末尾のフィールドを無視する。
template<typename T>
static bool readVersionedDesc(const T* src, T& out, const char* fn)
{
	if (!src || src->structSize < 2 * sizeof(uint32_t) || src->version == 0)
	{
		fprintf(stderr, "[GfxEngine] %s: structSize / version が不正です\n", fn);
		return false;
	}
	out = T{};
	std::memcpy(static_cast<void*>(&out), src, std::min<size_t>(src->structSize, sizeof(T)));
	out.structSize = static_cast<uint32_t>(sizeof(T));
	return true;
}

// バインディングレイアウトから、既存のバインドコマンドとルートパラメータの対応を求める
static DX12BindingSlots computeBindingSlots(const GfxBindingLayoutDesc& d)
{
	DX12BindingSlots out;
	out.numParameters = d.numParameters;
	for (uint32_t i = 0; i < d.numParameters; ++i)
	{
		const GfxRootParameter& p = d.parameters[i];
		out.paramTypes[i] = p.parameterType;
		if (p.parameterType == GfxRootParameterType::DescriptorTable)
		{
			bool     allSRV     = p.numRanges > 0;
			bool     allSampler = p.numRanges > 0;
			uint32_t cursor     = 0;
			uint32_t total      = 0;
			for (uint32_t r = p.firstRange; r < p.firstRange + p.numRanges; ++r)
			{
				const GfxDescriptorRange& range = d.ranges[r];
				allSRV     = allSRV     && range.rangeType == GfxDescriptorRangeType::SRV;
				allSampler = allSampler && range.rangeType == GfxDescriptorRangeType::Sampler;

				const uint32_t first = (range.offsetInDescriptorsFromTableStart == GFX_DESCRIPTOR_RANGE_OFFSET_APPEND)
					? cursor : range.offsetInDescriptorsFromTableStart;
				// 上限なしのレンジはバインドした数だけ埋める (テーブルの大きさに含めない)
				cursor = (range.numDescriptors == GFX_UNBOUNDED_DESCRIPTOR_COUNT) ? first : first + range.numDescriptors;
				total  = std::max(total, cursor);
			}
			if (allSRV && out.srvTableSlot == UINT32_MAX)
			{
				out.srvTableSlot  = i;
				out.srvTableCount = total;
			}
			else if (allSampler && out.samplerTableSlot == UINT32_MAX)
			{
				out.samplerTableSlot  = i;
				out.samplerTableCount = total;
			}
		}
		else if (p.parameterType == GfxRootParameterType::CBV
			  && p.registerSpace == 0
			  && out.cbvCount < DX12BindingSlots::MAX_CBV)
		{
			out.cbvBindRegisters[out.cbvCount] = p.shaderRegister;
			out.cbvRootSlots    [out.cbvCount] = i;
			++out.cbvCount;
		}
	}
	return out;
}

// GfxBindingLayoutDesc → D3D12 ルートシグネチャ (バージョン 1.1)
BindingLayoutHandle DX12DeviceImpl::createBindingLayout(const GfxBindingLayoutDesc* src)
{
	GfxBindingLayoutDesc d;
	if (!readVersionedDesc(src, d, "createBindingLayout"))
		return {};

	if (d.numParameters > GFX_MAX_ROOT_PARAMETERS || d.numRanges > GFX_MAX_DESCRIPTOR_RANGES
	 || d.numStaticSamplers > GFX_MAX_STATIC_SAMPLERS)
	{
		fprintf(stderr, "[GfxEngine] createBindingLayout: パラメータ / レンジ / 静的サンプラーの数が上限を超えています\n");
		return {};
	}

	D3D12_DESCRIPTOR_RANGE1    ranges  [GFX_MAX_DESCRIPTOR_RANGES] = {};
	D3D12_ROOT_PARAMETER1      params  [GFX_MAX_ROOT_PARAMETERS]   = {};
	D3D12_STATIC_SAMPLER_DESC  samplers[GFX_MAX_STATIC_SAMPLERS]   = {};

	for (uint32_t i = 0; i < d.numRanges; ++i)
	{
		const GfxDescriptorRange& r = d.ranges[i];
		ranges[i].RangeType                         = static_cast<D3D12_DESCRIPTOR_RANGE_TYPE>(r.rangeType);
		ranges[i].NumDescriptors                    = r.numDescriptors;
		ranges[i].BaseShaderRegister                = r.baseShaderRegister;
		ranges[i].RegisterSpace                     = r.registerSpace;
		ranges[i].Flags                             = static_cast<D3D12_DESCRIPTOR_RANGE_FLAGS>(r.flags);
		ranges[i].OffsetInDescriptorsFromTableStart = r.offsetInDescriptorsFromTableStart;
	}

	for (uint32_t i = 0; i < d.numParameters; ++i)
	{
		const GfxRootParameter& p = d.parameters[i];
		D3D12_ROOT_PARAMETER1&  o = params[i];
		o.ParameterType    = static_cast<D3D12_ROOT_PARAMETER_TYPE>(p.parameterType);
		o.ShaderVisibility = static_cast<D3D12_SHADER_VISIBILITY>(p.shaderVisibility);
		switch (p.parameterType)
		{
		case GfxRootParameterType::DescriptorTable:
			if (p.numRanges == 0 || p.firstRange + p.numRanges > d.numRanges)
			{
				fprintf(stderr, "[GfxEngine] createBindingLayout: parameters[%u] のレンジ [%u, %u) が ranges (%u 個) の範囲外です\n",
					i, p.firstRange, p.firstRange + p.numRanges, d.numRanges);
				return {};
			}
			o.DescriptorTable.NumDescriptorRanges = p.numRanges;
			o.DescriptorTable.pDescriptorRanges   = &ranges[p.firstRange];
			break;
		case GfxRootParameterType::Constants32Bit:
			o.Constants.ShaderRegister = p.shaderRegister;
			o.Constants.RegisterSpace  = p.registerSpace;
			o.Constants.Num32BitValues = p.num32BitValues;
			break;
		case GfxRootParameterType::CBV:
		case GfxRootParameterType::SRV:
		case GfxRootParameterType::UAV:
			o.Descriptor.ShaderRegister = p.shaderRegister;
			o.Descriptor.RegisterSpace  = p.registerSpace;
			o.Descriptor.Flags          = static_cast<D3D12_ROOT_DESCRIPTOR_FLAGS>(p.descriptorFlags);
			break;
		default:
			fprintf(stderr, "[GfxEngine] createBindingLayout: parameters[%u] の parameterType (%u) が不正です\n",
				i, static_cast<uint32_t>(p.parameterType));
			return {};
		}
	}

	for (uint32_t i = 0; i < d.numStaticSamplers; ++i)
	{
		const GfxStaticSamplerDesc& s = d.staticSamplers[i];
		D3D12_STATIC_SAMPLER_DESC&  o = samplers[i];
		o.Filter           = static_cast<D3D12_FILTER>(s.filter);
		o.AddressU         = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(s.addressU);
		o.AddressV         = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(s.addressV);
		o.AddressW         = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(s.addressW);
		o.MipLODBias       = s.mipLODBias;
		o.MaxAnisotropy    = s.maxAnisotropy;
		o.ComparisonFunc   = static_cast<D3D12_COMPARISON_FUNC>(s.comparisonFunc);
		o.BorderColor      = static_cast<D3D12_STATIC_BORDER_COLOR>(s.borderColor);
		o.MinLOD           = s.minLOD;
		o.MaxLOD           = s.maxLOD;
		o.ShaderRegister   = s.shaderRegister;
		o.RegisterSpace    = s.registerSpace;
		o.ShaderVisibility = static_cast<D3D12_SHADER_VISIBILITY>(s.shaderVisibility);
	}

	D3D12_VERSIONED_ROOT_SIGNATURE_DESC rsD = {};
	rsD.Version                    = D3D_ROOT_SIGNATURE_VERSION_1_1;
	rsD.Desc_1_1.NumParameters     = d.numParameters;
	rsD.Desc_1_1.pParameters       = d.numParameters > 0 ? params : nullptr;
	rsD.Desc_1_1.NumStaticSamplers = d.numStaticSamplers;
	rsD.Desc_1_1.pStaticSamplers   = d.numStaticSamplers > 0 ? samplers : nullptr;
	rsD.Desc_1_1.Flags             = static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(d.flags);

	ComPtr<ID3DBlob> sigBlob, errBlob;
	if (FAILED(D3D12SerializeVersionedRootSignature(&rsD, &sigBlob, &errBlob)))
	{
		fprintf(stderr, "[GfxEngine] createBindingLayout: シリアライズに失敗しました: %s\n",
			errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "(no message)");
		return {};
	}

	DX12BindingLayoutRes res;
	if (FAILED(m_device->CreateRootSignature(
			0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&res.rootSig))))
	{
		fprintf(stderr, "[GfxEngine] createBindingLayout: CreateRootSignature に失敗しました\n");
		return {};
	}
	setDebugName(res.rootSig.Get(), d.debugName);
	res.slots = computeBindingSlots(d);

	const uint64_t id = m_nextId++;
	m_bindingLayouts.emplace(id, std::move(res));
	return BindingLayoutHandle{ id };
}

// ルートシグネチャは記録済みのコマンドリストが参照しているため遅延解放する。
// それを使って作ったパイプラインは自分で参照を持つので、そのまま使い続けられる。
void DX12DeviceImpl::destroyBindingLayout(BindingLayoutHandle h)
{
	auto it = m_bindingLayouts.find(h.id);
	if (it == m_bindingLayouts.end())
		return;
	deferRelease(std::move(it->second.rootSig));
	m_bindingLayouts.erase(it);
}

// IPipelineVTable の vtable 定義
static const IPipelineVTable k_PipelineVTable =
{
	[](void* s)
	{
		auto* impl = static_cast<DX12PipelineImpl*>(s);
		impl->device()->destroyPipelineImpl(impl);
	}
};

const IPipelineVTable* DX12PipelineImpl::getVTable()
{
	return &k_PipelineVTable;
}

// GfxPipelineDesc から PSO を作成する
DX12PipelineImpl::DX12PipelineImpl()
{
	m_iface.vtable = getVTable();
	m_iface.self   = this;
}

bool DX12PipelineImpl::create(
	DX12DeviceImpl*                dev,
	const GfxGraphicsPipelineDesc& desc,
	const DX12BindingLayoutRes&    layout)
{
	m_dev      = dev;
	m_rootSig  = layout.rootSig;
	m_slots    = layout.slots;
	m_topology = static_cast<D3D_PRIMITIVE_TOPOLOGY>(desc.primitiveTopology);

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.pRootSignature = m_rootSig.Get();

	// シェーダー (VS は必須。PS は depth-only パスでは不要)
	auto setShader = [&](ShaderHandle h, D3D12_SHADER_BYTECODE& out, const char* stage) -> bool
	{
		if (!h.valid())
			return true;
		const DX12ShadRes* sh = dev->getShadRes(h.id);
		if (!sh || sh->bytecode.empty())
		{
			fprintf(stderr, "[GfxEngine] createGraphicsPipeline: %s のシェーダーハンドルが無効です\n", stage);
			return false;
		}
		out = { sh->bytecode.data(), sh->bytecode.size() };
		return true;
	};
	if (!desc.vs.valid())
	{
		fprintf(stderr, "[GfxEngine] createGraphicsPipeline: VS は必須です\n");
		return false;
	}
	if (!setShader(desc.vs, psoDesc.VS, "VS") || !setShader(desc.ps, psoDesc.PS, "PS")
	 || !setShader(desc.ds, psoDesc.DS, "DS") || !setShader(desc.hs, psoDesc.HS, "HS")
	 || !setShader(desc.gs, psoDesc.GS, "GS"))
		return false;

	// BlendState
	psoDesc.BlendState.AlphaToCoverageEnable  = desc.blendState.alphaToCoverageEnable  ? TRUE : FALSE;
	psoDesc.BlendState.IndependentBlendEnable = desc.blendState.independentBlendEnable ? TRUE : FALSE;
	for (uint32_t i = 0; i < GFX_MAX_RENDER_TARGETS; ++i)
	{
		const GfxRenderTargetBlendDesc&  b = desc.blendState.renderTarget[i];
		D3D12_RENDER_TARGET_BLEND_DESC&  o = psoDesc.BlendState.RenderTarget[i];
		o.BlendEnable           = b.blendEnable   ? TRUE : FALSE;
		o.LogicOpEnable         = b.logicOpEnable ? TRUE : FALSE;
		o.SrcBlend              = static_cast<D3D12_BLEND>(b.srcBlend);
		o.DestBlend             = static_cast<D3D12_BLEND>(b.destBlend);
		o.BlendOp               = static_cast<D3D12_BLEND_OP>(b.blendOp);
		o.SrcBlendAlpha         = static_cast<D3D12_BLEND>(b.srcBlendAlpha);
		o.DestBlendAlpha        = static_cast<D3D12_BLEND>(b.destBlendAlpha);
		o.BlendOpAlpha          = static_cast<D3D12_BLEND_OP>(b.blendOpAlpha);
		o.LogicOp               = static_cast<D3D12_LOGIC_OP>(b.logicOp);
		o.RenderTargetWriteMask = static_cast<UINT8>(b.renderTargetWriteMask);
	}
	psoDesc.SampleMask = desc.sampleMask;

	// RasterizerState
	{
		const GfxRasterizerDesc& r = desc.rasterizerState;
		D3D12_RASTERIZER_DESC&   o = psoDesc.RasterizerState;
		o.FillMode              = static_cast<D3D12_FILL_MODE>(r.fillMode);
		o.CullMode              = static_cast<D3D12_CULL_MODE>(r.cullMode);
		o.FrontCounterClockwise = r.frontCounterClockwise ? TRUE : FALSE;
		o.DepthBias             = r.depthBias;
		o.DepthBiasClamp        = r.depthBiasClamp;
		o.SlopeScaledDepthBias  = r.slopeScaledDepthBias;
		o.DepthClipEnable       = r.depthClipEnable       ? TRUE : FALSE;
		o.MultisampleEnable     = r.multisampleEnable     ? TRUE : FALSE;
		o.AntialiasedLineEnable = r.antialiasedLineEnable ? TRUE : FALSE;
		o.ForcedSampleCount     = r.forcedSampleCount;
		o.ConservativeRaster    = static_cast<D3D12_CONSERVATIVE_RASTERIZATION_MODE>(r.conservativeRaster);
	}

	// DepthStencilState
	{
		const GfxDepthStencilStateDesc& ds = desc.depthStencilState;
		D3D12_DEPTH_STENCIL_DESC&       o  = psoDesc.DepthStencilState;
		auto toStencilOp = [](const GfxStencilOpDesc& s)
		{
			D3D12_DEPTH_STENCILOP_DESC r = {};
			r.StencilFailOp      = static_cast<D3D12_STENCIL_OP>(s.stencilFailOp);
			r.StencilDepthFailOp = static_cast<D3D12_STENCIL_OP>(s.stencilDepthFailOp);
			r.StencilPassOp      = static_cast<D3D12_STENCIL_OP>(s.stencilPassOp);
			r.StencilFunc        = static_cast<D3D12_COMPARISON_FUNC>(s.stencilFunc);
			return r;
		};
		o.DepthEnable      = ds.depthEnable   ? TRUE : FALSE;
		o.DepthWriteMask   = static_cast<D3D12_DEPTH_WRITE_MASK>(ds.depthWriteMask);
		o.DepthFunc        = static_cast<D3D12_COMPARISON_FUNC>(ds.depthFunc);
		o.StencilEnable    = ds.stencilEnable ? TRUE : FALSE;
		o.StencilReadMask  = ds.stencilReadMask;
		o.StencilWriteMask = ds.stencilWriteMask;
		o.FrontFace        = toStencilOp(ds.frontFace);
		o.BackFace         = toStencilOp(ds.backFace);
	}

	// InputLayout (semanticName は desc 内の文字列を指す。desc は PSO 作成まで有効)
	D3D12_INPUT_ELEMENT_DESC inputElems[GFX_MAX_INPUT_ELEMENTS] = {};
	const uint32_t inputCount = std::min(desc.inputLayout.elementCount, GFX_MAX_INPUT_ELEMENTS);
	for (uint32_t i = 0; i < inputCount; ++i)
	{
		const GfxInputElementDesc& e = desc.inputLayout.elements[i];
		inputElems[i].SemanticName         = e.semanticName;
		inputElems[i].SemanticIndex        = e.semanticIndex;
		inputElems[i].Format               = static_cast<DXGI_FORMAT>(e.format);
		inputElems[i].InputSlot            = e.inputSlot;
		inputElems[i].AlignedByteOffset    = e.alignedByteOffset;
		inputElems[i].InputSlotClass       = static_cast<D3D12_INPUT_CLASSIFICATION>(e.inputSlotClass);
		inputElems[i].InstanceDataStepRate = e.instanceDataStepRate;
	}
	if (inputCount > 0)
		psoDesc.InputLayout = { inputElems, inputCount };

	psoDesc.IBStripCutValue       = static_cast<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>(desc.ibStripCutValue);
	psoDesc.PrimitiveTopologyType = static_cast<D3D12_PRIMITIVE_TOPOLOGY_TYPE>(desc.primitiveTopologyType);

	psoDesc.NumRenderTargets = std::min(desc.numRenderTargets, GFX_MAX_RENDER_TARGETS);
	for (uint32_t i = 0; i < psoDesc.NumRenderTargets; ++i)
		psoDesc.RTVFormats[i] = static_cast<DXGI_FORMAT>(desc.rtvFormats[i]);
	psoDesc.DSVFormat  = static_cast<DXGI_FORMAT>(desc.dsvFormat);
	psoDesc.SampleDesc = { desc.sampleCount, desc.sampleQuality };
	psoDesc.NodeMask   = desc.nodeMask;
	psoDesc.Flags      = static_cast<D3D12_PIPELINE_STATE_FLAGS>(desc.flags);

	if (FAILED(dev->device()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pso))))
	{
		fprintf(stderr, "[GfxEngine] createGraphicsPipeline: CreateGraphicsPipelineState に失敗しました (%s)\n",
			desc.debugName ? desc.debugName : "unnamed");
		return false;
	}
	setDebugName(m_pso.Get(), desc.debugName);
	return true;
}

// v2: GfxGraphicsPipelineDesc から PSO を作成し IPipeline* を PipelineHandle に格納して返す
PipelineHandle DX12DeviceImpl::createGraphicsPipeline(const GfxGraphicsPipelineDesc* src)
{
	GfxGraphicsPipelineDesc d;
	if (!readVersionedDesc(src, d, "createGraphicsPipeline"))
		return {};

	auto layoutIt = m_bindingLayouts.find(d.bindingLayout.id);
	if (layoutIt == m_bindingLayouts.end())
	{
		fprintf(stderr, "[GfxEngine] createGraphicsPipeline: bindingLayout が無効です (%s)\n",
			d.debugName ? d.debugName : "unnamed");
		return {};
	}

	// セマンティクス名は必ず終端させる
	for (auto& e : d.inputLayout.elements)
		e.semanticName[GFX_MAX_SEMANTIC_NAME - 1] = '\0';

	auto* impl = new DX12PipelineImpl();
	if (!impl->create(this, d, layoutIt->second))
	{
		delete impl;
		return {};
	}

	// PipelineHandle.id に IPipeline* (impl が所有するラッパー) を直接格納する
	// bindPipeline が IPipeline* を直接受け取る設計と整合する
	return PipelineHandle{ reinterpret_cast<uint64_t>(impl->iface()) };
}

// v1 互換: GfxPipelineDesc を v2 (バインディングレイアウト + パイプライン) に変換して作成する
PipelineHandle DX12DeviceImpl::createPipeline(const GfxPipelineDesc* desc)
{
	if (!desc)
		return {};

	// paramCount == 0 は設定漏れとして扱いエラーを返す
	// GfxRootSignatureDesc::defaultLayout() または明示的な add() を使うこと
	if (desc->rootSignature.paramCount == 0)
	{
		fprintf(stderr, "[GfxEngine] createPipeline: rootSignature.paramCount == 0. "
			"Use GfxRootSignatureDesc::defaultLayout() or add() explicitly.\n");
		return {};
	}

	GfxBindingLayoutDesc layoutDesc = gfxConvertRootSignature(desc->rootSignature);
	layoutDesc.debugName = desc->debugName;
	const BindingLayoutHandle layout = createBindingLayout(&layoutDesc);
	if (!layout.valid())
		return {};

	GfxGraphicsPipelineDesc pipeDesc = gfxConvertPipelineDesc(*desc);
	pipeDesc.bindingLayout = layout;
	const PipelineHandle h = createGraphicsPipeline(&pipeDesc);

	// パイプラインがルートシグネチャの参照を持つので、レイアウトのハンドルはここで手放す
	destroyBindingLayout(layout);
	return h;
}

void DX12DeviceImpl::destroyPipeline(PipelineHandle h)
{
	if (!h.valid()) return;
	auto* p = reinterpret_cast<IPipeline*>(h.id);
	destroyPipelineImpl(static_cast<DX12PipelineImpl*>(p->self));
}

// PSO / ルートシグネチャは記録済み・実行中のコマンドリストが参照しているため遅延解放する。
// impl (とラッパー) は CPU 側の情報だけなので即時に解放してよい。
void DX12DeviceImpl::destroyPipelineImpl(DX12PipelineImpl* impl)
{
	if (!impl) return;
	deferRelease(impl->takePSO());
	deferRelease(impl->takeRootSig());
	delete impl;
}

// ============================================================
//  BLAS (Bottom-Level Acceleration Structure)
// ============================================================
BLASHandle DX12DeviceImpl::createBLAS(const GfxBLASDesc* desc)
{
	if (!supportsRaytracing())
	{
		OutputDebugStringA("[GfxEngine] createBLAS: DXR not supported\n");
		return {};
	}

	const auto* vbRes = getBuffer(desc->vertexBuffer.id);
	if (!vbRes || !vbRes->res) return {};

	D3D12_GPU_VIRTUAL_ADDRESS vbAddr =
		vbRes->res->GetGPUVirtualAddress() + desc->positionOffset;

	D3D12_RAYTRACING_GEOMETRY_DESC geomDesc = {};
	geomDesc.Type  = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
	geomDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;

	auto& tri = geomDesc.Triangles;
	tri.VertexBuffer.StartAddress  = vbAddr;
	tri.VertexBuffer.StrideInBytes = desc->vertexStride;
	tri.VertexCount                = desc->vertexCount;
	tri.VertexFormat               = DXGI_FORMAT_R32G32B32_FLOAT;

	if (desc->indexBuffer.valid())
	{
		const auto* ibRes = getBuffer(desc->indexBuffer.id);
		if (ibRes && ibRes->res)
		{
			tri.IndexBuffer = ibRes->res->GetGPUVirtualAddress();
			tri.IndexCount  = desc->indexCount;
			tri.IndexFormat = DXGI_FORMAT_R32_UINT;
		}
	}

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
	inputs.Type           = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
	inputs.Flags          = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
	inputs.NumDescs       = 1;
	inputs.DescsLayout    = D3D12_ELEMENTS_LAYOUT_ARRAY;
	inputs.pGeometryDescs = &geomDesc;

	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild = {};
	m_device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);
	if (prebuild.ResultDataMaxSizeInBytes == 0) return {};

	auto makeASBuffer = [&](uint64_t size, D3D12_RESOURCE_STATES initState) -> ComPtr<ID3D12Resource>
	{
		ComPtr<ID3D12Resource> res;
		D3D12_HEAP_PROPERTIES hp = {};
		hp.Type = D3D12_HEAP_TYPE_DEFAULT;
		D3D12_RESOURCE_DESC rd = {};
		rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
		rd.Width            = size;
		rd.Height           = 1;
		rd.DepthOrArraySize = 1;
		rd.MipLevels        = 1;
		rd.SampleDesc.Count = 1;
		rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
			initState, nullptr, IID_PPV_ARGS(&res));
		return res;
	};

	DX12BLASRes blas;
	// result: RAYTRACING_ACCELERATION_STRUCTURE で作成する (AS バッファの唯一の初期状態)
	// scratch: バッファは初期状態の指定に関わらず COMMON で作られるため COMMON を指定し、下でバリアを張る
	blas.result  = makeASBuffer(prebuild.ResultDataMaxSizeInBytes,
	                            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
	blas.scratch = makeASBuffer(prebuild.ScratchDataSizeInBytes,
	                            D3D12_RESOURCE_STATE_COMMON);
	if (!blas.result || !blas.scratch) return {};

	// GPU コマンドで BLAS を構築する
	ComPtr<ID3D12CommandAllocator> alloc;
	ComPtr<ID3D12GraphicsCommandList4> cl;
	m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
	{
		ComPtr<ID3D12GraphicsCommandList> tmp;
		m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
			alloc.Get(), nullptr, IID_PPV_ARGS(&tmp));
		tmp->QueryInterface(IID_PPV_ARGS(&cl));
	}

	// scratch を COMMON → UNORDERED_ACCESS に遷移してから Build を呼ぶ
	// result は RAYTRACING_ACCELERATION_STRUCTURE で作成済みなのでバリア不要
	{
		D3D12_RESOURCE_BARRIER b = {};
		b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b.Transition.pResource   = blas.scratch.Get();
		b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
		b.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		cl->ResourceBarrier(1, &b);
	}

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
	buildDesc.DestAccelerationStructureData    = blas.result->GetGPUVirtualAddress();
	buildDesc.Inputs                           = inputs;
	buildDesc.ScratchAccelerationStructureData = blas.scratch->GetGPUVirtualAddress();

	cl->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	barrier.UAV.pResource = blas.result.Get();
	cl->ResourceBarrier(1, &barrier);
	executeAndWait(m_cmdQueue.Get(), cl.Get(), m_device.Get());

	if (desc->debugName)
	{
		wchar_t wname[128];
		MultiByteToWideChar(CP_UTF8, 0, desc->debugName, -1, wname, 128);
		blas.result->SetName(wname);
	}

	const uint64_t id = m_nextId++;
	m_blasMap[id] = std::move(blas);
	return { id };
}

void DX12DeviceImpl::destroyBLAS(BLASHandle h)
{
	auto it = m_blasMap.find(h.id);
	if (it == m_blasMap.end())
		return;

	// TLAS が GPU アドレスで参照しているため遅延解放する
	deferRelease(it->second.result);
	deferRelease(it->second.scratch);
	m_blasMap.erase(it);
}

// ============================================================
//  TLAS (Top-Level Acceleration Structure)
// ============================================================

// インスタンスバッファを CPU から書き込む共通処理
static void fillInstanceBuffer(
	ID3D12Resource*           buf,
	const GfxTLASInstance*    instances,
	uint32_t                  count,
	const std::unordered_map<uint64_t, DX12BLASRes>& blasMap)
{
	D3D12_RAYTRACING_INSTANCE_DESC* mapped = nullptr;
	buf->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
	for (uint32_t i = 0; i < count; ++i)
	{
		const auto& inst = instances[i];
		auto& d = mapped[i];
		memset(&d, 0, sizeof(d));

		// 3x4 変換行列をコピー（行優先）
		memcpy(d.Transform, inst.transform, sizeof(float) * 12);

		d.InstanceID                          = inst.instanceID;
		d.InstanceMask                        = 0xFF;
		d.InstanceContributionToHitGroupIndex = inst.hitGroupIndex;
		d.Flags                               = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;

		auto it = blasMap.find(inst.blas.id);
		if (it != blasMap.end())
			d.AccelerationStructure = it->second.result->GetGPUVirtualAddress();
	}
	D3D12_RANGE writeRange = { 0, sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * count };
	buf->Unmap(0, &writeRange);
}

TLASHandle DX12DeviceImpl::createTLAS(const GfxTLASInstance* instances, uint32_t count)
{
	if (!supportsRaytracing() || count == 0) return {};

	// インスタンス記述子バッファ（Upload ヒープ）
	const uint64_t instBufSize = sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * count;
	ComPtr<ID3D12Resource> instBuf;
	{
		D3D12_HEAP_PROPERTIES hp = {};
		hp.Type = D3D12_HEAP_TYPE_UPLOAD;
		D3D12_RESOURCE_DESC rd = {};
		rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
		rd.Width            = instBufSize;
		rd.Height           = 1;
		rd.DepthOrArraySize = 1;
		rd.MipLevels        = 1;
		rd.SampleDesc.Count = 1;
		rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&instBuf));
	}
	fillInstanceBuffer(instBuf.Get(), instances, count, m_blasMap);

	// TLAS プリビルド情報を取得する
	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
	inputs.Type          = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
	inputs.Flags         = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
	inputs.NumDescs      = count;
	inputs.DescsLayout   = D3D12_ELEMENTS_LAYOUT_ARRAY;
	inputs.InstanceDescs = instBuf->GetGPUVirtualAddress();

	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild = {};
	m_device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);
	if (prebuild.ResultDataMaxSizeInBytes == 0) return {};

	auto makeASBuffer2 = [&](uint64_t size, D3D12_RESOURCE_STATES initState) -> ComPtr<ID3D12Resource>
	{
		ComPtr<ID3D12Resource> res;
		D3D12_HEAP_PROPERTIES hp = {};
		hp.Type = D3D12_HEAP_TYPE_DEFAULT;
		D3D12_RESOURCE_DESC rd = {};
		rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
		rd.Width            = size;
		rd.Height           = 1;
		rd.DepthOrArraySize = 1;
		rd.MipLevels        = 1;
		rd.SampleDesc.Count = 1;
		rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
			initState, nullptr, IID_PPV_ARGS(&res));
		return res;
	};

	DX12TLASRes tlas;
	tlas.result  = makeASBuffer2(prebuild.ResultDataMaxSizeInBytes,
	                             D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
	tlas.scratch = makeASBuffer2(prebuild.ScratchDataSizeInBytes,
	                             D3D12_RESOURCE_STATE_COMMON);   // バッファは COMMON で作成される
	tlas.instanceBuf  = instBuf;
	tlas.maxInstances = count;
	tlas.builtCount   = count;
	if (!tlas.result || !tlas.scratch) return {};

	// GPU コマンドで TLAS を構築する
	ComPtr<ID3D12CommandAllocator> alloc;
	ComPtr<ID3D12GraphicsCommandList4> cl;
	m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
	{
		ComPtr<ID3D12GraphicsCommandList> tmp;
		m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
			alloc.Get(), nullptr, IID_PPV_ARGS(&tmp));
		tmp->QueryInterface(IID_PPV_ARGS(&cl));
	}

	// scratch を COMMON → UNORDERED_ACCESS に遷移してから Build を呼ぶ
	// result は RAYTRACING_ACCELERATION_STRUCTURE で作成済みなのでバリア不要
	{
		D3D12_RESOURCE_BARRIER b = {};
		b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b.Transition.pResource   = tlas.scratch.Get();
		b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
		b.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		cl->ResourceBarrier(1, &b);
	}

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
	buildDesc.DestAccelerationStructureData    = tlas.result->GetGPUVirtualAddress();
	buildDesc.Inputs                           = inputs;
	buildDesc.ScratchAccelerationStructureData = tlas.scratch->GetGPUVirtualAddress();

	cl->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	barrier.UAV.pResource = tlas.result.Get();
	cl->ResourceBarrier(1, &barrier);
	executeAndWait(m_cmdQueue.Get(), cl.Get(), m_device.Get());

	const uint64_t id = m_nextId++;
	m_tlasMap[id] = std::move(tlas);
	return { id };
}

void DX12DeviceImpl::destroyTLAS(TLASHandle h)
{
	auto it = m_tlasMap.find(h.id);
	if (it == m_tlasMap.end())
		return;

	deferRelease(it->second.result);
	deferRelease(it->second.scratch);
	deferRelease(it->second.instanceBuf);
	m_tlasMap.erase(it);
}

void DX12DeviceImpl::updateTLAS(TLASHandle h, const GfxTLASInstance* instances, uint32_t count)
{
	auto it = m_tlasMap.find(h.id);
	if (it == m_tlasMap.end()) return;

	auto& tlas = it->second;
	if (count == 0 || count > tlas.maxInstances) return;

	// インスタンスバッファを更新 (CPU 書き込み)
	// 前回のビルドは executeAndWait で完了済みのため、上書きしても競合しない
	fillInstanceBuffer(tlas.instanceBuf.Get(), instances, count, m_blasMap);

	// PERFORM_UPDATE はインスタンス数が前回のビルドと同じ場合のみ有効。
	// 数が変わった場合はフルリビルドする (サイズは作成時の maxInstances 分を確保済み)
	const bool canUpdate = (count == tlas.builtCount);

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
	inputs.Type          = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
	inputs.Flags         = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
	if (canUpdate)
		inputs.Flags    |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
	inputs.NumDescs      = count;
	inputs.DescsLayout   = D3D12_ELEMENTS_LAYOUT_ARRAY;
	inputs.InstanceDescs = tlas.instanceBuf->GetGPUVirtualAddress();

	// 同期実行する (executeAndWait)。毎フレームの非同期化は Phase 2 で行う
	ComPtr<ID3D12CommandAllocator> alloc;
	ComPtr<ID3D12GraphicsCommandList4> cl;
	m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
	{
		ComPtr<ID3D12GraphicsCommandList> tmp;
		m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
			alloc.Get(), nullptr, IID_PPV_ARGS(&tmp));
		tmp->QueryInterface(IID_PPV_ARGS(&cl));
	}

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
	buildDesc.DestAccelerationStructureData    = tlas.result->GetGPUVirtualAddress();
	buildDesc.SourceAccelerationStructureData  = canUpdate ? tlas.result->GetGPUVirtualAddress() : 0;
	buildDesc.Inputs                           = inputs;
	buildDesc.ScratchAccelerationStructureData = tlas.scratch->GetGPUVirtualAddress();

	cl->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	barrier.UAV.pResource = tlas.result.Get();
	cl->ResourceBarrier(1, &barrier);
	executeAndWait(m_cmdQueue.Get(), cl.Get(), m_device.Get());
	tlas.builtCount = count;
}

// ============================================================
//  DXC を使って RT シェーダーをコンパイルするヘルパー
// ============================================================
static bool compileRTShader(
	const char*           hlslSource,
	const wchar_t*        entryPoint,
	const wchar_t*        profile,
	std::vector<uint8_t>& outBytecode)
{
#ifndef GFX_HAS_DXC
	OutputDebugStringA("[GfxEngine] compileRTShader: DXC not available (set GFX_DXC_DIR in cmake)\n");
	(void)hlslSource; (void)entryPoint; (void)profile; (void)outBytecode;
	return false;
#else
	// dxcompiler.dll を動的ロードする
	static HMODULE s_dxc = LoadLibraryW(L"dxcompiler.dll");
	if (!s_dxc)
	{
		OutputDebugStringA("[GfxEngine] dxcompiler.dll not found\n");
		return false;
	}

	typedef HRESULT(WINAPI* PFN_DxcCreateInstance)(REFCLSID, REFIID, LPVOID*);
	auto pfn = reinterpret_cast<PFN_DxcCreateInstance>(
		GetProcAddress(s_dxc, "DxcCreateInstance"));
	if (!pfn) return false;

	ComPtr<IDxcCompiler3> compiler;
	ComPtr<IDxcUtils>     utils;
	pfn(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler));
	pfn(CLSID_DxcUtils,    IID_PPV_ARGS(&utils));
	if (!compiler || !utils) return false;

	DxcBuffer src;
	src.Ptr      = hlslSource;
	src.Size     = strlen(hlslSource);
	src.Encoding = CP_UTF8;

	const wchar_t* args[] = {
		L"-E", entryPoint,
		L"-T", profile,
		L"-HV", L"2021",
		L"-Zpc",   // column-major
	};

	ComPtr<IDxcResult> result;
	HRESULT hr = compiler->Compile(&src, args, _countof(args),
		nullptr, IID_PPV_ARGS(&result));

	ComPtr<IDxcBlobUtf8> errors;
	result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
	if (errors && errors->GetStringLength() > 0)
	{
		OutputDebugStringA("[GfxEngine] RT shader compile error:\n");
		OutputDebugStringA(errors->GetStringPointer());
		fprintf(stderr, "[GfxEngine] RT shader compile error:\n%s\n",
			errors->GetStringPointer());
	}

	if (FAILED(hr)) return false;

	HRESULT status;
	result->GetStatus(&status);
	if (FAILED(status)) return false;

	ComPtr<IDxcBlob> blob;
	result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&blob), nullptr);
	if (!blob) return false;

	const uint8_t* p = static_cast<const uint8_t*>(blob->GetBufferPointer());
	outBytecode.assign(p, p + blob->GetBufferSize());
	return true;
#endif // GFX_HAS_DXC
}

// ============================================================
//  RT パイプライン (RTPSO)
// ============================================================
RTPipelineHandle DX12DeviceImpl::createRTPipeline(const GfxRTPipelineDesc* desc)
{
	if (!supportsRaytracing()) return {};
	if (!desc->raygenSource || !desc->missSource || !desc->closestHitSource) return {};
#ifndef GFX_HAS_DXC
	OutputDebugStringA("[GfxEngine] createRTPipeline: DXC not available\n");
	return {};
#else
	// 3つのシェーダーソースを1つの lib としてコンパイルする
	// 各ソースをそのまま結合する（重複定義は各ソース側で避けること）
	std::string combined =
		std::string(desc->raygenSource) + "\n" +
		std::string(desc->missSource)   + "\n" +
		std::string(desc->closestHitSource);

	std::vector<uint8_t> bytecode;
	if (!compileRTShader(combined.c_str(), L"", L"lib_6_3", bytecode))
	{
		OutputDebugStringA("[GfxEngine] createRTPipeline: shader compile failed\n");
		return {};
	}

	// エントリポイント名
	const wchar_t* k_raygen     = L"RayGen";
	const wchar_t* k_miss       = L"Miss";
	const wchar_t* k_closestHit = L"ClosestHit";
	const wchar_t* k_hitGroup   = L"HitGroup";

	// StateObject サブオブジェクト
	std::vector<D3D12_STATE_SUBOBJECT> subs;
	subs.reserve(16);

	// 1. DXIL ライブラリ
	D3D12_EXPORT_DESC exports[] = {
		{ k_raygen,     nullptr, D3D12_EXPORT_FLAG_NONE },
		{ k_miss,       nullptr, D3D12_EXPORT_FLAG_NONE },
		{ k_closestHit, nullptr, D3D12_EXPORT_FLAG_NONE },
	};
	D3D12_DXIL_LIBRARY_DESC libDesc = {};
	libDesc.DXILLibrary.pShaderBytecode = bytecode.data();
	libDesc.DXILLibrary.BytecodeLength  = bytecode.size();
	libDesc.NumExports   = _countof(exports);
	libDesc.pExports     = exports;
	subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &libDesc });

	// 2. Hit Group
	D3D12_HIT_GROUP_DESC hitGroupDesc = {};
	hitGroupDesc.HitGroupExport         = k_hitGroup;
	hitGroupDesc.Type                   = D3D12_HIT_GROUP_TYPE_TRIANGLES;
	hitGroupDesc.ClosestHitShaderImport = k_closestHit;
	subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hitGroupDesc });

	// 3. Shader Config (payload / attribute サイズ)
	D3D12_RAYTRACING_SHADER_CONFIG shaderCfg = {};
	shaderCfg.MaxPayloadSizeInBytes   = desc->maxPayloadSize;
	shaderCfg.MaxAttributeSizeInBytes = desc->maxAttributeSize;
	subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shaderCfg });

	// 4. Global Root Signature
	// UAV(u0): 出力テクスチャ, SRV(t0): TLAS, CBV(b0): カメラ定数
	// UAV テーブル: u0（RT 出力）
	D3D12_DESCRIPTOR_RANGE1 uavRange = {};
	uavRange.RangeType        = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
	uavRange.NumDescriptors   = 1;
	uavRange.BaseShaderRegister = 0;
	uavRange.OffsetInDescriptorsFromTableStart = 0;
	uavRange.Flags            = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;

	// SRV テーブル: t1（深度テクスチャ）
	D3D12_DESCRIPTOR_RANGE1 srvRange = {};
	srvRange.RangeType        = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	srvRange.NumDescriptors   = 1;
	srvRange.BaseShaderRegister = 1; // t1
	srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
	srvRange.Flags            = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;

	D3D12_ROOT_PARAMETER1 rtParams[4] = {};
	// param 0: UAV テーブル (u0) = RT 出力
	rtParams[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	rtParams[0].DescriptorTable.NumDescriptorRanges = 1;
	rtParams[0].DescriptorTable.pDescriptorRanges   = &uavRange;
	rtParams[0].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
	// param 1: SRV (t0) = TLAS（AS は直接 GPU アドレスで渡す）
	rtParams[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_SRV;
	rtParams[1].Descriptor.ShaderRegister           = 0;
	rtParams[1].Descriptor.RegisterSpace            = 0;
	rtParams[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
	// param 2: SRV テーブル (t1) = 深度テクスチャ
	rtParams[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	rtParams[2].DescriptorTable.NumDescriptorRanges = 1;
	rtParams[2].DescriptorTable.pDescriptorRanges   = &srvRange;
	rtParams[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
	// param 3: CBV (b0) = RTShadowCB（InvViewProj / LightPos）
	rtParams[3].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_CBV;
	rtParams[3].Descriptor.ShaderRegister           = 0;
	rtParams[3].Descriptor.RegisterSpace            = 0;
	rtParams[3].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

	// Static Sampler (s0) = リニアサンプラー
	D3D12_STATIC_SAMPLER_DESC staticSampler = {};
	staticSampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	staticSampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	staticSampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	staticSampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	staticSampler.ShaderRegister   = 0;
	staticSampler.RegisterSpace    = 0;
	staticSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	D3D12_VERSIONED_ROOT_SIGNATURE_DESC rsd = {};
	rsd.Version                    = D3D_ROOT_SIGNATURE_VERSION_1_1;
	rsd.Desc_1_1.NumParameters     = 4;
	rsd.Desc_1_1.pParameters       = rtParams;
	rsd.Desc_1_1.NumStaticSamplers = 1;
	rsd.Desc_1_1.pStaticSamplers   = &staticSampler;
	rsd.Desc_1_1.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE;

	ComPtr<ID3DBlob> rsSer, rsErr;
	D3D12SerializeVersionedRootSignature(&rsd, &rsSer, &rsErr);
	ComPtr<ID3D12RootSignature> globalRS;
	m_device->CreateRootSignature(0,
		rsSer->GetBufferPointer(), rsSer->GetBufferSize(),
		IID_PPV_ARGS(&globalRS));

	subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, globalRS.GetAddressOf() });
	// 5. Pipeline Config
	D3D12_RAYTRACING_PIPELINE_CONFIG pipelineCfg = {};
	pipelineCfg.MaxTraceRecursionDepth = desc->maxRecursionDepth;
	subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipelineCfg });

	// StateObject 作成
	D3D12_STATE_OBJECT_DESC soDesc = {};
	soDesc.Type          = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
	soDesc.NumSubobjects = static_cast<UINT>(subs.size());
	soDesc.pSubobjects   = subs.data();

	DX12RTPipelineRes rt;
	if (FAILED(m_device->CreateStateObject(&soDesc, IID_PPV_ARGS(&rt.pso))))
	{
		OutputDebugStringA("[GfxEngine] createRTPipeline: CreateStateObject failed\n");
		return {};
	}
	rt.pso->QueryInterface(IID_PPV_ARGS(&rt.props));
	rt.globalRS = globalRS;

	// Shader Table 作成
	// レイアウト: [RayGen][Miss][HitGroup] それぞれ 1 エントリ
	const uint64_t D3D12_SHADER_IDENTIFIER_SIZE = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
	const uint64_t stride = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT;
	const uint64_t tableSize = stride * 3;  // raygen / miss / hitgroup

	D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
	D3D12_RESOURCE_DESC rd = {};
	rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = tableSize;
	rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
	rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&rt.shaderTable));

	uint8_t* mapped = nullptr;
	rt.shaderTable->Map(0, nullptr, reinterpret_cast<void**>(&mapped));

	void* rgId = rt.props->GetShaderIdentifier(k_raygen);
	void* msId = rt.props->GetShaderIdentifier(k_miss);
	void* hgId = rt.props->GetShaderIdentifier(k_hitGroup);

	memcpy(mapped + stride * 0, rgId, D3D12_SHADER_IDENTIFIER_SIZE);
	memcpy(mapped + stride * 1, msId, D3D12_SHADER_IDENTIFIER_SIZE);
	memcpy(mapped + stride * 2, hgId, D3D12_SHADER_IDENTIFIER_SIZE);

	rt.shaderTable->Unmap(0, nullptr);
	rt.tableStride = stride;
	rt.tableSize   = tableSize;

	if (desc->debugName)
	{
		wchar_t wname[128];
		MultiByteToWideChar(CP_UTF8, 0, desc->debugName, -1, wname, 128);
		rt.pso->SetName(wname);
	}

	const uint64_t id = m_nextId++;
	m_rtPipelineMap[id] = std::move(rt);

	OutputDebugStringA("[GfxEngine] createRTPipeline: OK\n");
	return { id };
#endif // GFX_HAS_DXC
}

void DX12DeviceImpl::destroyRTPipeline(RTPipelineHandle h)
{
	auto it = m_rtPipelineMap.find(h.id);
	if (it == m_rtPipelineMap.end())
		return;

	// 実行中の DispatchRays が参照している可能性があるため遅延解放する
	deferRelease(it->second.pso);
	deferRelease(it->second.globalRS);
	deferRelease(it->second.shaderTable);
	m_rtPipelineMap.erase(it);
}
ID3D12PipelineState* DX12DeviceImpl::getOrCreatePSO(
	uint64_t             vsId,
	uint64_t             psId,
	const DXGI_FORMAT*   rtvFmts,
	uint32_t             rtvCount,
	DXGI_FORMAT          dsvFmt)
{
	// VS のレイアウト情報を取得してキーに含める
	const DX12ShadRes* vs = getShadRes(vsId);
	const DX12ShadRes* ps = getShadRes(psId);

	// layout は GfxPipelineDesc に移動済みのため layoutHash は 0 固定
	const uint64_t layoutHash = 0;

	DX12PSOKey key;
	key.vsId       = vsId;
	key.psId       = psId;
	key.rtvCount   = rtvCount;
	key.dsvFmt     = dsvFmt;
	key.layoutHash = layoutHash;
	for (uint32_t i = 0; i < rtvCount; ++i)
		key.rtvFmts[i] = rtvFmts[i];

	auto it = m_psoCache.find(key);
	if (it != m_psoCache.end())
		return it->second.Get();

	const DXGI_FORMAT rtvFmt = rtvCount > 0 ? rtvFmts[0] : DXGI_FORMAT_UNKNOWN;

	// シェーダーバイトコードを取得する (冒頭で取得済み)
	if (!vs || vs->bytecode.empty() || !ps || ps->bytecode.empty())
		return defaultPSOForFormat(rtvFmt);   // フォールバック

	// buildInputLayout: VertexAttribute → セマンティクス変換はグローバルテーブルが担う
	// getOrCreatePSO は後方互換パスのため vertexLayout 情報がない → 空レイアウト
	D3D12_INPUT_ELEMENT_DESC inputElems[GfxVertexLayout::MAX_ELEMENTS] = {};
	GfxVertexLayout emptyLayout;
	uint32_t inputCount = buildInputLayout(emptyLayout, inputElems);

	// PSO 作成
	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc               = {};
	psoDesc.pRootSignature                                    = m_defaultRootSig.Get();
	psoDesc.VS                                                = { vs->bytecode.data(), vs->bytecode.size() };

	// RT なし (深度専用パス) のときは PS を設定しない
	if (rtvFmt != DXGI_FORMAT_UNKNOWN)
		psoDesc.PS = { ps->bytecode.data(), ps->bytecode.size() };

	psoDesc.InputLayout = { inputElems, inputCount };

	psoDesc.RasterizerState.FillMode                          = D3D12_FILL_MODE_SOLID;
	psoDesc.RasterizerState.CullMode                          = D3D12_CULL_MODE_NONE;
	psoDesc.RasterizerState.FrontCounterClockwise             = FALSE;
	psoDesc.RasterizerState.DepthClipEnable                   = TRUE;
	psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask  = D3D12_COLOR_WRITE_ENABLE_ALL;
	psoDesc.SampleMask                                        = UINT_MAX;
	psoDesc.PrimitiveTopologyType                             = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

	// RT の設定 (MRT 対応)
	if (rtvCount > 0)
	{
		psoDesc.NumRenderTargets = rtvCount;
		for (uint32_t i = 0; i < rtvCount; ++i)
			psoDesc.RTVFormats[i] = rtvFmts[i];
	}
	else
	{
		psoDesc.NumRenderTargets = 0;
	}

	// DS の設定
	if (dsvFmt != DXGI_FORMAT_UNKNOWN)
	{
		psoDesc.DSVFormat                        = dsvFmt;
		psoDesc.DepthStencilState.DepthEnable    = TRUE;
		psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		psoDesc.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS;
		psoDesc.DepthStencilState.StencilEnable  = FALSE;
	}
	else
	{
		psoDesc.DSVFormat                        = DXGI_FORMAT_UNKNOWN;
		psoDesc.DepthStencilState.DepthEnable    = FALSE;
		psoDesc.DepthStencilState.StencilEnable  = FALSE;
	}

	psoDesc.SampleDesc = { 1, 0 };

	ComPtr<ID3D12PipelineState> pso;
	if (FAILED(m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso))))
	{
		OutputDebugStringA("[GfxEngine] getOrCreatePSO: CreateGraphicsPipelineState failed\n");
		return defaultPSOForFormat(rtvFmt);   // フォールバック
	}

	m_psoCache.emplace(key, std::move(pso));

	// PSO を新規作成したときはログを出力する
	// FPS スパイクと重なる場合は PSO キャッシュミスが原因
	char msg[256];
	snprintf(msg, sizeof(msg),
		"[GfxEngine] PSO created: vsId=%llu psId=%llu rtvCount=%u rtvFmt[0]=%u dsvFmt=%u layoutHash=%llu\n",
		(unsigned long long)vsId,
		(unsigned long long)psId,
		rtvCount,
		rtvCount > 0 ? (unsigned)rtvFmts[0] : 0,
		(unsigned)dsvFmt,
		(unsigned long long)key.layoutHash);
	OutputDebugStringA(msg);

	return m_psoCache.at(key).Get();
}

// submitCommandBuffers 用フェンスを起動時に作成する
bool DX12DeviceImpl::createSubmitFence()
{
	if (FAILED(m_device->CreateFence(
			0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_submitFence))))
		return false;

	m_submitEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	return m_submitEvent != nullptr;
}

// 全 GPU リソースを解放する
void DX12DeviceImpl::shutdown()
{
	if (!m_device)
		return;

	// GPU の全実行完了を待ってからリソースを解放する
	// (submit フェンス以外 = present / executeAndWait のコマンドもキュー全体で待つ)
	waitGPUIdle();
	if (m_cmdQueue && m_submitFence)
	{
		const uint64_t v = ++m_submitFenceValue;
		m_cmdQueue->Signal(m_submitFence.Get(), v);
		waitForFence(v);
	}

	// 遅延解放キューを全件処理する
	processDeferredReleases(true);

	for (auto& e : m_cbPool) delete e.impl;
	m_cbPool.clear();
	m_openCBs.clear();
	m_fixupPool.clear();

	m_psoCache.clear();
	m_defaultPSO_BGRA8.Reset();
	m_defaultPSO_RGBA8.Reset();
	m_defaultRootSig.Reset();
	m_buffers.clear();
	m_textures.clear();
	m_shaders.clear();
	m_samplers.clear();
	m_bindingLayouts.clear();
	m_queryPools.clear();
	m_views.clear();
	m_drawSig.Reset();
	m_drawIndexedSig.Reset();
	m_dispatchSig.Reset();
	m_blasMap.clear();
	m_tlasMap.clear();
	m_rtPipelineMap.clear();
	m_globalStates.clear();

	if (m_submitEvent)
	{
		CloseHandle(m_submitEvent);
		m_submitEvent = nullptr;
	}
	m_submitFence.Reset();

	m_cmdQueue.Reset();
	m_adapter.Reset();
	m_factory.Reset();
	m_device.Reset();
}

// デフォルト RootSignature を作成する
// デフォルト RootSignature と PSO を作成する
// dxcompiler.dll が存在する場合は実行時 HLSL コンパイルで PSO を生成する。
// 存在しない場合は RootSignature のみ作成し PSO は nullptr のままにする。
bool DX12DeviceImpl::createDefaultPSO()
{
	// ---- RootSignature ----
	// Slot 0: DescriptorTable (SRV  t0-t3, Pixel 可視) ← G-Buffer 4枚に対応
	// Slot 1: DescriptorTable (Sampler s0, Pixel 可視)
	// Slot 2: Root CBV        (b0, VS/PS 両方可視)
	D3D12_DESCRIPTOR_RANGE1 srvRange                          = {};
	srvRange.RangeType                                        = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	srvRange.NumDescriptors                                   = 4;  // t0, t1, t2, t3
	srvRange.BaseShaderRegister                               = 0;
	srvRange.RegisterSpace                                    = 0;
	srvRange.OffsetInDescriptorsFromTableStart                = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
	srvRange.Flags                                            = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;

	D3D12_DESCRIPTOR_RANGE1 samplerRange                      = {};
	samplerRange.RangeType                                    = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
	samplerRange.NumDescriptors                               = 1;
	samplerRange.BaseShaderRegister                           = 0;
	samplerRange.RegisterSpace                                = 0;
	samplerRange.OffsetInDescriptorsFromTableStart            = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
	samplerRange.Flags                                        = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;

	D3D12_ROOT_PARAMETER1 params[3]                           = {};

	// Param 0: SRV テーブル (t0, Pixel 可視)
	params[0].ParameterType                                   = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[0].DescriptorTable.NumDescriptorRanges             = 1;
	params[0].DescriptorTable.pDescriptorRanges               = &srvRange;
	params[0].ShaderVisibility                                = D3D12_SHADER_VISIBILITY_PIXEL;

	// Param 1: Sampler テーブル (s0, Pixel 可視)
	params[1].ParameterType                                   = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[1].DescriptorTable.NumDescriptorRanges             = 1;
	params[1].DescriptorTable.pDescriptorRanges               = &samplerRange;
	params[1].ShaderVisibility                                = D3D12_SHADER_VISIBILITY_PIXEL;

	// Param 2: Root CBV (b0, VS/PS 両方可視)
	// SetGraphicsRootConstantBufferView で GPU アドレスを直接渡す
	params[2].ParameterType                                   = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[2].Descriptor.ShaderRegister                       = 0;
	params[2].Descriptor.RegisterSpace                        = 0;
	params[2].Descriptor.Flags                                = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
	params[2].ShaderVisibility                                = D3D12_SHADER_VISIBILITY_ALL;

	D3D12_VERSIONED_ROOT_SIGNATURE_DESC rsDesc                = {};
	rsDesc.Version                                            = D3D_ROOT_SIGNATURE_VERSION_1_1;
	rsDesc.Desc_1_1.NumParameters                             = 3;
	rsDesc.Desc_1_1.pParameters                               = params;
	rsDesc.Desc_1_1.NumStaticSamplers                         = 0;
	rsDesc.Desc_1_1.Flags                                     =
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

	ComPtr<ID3DBlob> sigBlob;
	ComPtr<ID3DBlob> errBlob;
	if (FAILED(D3D12SerializeVersionedRootSignature(&rsDesc, &sigBlob, &errBlob)))
		return false;

	if (FAILED(m_device->CreateRootSignature(
			0,
			sigBlob->GetBufferPointer(),
			sigBlob->GetBufferSize(),
			IID_PPV_ARGS(&m_defaultRootSig))))
		return false;

	// ---- HLSL ソース ----
	// D3DCompile (d3dcompiler.lib) で SM 5.1 の DXBC を生成する。
	// dxcompiler.dll 不要。Windows SDK 付属の d3dcompiler_47.dll で動作する。
	static const char* k_VS_SRC =
		"float4 main(uint id : SV_VertexID) : SV_Position {"
		"  float2 uv = float2((id & 1u) << 1u, id >> 1u);"
		"  return float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, 0.0f, 1.0f);"
		"}";

	static const char* k_PS_SRC =
		"float4 main() : SV_Target {"
		"  return float4(1.0f, 1.0f, 1.0f, 1.0f);"
		"}";

	// D3DCompile を使って DXBC バイトコードを生成する
	auto compile = [](const char* src, const char* profile) -> ComPtr<ID3DBlob>
	{
		ComPtr<ID3DBlob> code;
		ComPtr<ID3DBlob> err;
		HRESULT hr = D3DCompile(
			src,
			strlen(src),
			nullptr,        // sourceName
			nullptr,        // defines
			nullptr,        // includes
			"main",
			profile,
			D3DCOMPILE_OPTIMIZATION_LEVEL1,
			0,
			&code,
			&err);

		if (FAILED(hr))
			return nullptr;

		return code;
	};

	ComPtr<ID3DBlob> vsBlob = compile(k_VS_SRC, "vs_5_1");
	ComPtr<ID3DBlob> psBlob = compile(k_PS_SRC, "ps_5_1");

	if (!vsBlob || !psBlob)
	{
		// コンパイル失敗は致命的にしない。PSO なしで続行する。
		return true;
	}

	// ---- PSO 作成ヘルパー ----
	auto makePSO = [&](DXGI_FORMAT fmt, ComPtr<ID3D12PipelineState>& out)
	{
		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc               = {};
		psoDesc.pRootSignature                                    = m_defaultRootSig.Get();
		psoDesc.VS                                                = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
		psoDesc.PS                                                = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
		psoDesc.RasterizerState.FillMode                          = D3D12_FILL_MODE_SOLID;
		psoDesc.RasterizerState.CullMode                          = D3D12_CULL_MODE_NONE;
		psoDesc.RasterizerState.FrontCounterClockwise             = FALSE;
		psoDesc.RasterizerState.DepthClipEnable                   = TRUE;
		psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask  = D3D12_COLOR_WRITE_ENABLE_ALL;
		psoDesc.DepthStencilState.DepthEnable                     = FALSE;
		psoDesc.DepthStencilState.StencilEnable                   = FALSE;
		psoDesc.SampleMask                                        = UINT_MAX;
		psoDesc.PrimitiveTopologyType                             = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		psoDesc.NumRenderTargets                                  = 1;
		psoDesc.RTVFormats[0]                                     = fmt;
		psoDesc.SampleDesc                                        = { 1, 0 };

		if (FAILED(m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&out))))
			out = nullptr;
	};

	makePSO(DXGI_FORMAT_R8G8B8A8_UNORM, m_defaultPSO_RGBA8);
	makePSO(DXGI_FORMAT_B8G8R8A8_UNORM, m_defaultPSO_BGRA8);

	return true;
}

// ============================================================
//  DX12CommandBufferImpl — 実装
// ============================================================

DX12CommandBufferImpl::DX12CommandBufferImpl()
{
	m_iface.vtable = getVTable();
	m_iface.self   = this;
}

// コマンドアロケータとコマンドリストを作成する
bool DX12CommandBufferImpl::create(
	DX12DeviceImpl*   dev,
	CommandBufferType type)
{
	m_dev  = dev;

	D3D12_COMMAND_LIST_TYPE d3dType =
		(type == CommandBufferType::Bundle)
			? D3D12_COMMAND_LIST_TYPE_BUNDLE
			: D3D12_COMMAND_LIST_TYPE_DIRECT;

	m_type = d3dType;

	if (FAILED(dev->device()->CreateCommandAllocator(d3dType, IID_PPV_ARGS(&m_alloc))))
		return false;

	// CommandList を作成してから CommandList4 に QueryInterface する
	ComPtr<ID3D12GraphicsCommandList> cmdList;
	if (FAILED(dev->device()->CreateCommandList(
			0, d3dType, m_alloc.Get(), nullptr,
			IID_PPV_ARGS(&cmdList))))
		return false;

	if (FAILED(cmdList->QueryInterface(IID_PPV_ARGS(&m_cmdList))))
		return false;

	m_cmdList->Close();
	return true;
}

// 記録を開始する
void DX12CommandBufferImpl::begin()
{
	// 同じ CB を release せずに再利用する場合、前回の実行完了を待ってから Reset する
	if (m_lastSubmitFence > m_dev->completedFence())
		m_dev->waitForFence(m_lastSubmitFence);

	m_alloc->Reset();
	m_cmdList->Reset(m_alloc.Get(), nullptr);

	// シェーダーバインド状態・状態追跡をリセットする
	m_boundVS             = 0;
	m_boundPS             = 0;
	m_currentRTVCount     = 0;
	m_currentDSVFmt       = DXGI_FORMAT_UNKNOWN;
	m_srvBindingCount     = 0;
	m_samplerBindingCount = 0;
	m_currentPipeline     = nullptr;
	m_topology            = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	for (auto& f : m_currentRTVFmts) f = DXGI_FORMAT_UNKNOWN;
	for (auto& b : m_srvBindings)     b = 0;
	for (auto& b : m_samplerBindings) b = 0;
	m_localStates.clear();
	m_firstSrvRingPos     = UINT64_MAX;
	m_firstSamplerRingPos = UINT64_MAX;
	m_computeBound        = false;
	m_explicitTableMask   = 0;
	m_bufferStates.clear();
	m_resolves.clear();

	// Direct のみ SRV / Sampler ヒープと RootSignature をバインドする
	// PSO は setRenderTargets() で RT フォーマットを見てセットする
	// Bundle は親 Direct の SetDescriptorHeaps を継承する
	if (m_type == D3D12_COMMAND_LIST_TYPE_DIRECT)
	{
		ID3D12DescriptorHeap* heaps[] = { m_dev->srvHeap(), m_dev->samplerHeap() };
		m_cmdList->SetDescriptorHeaps(2, heaps);
		m_cmdList->SetGraphicsRootSignature(m_dev->defaultRootSig());
		m_dev->markCBOpen(this);
	}
}

// 記録を終了する
void DX12CommandBufferImpl::end()
{
	// バッファは CB の中だけで追跡するので、次の CB のために COMMON へ戻す
	for (const auto& bs : m_bufferStates)
		if (bs.current != D3D12_RESOURCE_STATE_COMMON)
			transitionNow(bs.res, bs.current, D3D12_RESOURCE_STATE_COMMON);
	m_bufferStates.clear();

	m_cmdList->Close();
}

// Bundle を Direct に埋め込む
void DX12CommandBufferImpl::executeBundle(DX12CommandBufferImpl* bundle)
{
	if (bundle && bundle->m_type == D3D12_COMMAND_LIST_TYPE_BUNDLE)
		m_cmdList->ExecuteBundle(bundle->m_cmdList.Get());
}

// オブジェクトを解放する
void DX12CommandBufferImpl::release()
{
	if (m_dev && isDirect())
	{
		// Direct CB はプールに返却して再利用する (GPU 完了後に再割り当てされる)
		m_dev->returnToPool(this);
		return;
	}

	// Bundle は実行中の Direct から参照されている可能性があるため遅延解放する
	if (m_dev)
	{
		m_dev->deferRelease(m_cmdList);
		m_dev->deferRelease(m_alloc);
	}
	delete this;
}

// ---- 状態追跡 ----

void DX12CommandBufferImpl::transitionNow(
	ID3D12Resource*       res,
	D3D12_RESOURCE_STATES before,
	D3D12_RESOURCE_STATES after)
{
	DX12DeviceImpl::transition(m_cmdList.Get(), res, before, after);
}

void DX12CommandBufferImpl::requireState(ID3D12Resource* res, D3D12_RESOURCE_STATES state)
{
	// Bundle はバリアを記録できない (親 Direct 側で状態を合わせること)
	if (!res || !isDirect())
		return;

	for (auto& ls : m_localStates)
	{
		if (ls.res != res)
			continue;
		if (ls.current != state)
		{
			transitionNow(res, ls.current, state);
			ls.current = state;
		}
		return;
	}

	// この CB で初めて使うリソース: バリアは submit 時に先行実行する
	m_localStates.push_back({ res, state, state });
}

// 描画ターゲットをセットしクリア・ビューポートを設定する
void DX12CommandBufferImpl::setRenderTargets(
	uint32_t              rtCount,
	IRenderTarget* const* rts,
	IDepthStencil*        ds,
	bool                  clearRT,
	bool                  clearDS)
{
	rtCount = std::min<uint32_t>(rtCount, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT);

	D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
	DX12RenderTargetImpl*       rtImpls   [D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};

	for (uint32_t i = 0; i < rtCount; ++i)
	{
		rtImpls[i]    = static_cast<DX12RenderTargetImpl*>(rts[i]->self);
		rtvHandles[i] = rtImpls[i]->rtvHandle();
		requireState(rtImpls[i]->resource(), D3D12_RESOURCE_STATE_RENDER_TARGET);
	}

	D3D12_CPU_DESCRIPTOR_HANDLE        dsvH   = {};
	const D3D12_CPU_DESCRIPTOR_HANDLE* pDSV   = nullptr;
	DX12DepthStencilImpl*              dsImpl = nullptr;

	if (ds)
	{
		dsImpl = static_cast<DX12DepthStencilImpl*>(ds->self);
		requireState(dsImpl->resource(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
		dsvH = dsImpl->dsvHandle();
		pDSV = &dsvH;
	}

	m_cmdList->OMSetRenderTargets(
		rtCount,
		rtCount > 0 ? rtvHandles : nullptr,
		FALSE,
		pDSV);

	// クリア: 作成時に指定したクリア値 (最適化クリア値と一致) を使う
	if (clearRT)
	{
		for (uint32_t i = 0; i < rtCount; ++i)
			m_cmdList->ClearRenderTargetView(rtvHandles[i], rtImpls[i]->clearColor(), 0, nullptr);
	}

	if (clearDS && dsImpl)
	{
		D3D12_CLEAR_FLAGS flags = D3D12_CLEAR_FLAG_DEPTH;
		if (hasStencil(dsImpl->getFormat()))
			flags |= D3D12_CLEAR_FLAG_STENCIL;
		m_cmdList->ClearDepthStencilView(dsvH, flags, dsImpl->clearDepth(), dsImpl->clearStencil(), 0, nullptr);
	}

	// ビューポートとシザーを RT または DS サイズに合わせる
	uint32_t vw = 0, vh = 0;
	if (rtCount > 0)   { vw = rtImpls[0]->getWidth(); vh = rtImpls[0]->getHeight(); }
	else if (dsImpl)   { vw = dsImpl->getWidth();     vh = dsImpl->getHeight();     }

	if (vw > 0 && vh > 0)
	{
		D3D12_VIEWPORT vp = { 0, 0, static_cast<float>(vw), static_cast<float>(vh), 0, 1 };
		D3D12_RECT     sc = { 0, 0, static_cast<LONG>(vw), static_cast<LONG>(vh) };
		m_cmdList->RSSetViewports(1, &vp);
		m_cmdList->RSSetScissorRects(1, &sc);
	}

	// RT / DS フォーマットを全枚記録して bindShader で PSO を確定する際に使う
	m_currentRTVCount = rtCount;
	for (uint32_t i = 0; i < rtCount; ++i)
		m_currentRTVFmts[i] = toDXGI(rtImpls[i]->getFormat());

	m_currentDSVFmt = dsImpl ? toDXGI(dsImpl->getFormat()) : DXGI_FORMAT_UNKNOWN;

	// フォールバック PSO (1枚目のフォーマット基準) をセットしておく
	const DXGI_FORMAT fmt0 = rtCount > 0 ? m_currentRTVFmts[0] : DXGI_FORMAT_UNKNOWN;
	if (auto* pso = m_dev->defaultPSOForFormat(fmt0))
		m_cmdList->SetPipelineState(pso);
}

// パイプラインをバインドする
// IPipeline が保持する PSO を SetPipelineState で即セットする
// setRenderTargets の後に呼ぶこと。
void DX12CommandBufferImpl::bindPipeline(IPipeline* p)
{
	if (!p || !p->self)
		return;

	auto* impl = static_cast<DX12PipelineImpl*>(p->self);
	if (!impl->valid())
		return;

	// パイプラインごとのルートシグネチャをセットしてから PSO をセットする
	if (impl->isCompute())
		m_cmdList->SetComputeRootSignature(impl->rootSig());
	else
		m_cmdList->SetGraphicsRootSignature(impl->rootSig());
	m_cmdList->SetPipelineState(impl->pso());

	m_currentPipeline   = impl;
	m_computeBound      = impl->isCompute();
	m_explicitTableMask = 0;   // ルートシグネチャを設定し直したのでテーブルは未設定
	if (!impl->isCompute())
		m_topology = impl->topology();  // パイプラインの topology を初期値として設定

	// bindShader 系の状態をリセットして競合を防ぐ
	m_boundVS = 0;
	m_boundPS = 0;
}

void DX12CommandBufferImpl::bindShader(ShaderHandle h)
{
	if (!m_dev || !h.valid())
		return;

	const DX12ShadRes* sh = m_dev->getShadRes(h.id);
	if (!sh)
		return;

	m_cmdList->SetGraphicsRootSignature(m_dev->defaultRootSig());

	// ステージ別に ID を蓄積する
	if (sh->stage == ShaderStage::Vertex)
		m_boundVS = h.id;
	else if (sh->stage == ShaderStage::Fragment)
		m_boundPS = h.id;

	// VS と PS の両方が揃ったら PSO を作成・セットする
	if (m_boundVS != 0 && m_boundPS != 0)
	{
		ID3D12PipelineState* pso =
			m_dev->getOrCreatePSO(
				m_boundVS, m_boundPS,
				m_currentRTVFmts, m_currentRTVCount,
				m_currentDSVFmt);

		if (pso)
			m_cmdList->SetPipelineState(pso);
	}
}

// テクスチャを SRV としてバインドする
// リソースをシェーダー読み取り状態にする (必要なバリアは状態追跡が発行する)
void DX12CommandBufferImpl::bindTexture(
	TextureHandle h,
	uint32_t      slot)
{
	if (!m_dev || !h.valid())
		return;

	if (!isDirect())
	{
		fprintf(stderr, "[GfxEngine] bindTexture: Bundle 内ではテクスチャをバインドできません (無視しました)\n");
		return;
	}

	const uint32_t srvSlot = m_dev->getTexSRVSlot(h.id);
	if (srvSlot == UINT32_MAX)
		return;

	requireState(m_dev->getTexResource(h.id), k_ShaderReadState);

	// slot 番地に SRV スロット番号を記録する
	// draw/drawFullscreen 直前に flushSRVBindings() で連続コピーして SetDescriptorTable を呼ぶ
	if (slot < MAX_SRV_BINDINGS)
	{
		m_srvBindings[slot] = srvSlot;
		if (slot + 1 > m_srvBindingCount)
			m_srvBindingCount = slot + 1;
	}
	if (m_currentPipeline && m_currentPipeline->srvTableSlot() < 32)
		m_explicitTableMask &= ~(1u << m_currentPipeline->srvTableSlot());
}

// バインドされた SRV を一時リングの連続領域にコピーして SetGraphicsRootDescriptorTable を呼ぶ
void DX12CommandBufferImpl::flushSRVBindings()
{
	if (!m_dev || !isDirect())
		return;

	if (!m_currentPipeline)
	{
		assert(false && "bindPipeline must be called before draw");
		return;
	}

	const uint32_t tableSlot = m_currentPipeline->srvTableSlot();
	if (tableSlot == UINT32_MAX || (tableSlot < 32 && (m_explicitTableMask & (1u << tableSlot))))
		return;

	// ルートシグネチャのテーブルサイズ分は必ず埋める (未バインドは null SRV)
	const uint32_t count = std::max(m_srvBindingCount, m_currentPipeline->srvTableCount());
	if (count == 0)
		return;

	uint32_t tempSlot = 0;
	uint64_t ringPos  = 0;
	if (!m_dev->allocTempSRV(count, tempSlot, ringPos))
		return;
	if (m_firstSrvRingPos == UINT64_MAX)
		m_firstSrvRingPos = ringPos;

	const uint32_t descSize = m_dev->srvDescSize();
	D3D12_CPU_DESCRIPTOR_HANDLE dstBase = m_dev->gpuHeapCpuSRV(tempSlot);

	for (uint32_t i = 0; i < count; ++i)
	{
		const uint32_t srcSlot  = (i < m_srvBindingCount && i < MAX_SRV_BINDINGS) ? m_srvBindings[i] : 0;
		// 未設定(0)または範囲外は null SRV (slot 0) を使う
		const uint32_t safeSlot = (srcSlot < DX12DeviceImpl::SRV_STATIC_CAPACITY) ? srcSlot : 0;

		D3D12_CPU_DESCRIPTOR_HANDLE dst = dstBase;
		dst.ptr += static_cast<SIZE_T>(i) * descSize;

		m_dev->device()->CopyDescriptorsSimple(
			1, dst, m_dev->cpuStaticSRV(safeSlot), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	}

	setRootTable(tableSlot, m_dev->gpuSRV(tempSlot));
}

// UAV テクスチャをコンピュート/レイトレーシングシェーダーにバインドする
// slot はルートパラメータのインデックス
void DX12CommandBufferImpl::bindUAV(TextureHandle h, uint32_t slot)
{
	if (!m_dev || !h.valid()) return;

	const auto* tex = m_dev->getTexRes(h.id);
	if (!tex || tex->uavSlot == UINT32_MAX) return;

	requireState(tex->res.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	// UAV は静的領域にあるためシェーダー可視ヒープの同じ番号を直接参照できる
	m_cmdList->SetComputeRootDescriptorTable(slot, m_dev->gpuSRV(tex->uavSlot));
}

// レイトレーシングを実行する
void DX12CommandBufferImpl::dispatchRays(
	RTPipelineHandle pipe, TLASHandle tlas, TextureHandle output,
	TextureHandle depthTex, BufferHandle rtCB,
	uint32_t w, uint32_t h)
{
	if (!m_dev || !isDirect()) return;

	auto* rtPipe = m_dev->getRTPipelineRes(pipe.id);
	if (!rtPipe || !rtPipe->pso) return;

	const auto* tex = m_dev->getTexRes(output.id);
	auto* tlasRes = m_dev->getTLASRes(tlas.id);
	if (!tex || !tlasRes) return;

	// 状態: 出力は UAV、深度はシェーダー読み取り
	requireState(tex->res.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	if (depthTex.valid())
		requireState(m_dev->getTexResource(depthTex.id), k_ShaderReadState);

	// RT パイプラインをセット
	m_cmdList->SetComputeRootSignature(rtPipe->globalRS.Get());
	m_cmdList->SetPipelineState1(rtPipe->pso.Get());

	// param 0: UAV テーブル (u0) = RT 出力 (静的領域を直接参照)
	if (tex->uavSlot != UINT32_MAX)
		m_cmdList->SetComputeRootDescriptorTable(0, m_dev->gpuSRV(tex->uavSlot));

	// param 1: SRV (t0) = TLAS
	m_cmdList->SetComputeRootShaderResourceView(
		1, tlasRes->result->GetGPUVirtualAddress());

	// param 2: SRV テーブル (t1) = 深度テクスチャ
	// 一時リングにコピーしてバインドする
	if (depthTex.valid())
	{
		const uint32_t srcSlot = m_dev->getTexSRVSlot(depthTex.id);
		uint32_t dstSlot = 0;
		uint64_t ringPos = 0;
		if (srcSlot != UINT32_MAX && m_dev->allocTempSRV(1, dstSlot, ringPos))
		{
			if (m_firstSrvRingPos == UINT64_MAX)
				m_firstSrvRingPos = ringPos;
			m_dev->device()->CopyDescriptorsSimple(1, m_dev->gpuHeapCpuSRV(dstSlot),
				m_dev->cpuStaticSRV(srcSlot), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			m_cmdList->SetComputeRootDescriptorTable(2, m_dev->gpuSRV(dstSlot));
		}
	}

	// param 3: CBV (b0) = RTShadowCB
	if (rtCB.valid())
	{
		const auto* cb = m_dev->getBuffer(rtCB.id);
		if (cb && cb->res)
			m_cmdList->SetComputeRootConstantBufferView(3, cb->gpuAddress());
	}

	// DispatchRays
	const uint64_t stride = rtPipe->tableStride;
	D3D12_GPU_VIRTUAL_ADDRESS tableBase = rtPipe->shaderTable->GetGPUVirtualAddress();

	D3D12_DISPATCH_RAYS_DESC dr = {};
	dr.RayGenerationShaderRecord.StartAddress = tableBase;
	dr.RayGenerationShaderRecord.SizeInBytes  = stride;
	dr.MissShaderTable.StartAddress           = tableBase + stride;
	dr.MissShaderTable.SizeInBytes            = stride;
	dr.MissShaderTable.StrideInBytes          = stride;
	dr.HitGroupTable.StartAddress             = tableBase + stride * 2;
	dr.HitGroupTable.SizeInBytes              = stride;
	dr.HitGroupTable.StrideInBytes            = stride;
	dr.Width  = w;
	dr.Height = h;
	dr.Depth  = 1;

	m_cmdList->DispatchRays(&dr);

	// 同じ出力に続けて UAV 書き込みする場合に備えて UAV バリアを入れる
	// (SRV として読む場合の遷移は状態追跡が行う)
	D3D12_RESOURCE_BARRIER uavBarrier = {};
	uavBarrier.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	uavBarrier.UAV.pResource = tex->res.Get();
	m_cmdList->ResourceBarrier(1, &uavBarrier);
}

// UAV 状態に遷移する (Compute / DispatchRays で書き込む前に呼ぶ)
void DX12CommandBufferImpl::transitionToUAV(TextureHandle h)
{
	if (!m_dev || !h.valid()) return;
	requireState(m_dev->getTexResource(h.id), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

// バインドされたサンプラーを一時リングの連続領域にコピーして SetGraphicsRootDescriptorTable を呼ぶ
void DX12CommandBufferImpl::flushSamplerBindings()
{
	if (!m_dev || !isDirect() || !m_currentPipeline)
		return;

	const uint32_t tableSlot = m_currentPipeline->samplerTableSlot();
	if (tableSlot == UINT32_MAX || (tableSlot < 32 && (m_explicitTableMask & (1u << tableSlot))))
		return;

	// ルートシグネチャのテーブルサイズ分は必ず埋める (未バインドは既定サンプラー = 静的 slot 0)
	const uint32_t count = std::max(m_samplerBindingCount, m_currentPipeline->samplerTableCount());
	if (count == 0)
		return;

	uint32_t tempSlot = 0;
	uint64_t ringPos  = 0;
	if (!m_dev->allocTempSampler(count, tempSlot, ringPos))
		return;
	if (m_firstSamplerRingPos == UINT64_MAX)
		m_firstSamplerRingPos = ringPos;

	const uint32_t descSize = m_dev->samplerDescSize();
	D3D12_CPU_DESCRIPTOR_HANDLE dstBase = m_dev->gpuHeapCpuSampler(tempSlot);

	for (uint32_t i = 0; i < count; ++i)
	{
		const uint32_t srcSlot = (i < m_samplerBindingCount && i < MAX_SRV_BINDINGS) ? m_samplerBindings[i] : 0;

		D3D12_CPU_DESCRIPTOR_HANDLE dst = dstBase;
		dst.ptr += static_cast<SIZE_T>(i) * descSize;

		m_dev->device()->CopyDescriptorsSimple(
			1, dst, m_dev->cpuStaticSampler(srcSlot), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
	}

	setRootTable(tableSlot, m_dev->gpuSampler(tempSlot));
}

// サンプラーをバインドする
void DX12CommandBufferImpl::bindSampler(
	SamplerHandle h,
	uint32_t      slot)
{
	if (!m_dev || !h.valid())
		return;

	if (!isDirect())
	{
		fprintf(stderr, "[GfxEngine] bindSampler: Bundle 内ではサンプラーをバインドできません (無視しました)\n");
		return;
	}

	const auto* sam = m_dev->getSamplerRes(h.id);
	if (!sam)
		return;

	// SRV と同様にスロットを記録して draw 直前に一括セットする
	if (slot < MAX_SRV_BINDINGS)
	{
		m_samplerBindings[slot] = sam->slot;
		if (slot + 1 > m_samplerBindingCount)
			m_samplerBindingCount = slot + 1;
	}
	if (m_currentPipeline && m_currentPipeline->samplerTableSlot() < 32)
		m_explicitTableMask &= ~(1u << m_currentPipeline->samplerTableSlot());
}

// 定数バッファをバインドする
// slot はレジスタ番号 (b0=0, b1=1, ...)。パイプラインのルートシグネチャから CBV のルートスロットを引く
void DX12CommandBufferImpl::bindUniformBuffer(
	BufferHandle h,
	uint32_t     slot)
{
	if (!m_dev || !h.valid())
		return;

	const auto* buf = m_dev->getBuffer(h.id);
	if (!buf || !buf->res)
		return;

	if (!m_currentPipeline)
	{
		assert(false && "bindPipeline must be called before bindUniformBuffer");
		return;
	}

	const uint32_t cbvSlot = m_currentPipeline->cbvSlot(slot);
	if (cbvSlot == UINT32_MAX)
	{
		assert(false && "bindUniformBuffer: no CBV entry for the given register in rootSignature");
		return;
	}

	// Uniform バッファは現在のバージョン (直近の updateBuffer で書き込んだ領域) を参照する
	if (m_computeBound)
		m_cmdList->SetComputeRootConstantBufferView(cbvSlot, buf->gpuAddress());
	else
		m_cmdList->SetGraphicsRootConstantBufferView(cbvSlot, buf->gpuAddress());
}

// インデックス付き描画コマンドを記録する
void DX12CommandBufferImpl::draw(const GfxDrawCmd* cmd)
{
	flushSRVBindings();
	flushSamplerBindings();
	// setTopology で上書きされていれば優先、なければ pipeline の topology を使う
	m_dev->drawOnCmdList(m_cmdList.Get(), cmd, m_topology);
}

// フルスクリーントライアングルを描画する
void DX12CommandBufferImpl::drawFullscreen()
{
	flushSRVBindings();
	flushSamplerBindings();
	m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	m_cmdList->DrawInstanced(3, 1, 0, 0);
}

// ============================================================
//  DX12CommandBufferImpl — ABI v2 のコマンド (Phase 2b)
// ============================================================

static_assert(sizeof(GfxRect)     == sizeof(D3D12_RECT),     "GfxRect と D3D12_RECT の配置が一致しない");
static_assert(sizeof(GfxViewport) == sizeof(D3D12_VIEWPORT), "GfxViewport と D3D12_VIEWPORT の配置が一致しない");
static_assert(sizeof(GfxDrawArguments)        == sizeof(D3D12_DRAW_ARGUMENTS),         "draw args");
static_assert(sizeof(GfxDrawIndexedArguments) == sizeof(D3D12_DRAW_INDEXED_ARGUMENTS), "draw indexed args");
static_assert(sizeof(GfxDispatchArguments)    == sizeof(D3D12_DISPATCH_ARGUMENTS),     "dispatch args");

// Bundle で使えないコマンドは警告して無視する
bool DX12CommandBufferImpl::requireDirect(const char* fn)
{
	if (isDirect())
		return true;
	fprintf(stderr, "[GfxEngine] %s: Bundle では使えません (無視しました)\n", fn);
	return false;
}

// バッファの状態を CB の中で合わせる。UPLOAD ヒープ (GENERIC_READ 固定) は対象外。
void DX12CommandBufferImpl::requireBufferState(const DX12BufRes* buf, D3D12_RESOURCE_STATES state)
{
	if (!buf || !buf->res || buf->upload || !isDirect())
		return;
	ID3D12Resource* res = buf->res.Get();
	for (auto& bs : m_bufferStates)
	{
		if (bs.res != res)
			continue;
		if (bs.current != state)
		{
			transitionNow(res, bs.current, state);
			bs.current = state;
		}
		return;
	}
	// CB の開始時点のバッファは COMMON (前の CB の end() で戻している)
	if (state != D3D12_RESOURCE_STATE_COMMON)
		transitionNow(res, D3D12_RESOURCE_STATE_COMMON, state);
	m_bufferStates.push_back({ res, state });
}

// 明示的なバリアの後に、CB ローカルの追跡状態を上書きする
void DX12CommandBufferImpl::overrideLocalState(ID3D12Resource* res, D3D12_RESOURCE_STATES state)
{
	for (auto& ls : m_localStates)
		if (ls.res == res)
		{
			ls.current = state;
			return;
		}
}

ID3D12Resource* DX12CommandBufferImpl::resolveTextureRef(const GfxResourceRef& r) const
{
	switch (r.kind)
	{
	case GfxResourceKind::Texture:
		return m_dev->getTexResource(r.handle);
	case GfxResourceKind::RenderTarget:
	{
		auto* rt = reinterpret_cast<IRenderTarget*>(r.handle);
		return (rt && rt->self) ? static_cast<DX12RenderTargetImpl*>(rt->self)->resource() : nullptr;
	}
	case GfxResourceKind::DepthStencil:
	{
		auto* ds = reinterpret_cast<IDepthStencil*>(r.handle);
		return (ds && ds->self) ? static_cast<DX12DepthStencilImpl*>(ds->self)->resource() : nullptr;
	}
	default:
		return nullptr;
	}
}

void DX12CommandBufferImpl::setRootTable(uint32_t rootIndex, D3D12_GPU_DESCRIPTOR_HANDLE h)
{
	if (m_computeBound)
		m_cmdList->SetComputeRootDescriptorTable(rootIndex, h);
	else
		m_cmdList->SetGraphicsRootDescriptorTable(rootIndex, h);
}

void DX12CommandBufferImpl::flushBindings()
{
	flushSRVBindings();
	flushSamplerBindings();
}

void DX12CommandBufferImpl::setRootConstants(uint32_t rootIndex, uint32_t num32, const void* data, uint32_t destOffset)
{
	if (!data || num32 == 0)
		return;
	if (m_computeBound)
		m_cmdList->SetComputeRoot32BitConstants(rootIndex, num32, data, destOffset);
	else
		m_cmdList->SetGraphicsRoot32BitConstants(rootIndex, num32, data, destOffset);
}

void DX12CommandBufferImpl::setRootDescriptor(uint32_t rootIndex, BufferHandle buffer, uint64_t offset)
{
	if (!m_currentPipeline)
	{
		fprintf(stderr, "[GfxEngine] setRootDescriptor: bindPipeline の前には呼べません\n");
		return;
	}
	const DX12BindingSlots& slots = m_currentPipeline->slots();
	if (rootIndex >= slots.numParameters)
	{
		fprintf(stderr, "[GfxEngine] setRootDescriptor: rootIndex %u はレイアウトの範囲外です\n", rootIndex);
		return;
	}
	const DX12BufRes* buf = m_dev->getBuffer(buffer.id);
	if (!buf || !buf->res)
		return;

	const D3D12_GPU_VIRTUAL_ADDRESS va = buf->gpuAddress() + offset;
	switch (slots.paramTypes[rootIndex])
	{
	case GfxRootParameterType::CBV:
		requireBufferState(buf, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
		if (m_computeBound) m_cmdList->SetComputeRootConstantBufferView(rootIndex, va);
		else                m_cmdList->SetGraphicsRootConstantBufferView(rootIndex, va);
		break;
	case GfxRootParameterType::SRV:
		requireBufferState(buf, m_computeBound ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : k_ShaderReadState);
		if (m_computeBound) m_cmdList->SetComputeRootShaderResourceView(rootIndex, va);
		else                m_cmdList->SetGraphicsRootShaderResourceView(rootIndex, va);
		break;
	case GfxRootParameterType::UAV:
		if (buf->upload)
		{
			fprintf(stderr, "[GfxEngine] setRootDescriptor: UPLOAD ヒープのバッファは UAV にできません (BufferUsage::Storage で作成してください)\n");
			return;
		}
		requireBufferState(buf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		if (m_computeBound) m_cmdList->SetComputeRootUnorderedAccessView(rootIndex, va);
		else                m_cmdList->SetGraphicsRootUnorderedAccessView(rootIndex, va);
		break;
	default:
		fprintf(stderr, "[GfxEngine] setRootDescriptor: rootIndex %u はルート CBV / SRV / UAV ではありません\n", rootIndex);
		break;
	}
}

void DX12CommandBufferImpl::setDescriptorTable(uint32_t rootIndex, const GfxDescriptor* descs, uint32_t count)
{
	if (!requireDirect("setDescriptorTable") || !descs || count == 0)
		return;
	if (!m_currentPipeline)
	{
		fprintf(stderr, "[GfxEngine] setDescriptorTable: bindPipeline の前には呼べません\n");
		return;
	}
	const DX12BindingSlots& slots = m_currentPipeline->slots();
	if (rootIndex >= slots.numParameters || slots.paramTypes[rootIndex] != GfxRootParameterType::DescriptorTable)
	{
		fprintf(stderr, "[GfxEngine] setDescriptorTable: rootIndex %u はディスクリプタテーブルではありません\n", rootIndex);
		return;
	}

	const bool samplerTable = descs[0].type == GfxDescriptorType::Sampler;
	for (uint32_t i = 0; i < count; ++i)
	{
		if ((descs[i].type == GfxDescriptorType::Sampler) != samplerTable)
		{
			fprintf(stderr, "[GfxEngine] setDescriptorTable: Sampler とそれ以外は同じテーブルに入れられません\n");
			return;
		}
	}

	uint32_t base = 0;
	uint64_t pos  = 0;
	if (samplerTable ? !m_dev->allocTempSampler(count, base, pos) : !m_dev->allocTempSRV(count, base, pos))
		return;
	uint64_t& firstPos = samplerTable ? m_firstSamplerRingPos : m_firstSrvRingPos;
	if (firstPos == UINT64_MAX)
		firstPos = pos;

	ID3D12Device* device = m_dev->device();
	for (uint32_t i = 0; i < count; ++i)
	{
		const GfxDescriptor& d = descs[i];
		if (samplerTable)
		{
			const DX12SamplerRes* sam = m_dev->getSamplerRes(d.handle);
			device->CopyDescriptorsSimple(1, m_dev->gpuHeapCpuSampler(base + i),
				m_dev->cpuStaticSampler(sam ? sam->slot : 0), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
			continue;
		}

		const D3D12_CPU_DESCRIPTOR_HANDLE dst = m_dev->gpuHeapCpuSRV(base + i);
		switch (d.type)
		{
		case GfxDescriptorType::TextureSRV:
		{
			const uint32_t slot = m_dev->getTexSRVSlot(d.handle);
			if (slot != UINT32_MAX)
				requireState(m_dev->getTexResource(d.handle),
					m_computeBound ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : k_ShaderReadState);
			device->CopyDescriptorsSimple(1, dst, m_dev->cpuStaticSRV(slot != UINT32_MAX ? slot : 0),
				D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			break;
		}
		case GfxDescriptorType::TextureUAV:
		{
			const DX12TexRes* tex = m_dev->getTexRes(d.handle);
			if (tex && tex->uavSlot != UINT32_MAX)
			{
				requireState(tex->res.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
				device->CopyDescriptorsSimple(1, dst, m_dev->cpuStaticSRV(tex->uavSlot),
					D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
				break;
			}
			fprintf(stderr, "[GfxEngine] setDescriptorTable: [%u] は UAV を持たないテクスチャです (null UAV にしました)\n", i);
			[[fallthrough]];
		}
		case GfxDescriptorType::NullUAV:
		{
			D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {};
			uv.Format        = DXGI_FORMAT_R8G8B8A8_UNORM;
			uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			device->CreateUnorderedAccessView(nullptr, nullptr, &uv, dst);
			break;
		}
		case GfxDescriptorType::BufferCBV:
		{
			const DX12BufRes* buf = m_dev->getBuffer(d.handle);
			if (buf && buf->res && (buf->versionStride % 256) == 0)
			{
				requireBufferState(buf, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
				D3D12_CONSTANT_BUFFER_VIEW_DESC cv = { buf->gpuAddress(), static_cast<UINT>(buf->versionStride) };
				device->CreateConstantBufferView(&cv, dst);
				break;
			}
			fprintf(stderr, "[GfxEngine] setDescriptorTable: [%u] は CBV にできないバッファです (Uniform を使ってください)\n", i);
			[[fallthrough]];
		}
		case GfxDescriptorType::NullCBV:
			device->CreateConstantBufferView(nullptr, dst);
			break;
		case GfxDescriptorType::View:
		{
			const DX12ViewRes* v = m_dev->getView(d.handle);
			if (v && (v->type == GfxViewType::SRV || v->type == GfxViewType::UAV))
			{
				const bool isBuf = v->res->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER;
				if (isBuf)
				{
					const DX12BufRes* buf = nullptr;
					for (const auto& kv : m_dev->m_buffers)
						if (kv.second.res.Get() == v->res.Get()) { buf = &kv.second; break; }
					requireBufferState(buf, v->type == GfxViewType::UAV ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
						: (m_computeBound ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : k_ShaderReadState));
				}
				else
				{
					requireState(v->res.Get(), (v->type == GfxViewType::SRV && m_computeBound)
						? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : v->state);
				}
				device->CopyDescriptorsSimple(1, dst, m_dev->cpuStaticSRV(v->slot), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
				break;
			}
			fprintf(stderr, "[GfxEngine] setDescriptorTable: [%u] は SRV / UAV のビューではありません (null SRV にしました)\n", i);
			device->CopyDescriptorsSimple(1, dst, m_dev->cpuStaticSRV(0), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			break;
		}
		default:   // NullSRV
			device->CopyDescriptorsSimple(1, dst, m_dev->cpuStaticSRV(0), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			break;
		}
	}

	setRootTable(rootIndex, samplerTable ? m_dev->gpuSampler(base) : m_dev->gpuSRV(base));
	if (rootIndex < 32)
		m_explicitTableMask |= 1u << rootIndex;
}

void DX12CommandBufferImpl::setVertexBuffers(uint32_t startSlot, uint32_t count, const GfxVertexBufferBinding* bindings)
{
	if (!bindings || count == 0)
		return;
	D3D12_VERTEX_BUFFER_VIEW views[D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
	count = std::min<uint32_t>(count, D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT);
	for (uint32_t i = 0; i < count; ++i)
	{
		const DX12BufRes* buf = m_dev->getBuffer(bindings[i].buffer.id);
		if (!buf || !buf->res || bindings[i].offset >= buf->size)
			continue;   // 空のビュー (未バインド)
		requireBufferState(buf, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
		views[i].BufferLocation = buf->gpuAddress() + bindings[i].offset;
		views[i].SizeInBytes    = bindings[i].size ? bindings[i].size
		                                           : static_cast<UINT>(buf->size - bindings[i].offset);
		views[i].StrideInBytes  = bindings[i].stride ? bindings[i].stride : buf->stride;
	}
	m_cmdList->IASetVertexBuffers(startSlot, count, views);
}

void DX12CommandBufferImpl::setIndexBuffer(const GfxIndexBufferBinding* b)
{
	if (!b)
		return;
	const DX12BufRes* buf = m_dev->getBuffer(b->buffer.id);
	if (!buf || !buf->res || b->offset >= buf->size)
	{
		m_cmdList->IASetIndexBuffer(nullptr);
		return;
	}
	requireBufferState(buf, D3D12_RESOURCE_STATE_INDEX_BUFFER);
	D3D12_INDEX_BUFFER_VIEW v = {};
	v.BufferLocation = buf->gpuAddress() + b->offset;
	v.SizeInBytes    = b->size ? b->size : static_cast<UINT>(buf->size - b->offset);
	v.Format         = static_cast<DXGI_FORMAT>(b->format);
	m_cmdList->IASetIndexBuffer(&v);
}

void DX12CommandBufferImpl::drawInstanced(uint32_t vtx, uint32_t inst, uint32_t firstVtx, uint32_t firstInst)
{
	flushBindings();
	m_cmdList->IASetPrimitiveTopology(m_topology);
	m_cmdList->DrawInstanced(vtx, inst, firstVtx, firstInst);
}

void DX12CommandBufferImpl::drawIndexedInstanced(uint32_t idx, uint32_t inst, uint32_t firstIdx, int32_t baseVtx, uint32_t firstInst)
{
	flushBindings();
	m_cmdList->IASetPrimitiveTopology(m_topology);
	m_cmdList->DrawIndexedInstanced(idx, inst, firstIdx, baseVtx, firstInst);
}

void DX12CommandBufferImpl::executeIndirect(ID3D12CommandSignature* sig, const GfxIndirectDesc* desc, bool compute)
{
	if (!desc || !sig)
		return;
	const DX12BufRes* args = m_dev->getBuffer(desc->argumentBuffer.id);
	if (!args || !args->res)
	{
		fprintf(stderr, "[GfxEngine] executeIndirect: argumentBuffer が無効です\n");
		return;
	}
	const DX12BufRes* cnt = desc->countBuffer.valid() ? m_dev->getBuffer(desc->countBuffer.id) : nullptr;

	flushBindings();
	if (!compute)
		m_cmdList->IASetPrimitiveTopology(m_topology);

	requireBufferState(args, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
	if (cnt)
		requireBufferState(cnt, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);

	m_cmdList->ExecuteIndirect(sig, desc->maxCommandCount,
		args->res.Get(), args->versionStride * args->current + desc->argumentOffset,
		cnt ? cnt->res.Get() : nullptr, cnt ? cnt->versionStride * cnt->current + desc->countOffset : 0);
}

void DX12CommandBufferImpl::dispatch(uint32_t x, uint32_t y, uint32_t z)
{
	if (!m_computeBound)
	{
		fprintf(stderr, "[GfxEngine] dispatch: Compute パイプラインがバインドされていません\n");
		return;
	}
	flushBindings();
	m_cmdList->Dispatch(x, y, z);
}

void DX12CommandBufferImpl::barrier(const GfxBarrierDesc* barriers, uint32_t count)
{
	if (!requireDirect("barrier") || !barriers)
		return;

	for (uint32_t i = 0; i < count; ++i)
	{
		const GfxBarrierDesc& b = barriers[i];

		if (b.type == GfxBarrierType::UAV)
		{
			ID3D12Resource* res = nullptr;
			if (b.resource.kind == GfxResourceKind::Buffer)
			{
				const DX12BufRes* buf = m_dev->getBuffer(b.resource.handle);
				res = buf ? buf->res.Get() : nullptr;
			}
			else if (b.resource.kind != GfxResourceKind::None)
			{
				res = resolveTextureRef(b.resource);
			}
			D3D12_RESOURCE_BARRIER rb = {};
			rb.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
			rb.UAV.pResource = res;
			m_cmdList->ResourceBarrier(1, &rb);
			continue;
		}

		const bool tracked = b.before == GfxResourceState::Tracked;
		const auto before  = static_cast<D3D12_RESOURCE_STATES>(b.before);
		const auto after   = static_cast<D3D12_RESOURCE_STATES>(b.after);

		if (b.resource.kind == GfxResourceKind::Buffer)
		{
			const DX12BufRes* buf = m_dev->getBuffer(b.resource.handle);
			if (!buf || !buf->res)
				continue;
			if (buf->upload)
			{
				fprintf(stderr, "[GfxEngine] barrier: UPLOAD ヒープのバッファは GENERIC_READ から遷移できません (無視しました)\n");
				continue;
			}
			if (!tracked)
			{
				requireBufferState(buf, before);
				for (auto& bs : m_bufferStates)
					if (bs.res == buf->res.Get())
						bs.current = after;
				if (before != after)
					transitionNow(buf->res.Get(), before, after);
			}
			else
			{
				requireBufferState(buf, after);
			}
			continue;
		}

		ID3D12Resource* res = resolveTextureRef(b.resource);
		if (!res)
			continue;

		if (b.subresource == GFX_ALL_SUBRESOURCES)
		{
			if (tracked)
			{
				requireState(res, after);
			}
			else
			{
				requireState(res, before);
				if (before != after)
					transitionNow(res, before, after);
				overrideLocalState(res, after);
			}
			continue;
		}

		// サブリソース単位: 追跡はリソース全体の状態なので更新しない
		if (tracked)
		{
			fprintf(stderr, "[GfxEngine] barrier: サブリソースを指定したバリアは before を明示してください (無視しました)\n");
			continue;
		}
		requireState(res, before);
		D3D12_RESOURCE_BARRIER rb = {};
		rb.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		rb.Transition.pResource   = res;
		rb.Transition.StateBefore = before;
		rb.Transition.StateAfter  = after;
		rb.Transition.Subresource = b.subresource;
		m_cmdList->ResourceBarrier(1, &rb);
	}
}

void DX12CommandBufferImpl::setViewports(uint32_t count, const GfxViewport* viewports)
{
	if (!requireDirect("setViewports") || !viewports || count == 0)
		return;
	m_cmdList->RSSetViewports(std::min<uint32_t>(count, D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE),
		reinterpret_cast<const D3D12_VIEWPORT*>(viewports));
}

void DX12CommandBufferImpl::setScissors(uint32_t count, const GfxRect* rects)
{
	if (!requireDirect("setScissors") || !rects || count == 0)
		return;
	m_cmdList->RSSetScissorRects(std::min<uint32_t>(count, D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE),
		reinterpret_cast<const D3D12_RECT*>(rects));
}

void DX12CommandBufferImpl::setStencilRef(uint32_t ref)       { m_cmdList->OMSetStencilRef(ref); }
void DX12CommandBufferImpl::setBlendFactor(const float* rgba) { if (rgba) m_cmdList->OMSetBlendFactor(rgba); }

void DX12CommandBufferImpl::copyBuffer(BufferHandle dst, uint64_t dstOffset, BufferHandle src, uint64_t srcOffset, uint64_t size)
{
	if (!requireDirect("copyBuffer"))
		return;
	const DX12BufRes* d = m_dev->getBuffer(dst.id);
	const DX12BufRes* s = m_dev->getBuffer(src.id);
	if (!d || !s || !d->res || !s->res)
		return;
	if (d->upload)
	{
		fprintf(stderr, "[GfxEngine] copyBuffer: UPLOAD ヒープのバッファにはコピーできません\n");
		return;
	}
	requireBufferState(d, D3D12_RESOURCE_STATE_COPY_DEST);
	requireBufferState(s, D3D12_RESOURCE_STATE_COPY_SOURCE);
	m_cmdList->CopyBufferRegion(d->res.Get(), d->versionStride * d->current + dstOffset,
		s->res.Get(), s->versionStride * s->current + srcOffset, size);
}

void DX12CommandBufferImpl::copyTexture(
	GfxResourceRef dst, uint32_t dstSub, uint32_t dstX, uint32_t dstY, uint32_t dstZ,
	GfxResourceRef src, uint32_t srcSub, const GfxBox* srcBox)
{
	if (!requireDirect("copyTexture"))
		return;
	ID3D12Resource* d = resolveTextureRef(dst);
	ID3D12Resource* s = resolveTextureRef(src);
	if (!d || !s)
		return;
	requireState(d, D3D12_RESOURCE_STATE_COPY_DEST);
	requireState(s, D3D12_RESOURCE_STATE_COPY_SOURCE);

	D3D12_TEXTURE_COPY_LOCATION dl = {};
	dl.pResource        = d;
	dl.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	dl.SubresourceIndex = dstSub;
	D3D12_TEXTURE_COPY_LOCATION sl = {};
	sl.pResource        = s;
	sl.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	sl.SubresourceIndex = srcSub;

	D3D12_BOX box = {};
	if (srcBox)
		box = { srcBox->left, srcBox->top, srcBox->front, srcBox->right, srcBox->bottom, srcBox->back };
	m_cmdList->CopyTextureRegion(&dl, dstX, dstY, dstZ, &sl, srcBox ? &box : nullptr);
}

void DX12CommandBufferImpl::clearRenderTarget(IRenderTarget* rt, const float* rgba, uint32_t rectCount, const GfxRect* rects)
{
	if (!requireDirect("clearRenderTarget") || !rt || !rt->self || !rgba)
		return;
	auto* impl = static_cast<DX12RenderTargetImpl*>(rt->self);
	requireState(impl->resource(), D3D12_RESOURCE_STATE_RENDER_TARGET);
	m_cmdList->ClearRenderTargetView(impl->rtvHandle(), rgba,
		rects ? rectCount : 0, rects ? reinterpret_cast<const D3D12_RECT*>(rects) : nullptr);
}

void DX12CommandBufferImpl::clearDepthStencil(IDepthStencil* ds, GfxClearFlags flags, float depth, uint8_t stencil,
                                              uint32_t rectCount, const GfxRect* rects)
{
	if (!requireDirect("clearDepthStencil") || !ds || !ds->self)
		return;
	auto* impl = static_cast<DX12DepthStencilImpl*>(ds->self);
	D3D12_CLEAR_FLAGS f = static_cast<D3D12_CLEAR_FLAGS>(flags);
	if (!hasStencil(impl->getFormat()))
		f &= ~D3D12_CLEAR_FLAG_STENCIL;
	if (f == 0)
		return;
	requireState(impl->resource(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
	m_cmdList->ClearDepthStencilView(impl->dsvHandle(), f, depth, stencil,
		rects ? rectCount : 0, rects ? reinterpret_cast<const D3D12_RECT*>(rects) : nullptr);
}

// PIX のイベント (metadata 0 = Unicode 文字列)
void DX12CommandBufferImpl::beginEvent(const char* name)
{
	wchar_t wname[128] = {};
	MultiByteToWideChar(CP_UTF8, 0, name ? name : "", -1, wname, 127);
	m_cmdList->BeginEvent(0, wname, static_cast<UINT>((wcslen(wname) + 1) * sizeof(wchar_t)));
}

void DX12CommandBufferImpl::endEvent()
{
	m_cmdList->EndEvent();
}

void DX12CommandBufferImpl::writeTimestamp(QueryPoolHandle pool, uint32_t index)
{
	if (!requireDirect("writeTimestamp"))
		return;
	DX12QueryPoolRes* qp = m_dev->getQueryPool(pool.id);
	if (!qp || qp->type != GfxQueryType::Timestamp || index >= qp->count)
	{
		fprintf(stderr, "[GfxEngine] writeTimestamp: タイムスタンプのプールではないか、index が範囲外です\n");
		return;
	}
	m_cmdList->EndQuery(qp->heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index);
}

void DX12CommandBufferImpl::beginQuery(QueryPoolHandle pool, uint32_t index)
{
	if (!requireDirect("beginQuery"))
		return;
	DX12QueryPoolRes* qp = m_dev->getQueryPool(pool.id);
	if (!qp || qp->type == GfxQueryType::Timestamp || index >= qp->count)
	{
		fprintf(stderr, "[GfxEngine] beginQuery: オクルージョンのプールではないか、index が範囲外です\n");
		return;
	}
	m_cmdList->BeginQuery(qp->heap.Get(), qp->queryType(), index);
}

void DX12CommandBufferImpl::endQuery(QueryPoolHandle pool, uint32_t index)
{
	if (!requireDirect("endQuery"))
		return;
	DX12QueryPoolRes* qp = m_dev->getQueryPool(pool.id);
	if (!qp || qp->type == GfxQueryType::Timestamp || index >= qp->count)
	{
		fprintf(stderr, "[GfxEngine] endQuery: オクルージョンのプールではないか、index が範囲外です\n");
		return;
	}
	m_cmdList->EndQuery(qp->heap.Get(), qp->queryType(), index);
}

void DX12CommandBufferImpl::resolveQueries(QueryPoolHandle pool, uint32_t first, uint32_t count)
{
	if (!requireDirect("resolveQueries"))
		return;
	DX12QueryPoolRes* qp = m_dev->getQueryPool(pool.id);
	if (!qp || count == 0 || first + count > qp->count)
	{
		fprintf(stderr, "[GfxEngine] resolveQueries: プールが無効か、範囲外です\n");
		return;
	}
	m_cmdList->ResolveQueryData(qp->heap.Get(), qp->queryType(), first, count,
		qp->readback.Get(), static_cast<UINT64>(first) * sizeof(uint64_t));
	m_resolves.push_back({ pool.id, first, count });
}

// ============================================================
//  DX12DeviceImpl — Compute パイプライン / クエリ (Phase 2b)
// ============================================================

bool DX12DeviceImpl::createCommandSignatures()
{
	auto make = [&](D3D12_INDIRECT_ARGUMENT_TYPE type, UINT stride, ComPtr<ID3D12CommandSignature>& out) -> bool
	{
		D3D12_INDIRECT_ARGUMENT_DESC arg = {};
		arg.Type = type;
		D3D12_COMMAND_SIGNATURE_DESC sd = {};
		sd.ByteStride       = stride;
		sd.NumArgumentDescs = 1;
		sd.pArgumentDescs   = &arg;
		return SUCCEEDED(m_device->CreateCommandSignature(&sd, nullptr, IID_PPV_ARGS(&out)));
	};
	return make(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW,         sizeof(D3D12_DRAW_ARGUMENTS),         m_drawSig)
		&& make(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED, sizeof(D3D12_DRAW_INDEXED_ARGUMENTS), m_drawIndexedSig)
		&& make(D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH,     sizeof(D3D12_DISPATCH_ARGUMENTS),     m_dispatchSig);
}

bool DX12PipelineImpl::createCompute(
	DX12DeviceImpl*               dev,
	const GfxComputePipelineDesc& desc,
	const DX12BindingLayoutRes&   layout)
{
	m_dev       = dev;
	m_rootSig   = layout.rootSig;
	m_slots     = layout.slots;
	m_isCompute = true;

	const DX12ShadRes* cs = desc.cs.valid() ? dev->getShadRes(desc.cs.id) : nullptr;
	if (!cs || cs->bytecode.empty() || cs->stage != ShaderStage::Compute)
	{
		fprintf(stderr, "[GfxEngine] createComputePipeline: CS が無効です (%s)\n", desc.debugName ? desc.debugName : "unnamed");
		return false;
	}

	D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
	pd.pRootSignature = m_rootSig.Get();
	pd.CS             = { cs->bytecode.data(), cs->bytecode.size() };
	pd.NodeMask       = desc.nodeMask;
	pd.Flags          = static_cast<D3D12_PIPELINE_STATE_FLAGS>(desc.flags);
	if (FAILED(dev->device()->CreateComputePipelineState(&pd, IID_PPV_ARGS(&m_pso))))
	{
		fprintf(stderr, "[GfxEngine] createComputePipeline: CreateComputePipelineState に失敗しました (%s)\n",
			desc.debugName ? desc.debugName : "unnamed");
		return false;
	}
	setDebugName(m_pso.Get(), desc.debugName);
	return true;
}

PipelineHandle DX12DeviceImpl::createComputePipeline(const GfxComputePipelineDesc* src)
{
	GfxComputePipelineDesc d;
	if (!readVersionedDesc(src, d, "createComputePipeline"))
		return {};

	auto layoutIt = m_bindingLayouts.find(d.bindingLayout.id);
	if (layoutIt == m_bindingLayouts.end())
	{
		fprintf(stderr, "[GfxEngine] createComputePipeline: bindingLayout が無効です (%s)\n",
			d.debugName ? d.debugName : "unnamed");
		return {};
	}

	auto* impl = new DX12PipelineImpl();
	if (!impl->createCompute(this, d, layoutIt->second))
	{
		delete impl;
		return {};
	}
	return PipelineHandle{ reinterpret_cast<uint64_t>(impl->iface()) };
}

QueryPoolHandle DX12DeviceImpl::createQueryPool(const GfxQueryPoolDesc* src)
{
	GfxQueryPoolDesc d;
	if (!readVersionedDesc(src, d, "createQueryPool"))
		return {};
	if (d.count == 0)
		return {};

	DX12QueryPoolRes qp;
	qp.type  = d.type;
	qp.count = d.count;
	qp.resolveFence.assign(d.count, 0);

	D3D12_QUERY_HEAP_DESC hd = {};
	hd.Type  = (d.type == GfxQueryType::Timestamp) ? D3D12_QUERY_HEAP_TYPE_TIMESTAMP : D3D12_QUERY_HEAP_TYPE_OCCLUSION;
	hd.Count = d.count;
	if (FAILED(m_device->CreateQueryHeap(&hd, IID_PPV_ARGS(&qp.heap))))
	{
		fprintf(stderr, "[GfxEngine] createQueryPool: CreateQueryHeap に失敗しました\n");
		return {};
	}
	setDebugName(qp.heap.Get(), d.debugName);

	qp.readback = createBufferResource(m_device.Get(), static_cast<uint64_t>(d.count) * sizeof(uint64_t),
		D3D12_HEAP_TYPE_READBACK);
	if (!qp.readback)
		return {};
	void* p = nullptr;
	if (FAILED(qp.readback->Map(0, nullptr, &p)))
		return {};
	qp.mapped = static_cast<const uint64_t*>(p);

	const uint64_t id = m_nextId++;
	m_queryPools.emplace(id, std::move(qp));
	return QueryPoolHandle{ id };
}

void DX12DeviceImpl::destroyQueryPool(QueryPoolHandle h)
{
	auto it = m_queryPools.find(h.id);
	if (it == m_queryPools.end())
		return;
	deferRelease(std::move(it->second.heap));
	deferRelease(std::move(it->second.readback));
	m_queryPools.erase(it);
}

bool DX12DeviceImpl::readQueryResults(QueryPoolHandle h, uint32_t first, uint32_t count, uint64_t* out)
{
	DX12QueryPoolRes* qp = getQueryPool(h.id);
	if (!qp || !out || count == 0 || first + count > qp->count)
		return false;

	const uint64_t completed = completedFence();
	for (uint32_t i = first; i < first + count; ++i)
		if (qp->resolveFence[i] == 0 || qp->resolveFence[i] > completed)
			return false;

	std::memcpy(out, qp->mapped + first, static_cast<size_t>(count) * sizeof(uint64_t));
	return true;
}

uint64_t DX12DeviceImpl::getTimestampFrequency() const
{
	UINT64 f = 0;
	if (m_cmdQueue)
		m_cmdQueue->GetTimestampFrequency(&f);
	return f;
}

// ============================================================
//  DX12DeviceImpl — テクスチャ / ビュー / サンプラー の v2 (Phase 2b-2)
// ============================================================

// 深度フォーマットを、リソース (Typeless) / DSV / SRV のフォーマットに分ける
struct DX12DepthFormats { DXGI_FORMAT resource, dsv, srv; };
static DX12DepthFormats splitDepthFormat(DXGI_FORMAT f)
{
	switch (f)
	{
	case DXGI_FORMAT_D32_FLOAT:            return { DXGI_FORMAT_R32_TYPELESS,      DXGI_FORMAT_D32_FLOAT,            DXGI_FORMAT_R32_FLOAT };
	case DXGI_FORMAT_D24_UNORM_S8_UINT:    return { DXGI_FORMAT_R24G8_TYPELESS,    DXGI_FORMAT_D24_UNORM_S8_UINT,    DXGI_FORMAT_R24_UNORM_X8_TYPELESS };
	case DXGI_FORMAT_D16_UNORM:            return { DXGI_FORMAT_R16_TYPELESS,      DXGI_FORMAT_D16_UNORM,            DXGI_FORMAT_R16_UNORM };
	case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return { DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_D32_FLOAT_S8X24_UINT, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS };
	default:                               return { f, f, f };
	}
}

static bool isDepthDXGI(DXGI_FORMAT f)
{
	return f == DXGI_FORMAT_D32_FLOAT || f == DXGI_FORMAT_D24_UNORM_S8_UINT
		|| f == DXGI_FORMAT_D16_UNORM || f == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
}

// 初期データの転送に使う 1 ピクセルのバイト数 (ブロック圧縮などは未対応 = 0)
static uint32_t dxgiBytesPerPixel(DXGI_FORMAT f)
{
	switch (f)
	{
	case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32A32_UINT: case DXGI_FORMAT_R32G32B32A32_SINT:
		return 16;
	case DXGI_FORMAT_R32G32B32_FLOAT: case DXGI_FORMAT_R32G32B32_UINT: case DXGI_FORMAT_R32G32B32_SINT:
		return 12;
	case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM: case DXGI_FORMAT_R16G16B16A16_UINT:
	case DXGI_FORMAT_R16G16B16A16_SNORM: case DXGI_FORMAT_R16G16B16A16_SINT:
	case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R32G32_UINT: case DXGI_FORMAT_R32G32_SINT:
		return 8;
	case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8G8_UINT: case DXGI_FORMAT_R8G8_SNORM: case DXGI_FORMAT_R8G8_SINT:
	case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R16_UINT: case DXGI_FORMAT_R16_SNORM: case DXGI_FORMAT_R16_SINT:
		return 2;
	case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_UINT: case DXGI_FORMAT_R8_SNORM: case DXGI_FORMAT_R8_SINT:
		return 1;
	case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_UINT:
	case DXGI_FORMAT_R8G8B8A8_SNORM: case DXGI_FORMAT_R8G8B8A8_SINT: case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_UINT:
	case DXGI_FORMAT_R11G11B10_FLOAT: case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16G16_UNORM:
	case DXGI_FORMAT_R16G16_UINT: case DXGI_FORMAT_R16G16_SNORM: case DXGI_FORMAT_R16G16_SINT:
	case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R32_UINT: case DXGI_FORMAT_R32_SINT:
		return 4;
	default:
		return 0;
	}
}

bool DX12DeviceImpl::checkSampleCount(DXGI_FORMAT fmt, uint32_t count, uint32_t quality, const char* fn)
{
	if (count <= 1)
		return true;
	D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS q = {};
	q.Format      = fmt;
	q.SampleCount = count;
	if (FAILED(m_device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &q, sizeof(q)))
	 || q.NumQualityLevels == 0 || quality >= q.NumQualityLevels)
	{
		fprintf(stderr, "[GfxEngine] %s: フォーマット %d は sampleCount=%u / quality=%u の MSAA に対応していません\n",
			fn, static_cast<int>(fmt), count, quality);
		return false;
	}
	return true;
}

// mip 0 / サブリソース 0 に初期データを転送し、finalState に遷移させる (同期)
bool DX12DeviceImpl::uploadSubresource0(
	ID3D12Resource* res, DXGI_FORMAT fmt, uint32_t width, uint32_t height,
	const void* data, uint32_t rowPitch, D3D12_RESOURCE_STATES finalState)
{
	const uint32_t bpp = dxgiBytesPerPixel(fmt);
	if (bpp == 0)
	{
		fprintf(stderr, "[GfxEngine] createTexture: フォーマット %d の初期データの転送には未対応です\n", static_cast<int>(fmt));
		return false;
	}
	const uint32_t rowBytes    = width * bpp;
	const uint32_t srcRowPitch = rowPitch > 0 ? rowPitch : rowBytes;
	const UINT     dstRowPitch = (rowBytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);

	ComPtr<ID3D12Resource> upload = createBufferResource(m_device.Get(), static_cast<UINT64>(dstRowPitch) * height, D3D12_HEAP_TYPE_UPLOAD);
	if (!upload)
		return false;
	uint8_t* dst = nullptr;
	if (FAILED(upload->Map(0, nullptr, reinterpret_cast<void**>(&dst))))
		return false;
	for (uint32_t row = 0; row < height; ++row)
		memcpy(dst + static_cast<size_t>(row) * dstRowPitch,
		       static_cast<const uint8_t*>(data) + static_cast<size_t>(row) * srcRowPitch, rowBytes);
	upload->Unmap(0, nullptr);

	ComPtr<ID3D12CommandAllocator>     alloc;
	ComPtr<ID3D12GraphicsCommandList4> cl;
	if (!createTransientCmdList(m_device.Get(), alloc, cl))
		return false;

	D3D12_TEXTURE_COPY_LOCATION dl = {};
	dl.pResource        = res;
	dl.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	dl.SubresourceIndex = 0;
	D3D12_TEXTURE_COPY_LOCATION sl = {};
	sl.pResource                          = upload.Get();
	sl.Type                               = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	sl.PlacedFootprint.Footprint.Format   = fmt;
	sl.PlacedFootprint.Footprint.Width    = width;
	sl.PlacedFootprint.Footprint.Height   = height;
	sl.PlacedFootprint.Footprint.Depth    = 1;
	sl.PlacedFootprint.Footprint.RowPitch = dstRowPitch;
	cl->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
	if (finalState != D3D12_RESOURCE_STATE_COPY_DEST)
		transition(cl.Get(), res, D3D12_RESOURCE_STATE_COPY_DEST, finalState);
	executeAndWait(m_cmdQueue.Get(), cl.Get(), m_device.Get());
	return true;
}

TextureHandle DX12DeviceImpl::createTextureResource(const GfxTextureResourceDesc* src)
{
	GfxTextureResourceDesc d;
	if (!readVersionedDesc(src, d, "createTextureResource"))
		return {};

	const bool usageSRV = (d.usage & GfxTextureUsage::ShaderResource)  != GfxTextureUsage::None;
	const bool usageRT  = (d.usage & GfxTextureUsage::RenderTarget)    != GfxTextureUsage::None;
	const bool usageDS  = (d.usage & GfxTextureUsage::DepthStencil)    != GfxTextureUsage::None;
	const bool usageUAV = (d.usage & GfxTextureUsage::UnorderedAccess) != GfxTextureUsage::None;
	const bool msaa     = d.sampleCount > 1;
	const bool cube     = d.dimension == GfxTextureDimension::TextureCube;
	const char* name    = d.debugName ? d.debugName : "unnamed";

	if (d.width == 0 || d.height == 0 || d.depthOrArraySize == 0 || d.sampleCount == 0)
	{
		fprintf(stderr, "[GfxEngine] createTextureResource: サイズ / サンプル数が 0 です (%s)\n", name);
		return {};
	}
	if (cube && (d.depthOrArraySize % 6) != 0)
	{
		fprintf(stderr, "[GfxEngine] createTextureResource: Cube の配列数は 6 の倍数にしてください (%s)\n", name);
		return {};
	}
	if (msaa && (d.dimension != GfxTextureDimension::Texture2D || d.mipLevels > 1 || usageUAV || d.initialData))
	{
		fprintf(stderr, "[GfxEngine] createTextureResource: MSAA は 2D・mip 1・UAV なし・初期データなしのみです (%s)\n", name);
		return {};
	}
	if (usageDS && (usageRT || usageUAV))
	{
		fprintf(stderr, "[GfxEngine] createTextureResource: DepthStencil は RenderTarget / UnorderedAccess と併用できません (%s)\n", name);
		return {};
	}

	const DXGI_FORMAT      requested = static_cast<DXGI_FORMAT>(d.format);
	const DX12DepthFormats depthFmts = splitDepthFormat(requested);
	// 深度フォーマットを SRV でも読むときは Typeless でリソースを作る
	const DXGI_FORMAT resourceFmt = (isDepthDXGI(requested) && usageSRV) ? depthFmts.resource : requested;

	D3D12_RESOURCE_DESC rd = {};
	rd.Dimension          = d.dimension == GfxTextureDimension::Texture1D ? D3D12_RESOURCE_DIMENSION_TEXTURE1D
	                      : d.dimension == GfxTextureDimension::Texture3D ? D3D12_RESOURCE_DIMENSION_TEXTURE3D
	                      :                                                 D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	rd.Width              = d.width;
	rd.Height             = d.dimension == GfxTextureDimension::Texture1D ? 1 : d.height;
	rd.DepthOrArraySize   = static_cast<UINT16>(d.depthOrArraySize);
	rd.MipLevels          = static_cast<UINT16>(d.mipLevels);
	rd.Format             = resourceFmt;
	rd.SampleDesc.Count   = d.sampleCount;
	rd.SampleDesc.Quality = d.sampleQuality;
	if (usageRT)  rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	if (usageDS)  rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
	if (usageUAV) rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	if (usageDS && !usageSRV) rd.Flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

	if (msaa && !checkSampleCount(usageDS ? depthFmts.dsv : resourceFmt, d.sampleCount, d.sampleQuality, "createTextureResource"))
		return {};

	// 最適化クリア値
	D3D12_CLEAR_VALUE cv = {};
	const bool useClear = d.useClearValue && (usageRT || usageDS);
	if (useClear)
	{
		cv.Format = d.clearValue.format != GfxFormat::Unknown ? static_cast<DXGI_FORMAT>(d.clearValue.format)
		          : usageDS ? depthFmts.dsv : resourceFmt;
		if (usageDS)
		{
			cv.DepthStencil.Depth   = d.clearValue.depth;
			cv.DepthStencil.Stencil = d.clearValue.stencil;
		}
		else
		{
			memcpy(cv.Color, d.clearValue.color, sizeof(cv.Color));
		}
	}

	// 状態: 指定がなければ用途から決める
	D3D12_RESOURCE_STATES finalState = static_cast<D3D12_RESOURCE_STATES>(d.initialState);
	if (d.initialState == GfxResourceState::Tracked)
		finalState = usageRT  ? D3D12_RESOURCE_STATE_RENDER_TARGET
		           : usageDS  ? D3D12_RESOURCE_STATE_DEPTH_WRITE
		           : usageSRV ? k_ShaderReadState
		           : usageUAV ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
		           :            D3D12_RESOURCE_STATE_COMMON;
	const D3D12_RESOURCE_STATES createState = d.initialData ? D3D12_RESOURCE_STATE_COPY_DEST : finalState;

	D3D12_HEAP_PROPERTIES hp = {};
	hp.Type = D3D12_HEAP_TYPE_DEFAULT;

	DX12TexRes tex;
	tex.slot = UINT32_MAX;
	if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, createState,
			useClear ? &cv : nullptr, IID_PPV_ARGS(&tex.res))))
	{
		fprintf(stderr, "[GfxEngine] createTextureResource: CreateCommittedResource に失敗しました (%s)\n", name);
		return {};
	}
	setDebugName(tex.res.Get(), d.debugName);

	if (d.initialData && !uploadSubresource0(tex.res.Get(), resourceFmt, d.width, rd.Height, d.initialData, d.rowPitch, finalState))
		return {};

	tex.dsvFormat = depthFmts.dsv;
	tex.srvFormat = isDepthDXGI(requested) ? depthFmts.srv : requested;
	const UINT arraySize = rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : d.depthOrArraySize;

	// 既定の SRV (全体)
	if (usageSRV)
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
		sv.Format                  = tex.srvFormat;
		sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		const UINT mips = d.initialData ? 1u : static_cast<UINT>(-1);   // 初期データは mip 0 のみ
		if (d.dimension == GfxTextureDimension::Texture1D)
		{
			if (arraySize > 1) { sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY; sv.Texture1DArray.MipLevels = mips; sv.Texture1DArray.ArraySize = arraySize; }
			else               { sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;      sv.Texture1D.MipLevels      = mips; }
		}
		else if (d.dimension == GfxTextureDimension::Texture3D)
		{
			sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D; sv.Texture3D.MipLevels = mips;
		}
		else if (cube)
		{
			if (arraySize > 6) { sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY; sv.TextureCubeArray.MipLevels = mips; sv.TextureCubeArray.NumCubes = arraySize / 6; }
			else               { sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;      sv.TextureCube.MipLevels      = mips; }
		}
		else if (msaa)
		{
			if (arraySize > 1) { sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY; sv.Texture2DMSArray.ArraySize = arraySize; }
			else               { sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS; }
		}
		else
		{
			if (arraySize > 1) { sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY; sv.Texture2DArray.MipLevels = mips; sv.Texture2DArray.ArraySize = arraySize; }
			else               { sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;      sv.Texture2D.MipLevels      = mips; }
		}
		D3D12_CPU_DESCRIPTOR_HANDLE cpu = allocStaticSRV(tex.slot);
		if (tex.slot == UINT32_MAX)
			return {};
		m_device->CreateShaderResourceView(tex.res.Get(), &sv, cpu);
		publishStaticSRV(tex.slot);
	}

	// 既定の UAV (mip 0、全スライス)
	if (usageUAV)
	{
		D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {};
		uv.Format = requested;
		if (d.dimension == GfxTextureDimension::Texture1D)
		{
			if (arraySize > 1) { uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY; uv.Texture1DArray.ArraySize = arraySize; }
			else               { uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D; }
		}
		else if (d.dimension == GfxTextureDimension::Texture3D)
		{
			uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D; uv.Texture3D.WSize = d.depthOrArraySize;
		}
		else if (arraySize > 1)
		{
			uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY; uv.Texture2DArray.ArraySize = arraySize;
		}
		else
		{
			uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		}
		D3D12_CPU_DESCRIPTOR_HANDLE cpu = allocStaticSRV(tex.uavSlot);
		if (tex.uavSlot != UINT32_MAX)
		{
			m_device->CreateUnorderedAccessView(tex.res.Get(), nullptr, &uv, cpu);
			publishStaticSRV(tex.uavSlot);
		}
	}

	registerResourceState(tex.res.Get(), finalState);

	const uint64_t id = m_nextId++;
	m_textures.emplace(id, std::move(tex));
	return TextureHandle{ id };
}

// ビューの対象リソースと、既定のフォーマット (Unknown のときに使う) を求める
bool DX12DeviceImpl::resolveViewResource(const GfxResourceRef& ref, GfxViewType type,
	ComPtr<ID3D12Resource>& outRes, DXGI_FORMAT& outDefaultFormat, bool& outIsBuffer)
{
	outIsBuffer = false;
	switch (ref.kind)
	{
	case GfxResourceKind::Buffer:
	{
		const DX12BufRes* buf = getBuffer(ref.handle);
		if (!buf || !buf->res) return false;
		outRes           = buf->res;
		outDefaultFormat = DXGI_FORMAT_UNKNOWN;
		outIsBuffer      = true;
		return true;
	}
	case GfxResourceKind::Texture:
	{
		auto it = m_textures.find(ref.handle);
		if (it != m_textures.end())
		{
			outRes           = it->second.res;
			outDefaultFormat = (type == GfxViewType::DSV) ? it->second.dsvFormat : it->second.srvFormat;
			if (outDefaultFormat == DXGI_FORMAT_UNKNOWN)
				outDefaultFormat = outRes->GetDesc().Format;
			return true;
		}
		ID3D12Resource* raw = getTexResource(ref.handle);   // asTexture() のハンドル
		if (!raw) return false;
		outRes = raw;
		break;
	}
	case GfxResourceKind::RenderTarget:
	{
		auto* rt = reinterpret_cast<IRenderTarget*>(ref.handle);
		if (!rt || !rt->self) return false;
		outRes = static_cast<DX12RenderTargetImpl*>(rt->self)->resource();
		break;
	}
	case GfxResourceKind::DepthStencil:
	{
		auto* ds = reinterpret_cast<IDepthStencil*>(ref.handle);
		if (!ds || !ds->self) return false;
		outRes = static_cast<DX12DepthStencilImpl*>(ds->self)->resource();
		break;
	}
	default:
		return false;
	}
	if (!outRes)
		return false;
	// v1 の RT / DS: Typeless の深度は用途に合わせて読み替える
	const DXGI_FORMAT f = outRes->GetDesc().Format;
	switch (f)
	{
	case DXGI_FORMAT_R32_TYPELESS:   outDefaultFormat = (type == GfxViewType::DSV) ? DXGI_FORMAT_D32_FLOAT         : DXGI_FORMAT_R32_FLOAT;             break;
	case DXGI_FORMAT_R24G8_TYPELESS: outDefaultFormat = (type == GfxViewType::DSV) ? DXGI_FORMAT_D24_UNORM_S8_UINT : DXGI_FORMAT_R24_UNORM_X8_TYPELESS; break;
	case DXGI_FORMAT_R16_TYPELESS:   outDefaultFormat = (type == GfxViewType::DSV) ? DXGI_FORMAT_D16_UNORM         : DXGI_FORMAT_R16_UNORM;             break;
	default:                         outDefaultFormat = f; break;
	}
	return true;
}

ViewHandle DX12DeviceImpl::createView(const GfxViewDesc* src)
{
	GfxViewDesc d;
	if (!readVersionedDesc(src, d, "createView"))
		return {};
	const char* name = d.debugName ? d.debugName : "unnamed";

	DX12ViewRes view;
	DXGI_FORMAT defaultFmt = DXGI_FORMAT_UNKNOWN;
	bool        isBuffer   = false;
	if (!resolveViewResource(d.resource, d.type, view.res, defaultFmt, isBuffer))
	{
		fprintf(stderr, "[GfxEngine] createView: resource が無効です (%s)\n", name);
		return {};
	}
	if (isBuffer != (d.dimension == GfxViewDimension::Buffer))
	{
		fprintf(stderr, "[GfxEngine] createView: バッファには Buffer 次元、テクスチャにはテクスチャの次元を指定してください (%s)\n", name);
		return {};
	}

	const D3D12_RESOURCE_DESC rd = view.res->GetDesc();
	const DXGI_FORMAT fmt  = d.format != GfxFormat::Unknown ? static_cast<DXGI_FORMAT>(d.format)
	                       : d.rawBuffer ? DXGI_FORMAT_R32_TYPELESS : defaultFmt;
	const UINT  mip        = d.mostDetailedMip;
	const UINT  mips       = d.mipLevels;                                    // UINT_MAX = 残り全部
	const UINT  first      = d.firstArraySlice;
	const UINT  totalSlices = rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? std::max<UINT>(1, rd.DepthOrArraySize >> mip)
	                                                                             : rd.DepthOrArraySize;
	const UINT  slices     = d.arraySize == GFX_ALL_MIPS ? (totalSlices > first ? totalSlices - first : 0) : d.arraySize;

	view.type   = d.type;
	view.format = fmt;
	view.width  = std::max<UINT>(1, static_cast<UINT>(rd.Width >> mip));
	view.height = std::max<UINT>(1, rd.Height >> mip);

	switch (d.type)
	{
	case GfxViewType::SRV:
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
		sv.Format                  = fmt;
		sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		switch (d.dimension)
		{
		case GfxViewDimension::Buffer:
			sv.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
			sv.Buffer.FirstElement        = d.firstElement;
			sv.Buffer.NumElements         = d.numElements;
			sv.Buffer.StructureByteStride = d.rawBuffer ? 0 : d.structureByteStride;
			sv.Buffer.Flags               = d.rawBuffer ? D3D12_BUFFER_SRV_FLAG_RAW : D3D12_BUFFER_SRV_FLAG_NONE;
			if (d.structureByteStride > 0 && !d.rawBuffer) sv.Format = DXGI_FORMAT_UNKNOWN;
			break;
		case GfxViewDimension::Texture1D:
			sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
			sv.Texture1D     = { mip, mips, d.minLODClamp };
			break;
		case GfxViewDimension::Texture1DArray:
			sv.ViewDimension  = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
			sv.Texture1DArray = { mip, mips, first, slices, d.minLODClamp };
			break;
		case GfxViewDimension::Texture2D:
			sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			sv.Texture2D     = { mip, mips, d.planeSlice, d.minLODClamp };
			break;
		case GfxViewDimension::Texture2DArray:
			sv.ViewDimension  = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
			sv.Texture2DArray = { mip, mips, first, slices, d.planeSlice, d.minLODClamp };
			break;
		case GfxViewDimension::Texture2DMS:
			sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
			break;
		case GfxViewDimension::Texture2DMSArray:
			sv.ViewDimension    = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
			sv.Texture2DMSArray = { first, slices };
			break;
		case GfxViewDimension::Texture3D:
			sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
			sv.Texture3D     = { mip, mips, d.minLODClamp };
			break;
		case GfxViewDimension::TextureCube:
			sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
			sv.TextureCube   = { mip, mips, d.minLODClamp };
			break;
		case GfxViewDimension::TextureCubeArray:
			sv.ViewDimension    = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
			sv.TextureCubeArray = { mip, mips, first, d.arraySize == GFX_ALL_MIPS ? slices / 6 : d.arraySize, d.minLODClamp };
			break;
		}
		D3D12_CPU_DESCRIPTOR_HANDLE cpu = allocStaticSRV(view.slot);
		if (view.slot == UINT32_MAX)
			return {};
		m_device->CreateShaderResourceView(view.res.Get(), &sv, cpu);
		publishStaticSRV(view.slot);
		view.state = isBuffer ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
		                      : k_ShaderReadState;
		break;
	}
	case GfxViewType::UAV:
	{
		D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {};
		uv.Format = fmt;
		switch (d.dimension)
		{
		case GfxViewDimension::Buffer:
			uv.ViewDimension              = D3D12_UAV_DIMENSION_BUFFER;
			uv.Buffer.FirstElement        = d.firstElement;
			uv.Buffer.NumElements         = d.numElements;
			uv.Buffer.StructureByteStride = d.rawBuffer ? 0 : d.structureByteStride;
			uv.Buffer.Flags               = d.rawBuffer ? D3D12_BUFFER_UAV_FLAG_RAW : D3D12_BUFFER_UAV_FLAG_NONE;
			if (d.structureByteStride > 0 && !d.rawBuffer) uv.Format = DXGI_FORMAT_UNKNOWN;
			break;
		case GfxViewDimension::Texture1D:
			uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D;  uv.Texture1D.MipSlice = mip; break;
		case GfxViewDimension::Texture1DArray:
			uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY; uv.Texture1DArray = { mip, first, slices }; break;
		case GfxViewDimension::Texture2D:
			uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;  uv.Texture2D = { mip, d.planeSlice }; break;
		case GfxViewDimension::Texture2DArray:
		case GfxViewDimension::TextureCube:
		case GfxViewDimension::TextureCubeArray:
			uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY; uv.Texture2DArray = { mip, first, slices, d.planeSlice }; break;
		case GfxViewDimension::Texture3D:
			uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;  uv.Texture3D = { mip, first, slices }; break;
		default:
			fprintf(stderr, "[GfxEngine] createView: MSAA の UAV は作れません (%s)\n", name);
			return {};
		}
		D3D12_CPU_DESCRIPTOR_HANDLE cpu = allocStaticSRV(view.slot);
		if (view.slot == UINT32_MAX)
			return {};
		m_device->CreateUnorderedAccessView(view.res.Get(), nullptr, &uv, cpu);
		publishStaticSRV(view.slot);
		view.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		break;
	}
	case GfxViewType::RTV:
	{
		D3D12_RENDER_TARGET_VIEW_DESC rv = {};
		rv.Format = fmt;
		switch (d.dimension)
		{
		case GfxViewDimension::Texture1D:        rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1D;        rv.Texture1D.MipSlice = mip; break;
		case GfxViewDimension::Texture1DArray:   rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1DARRAY;   rv.Texture1DArray = { mip, first, slices }; break;
		case GfxViewDimension::Texture2D:        rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;        rv.Texture2D = { mip, d.planeSlice }; break;
		case GfxViewDimension::Texture2DMS:      rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;      break;
		case GfxViewDimension::Texture2DMSArray: rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY; rv.Texture2DMSArray = { first, slices }; break;
		case GfxViewDimension::Texture3D:        rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;        rv.Texture3D = { mip, first, slices }; break;
		case GfxViewDimension::Texture2DArray:
		case GfxViewDimension::TextureCube:
		case GfxViewDimension::TextureCubeArray: rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;   rv.Texture2DArray = { mip, first, slices, d.planeSlice }; break;
		default:
			fprintf(stderr, "[GfxEngine] createView: バッファの RTV は作れません (%s)\n", name);
			return {};
		}
		D3D12_CPU_DESCRIPTOR_HANDLE cpu = allocRTV(view.slot);
		if (view.slot == UINT32_MAX)
			return {};
		m_device->CreateRenderTargetView(view.res.Get(), &rv, cpu);
		view.cpu   = cpu;
		view.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
		break;
	}
	case GfxViewType::DSV:
	{
		D3D12_DEPTH_STENCIL_VIEW_DESC dv = {};
		dv.Format = fmt;
		dv.Flags  = static_cast<D3D12_DSV_FLAGS>(d.dsvFlags);
		switch (d.dimension)
		{
		case GfxViewDimension::Texture1D:        dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1D;        dv.Texture1D.MipSlice = mip; break;
		case GfxViewDimension::Texture1DArray:   dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1DARRAY;   dv.Texture1DArray = { mip, first, slices }; break;
		case GfxViewDimension::Texture2D:        dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;        dv.Texture2D.MipSlice = mip; break;
		case GfxViewDimension::Texture2DMS:      dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;      break;
		case GfxViewDimension::Texture2DMSArray: dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY; dv.Texture2DMSArray = { first, slices }; break;
		case GfxViewDimension::Texture2DArray:
		case GfxViewDimension::TextureCube:
		case GfxViewDimension::TextureCubeArray: dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;   dv.Texture2DArray = { mip, first, slices }; break;
		default:
			fprintf(stderr, "[GfxEngine] createView: この次元の DSV は作れません (%s)\n", name);
			return {};
		}
		D3D12_CPU_DESCRIPTOR_HANDLE cpu = allocDSV(view.slot);
		if (view.slot == UINT32_MAX)
			return {};
		m_device->CreateDepthStencilView(view.res.Get(), &dv, cpu);
		view.cpu   = cpu;
		// 深度もステンシルも読み取り専用なら DEPTH_READ (シェーダーから同時に読める)
		const bool readOnly = (d.dsvFlags & (GfxDSVFlags::ReadOnlyDepth | GfxDSVFlags::ReadOnlyStencil))
		                   == (GfxDSVFlags::ReadOnlyDepth | GfxDSVFlags::ReadOnlyStencil)
		                   || ((d.dsvFlags & GfxDSVFlags::ReadOnlyDepth) != GfxDSVFlags::None && !hasStencilDXGI(fmt));
		view.state = readOnly ? (D3D12_RESOURCE_STATE_DEPTH_READ | k_ShaderReadState) : D3D12_RESOURCE_STATE_DEPTH_WRITE;
		view.hasStencil = hasStencilDXGI(fmt);
		break;
	}
	}

	const uint64_t id = m_nextId++;
	m_views.emplace(id, std::move(view));
	return ViewHandle{ id };
}

void DX12DeviceImpl::destroyView(ViewHandle h)
{
	auto it = m_views.find(h.id);
	if (it == m_views.end())
		return;
	DX12ViewRes& v = it->second;
	switch (v.type)
	{
	case GfxViewType::RTV: freeRTVDeferred(v.slot);       break;
	case GfxViewType::DSV: freeDSVDeferred(v.slot);       break;
	default:               freeStaticSRVDeferred(v.slot); break;
	}
	deferRelease(std::move(v.res));
	m_views.erase(it);
}

SamplerHandle DX12DeviceImpl::createSamplerState(const GfxSamplerStateDesc* src)
{
	GfxSamplerStateDesc d;
	if (!readVersionedDesc(src, d, "createSamplerState"))
		return {};

	DX12SamplerRes sam;
	D3D12_CPU_DESCRIPTOR_HANDLE cpu = allocStaticSampler(sam.slot);
	if (sam.slot == UINT32_MAX)
		return {};

	D3D12_SAMPLER_DESC sd = {};
	sd.Filter         = static_cast<D3D12_FILTER>(d.filter);
	sd.AddressU       = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(d.addressU);
	sd.AddressV       = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(d.addressV);
	sd.AddressW       = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(d.addressW);
	sd.MipLODBias     = d.mipLODBias;
	sd.MaxAnisotropy  = d.maxAnisotropy;
	sd.ComparisonFunc = static_cast<D3D12_COMPARISON_FUNC>(d.comparisonFunc);
	memcpy(sd.BorderColor, d.borderColor, sizeof(sd.BorderColor));
	sd.MinLOD         = d.minLOD;
	sd.MaxLOD         = d.maxLOD;
	m_device->CreateSampler(&sd, cpu);
	publishStaticSampler(sam.slot);

	const uint64_t id = m_nextId++;
	m_samplers.emplace(id, sam);
	return SamplerHandle{ id };
}

// ============================================================
//  DX12CommandBufferImpl — ビュー / MSAA のコマンド (Phase 2b-2)
// ============================================================

void DX12CommandBufferImpl::setRenderTargetViews(uint32_t count, const ViewHandle* rtvs, ViewHandle dsv)
{
	if (!requireDirect("setRenderTargetViews"))
		return;
	count = std::min<uint32_t>(count, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT);

	D3D12_CPU_DESCRIPTOR_HANDLE handles[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
	uint32_t vw = 0, vh = 0;
	for (uint32_t i = 0; i < count; ++i)
	{
		const DX12ViewRes* v = rtvs ? m_dev->getView(rtvs[i].id) : nullptr;
		if (!v || v->type != GfxViewType::RTV)
		{
			fprintf(stderr, "[GfxEngine] setRenderTargetViews: rtvs[%u] は RTV ではありません\n", i);
			return;
		}
		requireState(v->res.Get(), v->state);
		handles[i]          = v->cpu;
		m_currentRTVFmts[i] = v->format;
		if (i == 0) { vw = v->width; vh = v->height; }
	}

	const DX12ViewRes* dv = dsv.valid() ? m_dev->getView(dsv.id) : nullptr;
	if (dsv.valid() && (!dv || dv->type != GfxViewType::DSV))
	{
		fprintf(stderr, "[GfxEngine] setRenderTargetViews: dsv は DSV ではありません\n");
		return;
	}
	if (dv)
	{
		requireState(dv->res.Get(), dv->state);
		if (count == 0) { vw = dv->width; vh = dv->height; }
	}

	m_cmdList->OMSetRenderTargets(count, count ? handles : nullptr, FALSE, dv ? &dv->cpu : nullptr);
	m_currentRTVCount = count;
	m_currentDSVFmt   = dv ? dv->format : DXGI_FORMAT_UNKNOWN;

	if (vw > 0 && vh > 0)
	{
		D3D12_VIEWPORT vp = { 0, 0, static_cast<float>(vw), static_cast<float>(vh), 0, 1 };
		D3D12_RECT     sc = { 0, 0, static_cast<LONG>(vw), static_cast<LONG>(vh) };
		m_cmdList->RSSetViewports(1, &vp);
		m_cmdList->RSSetScissorRects(1, &sc);
	}
}

void DX12CommandBufferImpl::clearRenderTargetView(ViewHandle rtv, const float* rgba, uint32_t rectCount, const GfxRect* rects)
{
	if (!requireDirect("clearRenderTargetView") || !rgba)
		return;
	const DX12ViewRes* v = m_dev->getView(rtv.id);
	if (!v || v->type != GfxViewType::RTV)
		return;
	requireState(v->res.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
	m_cmdList->ClearRenderTargetView(v->cpu, rgba, rects ? rectCount : 0,
		rects ? reinterpret_cast<const D3D12_RECT*>(rects) : nullptr);
}

void DX12CommandBufferImpl::clearDepthStencilView(ViewHandle dsv, GfxClearFlags flags, float depth, uint8_t stencil,
                                                  uint32_t rectCount, const GfxRect* rects)
{
	if (!requireDirect("clearDepthStencilView"))
		return;
	const DX12ViewRes* v = m_dev->getView(dsv.id);
	if (!v || v->type != GfxViewType::DSV)
		return;
	D3D12_CLEAR_FLAGS f = static_cast<D3D12_CLEAR_FLAGS>(flags);
	if (!v->hasStencil)
		f &= ~D3D12_CLEAR_FLAG_STENCIL;
	if (f == 0)
		return;
	requireState(v->res.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
	m_cmdList->ClearDepthStencilView(v->cpu, f, depth, stencil, rects ? rectCount : 0,
		rects ? reinterpret_cast<const D3D12_RECT*>(rects) : nullptr);
}

void DX12CommandBufferImpl::resolve(GfxResourceRef dst, uint32_t dstSub, GfxResourceRef src, uint32_t srcSub, GfxFormat format)
{
	if (!requireDirect("resolve"))
		return;
	ID3D12Resource* d = resolveTextureRef(dst);
	ID3D12Resource* s = resolveTextureRef(src);
	if (!d || !s)
	{
		fprintf(stderr, "[GfxEngine] resolve: dst / src が無効です\n");
		return;
	}
	DXGI_FORMAT fmt = static_cast<DXGI_FORMAT>(format);
	if (fmt == DXGI_FORMAT_UNKNOWN)
		fmt = d->GetDesc().Format;
	requireState(d, D3D12_RESOURCE_STATE_RESOLVE_DEST);
	requireState(s, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
	m_cmdList->ResolveSubresource(d, dstSub, s, srcSub, fmt);
}

// ============================================================
//  DX12RenderTargetImpl — 実装
// ============================================================

DX12RenderTargetImpl::DX12RenderTargetImpl()
{
	m_iface.vtable = getVTable();
	m_iface.self   = this;
}

// オフスクリーン RT を作成する
bool DX12RenderTargetImpl::create(
	DX12DeviceImpl*            dev,
	const GfxRenderTargetDesc* desc)
{
	if (desc->sampleCount > 1 && desc->allowUAV)
	{
		fprintf(stderr, "[GfxEngine] createRenderTarget: MSAA (sampleCount=%u) の RT は allowUAV にできません\n", desc->sampleCount);
		return false;
	}
	if (!dev->checkSampleCount(toDXGI(desc->format), desc->sampleCount, 0, "createRenderTarget"))
		return false;

	m_dev         = dev;
	m_desc        = *desc;
	m_desc.sampleCount = desc->sampleCount > 0 ? desc->sampleCount : 1;
	m_width       = desc->width  ? desc->width  : 1;
	m_height      = desc->height ? desc->height : 1;
	m_format      = desc->format;
	m_ownResource = true;

	return allocateResource();
}

// SwapChain バックバッファから RT を作成する
bool DX12RenderTargetImpl::createFromBuffer(
	DX12DeviceImpl* dev,
	ID3D12Resource* buffer,
	PixelFormat     fmt,
	uint32_t        w,
	uint32_t        h)
{
	m_dev         = dev;
	m_ownResource = false;
	m_width       = w;
	m_height      = h;
	m_format      = fmt;

	// ComPtr に入れるため AddRef してから Attach する
	buffer->AddRef();
	m_resource.Attach(buffer);

	m_rtvHandle = dev->allocRTV(m_rtvSlot);
	if (m_rtvSlot == UINT32_MAX)
		return false;
	dev->device()->CreateRenderTargetView(m_resource.Get(), nullptr, m_rtvHandle);

	// バックバッファは PRESENT (= COMMON) 状態で渡される
	dev->registerResourceState(m_resource.Get(), D3D12_RESOURCE_STATE_PRESENT);
	return true;
}

// テクスチャリソースとビューを作成する
bool DX12RenderTargetImpl::allocateResource()
{
	D3D12_HEAP_PROPERTIES hp = {};
	hp.Type = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC rd = {};
	rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	rd.Width            = m_width;
	rd.Height           = m_height;
	rd.DepthOrArraySize = 1;
	rd.MipLevels        = 1;
	rd.Format           = toDXGI(m_format);
	rd.SampleDesc.Count = m_desc.sampleCount;
	rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

	if (m_desc.allowUAV)
		rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

	D3D12_CLEAR_VALUE cv = {};
	cv.Format = toDXGI(m_format);
	memcpy(cv.Color, m_desc.clearColor, sizeof(cv.Color));

	if (FAILED(m_dev->device()->CreateCommittedResource(
			&hp, D3D12_HEAP_FLAG_NONE, &rd,
			D3D12_RESOURCE_STATE_RENDER_TARGET, &cv,
			IID_PPV_ARGS(&m_resource))))
		return false;

	setDebugName(m_resource.Get(), m_desc.debugName);

	// RTV (リサイズのたびに新しいスロットを確保する。旧スロットは遅延返却)
	m_rtvHandle = m_dev->allocRTV(m_rtvSlot);
	if (m_rtvSlot == UINT32_MAX)
	{
		m_resource.Reset();
		return false;
	}
	m_dev->device()->CreateRenderTargetView(m_resource.Get(), nullptr, m_rtvHandle);

	// SRV (asTexture 用)
	if (m_desc.allowUAV || m_desc.shaderRead)
	{
		D3D12_CPU_DESCRIPTOR_HANDLE srvCPU = m_dev->allocStaticSRV(m_srvSlot);
		if (m_srvSlot != UINT32_MAX)
		{
			D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
			sv.Format                  = toDXGI(m_format);
			sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			if (m_desc.sampleCount > 1)
			{
				sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
			}
			else
			{
				sv.ViewDimension       = D3D12_SRV_DIMENSION_TEXTURE2D;
				sv.Texture2D.MipLevels = 1;
			}

			m_dev->device()->CreateShaderResourceView(m_resource.Get(), &sv, srvCPU);
			m_dev->publishStaticSRV(m_srvSlot);
			m_dev->registerSRVSlot(m_srvSlot, this);
		}
	}

	// リソースは RENDER_TARGET 状態で作成される
	m_dev->registerResourceState(m_resource.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
	return true;
}

// リソースとディスクリプタを解放する
// immediate=false: GPU が使い終わるまで遅延する / true: 呼び出し側が GPU アイドルを保証する
void DX12RenderTargetImpl::freeResource(bool immediate)
{
	if (!m_dev)
		return;

	if (m_srvSlot != UINT32_MAX)
	{
		m_dev->unregisterSRVSlot(m_srvSlot);
		m_dev->freeStaticSRVDeferred(m_srvSlot);
		m_srvSlot = UINT32_MAX;
	}

	ID3D12Resource* raw = m_resource.Get();

	if (immediate)
	{
		if (m_rtvSlot != UINT32_MAX)
			m_dev->freeRTVNow(m_rtvSlot);
		if (raw)
			m_dev->unregisterResourceState(raw);
		m_resource.Reset();
	}
	else
	{
		if (m_rtvSlot != UINT32_MAX)
			m_dev->freeRTVDeferred(m_rtvSlot);
		if (raw)
		{
			DX12DeviceImpl* dev = m_dev;
			m_dev->deferRelease(m_resource, [dev, raw] { dev->unregisterResourceState(raw); });
		}
		m_resource.Reset();
	}
	m_rtvSlot = UINT32_MAX;
}

// 外部から呼ばれる destroy (遅延解放)
void DX12RenderTargetImpl::destroy()
{
	freeResource(false);
}

// バックバッファ用の即時解放 (呼び出し側が GPU アイドルを保証する)
void DX12RenderTargetImpl::destroyImmediate()
{
	freeResource(true);
}

// サイズを変更してリソースを再作成する
// asTexture() のハンドルは変わるため、リサイズ後は取得し直すこと
bool DX12RenderTargetImpl::resize(uint32_t w, uint32_t h)
{
	if (!m_ownResource || (w == m_width && h == m_height) || w == 0 || h == 0)
		return true;

	m_width       = w;
	m_height      = h;
	m_desc.width  = w;
	m_desc.height = h;

	freeResource(false);
	return allocateResource();
}

// SRV として参照するハンドルを返す
// shaderRead / allowUAV を指定して作成した RT のみ有効
TextureHandle DX12RenderTargetImpl::asTexture()
{
	if (m_srvSlot == UINT32_MAX)
		return {};
	// 上位ビットに RT フラグを立てて createTexture の id と区別する
	return { DX12DeviceImpl::RT_TEXTURE_FLAG | static_cast<uint64_t>(m_srvSlot) };
}

// ============================================================
//  DX12DepthStencilImpl — 実装
// ============================================================

DX12DepthStencilImpl::DX12DepthStencilImpl()
{
	m_iface.vtable = getVTable();
	m_iface.self   = this;
}

// DS リソースと DSV を作成する
bool DX12DepthStencilImpl::create(
	DX12DeviceImpl*            dev,
	const GfxDepthStencilDesc* desc)
{
	if (!dev->checkSampleCount(toDXGI(desc->format), desc->sampleCount, 0, "createDepthStencil"))
		return false;

	m_dev    = dev;
	m_desc   = *desc;
	m_desc.sampleCount = desc->sampleCount > 0 ? desc->sampleCount : 1;
	m_width  = desc->width  ? desc->width  : 1;
	m_height = desc->height ? desc->height : 1;
	m_format = desc->format;

	return allocateResource();
}

// テクスチャリソースと DSV / SRV を作成する
bool DX12DepthStencilImpl::allocateResource()
{
	D3D12_HEAP_PROPERTIES hp = {};
	hp.Type = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC rd = {};
	rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	rd.Width            = m_width;
	rd.Height           = m_height;
	rd.DepthOrArraySize = 1;
	rd.MipLevels        = 1;
	rd.SampleDesc.Count = m_desc.sampleCount;
	rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

	// SRV としても使うため TYPELESS フォーマットでリソースを作成する
	DXGI_FORMAT resourceFmt = toDXGI(m_format);
	DXGI_FORMAT dsvFmt      = toDXGI(m_format);
	DXGI_FORMAT srvFmt      = DXGI_FORMAT_UNKNOWN;
	switch (m_format)
	{
	case PixelFormat::D32_FLOAT:
		resourceFmt = DXGI_FORMAT_R32_TYPELESS;
		dsvFmt      = DXGI_FORMAT_D32_FLOAT;
		srvFmt      = DXGI_FORMAT_R32_FLOAT;
		break;
	case PixelFormat::D24_UNORM_S8_UINT:
		resourceFmt = DXGI_FORMAT_R24G8_TYPELESS;
		dsvFmt      = DXGI_FORMAT_D24_UNORM_S8_UINT;
		srvFmt      = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
		break;
	case PixelFormat::D16_UNORM:
		resourceFmt = DXGI_FORMAT_R16_TYPELESS;
		dsvFmt      = DXGI_FORMAT_D16_UNORM;
		srvFmt      = DXGI_FORMAT_R16_UNORM;
		break;
	default:
		break;
	}
	rd.Format = resourceFmt;

	D3D12_CLEAR_VALUE cv = {};
	cv.Format                  = dsvFmt;
	cv.DepthStencil.Depth      = m_desc.clearDepth;
	cv.DepthStencil.Stencil    = m_desc.clearStencil;

	if (FAILED(m_dev->device()->CreateCommittedResource(
			&hp, D3D12_HEAP_FLAG_NONE, &rd,
			D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
			IID_PPV_ARGS(&m_resource))))
		return false;

	setDebugName(m_resource.Get(), m_desc.debugName);

	// DSV (リサイズのたびに新しいスロットを確保する。旧スロットは遅延返却)
	m_dsvHandle = m_dev->allocDSV(m_dsvSlot);
	if (m_dsvSlot == UINT32_MAX)
	{
		m_resource.Reset();
		return false;
	}

	D3D12_DEPTH_STENCIL_VIEW_DESC dsvd = {};
	dsvd.Format        = dsvFmt;
	dsvd.ViewDimension = m_desc.sampleCount > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
	m_dev->device()->CreateDepthStencilView(m_resource.Get(), &dsvd, m_dsvHandle);

	// Depth リソースから SRV を作成してシェーダーから読めるようにする
	if (srvFmt != DXGI_FORMAT_UNKNOWN)
	{
		D3D12_CPU_DESCRIPTOR_HANDLE srvCPU = m_dev->allocStaticSRV(m_srvSlot);
		if (m_srvSlot != UINT32_MAX)
		{
			D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
			sv.Format                    = srvFmt;
			sv.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			if (m_desc.sampleCount > 1)
			{
				sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
			}
			else
			{
				sv.ViewDimension       = D3D12_SRV_DIMENSION_TEXTURE2D;
				sv.Texture2D.MipLevels = 1;
			}

			m_dev->device()->CreateShaderResourceView(m_resource.Get(), &sv, srvCPU);
			m_dev->publishStaticSRV(m_srvSlot);
			m_dev->registerDSSRVSlot(m_srvSlot, this);
		}
	}

	m_dev->registerResourceState(m_resource.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
	return true;
}

// リソースとディスクリプタを遅延解放する
void DX12DepthStencilImpl::freeResource()
{
	if (!m_dev)
		return;

	if (m_dsvSlot != UINT32_MAX)
	{
		m_dev->freeDSVDeferred(m_dsvSlot);
		m_dsvSlot = UINT32_MAX;
	}
	if (m_srvSlot != UINT32_MAX)
	{
		m_dev->unregisterDSSRVSlot(m_srvSlot);
		m_dev->freeStaticSRVDeferred(m_srvSlot);
		m_srvSlot = UINT32_MAX;
	}
	if (ID3D12Resource* raw = m_resource.Get())
	{
		DX12DeviceImpl* dev = m_dev;
		m_dev->deferRelease(m_resource, [dev, raw] { dev->unregisterResourceState(raw); });
	}
	m_resource.Reset();
}

// 外部から呼ばれる destroy
void DX12DepthStencilImpl::destroy()
{
	freeResource();
}

// サイズを変更してリソースを再作成する
bool DX12DepthStencilImpl::resize(uint32_t w, uint32_t h)
{
	if ((w == m_width && h == m_height) || w == 0 || h == 0)
		return true;

	m_width       = w;
	m_height      = h;
	m_desc.width  = w;
	m_desc.height = h;

	freeResource();
	return allocateResource();
}

// デプスを SRV として参照するハンドルを返す
TextureHandle DX12DepthStencilImpl::asTexture()
{
	if (m_srvSlot == UINT32_MAX)
		return {};
	return { DX12DeviceImpl::RT_TEXTURE_FLAG | static_cast<uint64_t>(m_srvSlot) };
}

// ============================================================
//  DX12SwapChainImpl — 実装
// ============================================================

DX12SwapChainImpl::DX12SwapChainImpl()
{
	m_iface.vtable = getVTable();
	m_iface.self   = this;
}

// スワップチェーンを作成する
bool DX12SwapChainImpl::create(
	DX12DeviceImpl*         dev,
	const GfxSwapChainDesc* desc)
{
	m_dev         = dev;
	m_width       = desc->width;
	m_height      = desc->height;
	m_format      = desc->format;
	m_vsync       = desc->vsync;
	// FLIP モデルは 2 枚以上が必要
	m_bufferCount = std::max(2u, std::min(desc->bufferCount, MAX_BUFFERS));

	if (FAILED(dev->device()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence))))
		return false;

	m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	if (!m_fenceEvent)
		return false;

	// ヘッドレスモードはここで終了
	if (!desc->nativeWindowHandle)
		return true;

	// vsync OFF 時にティアリング (可変リフレッシュレート) を使えるか確認する
	BOOL allowTearing = FALSE;
	if (FAILED(dev->factory()->CheckFeatureSupport(
			DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing))))
		allowTearing = FALSE;
	m_allowTearing = (allowTearing == TRUE);
	m_swapFlags    = m_allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

	DXGI_SWAP_CHAIN_DESC1 sd = {};
	sd.Width        = m_width;
	sd.Height       = m_height;
	sd.Format       = toDXGI(m_format);
	sd.SampleDesc   = { 1, 0 };
	sd.BufferUsage  = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.BufferCount  = m_bufferCount;
	sd.SwapEffect   = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	sd.Flags        = m_swapFlags;

	DXGI_SWAP_CHAIN_FULLSCREEN_DESC fsd = {};
	fsd.Windowed = TRUE;

	HWND hwnd = static_cast<HWND>(desc->nativeWindowHandle);
	ComPtr<IDXGISwapChain1> sc1;

	if (FAILED(dev->factory()->CreateSwapChainForHwnd(
			dev->cmdQueue(), hwnd, &sd, &fsd, nullptr, &sc1)))
		return false;

	dev->factory()->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

	if (FAILED(sc1.As(&m_swapChain)))
		return false;

	m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

	return createBackBufferRTs()
		&& createPresentCmdList();
}

// Present 専用コマンドリストを作成する
bool DX12SwapChainImpl::createPresentCmdList()
{
	auto* dev = m_dev->device();

	if (FAILED(dev->CreateCommandAllocator(
			D3D12_COMMAND_LIST_TYPE_DIRECT,
			IID_PPV_ARGS(&m_presentAlloc))))
		return false;

	{
		ComPtr<ID3D12GraphicsCommandList> cmdList;
		if (FAILED(dev->CreateCommandList(
				0, D3D12_COMMAND_LIST_TYPE_DIRECT,
				m_presentAlloc.Get(), nullptr,
				IID_PPV_ARGS(&cmdList))))
			return false;
		if (FAILED(cmdList->QueryInterface(IID_PPV_ARGS(&m_presentCmdList))))
			return false;
	}

	m_presentCmdList->Close();

	// present コマンドの GPU 完了を追跡するフェンス
	if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_presentFence))))
		return false;
	m_presentEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	if (!m_presentEvent)
		return false;

	return true;
}

// バックバッファごとに RenderTargetImpl を作成する
bool DX12SwapChainImpl::createBackBufferRTs()
{
	if (!m_swapChain)
		return true;

	for (uint32_t i = 0; i < m_bufferCount; ++i)
	{
		ComPtr<ID3D12Resource> buf;

		if (FAILED(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&buf))))
			return false;

		m_backBufferRTs[i] = new DX12RenderTargetImpl();

		if (!m_backBufferRTs[i]->createFromBuffer(
				m_dev, buf.Get(), m_format, m_width, m_height))
			return false;
	}

	return true;
}

// 全バックバッファ RT を即時解放する (呼び出し側が GPU アイドルを保証する)
// ResizeBuffers の前にバックバッファへの参照をすべて手放す必要があるため遅延解放しない
void DX12SwapChainImpl::releaseBackBufferRTs()
{
	for (uint32_t i = 0; i < m_bufferCount; ++i)
	{
		if (m_backBufferRTs[i])
		{
			m_backBufferRTs[i]->destroyImmediate();
			delete m_backBufferRTs[i];
			m_backBufferRTs[i] = nullptr;
		}
	}
}

// スワップチェーンのサイズを変更する
bool DX12SwapChainImpl::resize(uint32_t w, uint32_t h)
{
	// 最小化中 (0x0) はリサイズしない
	if (!m_swapChain || w == 0 || h == 0 || (w == m_width && h == m_height))
		return true;

	// SwapChain の fence と DX12Device の描画 fence の両方を待つ
	waitForGPU();
	m_dev->waitGPUIdle();  // フレームインフライトの描画コマンドも完了させる
	releaseBackBufferRTs();

	m_width  = w;
	m_height = h;

	HRESULT hr = m_swapChain->ResizeBuffers(
		m_bufferCount, w, h,
		toDXGI(m_format),
		m_swapFlags);

	if (FAILED(hr))
		return false;

	m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

	return createBackBufferRTs();
}

// バックバッファを確定し GPU フェンスを待機する
bool DX12SwapChainImpl::acquireNextBuffer()
{
	if (m_dev)
		m_dev->processDeferredReleases();

	if (!m_swapChain)
		return true;

	if (m_fence->GetCompletedValue() < m_fenceValues[m_frameIndex])
	{
		m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex], m_fenceEvent);
		WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);
	}

	return true;
}

// 現フレームのバックバッファ RT を返す
IRenderTarget* DX12SwapChainImpl::getRenderTarget()
{
	if (!m_swapChain || !m_backBufferRTs[m_frameIndex])
		return nullptr;

	// ラッパーはバックバッファ RT impl が所有する。アプリは destroy / delete しないこと。
	return m_backBufferRTs[m_frameIndex]->iface();
}

// バックバッファを PRESENT 状態に遷移させて画面に表示する
void DX12SwapChainImpl::present()
{
	if (!m_swapChain)
		return;

	auto* rtImpl = m_backBufferRTs[m_frameIndex];
	ID3D12Resource* res = rtImpl ? rtImpl->resource() : nullptr;
	const D3D12_RESOURCE_STATES state = res ? m_dev->globalState(res) : D3D12_RESOURCE_STATE_PRESENT;

	if (res && state != D3D12_RESOURCE_STATE_PRESENT)
	{
		// 前回の present コマンドが GPU で完了するまで待機してから Allocator を Reset する
		if (m_presentFenceVal > 0 &&
			m_presentFence->GetCompletedValue() < m_presentFenceVal)
		{
			m_presentFence->SetEventOnCompletion(m_presentFenceVal, m_presentEvent);
			WaitForSingleObjectEx(m_presentEvent, INFINITE, FALSE);
		}

		m_presentAlloc->Reset();
		m_presentCmdList->Reset(m_presentAlloc.Get(), nullptr);

		DX12DeviceImpl::transition(
			m_presentCmdList.Get(),
			res,
			state,
			D3D12_RESOURCE_STATE_PRESENT);

		m_dev->setGlobalState(res, D3D12_RESOURCE_STATE_PRESENT);

		m_presentCmdList->Close();

		ID3D12CommandList* lists[] = { m_presentCmdList.Get() };
		m_dev->cmdQueue()->ExecuteCommandLists(1, lists);

		// present コマンドの完了を追跡するためにフェンスを Signal する
		++m_presentFenceVal;
		m_dev->cmdQueue()->Signal(m_presentFence.Get(), m_presentFenceVal);
	}

	// vsync ON: 垂直同期 / OFF: ティアリング対応なら即時表示
	const UINT syncInterval = m_vsync ? 1 : 0;
	const UINT presentFlags = (!m_vsync && m_allowTearing) ? DXGI_PRESENT_ALLOW_TEARING : 0;
	m_swapChain->Present(syncInterval, presentFlags);
	moveToNextFrame();
}

// GPU の完了を待機する
// GPU の全コマンド完了を待機し、全バッファのフェンス値を統一する
void DX12SwapChainImpl::waitForGPU()
{
	// 現在のフェンス値より大きい値で Signal して全コマンド完了を待つ
	const uint64_t signal = ++m_fenceValues[m_frameIndex];
	m_dev->cmdQueue()->Signal(m_fence.Get(), signal);
	m_fence->SetEventOnCompletion(signal, m_fenceEvent);
	WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);

	// 全バッファのフェンス値を完了済みの値に揃える
	// これにより次の acquireNextBuffer で待機が発生しなくなる
	for (uint32_t i = 0; i < m_bufferCount; ++i)
		m_fenceValues[i] = signal;
}

// 次フレームへ進む
void DX12SwapChainImpl::moveToNextFrame()
{
	// 現フレームの描画完了を GPU に通知する
	// フェンス値をインクリメントして Signal する
	const uint64_t signalValue = ++m_fenceValues[m_frameIndex];
	m_dev->cmdQueue()->Signal(m_fence.Get(), signalValue);

	// バックバッファ index を次フレームへ進める
	m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

	// 次フレームのバッファがまだ GPU で使用中なら完了を待つ
	// (acquireNextBuffer 側でも待つが、ここで先取りして待つことで
	//  CPU 側の準備時間中に GPU を走らせ続けられる)
	if (m_fence->GetCompletedValue() < m_fenceValues[m_frameIndex])
	{
		m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex], m_fenceEvent);
		WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);
	}
}

// スワップチェーンを破棄する
void DX12SwapChainImpl::destroy()
{
	if (m_fence)
		waitForGPU();
	releaseBackBufferRTs();
	m_presentCmdList.Reset();
	m_presentAlloc.Reset();
	m_presentFence.Reset();
	if (m_presentEvent)
	{
		CloseHandle(m_presentEvent);
		m_presentEvent = nullptr;
	}

	if (m_fenceEvent)
	{
		CloseHandle(m_fenceEvent);
		m_fenceEvent = nullptr;
	}

	m_swapChain.Reset();
}

#endif // _WIN32
