#pragma once
// ============================================================
//  backends/dx12/DX12Device.h
//  デバイス (キュー、フェンス、遅延解放、ディスクリプタ、リソース管理)
//  バックエンドの各ソースはこのヘッダーだけを include すればよい
// ============================================================

#include "DX12CommandBuffer.h"
#include "DX12Resources.h"
#include "DX12SwapChain.h"
#include "DX12Pipeline.h"
#include "DX12ShaderCompiler.h"

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

	// HLSL のコンパイラー (DXC)
	DX12ShaderCompiler& shaderCompiler() { return m_shaderCompiler; }
	// コンパイルのエラー / 警告をデバッグメッセージで通知する
	void reportShaderMessages(const char* what, const char* name, const std::string& messages, bool failed);

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

	// デバッグメッセージの通知先 (GfxDeviceConfig::debugCallback。nullptr なら stderr)
	PFN_GfxDebugMessage m_debugCallback = nullptr;
	void*               m_debugUser     = nullptr;

public:
	// デバッグメッセージを通知する (コールバックがなければ stderr に出力する)
	void emitDebugMessage(GfxMessageSeverity severity, int32_t id, const char* text);

private:

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
	bool createCoreDevice(const GfxDeviceConfig& cfg);
	bool createDescriptorHeaps();
	bool createDefaultPSO();
	bool createSubmitFence();
	bool createCommandSignatures();

	DX12ShaderCompiler m_shaderCompiler;

	// ExecuteIndirect 用のコマンドシグネチャ (引数のみ、ルート引数の変更なし)
	ComPtr<ID3D12CommandSignature> m_drawSig;
	ComPtr<ID3D12CommandSignature> m_drawIndexedSig;
	ComPtr<ID3D12CommandSignature> m_dispatchSig;

	// RT フォーマットごとに PSO を用意する
	ComPtr<ID3D12RootSignature> m_defaultRootSig;
	ComPtr<ID3D12PipelineState> m_defaultPSO_RGBA8;   // RGBA8_UNORM 用 (G-Buffer 等)
	ComPtr<ID3D12PipelineState> m_defaultPSO_BGRA8;   // BGRA8_UNORM 用 (バックバッファ)
};
