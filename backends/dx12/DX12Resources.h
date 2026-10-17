#pragma once
// ============================================================
//  backends/dx12/DX12Resources.h
//  RenderTarget / DepthStencil と、デバイスが管理するリソースの補助構造体
// ============================================================

#include "DX12Utils.h"

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
	bool        readback      = false;   // READBACK ヒープ (COPY_DEST 固定、常時 Map)
	uint64_t    readFence     = 0;       // 最後にこのバッファへコピーした submit のフェンス値 (readback のみ)
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
	std::vector<uint8_t>     bytecode;      // DXIL コンテナ (リフレクションは外してある)
	ShaderStage              stage = ShaderStage::Vertex;
	std::vector<std::string> dependencies;  // #include で読み込んだファイル
	std::vector<uint8_t>     reflection;    // リフレクションのパート (空なら bytecode から作る)
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
