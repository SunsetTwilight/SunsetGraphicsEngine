// ============================================================
//  backends/dx12/DX12SwapChain.cpp
//  スワップチェーン
// ============================================================

#include "DX12Device.h"

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
// これまでに積んだコマンドがすべて終わるまで待つ。
// Signal の値は必ず単調に増やす (前に Signal した値以下だと、まだ終わっていないのに完了済みと判定される)
void DX12SwapChainImpl::waitForGPU()
{
	const uint64_t signal = ++m_fenceCounter;
	m_dev->cmdQueue()->Signal(m_fence.Get(), signal);
	if (m_fence->GetCompletedValue() < signal)
	{
		m_fence->SetEventOnCompletion(signal, m_fenceEvent);
		WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);
	}
	// どのバックバッファも、これで完了済みになっている (m_fenceValues[i] <= signal)
}

// 次フレームへ進む
void DX12SwapChainImpl::moveToNextFrame()
{
	// 現フレームの描画完了を GPU に通知する (このバックバッファはこの値の完了で再利用できる)
	const uint64_t signalValue = ++m_fenceCounter;
	m_fenceValues[m_frameIndex] = signalValue;
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
