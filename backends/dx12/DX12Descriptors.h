#pragma once
// ============================================================
//  backends/dx12/DX12Descriptors.h
//  ディスクリプタヒープとフェンス付きの一時ディスクリプタリング
// ============================================================

#include "DX12Common.h"

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
