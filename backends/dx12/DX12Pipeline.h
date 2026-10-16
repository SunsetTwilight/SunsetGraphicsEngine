#pragma once
// ============================================================
//  backends/dx12/DX12Pipeline.h
//  パイプライン (PSO + ルートシグネチャ) と旧経路の PSO キャッシュのキー
// ============================================================

#include "DX12Utils.h"

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
