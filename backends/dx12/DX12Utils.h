#pragma once
// ============================================================
//  backends/dx12/DX12Utils.h
//  フォーマット変換と、複数のソースから使う小さなヘルパー
//  (元の DX12Backend.cpp の static 関数を inline にしたもの)
// ============================================================

#include "DX12Common.h"

// ============================================================
//  ヘルパー関数
// ============================================================

// PixelFormat → DXGI_FORMAT 変換
inline DXGI_FORMAT toDXGI(PixelFormat fmt)
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
inline bool isDepthFormat(PixelFormat fmt)
{
	return fmt == PixelFormat::D32_FLOAT
		|| fmt == PixelFormat::D24_UNORM_S8_UINT
		|| fmt == PixelFormat::D16_UNORM;
}

// VertexAttribute 列挙型 → HLSL セマンティクス文字列・インデックス変換テーブル
// エンジン外から VertexAttribute 列挙値で指定し、エンジン内でここで文字列に変換する。
struct SemanticInfo { const char* name; uint32_t index; };
inline const SemanticInfo k_SemanticTable[] =
{
	{ "POSITION", 0 },  // VertexAttribute::Position
	{ "NORMAL",   0 },  // VertexAttribute::Normal
	{ "TANGENT",  0 },  // VertexAttribute::Tangent
	{ "TEXCOORD", 0 },  // VertexAttribute::TexCoord0
	{ "TEXCOORD", 1 },  // VertexAttribute::TexCoord1
	{ "COLOR",    0 },  // VertexAttribute::Color0
};
inline constexpr uint32_t k_SemanticTableCount =
	sizeof(k_SemanticTable) / sizeof(k_SemanticTable[0]);

// VertexAttribute + PixelFormat から D3D12_INPUT_ELEMENT_DESC を生成するヘルパー
inline void vertexElementToD3D12(
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
inline uint32_t buildInputLayout(
	const GfxVertexLayout&      layout,
	D3D12_INPUT_ELEMENT_DESC    outElems[GfxVertexLayout::MAX_ELEMENTS])
{
	if (layout.stride == 0) return 0;  // stride=0 は InputLayout なし
	uint32_t count = 0;
	for (uint32_t i = 0; i < layout.elementCount; ++i)
		vertexElementToD3D12(layout.elements[i], outElems[count++]);
	return count;
}

// フォーマットの 1 ピクセルあたりのバイト数 (テクスチャアップロード用)
inline uint32_t bytesPerPixel(PixelFormat fmt)
{
	switch (fmt)
	{
	case PixelFormat::RGBA16_FLOAT: return 8;
	case PixelFormat::D16_UNORM:    return 2;
	default:                        return 4;   // RGBA8 / BGRA8 / R32 / D32 / D24S8
	}
}

inline bool hasStencil(PixelFormat fmt)
{
	return fmt == PixelFormat::D24_UNORM_S8_UINT;
}

inline bool hasStencilDXGI(DXGI_FORMAT fmt)
{
	return fmt == DXGI_FORMAT_D24_UNORM_S8_UINT || fmt == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
}

// シェーダーから読み取る状態 (PS / 非 PS の両方)
inline constexpr D3D12_RESOURCE_STATES k_ShaderReadState =
	D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

// D3D12 オブジェクトにデバッグ名を付ける (PIX / デバッグレイヤーの表示用)
inline void setDebugName(ID3D12Object* obj, const char* name)
{
	if (!obj || !name) return;
	wchar_t wname[128] = {};
	MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 128);
	obj->SetName(wname);
}

// バッファリソースを 1 つ作成する
inline ComPtr<ID3D12Resource> createBufferResource(
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
inline bool createTransientCmdList(
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
inline void executeAndWait(
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

// structSize / version 付きの記述子を読む。
// 呼び出し側の structSize が小さければ (古いヘッダでビルドされたアプリ) 足りない分は既定値のまま、
// 大きければ (新しいヘッダ) 知らない末尾のフィールドを無視する。
template<typename T>
inline bool readVersionedDesc(const T* src, T& out, const char* fn)
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

// 深度フォーマットを、リソース (Typeless) / DSV / SRV のフォーマットに分ける
struct DX12DepthFormats { DXGI_FORMAT resource, dsv, srv; };
inline DX12DepthFormats splitDepthFormat(DXGI_FORMAT f)
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

inline bool isDepthDXGI(DXGI_FORMAT f)
{
	return f == DXGI_FORMAT_D32_FLOAT || f == DXGI_FORMAT_D24_UNORM_S8_UINT
		|| f == DXGI_FORMAT_D16_UNORM || f == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
}

// 初期データの転送に使う 1 ピクセルのバイト数 (ブロック圧縮などは未対応 = 0)
inline uint32_t dxgiBytesPerPixel(DXGI_FORMAT f)
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
