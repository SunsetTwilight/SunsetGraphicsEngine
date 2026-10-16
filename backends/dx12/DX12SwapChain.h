#pragma once
// ============================================================
//  backends/dx12/DX12SwapChain.h
//  スワップチェーン
// ============================================================

#include "DX12Resources.h"

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
