#pragma once
// ============================================================
//  gfx/abi/PipelineState.h
//  ABI v2: パイプラインステートとバインディングレイアウト (X マクロによる記述子定義)
//  ※ GfxBackendABI.h の一部。単独で include してもよい (前のパートを読み込む)
// ============================================================

#include "Types.h"
// ============================================================
//  ABI v2 記述子 (パイプラインステート / バインディングレイアウト)
//
//  ・D3D12 の構造体 (D3D12_GRAPHICS_PIPELINE_STATE_DESC / D3D12_ROOT_SIGNATURE_DESC1)
//    のメンバーと 1 対 1 で対応させる。列挙値も D3D12 の値と同じ (名前は API 非依存)。
//  ・トップレベルの記述子 (GfxGraphicsPipelineDesc / GfxBindingLayoutDesc) は
//    先頭に structSize と version を持つ。バックエンドは structSize までしか読まないので、
//    フィールドは必ず末尾に追加すること (古い DLL は知らないフィールドを無視できる)。
//  ・入れ子の構造体 (ブレンドなど) はトップレベルの version で管理する。
//  ・可変長の配列は固定長配列 + 個数で持つ (ポインタを持たないので、そのままシリアライズできる)。
//
//  構造体の定義は GFX_FIELDS_<型名>(F, FA) の X マクロで書く。
//    F (kind, type, name, default, min, max, tooltip)  … スカラーのフィールド
//    FA(kind, type, name, count, tooltip)             … 固定長配列のフィールド
//  同じマクロから GfxSchema.h がフィールドのメタデータ表 (名前・型・オフセット・範囲) を生成する。
//  列挙型も GFX_ENUM_<型名>(E) で書き、列挙子の名前表を同様に生成する。
// ============================================================

#define GFX_DECLARE_ENUMERATOR(name, value)                  name = value,
#define GFX_DECLARE_FIELD(kind, type, name, def, mn, mx, tip) type name = def;
#define GFX_DECLARE_ARRAY(kind, type, name, count, tip)      type name[count] = {};

#define GFX_DECLARE_ENUM(Type, Underlying) \
	enum class Type : Underlying { GFX_ENUM_##Type(GFX_DECLARE_ENUMERATOR) };

// ビットフラグの列挙型 (| & ~ を使えるようにする)
#define GFX_DECLARE_FLAGS(Type, Underlying)                                              \
	GFX_DECLARE_ENUM(Type, Underlying)                                                   \
	constexpr Type operator|(Type a, Type b) { return static_cast<Type>(static_cast<Underlying>(a) | static_cast<Underlying>(b)); } \
	constexpr Type operator&(Type a, Type b) { return static_cast<Type>(static_cast<Underlying>(a) & static_cast<Underlying>(b)); } \
	constexpr Type operator~(Type a)         { return static_cast<Type>(static_cast<Underlying>(~static_cast<Underlying>(a))); }   \
	constexpr Type& operator|=(Type& a, Type b) { return a = a | b; }

constexpr uint32_t GFX_MAX_RENDER_TARGETS         = 8;
constexpr uint32_t GFX_MAX_INPUT_ELEMENTS         = 16;
constexpr uint32_t GFX_MAX_ROOT_PARAMETERS        = 16;
constexpr uint32_t GFX_MAX_DESCRIPTOR_RANGES      = 32;
constexpr uint32_t GFX_MAX_STATIC_SAMPLERS        = 16;
constexpr uint32_t GFX_MAX_SEMANTIC_NAME          = 32;     // 終端の '\0' を含む
constexpr uint32_t GFX_APPEND_ALIGNED_ELEMENT     = 0xFFFFFFFFu;  // D3D12_APPEND_ALIGNED_ELEMENT
constexpr uint32_t GFX_DESCRIPTOR_RANGE_OFFSET_APPEND = 0xFFFFFFFFu;  // D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
constexpr uint32_t GFX_UNBOUNDED_DESCRIPTOR_COUNT = 0xFFFFFFFFu;  // NumDescriptors = UINT_MAX
constexpr float    GFX_FLOAT32_MAX                = 3.402823466e+38f;

constexpr uint32_t GFX_GRAPHICS_PIPELINE_DESC_VERSION = 1u;
constexpr uint32_t GFX_BINDING_LAYOUT_DESC_VERSION    = 1u;

struct BindingLayoutHandle { uint64_t id = 0; bool valid() const { return id != 0; } };

// ---- 列挙型 ------------------------------------------------

// DXGI_FORMAT と同じ値 (頂点フォーマットと RTV / DSV フォーマット用)
#define GFX_ENUM_GfxFormat(E) \
	E(Unknown, 0) \
	E(R32G32B32A32_Float, 2) E(R32G32B32A32_Uint, 3) E(R32G32B32A32_Sint, 4) \
	E(R32G32B32_Float, 6)    E(R32G32B32_Uint, 7)    E(R32G32B32_Sint, 8) \
	E(R16G16B16A16_Float, 10) E(R16G16B16A16_Unorm, 11) E(R16G16B16A16_Uint, 12) \
	E(R16G16B16A16_Snorm, 13) E(R16G16B16A16_Sint, 14) \
	E(R32G32_Float, 16) E(R32G32_Uint, 17) E(R32G32_Sint, 18) \
	E(D32_Float_S8X24_Uint, 20) \
	E(R10G10B10A2_Unorm, 24) E(R10G10B10A2_Uint, 25) E(R11G11B10_Float, 26) \
	E(R8G8B8A8_Unorm, 28) E(R8G8B8A8_Unorm_sRGB, 29) E(R8G8B8A8_Uint, 30) \
	E(R8G8B8A8_Snorm, 31) E(R8G8B8A8_Sint, 32) \
	E(R16G16_Float, 34) E(R16G16_Unorm, 35) E(R16G16_Uint, 36) E(R16G16_Snorm, 37) E(R16G16_Sint, 38) \
	E(D32_Float, 40) E(R32_Float, 41) E(R32_Uint, 42) E(R32_Sint, 43) \
	E(D24_Unorm_S8_Uint, 45) \
	E(R8G8_Unorm, 49) E(R8G8_Uint, 50) E(R8G8_Snorm, 51) E(R8G8_Sint, 52) \
	E(R16_Float, 54) E(D16_Unorm, 55) E(R16_Unorm, 56) E(R16_Uint, 57) E(R16_Snorm, 58) E(R16_Sint, 59) \
	E(R8_Unorm, 61) E(R8_Uint, 62) E(R8_Snorm, 63) E(R8_Sint, 64) \
	E(B8G8R8A8_Unorm, 87) E(B8G8R8A8_Unorm_sRGB, 91) \
	E(R32G32B32A32_Typeless, 1) E(R16G16B16A16_Typeless, 9) E(R32G8X24_Typeless, 19) \
	E(R32_Float_X8X24_Typeless, 21) E(R8G8B8A8_Typeless, 27) E(R16G16_Typeless, 33) E(R32_Typeless, 39) \
	E(R24G8_Typeless, 44) E(R24_Unorm_X8_Typeless, 46) E(R16_Typeless, 53) E(B8G8R8A8_Typeless, 90)
GFX_DECLARE_ENUM(GfxFormat, uint32_t)

// D3D12_BLEND
#define GFX_ENUM_GfxBlend(E) \
	E(Zero, 1) E(One, 2) E(SrcColor, 3) E(InvSrcColor, 4) E(SrcAlpha, 5) E(InvSrcAlpha, 6) \
	E(DestAlpha, 7) E(InvDestAlpha, 8) E(DestColor, 9) E(InvDestColor, 10) E(SrcAlphaSat, 11) \
	E(BlendFactor, 14) E(InvBlendFactor, 15) \
	E(Src1Color, 16) E(InvSrc1Color, 17) E(Src1Alpha, 18) E(InvSrc1Alpha, 19)
GFX_DECLARE_ENUM(GfxBlend, uint32_t)

// D3D12_BLEND_OP
#define GFX_ENUM_GfxBlendOp(E) \
	E(Add, 1) E(Subtract, 2) E(RevSubtract, 3) E(Min, 4) E(Max, 5)
GFX_DECLARE_ENUM(GfxBlendOp, uint32_t)

// D3D12_LOGIC_OP
#define GFX_ENUM_GfxLogicOp(E) \
	E(Clear, 0) E(Set, 1) E(Copy, 2) E(CopyInverted, 3) E(Noop, 4) E(Invert, 5) \
	E(And, 6) E(Nand, 7) E(Or, 8) E(Nor, 9) E(Xor, 10) E(Equiv, 11) \
	E(AndReverse, 12) E(AndInverted, 13) E(OrReverse, 14) E(OrInverted, 15)
GFX_DECLARE_ENUM(GfxLogicOp, uint32_t)

// D3D12_COLOR_WRITE_ENABLE (ビットフラグ)
#define GFX_ENUM_GfxColorWriteMask(E) \
	E(None, 0) E(Red, 1) E(Green, 2) E(Blue, 4) E(Alpha, 8) E(All, 15)
GFX_DECLARE_FLAGS(GfxColorWriteMask, uint8_t)

// D3D12_FILL_MODE
#define GFX_ENUM_GfxFillMode(E) E(Wireframe, 2) E(Solid, 3)
GFX_DECLARE_ENUM(GfxFillMode, uint32_t)

// D3D12_CULL_MODE
#define GFX_ENUM_GfxCullMode(E) E(None, 1) E(Front, 2) E(Back, 3)
GFX_DECLARE_ENUM(GfxCullMode, uint32_t)

// D3D12_CONSERVATIVE_RASTERIZATION_MODE
#define GFX_ENUM_GfxConservativeRaster(E) E(Off, 0) E(On, 1)
GFX_DECLARE_ENUM(GfxConservativeRaster, uint32_t)

// D3D12_COMPARISON_FUNC
#define GFX_ENUM_GfxCompareFunc(E) \
	E(None, 0) E(Never, 1) E(Less, 2) E(Equal, 3) E(LessEqual, 4) \
	E(Greater, 5) E(NotEqual, 6) E(GreaterEqual, 7) E(Always, 8)
GFX_DECLARE_ENUM(GfxCompareFunc, uint32_t)

// D3D12_DEPTH_WRITE_MASK
#define GFX_ENUM_GfxDepthWriteMask(E) E(Zero, 0) E(All, 1)
GFX_DECLARE_ENUM(GfxDepthWriteMask, uint32_t)

// D3D12_STENCIL_OP
#define GFX_ENUM_GfxStencilOp(E) \
	E(Keep, 1) E(Zero, 2) E(Replace, 3) E(IncrSat, 4) E(DecrSat, 5) E(Invert, 6) E(Incr, 7) E(Decr, 8)
GFX_DECLARE_ENUM(GfxStencilOp, uint32_t)

// D3D12_INPUT_CLASSIFICATION
#define GFX_ENUM_GfxInputClassification(E) E(PerVertex, 0) E(PerInstance, 1)
GFX_DECLARE_ENUM(GfxInputClassification, uint32_t)

// D3D12_INDEX_BUFFER_STRIP_CUT_VALUE
#define GFX_ENUM_GfxIndexBufferStripCut(E) E(Disabled, 0) E(Value0xFFFF, 1) E(Value0xFFFFFFFF, 2)
GFX_DECLARE_ENUM(GfxIndexBufferStripCut, uint32_t)

// D3D12_PRIMITIVE_TOPOLOGY_TYPE (PSO が持つトポロジの種類)
#define GFX_ENUM_GfxPrimitiveTopologyType(E) \
	E(Undefined, 0) E(Point, 1) E(Line, 2) E(Triangle, 3) E(Patch, 4)
GFX_DECLARE_ENUM(GfxPrimitiveTopologyType, uint32_t)

// D3D_PRIMITIVE_TOPOLOGY (IASetPrimitiveTopology に渡す実際のトポロジ)
#define GFX_ENUM_GfxPrimitiveTopology(E) \
	E(Undefined, 0) E(PointList, 1) E(LineList, 2) E(LineStrip, 3) E(TriangleList, 4) E(TriangleStrip, 5) \
	E(LineListAdj, 10) E(LineStripAdj, 11) E(TriangleListAdj, 12) E(TriangleStripAdj, 13) \
	E(PatchList1, 33)  E(PatchList2, 34)  E(PatchList3, 35)  E(PatchList4, 36) \
	E(PatchList5, 37)  E(PatchList6, 38)  E(PatchList7, 39)  E(PatchList8, 40) \
	E(PatchList9, 41)  E(PatchList10, 42) E(PatchList11, 43) E(PatchList12, 44) \
	E(PatchList13, 45) E(PatchList14, 46) E(PatchList15, 47) E(PatchList16, 48) \
	E(PatchList17, 49) E(PatchList18, 50) E(PatchList19, 51) E(PatchList20, 52) \
	E(PatchList21, 53) E(PatchList22, 54) E(PatchList23, 55) E(PatchList24, 56) \
	E(PatchList25, 57) E(PatchList26, 58) E(PatchList27, 59) E(PatchList28, 60) \
	E(PatchList29, 61) E(PatchList30, 62) E(PatchList31, 63) E(PatchList32, 64)
GFX_DECLARE_ENUM(GfxPrimitiveTopology, uint32_t)

// D3D12_PIPELINE_STATE_FLAGS (ビットフラグ)
#define GFX_ENUM_GfxPipelineStateFlags(E) \
	E(None, 0) E(ToolDebug, 0x1) E(DynamicDepthBias, 0x4) E(DynamicIndexBufferStripCut, 0x8)
GFX_DECLARE_FLAGS(GfxPipelineStateFlags, uint32_t)

// D3D12_DESCRIPTOR_RANGE_TYPE
#define GFX_ENUM_GfxDescriptorRangeType(E) E(SRV, 0) E(UAV, 1) E(CBV, 2) E(Sampler, 3)
GFX_DECLARE_ENUM(GfxDescriptorRangeType, uint32_t)

// D3D12_DESCRIPTOR_RANGE_FLAGS (ビットフラグ)
#define GFX_ENUM_GfxDescriptorRangeFlags(E) \
	E(None, 0) E(DescriptorsVolatile, 0x1) E(DataVolatile, 0x2) \
	E(DataStaticWhileSetAtExecute, 0x4) E(DataStatic, 0x8) \
	E(DescriptorsStaticKeepingBufferBoundsChecks, 0x10000)
GFX_DECLARE_FLAGS(GfxDescriptorRangeFlags, uint32_t)

// D3D12_ROOT_PARAMETER_TYPE
#define GFX_ENUM_GfxRootParameterType(E) \
	E(DescriptorTable, 0) E(Constants32Bit, 1) E(CBV, 2) E(SRV, 3) E(UAV, 4)
GFX_DECLARE_ENUM(GfxRootParameterType, uint32_t)

// D3D12_SHADER_VISIBILITY
#define GFX_ENUM_GfxShaderVisibility(E) \
	E(All, 0) E(Vertex, 1) E(Hull, 2) E(Domain, 3) E(Geometry, 4) E(Pixel, 5) E(Amplification, 6) E(Mesh, 7)
GFX_DECLARE_ENUM(GfxShaderVisibility, uint32_t)

// D3D12_ROOT_DESCRIPTOR_FLAGS (ビットフラグ)
#define GFX_ENUM_GfxRootDescriptorFlags(E) \
	E(None, 0) E(DataVolatile, 0x2) E(DataStaticWhileSetAtExecute, 0x4) E(DataStatic, 0x8)
GFX_DECLARE_FLAGS(GfxRootDescriptorFlags, uint32_t)

// D3D12_ROOT_SIGNATURE_FLAGS (ビットフラグ)
#define GFX_ENUM_GfxRootSignatureFlags(E) \
	E(None, 0) E(AllowInputAssemblerInputLayout, 0x1) \
	E(DenyVertexShaderRootAccess, 0x2) E(DenyHullShaderRootAccess, 0x4) \
	E(DenyDomainShaderRootAccess, 0x8) E(DenyGeometryShaderRootAccess, 0x10) \
	E(DenyPixelShaderRootAccess, 0x20) E(AllowStreamOutput, 0x40) E(LocalRootSignature, 0x80) \
	E(DenyAmplificationShaderRootAccess, 0x100) E(DenyMeshShaderRootAccess, 0x200) \
	E(CbvSrvUavHeapDirectlyIndexed, 0x400) E(SamplerHeapDirectlyIndexed, 0x800)
GFX_DECLARE_FLAGS(GfxRootSignatureFlags, uint32_t)

// D3D12_FILTER
#define GFX_ENUM_GfxFilter(E) \
	E(MinMagMipPoint, 0x00) E(MinMagPointMipLinear, 0x01) E(MinPointMagLinearMipPoint, 0x04) \
	E(MinPointMagMipLinear, 0x05) E(MinLinearMagMipPoint, 0x10) E(MinLinearMagPointMipLinear, 0x11) \
	E(MinMagLinearMipPoint, 0x14) E(MinMagMipLinear, 0x15) E(Anisotropic, 0x55) \
	E(ComparisonMinMagMipPoint, 0x80) E(ComparisonMinMagPointMipLinear, 0x81) \
	E(ComparisonMinPointMagLinearMipPoint, 0x84) E(ComparisonMinPointMagMipLinear, 0x85) \
	E(ComparisonMinLinearMagMipPoint, 0x90) E(ComparisonMinLinearMagPointMipLinear, 0x91) \
	E(ComparisonMinMagLinearMipPoint, 0x94) E(ComparisonMinMagMipLinear, 0x95) E(ComparisonAnisotropic, 0xD5) \
	E(MinimumMinMagMipPoint, 0x100) E(MinimumMinMagPointMipLinear, 0x101) \
	E(MinimumMinPointMagLinearMipPoint, 0x104) E(MinimumMinPointMagMipLinear, 0x105) \
	E(MinimumMinLinearMagMipPoint, 0x110) E(MinimumMinLinearMagPointMipLinear, 0x111) \
	E(MinimumMinMagLinearMipPoint, 0x114) E(MinimumMinMagMipLinear, 0x115) E(MinimumAnisotropic, 0x155) \
	E(MaximumMinMagMipPoint, 0x180) E(MaximumMinMagPointMipLinear, 0x181) \
	E(MaximumMinPointMagLinearMipPoint, 0x184) E(MaximumMinPointMagMipLinear, 0x185) \
	E(MaximumMinLinearMagMipPoint, 0x190) E(MaximumMinLinearMagPointMipLinear, 0x191) \
	E(MaximumMinMagLinearMipPoint, 0x194) E(MaximumMinMagMipLinear, 0x195) E(MaximumAnisotropic, 0x1D5)
GFX_DECLARE_ENUM(GfxFilter, uint32_t)

// D3D12_TEXTURE_ADDRESS_MODE
#define GFX_ENUM_GfxTextureAddressMode(E) \
	E(Wrap, 1) E(Mirror, 2) E(Clamp, 3) E(Border, 4) E(MirrorOnce, 5)
GFX_DECLARE_ENUM(GfxTextureAddressMode, uint32_t)

// D3D12_STATIC_BORDER_COLOR
#define GFX_ENUM_GfxStaticBorderColor(E) \
	E(TransparentBlack, 0) E(OpaqueBlack, 1) E(OpaqueWhite, 2) E(OpaqueBlackUint, 3) E(OpaqueWhiteUint, 4)
GFX_DECLARE_ENUM(GfxStaticBorderColor, uint32_t)

// ---- パイプラインステート ----------------------------------
//  既定値は D3D12 の既定値 (CD3DX12_*_DESC(D3D12_DEFAULT)) と同じ。

// D3D12_RENDER_TARGET_BLEND_DESC
#define GFX_FIELDS_GfxRenderTargetBlendDesc(F, FA) \
	F(Bool,  bool,              blendEnable,           false,                  0, 1, "D3D12_RENDER_TARGET_BLEND_DESC::BlendEnable") \
	F(Bool,  bool,              logicOpEnable,         false,                  0, 1, "D3D12_RENDER_TARGET_BLEND_DESC::LogicOpEnable") \
	F(Enum,  GfxBlend,          srcBlend,              GfxBlend::One,          0, 0, "D3D12_RENDER_TARGET_BLEND_DESC::SrcBlend") \
	F(Enum,  GfxBlend,          destBlend,             GfxBlend::Zero,         0, 0, "D3D12_RENDER_TARGET_BLEND_DESC::DestBlend") \
	F(Enum,  GfxBlendOp,        blendOp,               GfxBlendOp::Add,        0, 0, "D3D12_RENDER_TARGET_BLEND_DESC::BlendOp") \
	F(Enum,  GfxBlend,          srcBlendAlpha,         GfxBlend::One,          0, 0, "D3D12_RENDER_TARGET_BLEND_DESC::SrcBlendAlpha") \
	F(Enum,  GfxBlend,          destBlendAlpha,        GfxBlend::Zero,         0, 0, "D3D12_RENDER_TARGET_BLEND_DESC::DestBlendAlpha") \
	F(Enum,  GfxBlendOp,        blendOpAlpha,          GfxBlendOp::Add,        0, 0, "D3D12_RENDER_TARGET_BLEND_DESC::BlendOpAlpha") \
	F(Enum,  GfxLogicOp,        logicOp,               GfxLogicOp::Noop,       0, 0, "D3D12_RENDER_TARGET_BLEND_DESC::LogicOp") \
	F(Flags, GfxColorWriteMask, renderTargetWriteMask, GfxColorWriteMask::All, 0, 0, "D3D12_RENDER_TARGET_BLEND_DESC::RenderTargetWriteMask")
struct GfxRenderTargetBlendDesc
{
	GFX_FIELDS_GfxRenderTargetBlendDesc(GFX_DECLARE_FIELD, GFX_DECLARE_ARRAY)
};

// D3D12_BLEND_DESC
#define GFX_FIELDS_GfxBlendDesc(F, FA) \
	F(Bool,    bool,                     alphaToCoverageEnable,  false, 0, 1, "D3D12_BLEND_DESC::AlphaToCoverageEnable") \
	F(Bool,    bool,                     independentBlendEnable, false, 0, 1, "D3D12_BLEND_DESC::IndependentBlendEnable (false のときは renderTarget[0] だけが使われる)") \
	FA(Struct, GfxRenderTargetBlendDesc, renderTarget, GFX_MAX_RENDER_TARGETS, "D3D12_BLEND_DESC::RenderTarget")
struct GfxBlendDesc
{
	GFX_FIELDS_GfxBlendDesc(GFX_DECLARE_FIELD, GFX_DECLARE_ARRAY)
};

// D3D12_RASTERIZER_DESC
#define GFX_FIELDS_GfxRasterizerDesc(F, FA) \
	F(Enum,   GfxFillMode,           fillMode,              GfxFillMode::Solid,         0, 0, "D3D12_RASTERIZER_DESC::FillMode") \
	F(Enum,   GfxCullMode,           cullMode,              GfxCullMode::Back,          0, 0, "D3D12_RASTERIZER_DESC::CullMode") \
	F(Bool,   bool,                  frontCounterClockwise, false,                      0, 1, "D3D12_RASTERIZER_DESC::FrontCounterClockwise") \
	F(Int32,  int32_t,               depthBias,             0,      -2147483647 - 1, 2147483647, "D3D12_RASTERIZER_DESC::DepthBias") \
	F(Float,  float,                 depthBiasClamp,        0.0f,                  -1.0e6, 1.0e6, "D3D12_RASTERIZER_DESC::DepthBiasClamp") \
	F(Float,  float,                 slopeScaledDepthBias,  0.0f,                  -1.0e6, 1.0e6, "D3D12_RASTERIZER_DESC::SlopeScaledDepthBias") \
	F(Bool,   bool,                  depthClipEnable,       true,                       0, 1, "D3D12_RASTERIZER_DESC::DepthClipEnable") \
	F(Bool,   bool,                  multisampleEnable,     false,                      0, 1, "D3D12_RASTERIZER_DESC::MultisampleEnable") \
	F(Bool,   bool,                  antialiasedLineEnable, false,                      0, 1, "D3D12_RASTERIZER_DESC::AntialiasedLineEnable") \
	F(UInt32, uint32_t,              forcedSampleCount,     0,                          0, 16, "D3D12_RASTERIZER_DESC::ForcedSampleCount") \
	F(Enum,   GfxConservativeRaster, conservativeRaster,    GfxConservativeRaster::Off, 0, 0, "D3D12_RASTERIZER_DESC::ConservativeRaster")
struct GfxRasterizerDesc
{
	GFX_FIELDS_GfxRasterizerDesc(GFX_DECLARE_FIELD, GFX_DECLARE_ARRAY)
};

// D3D12_DEPTH_STENCILOP_DESC
#define GFX_FIELDS_GfxStencilOpDesc(F, FA) \
	F(Enum, GfxStencilOp,   stencilFailOp,      GfxStencilOp::Keep,     0, 0, "D3D12_DEPTH_STENCILOP_DESC::StencilFailOp") \
	F(Enum, GfxStencilOp,   stencilDepthFailOp, GfxStencilOp::Keep,     0, 0, "D3D12_DEPTH_STENCILOP_DESC::StencilDepthFailOp") \
	F(Enum, GfxStencilOp,   stencilPassOp,      GfxStencilOp::Keep,     0, 0, "D3D12_DEPTH_STENCILOP_DESC::StencilPassOp") \
	F(Enum, GfxCompareFunc, stencilFunc,        GfxCompareFunc::Always, 0, 0, "D3D12_DEPTH_STENCILOP_DESC::StencilFunc")
struct GfxStencilOpDesc
{
	GFX_FIELDS_GfxStencilOpDesc(GFX_DECLARE_FIELD, GFX_DECLARE_ARRAY)
};

// D3D12_DEPTH_STENCIL_DESC
#define GFX_FIELDS_GfxDepthStencilStateDesc(F, FA) \
	F(Bool,   bool,              depthEnable,      true,                   0, 1,   "D3D12_DEPTH_STENCIL_DESC::DepthEnable") \
	F(Enum,   GfxDepthWriteMask, depthWriteMask,   GfxDepthWriteMask::All, 0, 0,   "D3D12_DEPTH_STENCIL_DESC::DepthWriteMask") \
	F(Enum,   GfxCompareFunc,    depthFunc,        GfxCompareFunc::Less,   0, 0,   "D3D12_DEPTH_STENCIL_DESC::DepthFunc") \
	F(Bool,   bool,              stencilEnable,    false,                  0, 1,   "D3D12_DEPTH_STENCIL_DESC::StencilEnable") \
	F(UInt8,  uint8_t,           stencilReadMask,  0xFF,                   0, 255, "D3D12_DEPTH_STENCIL_DESC::StencilReadMask") \
	F(UInt8,  uint8_t,           stencilWriteMask, 0xFF,                   0, 255, "D3D12_DEPTH_STENCIL_DESC::StencilWriteMask") \
	F(Struct, GfxStencilOpDesc,  frontFace,        {},                     0, 0,   "D3D12_DEPTH_STENCIL_DESC::FrontFace") \
	F(Struct, GfxStencilOpDesc,  backFace,         {},                     0, 0,   "D3D12_DEPTH_STENCIL_DESC::BackFace")
struct GfxDepthStencilStateDesc
{
	GFX_FIELDS_GfxDepthStencilStateDesc(GFX_DECLARE_FIELD, GFX_DECLARE_ARRAY)
};

// D3D12_INPUT_ELEMENT_DESC (セマンティクス名は固定長の文字列で持つ)
#define GFX_FIELDS_GfxInputElementDesc(F, FA) \
	FA(Char,  char,                   semanticName,         GFX_MAX_SEMANTIC_NAME,                      "D3D12_INPUT_ELEMENT_DESC::SemanticName") \
	F(UInt32, uint32_t,               semanticIndex,        0,                                0, 65535, "D3D12_INPUT_ELEMENT_DESC::SemanticIndex") \
	F(Enum,   GfxFormat,              format,               GfxFormat::Unknown,               0, 0,     "D3D12_INPUT_ELEMENT_DESC::Format") \
	F(UInt32, uint32_t,               inputSlot,            0,                                0, 15,    "D3D12_INPUT_ELEMENT_DESC::InputSlot") \
	F(UInt32, uint32_t,               alignedByteOffset,    GFX_APPEND_ALIGNED_ELEMENT,       0, 0xFFFFFFFFu, "D3D12_INPUT_ELEMENT_DESC::AlignedByteOffset (0xFFFFFFFF = 直前の要素に続ける)") \
	F(Enum,   GfxInputClassification, inputSlotClass,       GfxInputClassification::PerVertex, 0, 0,    "D3D12_INPUT_ELEMENT_DESC::InputSlotClass") \
	F(UInt32, uint32_t,               instanceDataStepRate, 0,                                0, 0xFFFFFFFFu, "D3D12_INPUT_ELEMENT_DESC::InstanceDataStepRate")
struct GfxInputElementDesc
{
	GFX_FIELDS_GfxInputElementDesc(GFX_DECLARE_FIELD, GFX_DECLARE_ARRAY)
};

// D3D12_INPUT_LAYOUT_DESC
#define GFX_FIELDS_GfxInputLayoutDesc(F, FA) \
	FA(Struct, GfxInputElementDesc, elements,     GFX_MAX_INPUT_ELEMENTS,        "D3D12_INPUT_LAYOUT_DESC::pInputElementDescs") \
	F(UInt32,  uint32_t,            elementCount, 0, 0, GFX_MAX_INPUT_ELEMENTS,  "D3D12_INPUT_LAYOUT_DESC::NumElements")
struct GfxInputLayoutDesc
{
	GFX_FIELDS_GfxInputLayoutDesc(GFX_DECLARE_FIELD, GFX_DECLARE_ARRAY)

	// ヘルパー: 要素を末尾に追加する
	GfxInputLayoutDesc& add(
		const char*            semantic,
		uint32_t               semanticIndex,
		GfxFormat              format,
		uint32_t               inputSlot    = 0,
		uint32_t               byteOffset   = GFX_APPEND_ALIGNED_ELEMENT,
		GfxInputClassification slotClass    = GfxInputClassification::PerVertex,
		uint32_t               stepRate     = 0)
	{
		if (elementCount >= GFX_MAX_INPUT_ELEMENTS)
			return *this;
		GfxInputElementDesc& e = elements[elementCount++];
		uint32_t i = 0;
		for (; semantic && semantic[i] && i + 1 < GFX_MAX_SEMANTIC_NAME; ++i)
			e.semanticName[i] = semantic[i];
		e.semanticName[i]       = '\0';
		e.semanticIndex         = semanticIndex;
		e.format                = format;
		e.inputSlot             = inputSlot;
		e.alignedByteOffset     = byteOffset;
		e.inputSlotClass        = slotClass;
		e.instanceDataStepRate  = stepRate;
		return *this;
	}
};

// D3D12_GRAPHICS_PIPELINE_STATE_DESC
//  未対応のメンバー: StreamOutput / CachedPSO (必要になったら末尾に追加する)
//  D3D12 にないメンバー: primitiveTopology (bindPipeline 時に IASetPrimitiveTopology で設定する)
#define GFX_FIELDS_GfxGraphicsPipelineDesc(F, FA) \
	F(UInt32, uint32_t,                 structSize,            sizeof(GfxGraphicsPipelineDesc),    0, 0xFFFFFFFFu, "記述子のバイト数 (sizeof)。バックエンドはここまでしか読まない") \
	F(UInt32, uint32_t,                 version,               GFX_GRAPHICS_PIPELINE_DESC_VERSION, 0, 0xFFFFFFFFu, "記述子のバージョン") \
	F(Handle, BindingLayoutHandle,      bindingLayout,         {},   0, 0, "D3D12_GRAPHICS_PIPELINE_STATE_DESC::pRootSignature") \
	F(Handle, ShaderHandle,             vs,                    {},   0, 0, "D3D12_GRAPHICS_PIPELINE_STATE_DESC::VS") \
	F(Handle, ShaderHandle,             ps,                    {},   0, 0, "D3D12_GRAPHICS_PIPELINE_STATE_DESC::PS") \
	F(Handle, ShaderHandle,             ds,                    {},   0, 0, "D3D12_GRAPHICS_PIPELINE_STATE_DESC::DS") \
	F(Handle, ShaderHandle,             hs,                    {},   0, 0, "D3D12_GRAPHICS_PIPELINE_STATE_DESC::HS") \
	F(Handle, ShaderHandle,             gs,                    {},   0, 0, "D3D12_GRAPHICS_PIPELINE_STATE_DESC::GS") \
	F(Struct, GfxBlendDesc,             blendState,            {},   0, 0, "D3D12_GRAPHICS_PIPELINE_STATE_DESC::BlendState") \
	F(UInt32, uint32_t,                 sampleMask,            0xFFFFFFFFu, 0, 0xFFFFFFFFu, "D3D12_GRAPHICS_PIPELINE_STATE_DESC::SampleMask") \
	F(Struct, GfxRasterizerDesc,        rasterizerState,       {},   0, 0, "D3D12_GRAPHICS_PIPELINE_STATE_DESC::RasterizerState") \
	F(Struct, GfxDepthStencilStateDesc, depthStencilState,     {},   0, 0, "D3D12_GRAPHICS_PIPELINE_STATE_DESC::DepthStencilState") \
	F(Struct, GfxInputLayoutDesc,       inputLayout,           {},   0, 0, "D3D12_GRAPHICS_PIPELINE_STATE_DESC::InputLayout") \
	F(Enum,   GfxIndexBufferStripCut,   ibStripCutValue,       GfxIndexBufferStripCut::Disabled,   0, 0, "D3D12_GRAPHICS_PIPELINE_STATE_DESC::IBStripCutValue") \
	F(Enum,   GfxPrimitiveTopologyType, primitiveTopologyType, GfxPrimitiveTopologyType::Triangle, 0, 0, "D3D12_GRAPHICS_PIPELINE_STATE_DESC::PrimitiveTopologyType") \
	F(Enum,   GfxPrimitiveTopology,     primitiveTopology,     GfxPrimitiveTopology::TriangleList, 0, 0, "IASetPrimitiveTopology (bindPipeline 時に設定。primitiveTopologyType と一致させること)") \
	F(UInt32, uint32_t,                 numRenderTargets,      0,    0, GFX_MAX_RENDER_TARGETS, "D3D12_GRAPHICS_PIPELINE_STATE_DESC::NumRenderTargets") \
	FA(Enum,  GfxFormat,                rtvFormats,            GFX_MAX_RENDER_TARGETS,             "D3D12_GRAPHICS_PIPELINE_STATE_DESC::RTVFormats") \
	F(Enum,   GfxFormat,                dsvFormat,             GfxFormat::Unknown,  0, 0,  "D3D12_GRAPHICS_PIPELINE_STATE_DESC::DSVFormat") \
	F(UInt32, uint32_t,                 sampleCount,           1,                   1, 32, "D3D12_GRAPHICS_PIPELINE_STATE_DESC::SampleDesc.Count") \
	F(UInt32, uint32_t,                 sampleQuality,         0,                   0, 0xFFFFFFFFu, "D3D12_GRAPHICS_PIPELINE_STATE_DESC::SampleDesc.Quality") \
	F(UInt32, uint32_t,                 nodeMask,              0,                   0, 0xFFFFFFFFu, "D3D12_GRAPHICS_PIPELINE_STATE_DESC::NodeMask") \
	F(Flags,  GfxPipelineStateFlags,    flags,                 GfxPipelineStateFlags::None, 0, 0, "D3D12_GRAPHICS_PIPELINE_STATE_DESC::Flags") \
	F(CString, const char*,             debugName,             nullptr,             0, 0,  "ID3D12Object::SetName に渡す名前 (シリアライズ対象外)")
struct GfxGraphicsPipelineDesc
{
	GFX_FIELDS_GfxGraphicsPipelineDesc(GFX_DECLARE_FIELD, GFX_DECLARE_ARRAY)
};

// ---- バインディングレイアウト (ルートシグネチャ) -------------

// D3D12_DESCRIPTOR_RANGE1
#define GFX_FIELDS_GfxDescriptorRange(F, FA) \
	F(Enum,   GfxDescriptorRangeType,  rangeType,          GfxDescriptorRangeType::SRV,  0, 0, "D3D12_DESCRIPTOR_RANGE1::RangeType") \
	F(UInt32, uint32_t,                numDescriptors,     1,  0, 0xFFFFFFFFu, "D3D12_DESCRIPTOR_RANGE1::NumDescriptors (0xFFFFFFFF = 上限なし)") \
	F(UInt32, uint32_t,                baseShaderRegister, 0,  0, 0xFFFFFFFFu, "D3D12_DESCRIPTOR_RANGE1::BaseShaderRegister") \
	F(UInt32, uint32_t,                registerSpace,      0,  0, 0xFFFFFFFFu, "D3D12_DESCRIPTOR_RANGE1::RegisterSpace") \
	F(Flags,  GfxDescriptorRangeFlags, flags,              GfxDescriptorRangeFlags::None, 0, 0, "D3D12_DESCRIPTOR_RANGE1::Flags") \
	F(UInt32, uint32_t,                offsetInDescriptorsFromTableStart, GFX_DESCRIPTOR_RANGE_OFFSET_APPEND, 0, 0xFFFFFFFFu, "D3D12_DESCRIPTOR_RANGE1::OffsetInDescriptorsFromTableStart (0xFFFFFFFF = 直前のレンジに続ける)")
struct GfxDescriptorRange
{
	GFX_FIELDS_GfxDescriptorRange(GFX_DECLARE_FIELD, GFX_DECLARE_ARRAY)
};

// D3D12_ROOT_PARAMETER1
//  D3D12 の共用体は平坦化して持つ。parameterType に応じて次のフィールドを使う。
//    DescriptorTable : firstRange / numRanges (GfxBindingLayoutDesc::ranges の範囲)
//    Constants32Bit  : shaderRegister / registerSpace / num32BitValues
//    CBV / SRV / UAV : shaderRegister / registerSpace / descriptorFlags
#define GFX_FIELDS_GfxRootParameter(F, FA) \
	F(Enum,   GfxRootParameterType,   parameterType,    GfxRootParameterType::DescriptorTable, 0, 0, "D3D12_ROOT_PARAMETER1::ParameterType") \
	F(Enum,   GfxShaderVisibility,    shaderVisibility, GfxShaderVisibility::All,              0, 0, "D3D12_ROOT_PARAMETER1::ShaderVisibility") \
	F(UInt32, uint32_t,               firstRange,       0, 0, GFX_MAX_DESCRIPTOR_RANGES,       "DescriptorTable: ranges[] の先頭インデックス") \
	F(UInt32, uint32_t,               numRanges,        0, 0, GFX_MAX_DESCRIPTOR_RANGES,       "D3D12_ROOT_DESCRIPTOR_TABLE1::NumDescriptorRanges") \
	F(UInt32, uint32_t,               shaderRegister,   0, 0, 0xFFFFFFFFu,                     "D3D12_ROOT_CONSTANTS / D3D12_ROOT_DESCRIPTOR1::ShaderRegister") \
	F(UInt32, uint32_t,               registerSpace,    0, 0, 0xFFFFFFFFu,                     "D3D12_ROOT_CONSTANTS / D3D12_ROOT_DESCRIPTOR1::RegisterSpace") \
	F(UInt32, uint32_t,               num32BitValues,   0, 0, 64,                              "D3D12_ROOT_CONSTANTS::Num32BitValues") \
	F(Flags,  GfxRootDescriptorFlags, descriptorFlags,  GfxRootDescriptorFlags::None, 0, 0,    "D3D12_ROOT_DESCRIPTOR1::Flags")
struct GfxRootParameter
{
	GFX_FIELDS_GfxRootParameter(GFX_DECLARE_FIELD, GFX_DECLARE_ARRAY)
};

// D3D12_STATIC_SAMPLER_DESC
#define GFX_FIELDS_GfxStaticSamplerDesc(F, FA) \
	F(Enum,   GfxFilter,             filter,           GfxFilter::Anisotropic,            0, 0, "D3D12_STATIC_SAMPLER_DESC::Filter") \
	F(Enum,   GfxTextureAddressMode, addressU,         GfxTextureAddressMode::Wrap,       0, 0, "D3D12_STATIC_SAMPLER_DESC::AddressU") \
	F(Enum,   GfxTextureAddressMode, addressV,         GfxTextureAddressMode::Wrap,       0, 0, "D3D12_STATIC_SAMPLER_DESC::AddressV") \
	F(Enum,   GfxTextureAddressMode, addressW,         GfxTextureAddressMode::Wrap,       0, 0, "D3D12_STATIC_SAMPLER_DESC::AddressW") \
	F(Float,  float,                 mipLODBias,       0.0f,        -16.0, 15.99, "D3D12_STATIC_SAMPLER_DESC::MipLODBias") \
	F(UInt32, uint32_t,              maxAnisotropy,    16,               0, 16,   "D3D12_STATIC_SAMPLER_DESC::MaxAnisotropy") \
	F(Enum,   GfxCompareFunc,        comparisonFunc,   GfxCompareFunc::LessEqual,         0, 0, "D3D12_STATIC_SAMPLER_DESC::ComparisonFunc") \
	F(Enum,   GfxStaticBorderColor,  borderColor,      GfxStaticBorderColor::OpaqueWhite, 0, 0, "D3D12_STATIC_SAMPLER_DESC::BorderColor") \
	F(Float,  float,                 minLOD,           0.0f,             0, GFX_FLOAT32_MAX, "D3D12_STATIC_SAMPLER_DESC::MinLOD") \
	F(Float,  float,                 maxLOD,           GFX_FLOAT32_MAX,  0, GFX_FLOAT32_MAX, "D3D12_STATIC_SAMPLER_DESC::MaxLOD") \
	F(UInt32, uint32_t,              shaderRegister,   0,                0, 0xFFFFFFFFu,     "D3D12_STATIC_SAMPLER_DESC::ShaderRegister") \
	F(UInt32, uint32_t,              registerSpace,    0,                0, 0xFFFFFFFFu,     "D3D12_STATIC_SAMPLER_DESC::RegisterSpace") \
	F(Enum,   GfxShaderVisibility,   shaderVisibility, GfxShaderVisibility::All,          0, 0, "D3D12_STATIC_SAMPLER_DESC::ShaderVisibility")
struct GfxStaticSamplerDesc
{
	GFX_FIELDS_GfxStaticSamplerDesc(GFX_DECLARE_FIELD, GFX_DECLARE_ARRAY)
};

// D3D12_ROOT_SIGNATURE_DESC1
//  ルートパラメータの並び順がそのままルートパラメータのインデックスになる。
//  既存の bindTexture / bindSampler / bindUniformBuffer は次の規則で対応付ける。
//    bindTexture(slot)       : SRV レンジだけを持つ最初のテーブルの、先頭から slot 番目
//    bindSampler(slot)       : Sampler レンジだけを持つ最初のテーブルの、先頭から slot 番目
//    bindUniformBuffer(reg)  : registerSpace 0 / shaderRegister == reg のルート CBV
//  それ以外 (ルート定数、ルート SRV/UAV、混在テーブル) を設定するコマンドは Phase 2b で追加する。
#define GFX_FIELDS_GfxBindingLayoutDesc(F, FA) \
	F(UInt32,  uint32_t,              structSize,        sizeof(GfxBindingLayoutDesc),    0, 0xFFFFFFFFu, "記述子のバイト数 (sizeof)。バックエンドはここまでしか読まない") \
	F(UInt32,  uint32_t,              version,           GFX_BINDING_LAYOUT_DESC_VERSION, 0, 0xFFFFFFFFu, "記述子のバージョン") \
	FA(Struct, GfxRootParameter,      parameters,        GFX_MAX_ROOT_PARAMETERS,                           "D3D12_ROOT_SIGNATURE_DESC1::pParameters") \
	F(UInt32,  uint32_t,              numParameters,     0, 0, GFX_MAX_ROOT_PARAMETERS,                     "D3D12_ROOT_SIGNATURE_DESC1::NumParameters") \
	FA(Struct, GfxDescriptorRange,    ranges,            GFX_MAX_DESCRIPTOR_RANGES,                         "全テーブルのレンジ (各テーブルは firstRange / numRanges で参照する)") \
	F(UInt32,  uint32_t,              numRanges,         0, 0, GFX_MAX_DESCRIPTOR_RANGES,                   "ranges の使用数") \
	FA(Struct, GfxStaticSamplerDesc,  staticSamplers,    GFX_MAX_STATIC_SAMPLERS,                           "D3D12_ROOT_SIGNATURE_DESC1::pStaticSamplers") \
	F(UInt32,  uint32_t,              numStaticSamplers, 0, 0, GFX_MAX_STATIC_SAMPLERS,                     "D3D12_ROOT_SIGNATURE_DESC1::NumStaticSamplers") \
	F(Flags,   GfxRootSignatureFlags, flags,             GfxRootSignatureFlags::None, 0, 0,                 "D3D12_ROOT_SIGNATURE_DESC1::Flags") \
	F(CString, const char*,           debugName,         nullptr,                     0, 0,                 "ID3D12Object::SetName に渡す名前 (シリアライズ対象外)")
struct GfxBindingLayoutDesc
{
	GFX_FIELDS_GfxBindingLayoutDesc(GFX_DECLARE_FIELD, GFX_DECLARE_ARRAY)

	// ヘルパー: ディスクリプタテーブルを追加する。続く addRange() はこのテーブルに入る。
	GfxBindingLayoutDesc& beginTable(GfxShaderVisibility visibility)
	{
		if (numParameters < GFX_MAX_ROOT_PARAMETERS)
		{
			GfxRootParameter& p = parameters[numParameters++];
			p                   = {};
			p.parameterType     = GfxRootParameterType::DescriptorTable;
			p.shaderVisibility  = visibility;
			p.firstRange        = numRanges;
			p.numRanges         = 0;
		}
		return *this;
	}

	// ヘルパー: 直前の beginTable() のテーブルにレンジを追加する
	GfxBindingLayoutDesc& addRange(
		GfxDescriptorRangeType  type,
		uint32_t                count,
		uint32_t                baseRegister,
		uint32_t                space  = 0,
		GfxDescriptorRangeFlags rangeFlags = GfxDescriptorRangeFlags::None,
		uint32_t                offset = GFX_DESCRIPTOR_RANGE_OFFSET_APPEND)
	{
		if (numParameters == 0 || numRanges >= GFX_MAX_DESCRIPTOR_RANGES)
			return *this;
		GfxRootParameter& p = parameters[numParameters - 1];
		if (p.parameterType != GfxRootParameterType::DescriptorTable
		 || p.firstRange + p.numRanges != numRanges)
			return *this;
		GfxDescriptorRange& r = ranges[numRanges++];
		r.rangeType                         = type;
		r.numDescriptors                    = count;
		r.baseShaderRegister                = baseRegister;
		r.registerSpace                     = space;
		r.flags                             = rangeFlags;
		r.offsetInDescriptorsFromTableStart = offset;
		++p.numRanges;
		return *this;
	}

	// ヘルパー: ルート定数 (32bit 値 x num32BitValues) を追加する
	GfxBindingLayoutDesc& addConstants(
		uint32_t            shaderRegister,
		uint32_t            num32BitValues,
		uint32_t            space      = 0,
		GfxShaderVisibility visibility = GfxShaderVisibility::All)
	{
		if (numParameters < GFX_MAX_ROOT_PARAMETERS)
		{
			GfxRootParameter& p = parameters[numParameters++];
			p                   = {};
			p.parameterType     = GfxRootParameterType::Constants32Bit;
			p.shaderVisibility  = visibility;
			p.shaderRegister    = shaderRegister;
			p.registerSpace     = space;
			p.num32BitValues    = num32BitValues;
		}
		return *this;
	}

	// ヘルパー: ルートディスクリプタ (CBV / SRV / UAV) を追加する
	GfxBindingLayoutDesc& addRootDescriptor(
		GfxRootParameterType   type,
		uint32_t               shaderRegister,
		uint32_t               space      = 0,
		GfxShaderVisibility    visibility = GfxShaderVisibility::All,
		GfxRootDescriptorFlags descFlags  = GfxRootDescriptorFlags::None)
	{
		if (numParameters < GFX_MAX_ROOT_PARAMETERS)
		{
			GfxRootParameter& p = parameters[numParameters++];
			p                   = {};
			p.parameterType     = type;
			p.shaderVisibility  = visibility;
			p.shaderRegister    = shaderRegister;
			p.registerSpace     = space;
			p.descriptorFlags   = descFlags;
		}
		return *this;
	}

	// ヘルパー: 静的サンプラーを追加する
	GfxBindingLayoutDesc& addStaticSampler(const GfxStaticSamplerDesc& s)
	{
		if (numStaticSamplers < GFX_MAX_STATIC_SAMPLERS)
			staticSamplers[numStaticSamplers++] = s;
		return *this;
	}
};
