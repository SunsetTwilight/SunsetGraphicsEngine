#pragma once
// ============================================================
//  gfx/abi/Commands.h
//  ABI v2: Compute パイプライン、クエリ、バリア、動的ステート、間接描画、ディスクリプタ
//  ※ GfxBackendABI.h の一部。単独で include してもよい (前のパートを読み込む)
// ============================================================

#include "PipelineState.h"
// ============================================================
//  ABI v2: Compute パイプライン / クエリ / 明示的なコマンド (Phase 2b)
// ============================================================

constexpr uint32_t GFX_COMPUTE_PIPELINE_DESC_VERSION = 1u;
constexpr uint32_t GFX_QUERY_POOL_DESC_VERSION       = 1u;
constexpr uint32_t GFX_ALL_SUBRESOURCES              = 0xFFFFFFFFu;  // D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES

struct QueryPoolHandle { uint64_t id = 0; bool valid() const { return id != 0; } };

struct IRenderTarget;
struct IDepthStencil;

// D3D12_COMPUTE_PIPELINE_STATE_DESC (CachedPSO は未対応)
#define GFX_FIELDS_GfxComputePipelineDesc(F, FA) \
	F(UInt32,  uint32_t,              structSize,    sizeof(GfxComputePipelineDesc),    0, 0xFFFFFFFFu, "記述子のバイト数 (sizeof)") \
	F(UInt32,  uint32_t,              version,       GFX_COMPUTE_PIPELINE_DESC_VERSION, 0, 0xFFFFFFFFu, "記述子のバージョン") \
	F(Handle,  BindingLayoutHandle,   bindingLayout, {},      0, 0, "D3D12_COMPUTE_PIPELINE_STATE_DESC::pRootSignature") \
	F(Handle,  ShaderHandle,          cs,            {},      0, 0, "D3D12_COMPUTE_PIPELINE_STATE_DESC::CS") \
	F(UInt32,  uint32_t,              nodeMask,      0,       0, 0xFFFFFFFFu, "D3D12_COMPUTE_PIPELINE_STATE_DESC::NodeMask") \
	F(Flags,   GfxPipelineStateFlags, flags,         GfxPipelineStateFlags::None, 0, 0, "D3D12_COMPUTE_PIPELINE_STATE_DESC::Flags") \
	F(CString, const char*,           debugName,     nullptr, 0, 0, "ID3D12Object::SetName に渡す名前 (シリアライズ対象外)")
struct GfxComputePipelineDesc
{
	GFX_FIELDS_GfxComputePipelineDesc(GFX_DECLARE_FIELD, GFX_DECLARE_ARRAY)
};

// ---- クエリ ------------------------------------------------

// D3D12_QUERY_HEAP_TYPE 相当 (Occlusion / BinaryOcclusion は同じ OCCLUSION ヒープを使う)
#define GFX_ENUM_GfxQueryType(E) E(Timestamp, 0) E(Occlusion, 1) E(BinaryOcclusion, 2)
GFX_DECLARE_ENUM(GfxQueryType, uint32_t)

// D3D12_QUERY_HEAP_DESC
//  結果は 1 クエリにつき uint64_t。resolveQueries で記録した値を readQueryResults で読む。
#define GFX_FIELDS_GfxQueryPoolDesc(F, FA) \
	F(UInt32,  uint32_t,     structSize, sizeof(GfxQueryPoolDesc),    0, 0xFFFFFFFFu, "記述子のバイト数 (sizeof)") \
	F(UInt32,  uint32_t,     version,    GFX_QUERY_POOL_DESC_VERSION, 0, 0xFFFFFFFFu, "記述子のバージョン") \
	F(Enum,    GfxQueryType, type,       GfxQueryType::Timestamp,     0, 0, "D3D12_QUERY_HEAP_DESC::Type") \
	F(UInt32,  uint32_t,     count,      0,  0, 65536,                "D3D12_QUERY_HEAP_DESC::Count") \
	F(CString, const char*,  debugName,  nullptr, 0, 0,               "ID3D12Object::SetName に渡す名前 (シリアライズ対象外)")
struct GfxQueryPoolDesc
{
	GFX_FIELDS_GfxQueryPoolDesc(GFX_DECLARE_FIELD, GFX_DECLARE_ARRAY)
};

// ---- リソースの参照 (バリア / コピーの対象) ----------------

#define GFX_ENUM_GfxResourceKind(E) \
	E(None, 0) E(Texture, 1) E(Buffer, 2) E(RenderTarget, 3) E(DepthStencil, 4)
GFX_DECLARE_ENUM(GfxResourceKind, uint32_t)

//  handle は kind に応じて TextureHandle.id / BufferHandle.id / IRenderTarget* / IDepthStencil*
#define GFX_FIELDS_GfxResourceRef(F, FA) \
	F(Enum,   GfxResourceKind, kind,   GfxResourceKind::None, 0, 0, "参照するリソースの種類") \
	F(UInt32, uint32_t,        pad,    0,                     0, 0, "(予約)") \
	F(Handle, uint64_t,        handle, 0,                     0, 0, "TextureHandle.id / BufferHandle.id / IRenderTarget* / IDepthStencil*")
struct GfxResourceRef
{
	GFX_FIELDS_GfxResourceRef(GFX_DECLARE_FIELD, GFX_DECLARE_ARRAY)
};
inline GfxResourceRef gfxRef(TextureHandle h)  { GfxResourceRef r; r.kind = GfxResourceKind::Texture;      r.handle = h.id; return r; }
inline GfxResourceRef gfxRef(BufferHandle h)   { GfxResourceRef r; r.kind = GfxResourceKind::Buffer;       r.handle = h.id; return r; }
inline GfxResourceRef gfxRef(IRenderTarget* p) { GfxResourceRef r; r.kind = GfxResourceKind::RenderTarget; r.handle = reinterpret_cast<uint64_t>(p); return r; }
inline GfxResourceRef gfxRef(IDepthStencil* p) { GfxResourceRef r; r.kind = GfxResourceKind::DepthStencil; r.handle = reinterpret_cast<uint64_t>(p); return r; }

// ---- バリア ------------------------------------------------

// D3D12_RESOURCE_STATES (ビットフラグ)
#define GFX_ENUM_GfxResourceState(E) \
	E(Common, 0) E(VertexAndConstantBuffer, 0x1) E(IndexBuffer, 0x2) E(RenderTarget, 0x4) \
	E(UnorderedAccess, 0x8) E(DepthWrite, 0x10) E(DepthRead, 0x20) \
	E(NonPixelShaderResource, 0x40) E(PixelShaderResource, 0x80) E(StreamOut, 0x100) \
	E(IndirectArgument, 0x200) E(CopyDest, 0x400) E(CopySource, 0x800) \
	E(ResolveDest, 0x1000) E(ResolveSource, 0x2000) \
	E(RaytracingAccelerationStructure, 0x400000) E(ShadingRateSource, 0x1000000) \
	E(AllShaderResource, 0xC0) E(GenericRead, 0xAC3) \
	E(Tracked, 0xFFFFFFFF)
GFX_DECLARE_FLAGS(GfxResourceState, uint32_t)

// D3D12_RESOURCE_BARRIER_TYPE (Aliasing はプレースドリソースの対応後に追加する)
#define GFX_ENUM_GfxBarrierType(E) E(Transition, 0) E(UAV, 2)
GFX_DECLARE_ENUM(GfxBarrierType, uint32_t)

// D3D12_RESOURCE_BARRIER
//  Transition:
//    before = Tracked のとき、状態追跡が知っている状態から after へ遷移する (自動追跡と同じ)。
//    before を明示したとき、まず追跡を before に合わせ (必要なら自動でバリアを足す)、
//    before → after のバリアを記録して追跡を after に上書きする。
//    subresource を指定したバリアは追跡を更新しない。次に自動追跡で使う前に、
//    同じサブリソースを元の状態へ戻すバリアを記録すること (before の明示が必要)。
//  UAV: resource.kind = None なら全 UAV アクセスに対するバリア。
//  バッファの状態は CB の中だけで追跡する (CB の開始時は Common、end() で Common に戻す)。
#define GFX_FIELDS_GfxBarrierDesc(F, FA) \
	F(Enum,   GfxBarrierType,   type,        GfxBarrierType::Transition, 0, 0, "D3D12_RESOURCE_BARRIER::Type") \
	F(Struct, GfxResourceRef,   resource,    {},                         0, 0, "D3D12_RESOURCE_TRANSITION_BARRIER::pResource / D3D12_RESOURCE_UAV_BARRIER::pResource") \
	F(Flags,  GfxResourceState, before,      GfxResourceState::Tracked,  0, 0, "D3D12_RESOURCE_TRANSITION_BARRIER::StateBefore (Tracked = 状態追跡の値を使う)") \
	F(Flags,  GfxResourceState, after,       GfxResourceState::Common,   0, 0, "D3D12_RESOURCE_TRANSITION_BARRIER::StateAfter") \
	F(UInt32, uint32_t,         subresource, GFX_ALL_SUBRESOURCES,       0, 0xFFFFFFFFu, "D3D12_RESOURCE_TRANSITION_BARRIER::Subresource (0xFFFFFFFF = 全サブリソース)")
struct GfxBarrierDesc
{
	GFX_FIELDS_GfxBarrierDesc(GFX_DECLARE_FIELD, GFX_DECLARE_ARRAY)
};
inline GfxBarrierDesc gfxTransition(GfxResourceRef res, GfxResourceState after,
                                    GfxResourceState before = GfxResourceState::Tracked,
                                    uint32_t subresource = GFX_ALL_SUBRESOURCES)
{
	GfxBarrierDesc b;
	b.type = GfxBarrierType::Transition; b.resource = res; b.before = before; b.after = after; b.subresource = subresource;
	return b;
}
inline GfxBarrierDesc gfxUAVBarrier(GfxResourceRef res = {})
{
	GfxBarrierDesc b;
	b.type = GfxBarrierType::UAV; b.resource = res;
	return b;
}

// ---- 動的ステート ------------------------------------------

// D3D12_VIEWPORT
#define GFX_FIELDS_GfxViewport(F, FA) \
	F(Float, float, x,        0.0f, -32768.0, 32767.0, "D3D12_VIEWPORT::TopLeftX") \
	F(Float, float, y,        0.0f, -32768.0, 32767.0, "D3D12_VIEWPORT::TopLeftY") \
	F(Float, float, width,    0.0f, 0.0, 32767.0,      "D3D12_VIEWPORT::Width") \
	F(Float, float, height,   0.0f, 0.0, 32767.0,      "D3D12_VIEWPORT::Height") \
	F(Float, float, minDepth, 0.0f, 0.0, 1.0,          "D3D12_VIEWPORT::MinDepth") \
	F(Float, float, maxDepth, 1.0f, 0.0, 1.0,          "D3D12_VIEWPORT::MaxDepth")
struct GfxViewport
{
	GFX_FIELDS_GfxViewport(GFX_DECLARE_FIELD, GFX_DECLARE_ARRAY)
};

// D3D12_RECT
#define GFX_FIELDS_GfxRect(F, FA) \
	F(Int32, int32_t, left,   0, -2147483647 - 1, 2147483647, "D3D12_RECT::left") \
	F(Int32, int32_t, top,    0, -2147483647 - 1, 2147483647, "D3D12_RECT::top") \
	F(Int32, int32_t, right,  0, -2147483647 - 1, 2147483647, "D3D12_RECT::right") \
	F(Int32, int32_t, bottom, 0, -2147483647 - 1, 2147483647, "D3D12_RECT::bottom")
struct GfxRect
{
	GFX_FIELDS_GfxRect(GFX_DECLARE_FIELD, GFX_DECLARE_ARRAY)
};

// D3D12_BOX (コピー元の範囲)
struct GfxBox
{
	uint32_t left = 0, top = 0, front = 0, right = 0, bottom = 0, back = 0;
};

// D3D12_CLEAR_FLAGS
#define GFX_ENUM_GfxClearFlags(E) E(None, 0) E(Depth, 0x1) E(Stencil, 0x2)
GFX_DECLARE_FLAGS(GfxClearFlags, uint32_t)

// ---- 頂点 / インデックスバッファ ----------------------------

// D3D12_VERTEX_BUFFER_VIEW
struct GfxVertexBufferBinding
{
	BufferHandle buffer;
	uint64_t     offset = 0;   // バッファ先頭からのバイト数
	uint32_t     size   = 0;   // 0 = offset から末尾まで
	uint32_t     stride = 0;   // 0 = バッファ作成時の stride
};

// D3D12_INDEX_BUFFER_VIEW (format は R16_Uint / R32_Uint)
struct GfxIndexBufferBinding
{
	BufferHandle buffer;
	uint64_t     offset = 0;
	uint32_t     size   = 0;   // 0 = offset から末尾まで
	GfxFormat    format = GfxFormat::R32_Uint;
};

// ---- 間接実行 ----------------------------------------------

// 引数バッファの中身 (D3D12_DRAW_ARGUMENTS / D3D12_DRAW_INDEXED_ARGUMENTS / D3D12_DISPATCH_ARGUMENTS と同じ配置)
struct GfxDrawArguments        { uint32_t vertexCountPerInstance, instanceCount, startVertexLocation, startInstanceLocation; };
struct GfxDrawIndexedArguments { uint32_t indexCountPerInstance, instanceCount, startIndexLocation; int32_t baseVertexLocation; uint32_t startInstanceLocation; };
struct GfxDispatchArguments    { uint32_t threadGroupCountX, threadGroupCountY, threadGroupCountZ; };

// ExecuteIndirect の引数
//  countBuffer が有効なら、実行数は min(maxCommandCount, countBuffer の uint32)
struct GfxIndirectDesc
{
	BufferHandle argumentBuffer;
	uint64_t     argumentOffset  = 0;
	uint32_t     maxCommandCount = 1;
	uint32_t     reserved        = 0;
	BufferHandle countBuffer;
	uint64_t     countOffset     = 0;
};

// ---- ディスクリプタテーブルの中身 --------------------------

#define GFX_ENUM_GfxDescriptorType(E) \
	E(NullSRV, 0) E(NullUAV, 1) E(NullCBV, 2) \
	E(TextureSRV, 3) E(TextureUAV, 4) E(BufferCBV, 5) E(Sampler, 6) E(View, 7)
GFX_DECLARE_ENUM(GfxDescriptorType, uint32_t)

//  setDescriptorTable に渡す 1 個分。handle は type に応じて
//  TextureHandle.id (asTexture() の値も可) / BufferHandle.id (Uniform) / SamplerHandle.id /
//  ViewHandle.id (SRV / UAV のビュー)
//  Sampler とそれ以外は同じテーブルに混在できない (ヒープが異なる)。
struct GfxDescriptor
{
	GfxDescriptorType type   = GfxDescriptorType::NullSRV;
	uint32_t          reserved = 0;
	uint64_t          handle = 0;
};
inline GfxDescriptor gfxSRV(TextureHandle h)     { GfxDescriptor d; d.type = GfxDescriptorType::TextureSRV; d.handle = h.id; return d; }
inline GfxDescriptor gfxUAV(TextureHandle h)     { GfxDescriptor d; d.type = GfxDescriptorType::TextureUAV; d.handle = h.id; return d; }
inline GfxDescriptor gfxCBV(BufferHandle h)      { GfxDescriptor d; d.type = GfxDescriptorType::BufferCBV;  d.handle = h.id; return d; }
inline GfxDescriptor gfxSamplerDesc(SamplerHandle h) { GfxDescriptor d; d.type = GfxDescriptorType::Sampler; d.handle = h.id; return d; }
