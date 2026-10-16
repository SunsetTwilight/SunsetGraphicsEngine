#pragma once
// ============================================================
//  gfx/abi/Resources.h
//  ABI v2: テクスチャ、ビュー、サンプラーの記述子
//  ※ GfxBackendABI.h の一部。単独で include してもよい (前のパートを読み込む)
// ============================================================

#include "Commands.h"
// ============================================================
//  ABI v2: リソース / ビュー / サンプラー (Phase 2b-2)
//
//  テクスチャは createTextureResource で作り、TextureHandle (v1 と同じハンドル空間) を返す。
//  usage に ShaderResource があれば全体を覆う既定の SRV を、UnorderedAccess があれば
//  mip 0 の既定の UAV を作る (bindTexture / gfxSRV / gfxUAV でそのまま使える)。
//  サブリソースの範囲やフォーマットを変えたいとき、RTV / DSV が必要なときは createView を使う。
// ============================================================

constexpr uint32_t GFX_TEXTURE_RESOURCE_DESC_VERSION = 1u;
constexpr uint32_t GFX_VIEW_DESC_VERSION             = 1u;
constexpr uint32_t GFX_SAMPLER_STATE_DESC_VERSION    = 1u;
constexpr uint32_t GFX_ALL_MIPS                      = 0xFFFFFFFFu;  // mipLevels / arraySize = 残り全部

struct ViewHandle { uint64_t id = 0; bool valid() const { return id != 0; } };

// テクスチャの次元 (Cube は 2D の配列で、配列数は 6 の倍数)
#define GFX_ENUM_GfxTextureDimension(E) E(Texture1D, 0) E(Texture2D, 1) E(Texture3D, 2) E(TextureCube, 3)
GFX_DECLARE_ENUM(GfxTextureDimension, uint32_t)

// 使用フラグ (D3D12_RESOURCE_FLAGS に変換する。ShaderResource がないときは DENY_SHADER_RESOURCE)
#define GFX_ENUM_GfxTextureUsage(E) \
	E(None, 0) E(ShaderResource, 0x1) E(RenderTarget, 0x2) E(DepthStencil, 0x4) E(UnorderedAccess, 0x8)
GFX_DECLARE_FLAGS(GfxTextureUsage, uint32_t)

// D3D12_CLEAR_VALUE (最適化クリア値)
#define GFX_FIELDS_GfxClearValue(F, FA) \
	F(Enum,  GfxFormat, format,  GfxFormat::Unknown, 0, 0, "D3D12_CLEAR_VALUE::Format (Unknown = リソース / ビューのフォーマット)") \
	FA(Float, float,    color,   4,                        "D3D12_CLEAR_VALUE::Color") \
	F(Float, float,     depth,   1.0f, 0.0, 1.0,           "D3D12_CLEAR_VALUE::DepthStencil.Depth") \
	F(UInt8, uint8_t,   stencil, 0,    0, 255,             "D3D12_CLEAR_VALUE::DepthStencil.Stencil")
struct GfxClearValue
{
	GFX_FIELDS_GfxClearValue(GFX_DECLARE_FIELD, GFX_DECLARE_ARRAY)
};

// D3D12_RESOURCE_DESC (テクスチャ) + 作成時の状態
//  initialData は mip 0 / スライス 0 だけを転送する (MSAA / 深度には使えない)
#define GFX_FIELDS_GfxTextureResourceDesc(F, FA) \
	F(UInt32,  uint32_t,            structSize,       sizeof(GfxTextureResourceDesc),    0, 0xFFFFFFFFu, "記述子のバイト数 (sizeof)") \
	F(UInt32,  uint32_t,            version,          GFX_TEXTURE_RESOURCE_DESC_VERSION, 0, 0xFFFFFFFFu, "記述子のバージョン") \
	F(Enum,    GfxTextureDimension, dimension,        GfxTextureDimension::Texture2D,    0, 0, "D3D12_RESOURCE_DESC::Dimension") \
	F(UInt32,  uint32_t,            width,            1, 1, 16384,                       "D3D12_RESOURCE_DESC::Width") \
	F(UInt32,  uint32_t,            height,           1, 1, 16384,                       "D3D12_RESOURCE_DESC::Height") \
	F(UInt32,  uint32_t,            depthOrArraySize, 1, 1, 2048,                        "D3D12_RESOURCE_DESC::DepthOrArraySize (3D は奥行き、それ以外は配列数。Cube は 6 の倍数)") \
	F(UInt32,  uint32_t,            mipLevels,        1, 0, 15,                          "D3D12_RESOURCE_DESC::MipLevels (0 = 全段)") \
	F(Enum,    GfxFormat,           format,           GfxFormat::R8G8B8A8_Unorm,         0, 0, "D3D12_RESOURCE_DESC::Format (Typeless にするとビューで再解釈できる)") \
	F(UInt32,  uint32_t,            sampleCount,      1, 1, 32,                          "D3D12_RESOURCE_DESC::SampleDesc.Count") \
	F(UInt32,  uint32_t,            sampleQuality,    0, 0, 0xFFFFFFFFu,                 "D3D12_RESOURCE_DESC::SampleDesc.Quality") \
	F(Flags,   GfxTextureUsage,     usage,            GfxTextureUsage::ShaderResource,   0, 0, "D3D12_RESOURCE_DESC::Flags") \
	F(Bool,    bool,                useClearValue,    false, 0, 1,                       "pOptimizedClearValue を渡すか (RenderTarget / DepthStencil のみ)") \
	F(Struct,  GfxClearValue,       clearValue,       {},    0, 0,                       "CreateCommittedResource::pOptimizedClearValue") \
	F(Flags,   GfxResourceState,    initialState,     GfxResourceState::Tracked, 0, 0,   "作成時の状態 (Tracked = 用途から自動で決める)") \
	F(Pointer, const void*,         initialData,      nullptr, 0, 0,                     "mip 0 / スライス 0 の初期データ (シリアライズ対象外)") \
	F(UInt32,  uint32_t,            rowPitch,         0, 0, 0xFFFFFFFFu,                 "initialData の 1 行のバイト数 (0 = width * ピクセルサイズ)") \
	F(CString, const char*,         debugName,        nullptr, 0, 0,                     "ID3D12Object::SetName に渡す名前 (シリアライズ対象外)")
struct GfxTextureResourceDesc
{
	GFX_FIELDS_GfxTextureResourceDesc(GFX_DECLARE_FIELD, GFX_DECLARE_ARRAY)
};

#define GFX_ENUM_GfxViewType(E) E(SRV, 0) E(UAV, 1) E(RTV, 2) E(DSV, 3)
GFX_DECLARE_ENUM(GfxViewType, uint32_t)

// D3D12_SRV_DIMENSION 等に対応する (値は API 非依存。バックエンドで変換する)
#define GFX_ENUM_GfxViewDimension(E) \
	E(Buffer, 0) E(Texture1D, 1) E(Texture1DArray, 2) E(Texture2D, 3) E(Texture2DArray, 4) \
	E(Texture2DMS, 5) E(Texture2DMSArray, 6) E(Texture3D, 7) E(TextureCube, 8) E(TextureCubeArray, 9)
GFX_DECLARE_ENUM(GfxViewDimension, uint32_t)

// D3D12_DSV_FLAGS
#define GFX_ENUM_GfxDSVFlags(E) E(None, 0) E(ReadOnlyDepth, 0x1) E(ReadOnlyStencil, 0x2)
GFX_DECLARE_FLAGS(GfxDSVFlags, uint32_t)

// D3D12_SHADER_RESOURCE_VIEW_DESC / D3D12_UNORDERED_ACCESS_VIEW_DESC /
// D3D12_RENDER_TARGET_VIEW_DESC / D3D12_DEPTH_STENCIL_VIEW_DESC をまとめた記述子
//  使うフィールドは type と dimension で決まる:
//    mip       : SRV は mostDetailedMip + mipLevels、UAV / RTV / DSV は mostDetailedMip を MipSlice として使う
//    配列      : firstArraySlice + arraySize (3D の UAV / RTV では FirstWSlice + WSize)
//    Buffer    : firstElement + numElements + structureByteStride (0 なら format の型付き、rawBuffer で ByteAddress)
#define GFX_FIELDS_GfxViewDesc(F, FA) \
	F(UInt32,  uint32_t,         structSize,          sizeof(GfxViewDesc),         0, 0xFFFFFFFFu, "記述子のバイト数 (sizeof)") \
	F(UInt32,  uint32_t,         version,             GFX_VIEW_DESC_VERSION,       0, 0xFFFFFFFFu, "記述子のバージョン") \
	F(Enum,    GfxViewType,      type,                GfxViewType::SRV,            0, 0, "ビューの種類") \
	F(Struct,  GfxResourceRef,   resource,            {},                          0, 0, "対象のリソース (Texture / Buffer / RenderTarget / DepthStencil)") \
	F(Enum,    GfxViewDimension, dimension,           GfxViewDimension::Texture2D, 0, 0, "*_VIEW_DESC::ViewDimension") \
	F(Enum,    GfxFormat,        format,              GfxFormat::Unknown,          0, 0, "*_VIEW_DESC::Format (Unknown = リソースのフォーマット。Typeless のリソースは必須)") \
	F(UInt32,  uint32_t,         mostDetailedMip,     0, 0, 15,                    "Texture*.MostDetailedMip (SRV) / MipSlice (UAV / RTV / DSV)") \
	F(UInt32,  uint32_t,         mipLevels,           GFX_ALL_MIPS, 0, 0xFFFFFFFFu, "Texture*.MipLevels (SRV のみ。0xFFFFFFFF = 残り全部)") \
	F(UInt32,  uint32_t,         firstArraySlice,     0, 0, 2047,                  "Texture*Array.FirstArraySlice / Texture3D.FirstWSlice / TextureCubeArray.First2DArrayFace") \
	F(UInt32,  uint32_t,         arraySize,           GFX_ALL_MIPS, 0, 0xFFFFFFFFu, "Texture*Array.ArraySize / Texture3D.WSize / TextureCubeArray.NumCubes (0xFFFFFFFF = 残り全部)") \
	F(UInt32,  uint32_t,         planeSlice,          0, 0, 1,                     "Texture2D*.PlaneSlice") \
	F(Float,   float,            minLODClamp,         0.0f, 0.0, 15.0,             "Texture*.ResourceMinLODClamp (SRV のみ)") \
	F(Flags,   GfxDSVFlags,      dsvFlags,            GfxDSVFlags::None,           0, 0, "D3D12_DEPTH_STENCIL_VIEW_DESC::Flags") \
	F(UInt32,  uint32_t,         firstElement,        0, 0, 0xFFFFFFFFu,           "Buffer.FirstElement") \
	F(UInt32,  uint32_t,         numElements,         0, 0, 0xFFFFFFFFu,           "Buffer.NumElements") \
	F(UInt32,  uint32_t,         structureByteStride, 0, 0, 2048,                  "Buffer.StructureByteStride (0 = 型付き / raw)") \
	F(Bool,    bool,             rawBuffer,           false, 0, 1,                 "D3D12_BUFFER_SRV_FLAG_RAW / D3D12_BUFFER_UAV_FLAG_RAW (format は R32_Typeless)") \
	F(CString, const char*,      debugName,           nullptr, 0, 0,               "デバッグ用の名前 (シリアライズ対象外)")
struct GfxViewDesc
{
	GFX_FIELDS_GfxViewDesc(GFX_DECLARE_FIELD, GFX_DECLARE_ARRAY)
};

// D3D12_SAMPLER_DESC (全メンバー)
#define GFX_FIELDS_GfxSamplerStateDesc(F, FA) \
	F(UInt32,  uint32_t,              structSize,     sizeof(GfxSamplerStateDesc),    0, 0xFFFFFFFFu, "記述子のバイト数 (sizeof)") \
	F(UInt32,  uint32_t,              version,        GFX_SAMPLER_STATE_DESC_VERSION, 0, 0xFFFFFFFFu, "記述子のバージョン") \
	F(Enum,    GfxFilter,             filter,         GfxFilter::MinMagMipLinear,     0, 0, "D3D12_SAMPLER_DESC::Filter") \
	F(Enum,    GfxTextureAddressMode, addressU,       GfxTextureAddressMode::Wrap,    0, 0, "D3D12_SAMPLER_DESC::AddressU") \
	F(Enum,    GfxTextureAddressMode, addressV,       GfxTextureAddressMode::Wrap,    0, 0, "D3D12_SAMPLER_DESC::AddressV") \
	F(Enum,    GfxTextureAddressMode, addressW,       GfxTextureAddressMode::Wrap,    0, 0, "D3D12_SAMPLER_DESC::AddressW") \
	F(Float,   float,                 mipLODBias,     0.0f,  -16.0, 15.99,            "D3D12_SAMPLER_DESC::MipLODBias") \
	F(UInt32,  uint32_t,              maxAnisotropy,  1,     1, 16,                   "D3D12_SAMPLER_DESC::MaxAnisotropy") \
	F(Enum,    GfxCompareFunc,        comparisonFunc, GfxCompareFunc::Never,          0, 0, "D3D12_SAMPLER_DESC::ComparisonFunc (Comparison* フィルターのとき有効)") \
	FA(Float,  float,                 borderColor,    4,                                    "D3D12_SAMPLER_DESC::BorderColor (Border のとき有効)") \
	F(Float,   float,                 minLOD,         0.0f,            0, GFX_FLOAT32_MAX, "D3D12_SAMPLER_DESC::MinLOD") \
	F(Float,   float,                 maxLOD,         GFX_FLOAT32_MAX, 0, GFX_FLOAT32_MAX, "D3D12_SAMPLER_DESC::MaxLOD") \
	F(CString, const char*,           debugName,      nullptr, 0, 0,                   "デバッグ用の名前 (シリアライズ対象外)")
struct GfxSamplerStateDesc
{
	GFX_FIELDS_GfxSamplerStateDesc(GFX_DECLARE_FIELD, GFX_DECLARE_ARRAY)
};

inline GfxDescriptor gfxView(ViewHandle h) { GfxDescriptor d; d.type = GfxDescriptorType::View; d.handle = h.id; return d; }
