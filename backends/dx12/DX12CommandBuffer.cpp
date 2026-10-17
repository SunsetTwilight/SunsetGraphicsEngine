// ============================================================
//  backends/dx12/DX12CommandBuffer.cpp
//  コマンドバッファの記録 (v1 と v2 のコマンド、状態追跡)
// ============================================================

#include "DX12Device.h"

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
	m_readbacks.clear();

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
	if (!buf || !buf->res || buf->upload || buf->readback || !isDirect())   // READBACK は COPY_DEST 固定
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
	if (s->readback)
	{
		fprintf(stderr, "[GfxEngine] copyBuffer: Readback のバッファはコピー元にできません\n");
		return;
	}
	if (d->readback)
		m_readbacks.push_back(dst.id);   // submit のフェンス値を記録する (readBuffer で完了を確かめる)
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
