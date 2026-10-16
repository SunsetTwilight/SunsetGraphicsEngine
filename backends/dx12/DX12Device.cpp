// ============================================================
//  backends/dx12/DX12Device.cpp
//  デバイスの初期化と終了、submit、遅延解放、ディスクリプタ、バッファ / テクスチャ (v1)、
//  Compute パイプライン、クエリ
// ============================================================

#include "DX12Device.h"

// ============================================================
//  DX12DeviceImpl — 実装
// ============================================================

// デバイスを初期化する
bool DX12DeviceImpl::init(const GfxDeviceConfig* cfg)
{
	m_debugCallback = cfg->debugCallback;
	m_debugUser     = cfg->debugUser;

	bool debugLayer = false;
#ifdef _DEBUG
	if (cfg->validation)
	{
		ComPtr<ID3D12Debug1> dbg;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg))))
		{
			dbg->EnableDebugLayer();
			dbg->SetEnableGPUBasedValidation(TRUE);
			debugLayer = true;
		}
		else
		{
			// Graphics Tools (d3d12SDKLayers.dll) が入っていない環境
			emitDebugMessage(GfxMessageSeverity::Info, GFX_MESSAGE_ID_ENGINE,
				"D3D12 debug layer is not available (install the Graphics Tools optional feature)");
		}
	}
#else
	if (cfg->validation)
		emitDebugMessage(GfxMessageSeverity::Info, GFX_MESSAGE_ID_ENGINE,
			"validation is ignored because the backend is a release build");
#endif

	GfxDeviceConfig c = *cfg;
	c.validation = debugLayer;   // デバッグレイヤーを有効にできた場合だけ、デバッグ用のファクトリと InfoQueue を使う
	return createCoreDevice(c)
		&& createDescriptorHeaps()
		&& createDefaultPSO()
		&& createSubmitFence()
		&& createCommandSignatures();
}

static_assert(static_cast<uint32_t>(GfxMessageSeverity::Corruption) == D3D12_MESSAGE_SEVERITY_CORRUPTION, "severity");
static_assert(static_cast<uint32_t>(GfxMessageSeverity::Error)      == D3D12_MESSAGE_SEVERITY_ERROR,      "severity");
static_assert(static_cast<uint32_t>(GfxMessageSeverity::Warning)    == D3D12_MESSAGE_SEVERITY_WARNING,    "severity");

void DX12DeviceImpl::emitDebugMessage(GfxMessageSeverity severity, int32_t id, const char* text)
{
	if (m_debugCallback)
	{
		m_debugCallback(m_debugUser, severity, id, text);
		return;
	}
	static const char* k_sev[] = { "CORRUPTION", "ERROR", "WARNING", "INFO" };
	const uint32_t s = static_cast<uint32_t>(severity);
	fprintf(stderr, "[D3D12 %s #%d] %s\n", s < 4 ? k_sev[s] : "?", static_cast<int>(id), text);
}

// デバイス / コマンドキューを作成する
bool DX12DeviceImpl::createCoreDevice(const GfxDeviceConfig& cfg)
{
	const bool validation = cfg.validation;
	UINT flags = validation ? DXGI_CREATE_FACTORY_DEBUG : 0;

	if (FAILED(CreateDXGIFactory2(flags, IID_PPV_ARGS(&m_factory))))
		return false;

	auto tryAdapter = [this](IDXGIAdapter1* adapter) -> bool
	{
		DXGI_ADAPTER_DESC1 d;
		adapter->GetDesc1(&d);
		if (FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&m_device))))
			return false;
		ComPtr<IDXGIAdapter1> a(adapter);
		a.As(&m_adapter);
		WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, m_adapterName, 256, nullptr, nullptr);
		return true;
	};

	if (cfg.adapterType == GfxAdapterType::Software)
	{
		// WARP (ソフトウェアレンダラー)。GPU のない CI などで使う
		ComPtr<IDXGIAdapter1> warp;
		if (SUCCEEDED(m_factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))))
			tryAdapter(warp.Get());
	}
	else
	{
		ComPtr<IDXGIAdapter1> a1;
		for (UINT i = 0; m_factory->EnumAdapters1(i, &a1) != DXGI_ERROR_NOT_FOUND; ++i)
		{
			DXGI_ADAPTER_DESC1 d;
			a1->GetDesc1(&d);

			if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
				continue;

			if (tryAdapter(a1.Get()))
				break;
		}
	}

	if (!m_device)
		return false;

	// バリデーション有効時は、デバッグレイヤーのメッセージをコールバック (なければ stderr) に通知する
	// (デバッガ未接続でも検証結果を確認できるようにする)
	if (validation)
	{
		ComPtr<ID3D12InfoQueue1> iq;
		if (SUCCEEDED(m_device.As(&iq)))
		{
			DWORD cookie = 0;
			iq->RegisterMessageCallback(
				[](D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY sev,
				   D3D12_MESSAGE_ID id, LPCSTR desc, void* ctx)
				{
					if (sev > D3D12_MESSAGE_SEVERITY_WARNING)
						return;
					// D3D12_MESSAGE_SEVERITY の CORRUPTION / ERROR / WARNING は GfxMessageSeverity と同じ値
					static_cast<DX12DeviceImpl*>(ctx)->emitDebugMessage(
						static_cast<GfxMessageSeverity>(sev), static_cast<int32_t>(id), desc);
				},
				D3D12_MESSAGE_CALLBACK_FLAG_NONE, this, &cookie);
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
// シェーダーをコンパイル (または DXIL を登録) してハンドルを返す。失敗したら無効なハンドル
ShaderHandle DX12DeviceImpl::compileShader(const GfxShaderDesc* desc)
{
	if (!desc)
		return {};

	DX12ShadRes sh;
	sh.stage = desc->stage;

	if (desc->source)
	{
		const char* profile = desc->targetProfile ? desc->targetProfile : dx12DefaultShaderProfile(desc->stage);
		DX12ShaderOutput out;
		const bool ok = m_shaderCompiler.compile(*desc, profile, out);
		reportShaderMessages("compileShader", desc->sourceName, out.messages, !ok);
		if (!ok)
			return {};
		sh.bytecode     = std::move(out.bytecode);
		sh.dependencies = std::move(out.dependencies);
		if (out.reflection)
		{
			const uint8_t* r = static_cast<const uint8_t*>(out.reflection->GetBufferPointer());
			sh.reflection.assign(r, r + out.reflection->GetBufferSize());
		}
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

void DX12DeviceImpl::reportShaderMessages(const char* what, const char* name, const std::string& messages, bool failed)
{
	if (messages.empty() && !failed)
		return;
	std::string text = std::string(what) + (failed ? ": シェーダーのコンパイルに失敗しました" : ": シェーダーの警告");
	if (name && *name)
		text += std::string(" (") + name + ")";
	if (!messages.empty())
		text += "\n" + messages;
	emitDebugMessage(failed ? GfxMessageSeverity::Error : GfxMessageSeverity::Warning, GFX_MESSAGE_ID_SHADER_COMPILE, text.c_str());
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

// デフォルト RootSignature と PSO を作成する (旧経路の bindShader 用)
// シェーダーをコンパイルできない場合 (dxcompiler.dll がないなど) は RootSignature だけを作り、PSO は nullptr のままにする。
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

	// ---- HLSL ソース (DXC でコンパイルする) ----
	static const char* k_VS_SRC =
		"float4 main(uint id : SV_VertexID) : SV_Position {"
		"  float2 uv = float2((id & 1u) << 1u, id >> 1u);"
		"  return float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, 0.0f, 1.0f);"
		"}";

	static const char* k_PS_SRC =
		"float4 main() : SV_Target {"
		"  return float4(1.0f, 1.0f, 1.0f, 1.0f);"
		"}";

	auto compile = [&](const char* src, ShaderStage stage, DX12ShaderOutput& out) -> bool
	{
		GfxShaderDesc d = {};
		d.stage      = stage;
		d.source     = src;
		d.sourceName = "default_pso.hlsl";
		return m_shaderCompiler.compile(d, dx12DefaultShaderProfile(stage), out);
	};

	DX12ShaderOutput vsOut, psOut;
	if (!compile(k_VS_SRC, ShaderStage::Vertex, vsOut) || !compile(k_PS_SRC, ShaderStage::Fragment, psOut))
	{
		// コンパイル失敗は致命的にしない (旧経路の既定 PSO がないだけ)。理由は通知する
		reportShaderMessages("createDefaultPSO", nullptr, vsOut.messages.empty() ? psOut.messages : vsOut.messages, true);
		return true;
	}

	// ---- PSO 作成ヘルパー ----
	auto makePSO = [&](DXGI_FORMAT fmt, ComPtr<ID3D12PipelineState>& out)
	{
		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc               = {};
		psoDesc.pRootSignature                                    = m_defaultRootSig.Get();
		psoDesc.VS                                                = { vsOut.bytecode.data(), vsOut.bytecode.size() };
		psoDesc.PS                                                = { psOut.bytecode.data(), psOut.bytecode.size() };
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
