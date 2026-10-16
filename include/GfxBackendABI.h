#pragma once
// ============================================================
//  GfxBackendABI.h  (RT/DS 分離版)
//
//  責務の分割:
//    ISwapChain    … バックバッファの flip / Present のみ。
//                    RTV/DSV を一切持たない。
//    IRenderTarget … カラー RT (RTV/ImageView) 1 枚。
//                    SwapChain バックバッファ用でも独立 RT 用でも使える。
//    IDepthStencil … デプス/ステンシル (DSV/ImageView) 1 枚。
//                    SwapChain とは独立して作成・破棄できる。
//
//  フレームの流れ:
//    swapChain->acquireNextBuffer()   ← バックバッファ index を確定
//    swapChain->getRenderTarget()     ← そのフレームの IRenderTarget* を取得
//    cmdBuf->setRenderTargets(rt, ds) ← 明示的にバインド
//    ... draw ...
//    cmdBuf->endRenderPass()
//    swapChain->present()             ← flip
//
//  Deferred Rendering:
//    gbuf_albedo = device->createRenderTarget(albedoDesc)
//    gbuf_normal = device->createRenderTarget(normalDesc)
//    gbuf_depth  = device->createDepthStencil(depthDesc)
//    // swapChain は最終 Present 専用
// ============================================================

#include <cstdint>

// ============================================================
//  共通型
// ============================================================
enum class GraphicsAPI : uint32_t { DirectX12 = 0, Vulkan = 1, Auto = 0xFFFF };
enum class ShaderStage : uint32_t { Vertex = 0, Fragment = 1, Compute = 2 };

enum class PixelFormat : uint32_t {
	Unknown = 0,
	RGBA8_UNORM, BGRA8_UNORM,
	RGBA16_FLOAT,                  // HDR カラー RT 用
	R32_FLOAT,
	// デプス/ステンシル
	D32_FLOAT,
	D24_UNORM_S8_UINT,
	D16_UNORM,
};

// ============================================================
//  頂点レイアウト
// ============================================================

// 頂点属性のセマンティクス
enum class VertexAttribute : uint8_t
{
	Position  = 0,  // float3  POSITION
	Normal    = 1,  // float3  NORMAL
	Tangent   = 2,  // float4  TANGENT  (xyz=tangent, w=bitangent sign)
	TexCoord0 = 3,  // float2  TEXCOORD0
	TexCoord1 = 4,  // float2  TEXCOORD1
	Color0    = 5,  // float4  COLOR0
};

// 1 要素分のレイアウト記述
struct GfxVertexElement
{
	VertexAttribute attribute  = VertexAttribute::Position;
	PixelFormat     format     = PixelFormat::RGBA8_UNORM;  // 頂点要素のフォーマット
	uint32_t        offset     = 0;   // バッファ先頭からのバイトオフセット
};

// 頂点レイアウト全体
// 最大 8 要素をサポートする
struct GfxVertexLayout
{
	static constexpr uint32_t MAX_ELEMENTS = 8;

	GfxVertexElement elements[MAX_ELEMENTS] = {};
	uint32_t         elementCount           = 0;
	uint32_t         stride                 = 0;  // 1 頂点のバイトサイズ

	// ヘルパー: 要素を末尾に追加する
	GfxVertexLayout& add(
		VertexAttribute attr,
		PixelFormat     fmt,
		uint32_t        byteOffset)
	{
		if (elementCount < MAX_ELEMENTS)
		{
			elements[elementCount++] = { attr, fmt, byteOffset };
		}
		return *this;
	}

	// ヘルパー: レイアウトのハッシュ値を計算する (PSO キャッシュ用)
	uint64_t hash() const
	{
		uint64_t h = stride;
		for (uint32_t i = 0; i < elementCount; ++i)
		{
			h ^= (static_cast<uint64_t>(elements[i].offset)                    << 0)
			   | (static_cast<uint64_t>(elements[i].attribute)                 << 16)
			   | (static_cast<uint64_t>(static_cast<uint32_t>(elements[i].format)) << 24);
			h = h * 6364136223846793005ULL + 1442695040888963407ULL;
		}
		return h;
	}
};

// よく使う標準レイアウトを返すヘルパー関数
// Position のみ (float3)
inline GfxVertexLayout vertexLayoutP()
{
	GfxVertexLayout l;
	l.stride = 12;
	return l.add(VertexAttribute::Position, PixelFormat::R32_FLOAT, 0);  // R32 x3 として扱う
}
// Position + Normal (float3 + float3 = 24 bytes)
inline GfxVertexLayout vertexLayoutPN()
{
	GfxVertexLayout l;
	l.stride = 24;
	l.add(VertexAttribute::Position, PixelFormat::R32_FLOAT,  0);
	l.add(VertexAttribute::Normal,   PixelFormat::R32_FLOAT, 12);
	return l;
}
// Position + Normal + UV (float3 + float3 + float2 = 32 bytes)
inline GfxVertexLayout vertexLayoutPNU()
{
	GfxVertexLayout l;
	l.stride = 32;
	l.add(VertexAttribute::Position,  PixelFormat::R32_FLOAT,  0);
	l.add(VertexAttribute::Normal,    PixelFormat::R32_FLOAT, 12);
	l.add(VertexAttribute::TexCoord0, PixelFormat::R32_FLOAT, 24);
	return l;
}
// Position + Normal + Tangent + UV (float3 + float3 + float4 + float2 = 48 bytes)
inline GfxVertexLayout vertexLayoutPNTU()
{
	GfxVertexLayout l;
	l.stride = 48;
	l.add(VertexAttribute::Position,  PixelFormat::R32_FLOAT,  0);
	l.add(VertexAttribute::Normal,    PixelFormat::R32_FLOAT, 12);
	l.add(VertexAttribute::Tangent,   PixelFormat::R32_FLOAT, 24);
	l.add(VertexAttribute::TexCoord0, PixelFormat::R32_FLOAT, 40);
	return l;
}

enum class BufferUsage : uint32_t {
	Vertex  = 1 << 0,
	Index   = 1 << 1,
	Uniform = 1 << 2,
	Storage = 1 << 3,   // UAV として書き込める DEFAULT ヒープのバッファ (initialData はコピーで転送)
	Staging = 1 << 4,
};
inline BufferUsage operator|(BufferUsage a, BufferUsage b) {
	return static_cast<BufferUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline BufferUsage operator&(BufferUsage a, BufferUsage b) {
	return static_cast<BufferUsage>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}
// フラグ判定ヘルパー: (usage & flag) != 0
inline bool hasUsage(BufferUsage usage, BufferUsage flag) {
	return (static_cast<uint32_t>(usage) & static_cast<uint32_t>(flag)) != 0;
}

// 不透明ハンドル
struct BufferHandle  { uint64_t id = 0; bool valid() const { return id != 0; } };
struct TextureHandle { uint64_t id = 0; bool valid() const { return id != 0; } };
struct ShaderHandle  { uint64_t id = 0; bool valid() const { return id != 0; } };

// ============================================================
//  RT / DS 記述子
// ============================================================

// RenderTarget の作成パラメータ
// SwapChain バックバッファから作る場合は内部で自動設定されるため
// アプリが直接 fill するのはオフスクリーン RT の場合のみ
struct GfxRenderTargetDesc {
	uint32_t    width       = 0;          // 0 = SwapChain サイズに追従
	uint32_t    height      = 0;
	PixelFormat format      = PixelFormat::RGBA8_UNORM;
	uint32_t    sampleCount = 1;          // MSAA
	bool        allowUAV    = false;      // Compute シェーダーからの書き込み許可
	bool        shaderRead  = false;      // SRV として読み取り可能にする (asTexture() 用)
	float       clearColor[4] = { 0,0,0,1 };
	const char* debugName   = nullptr;
};

// DepthStencil の作成パラメータ
struct GfxDepthStencilDesc {
	uint32_t    width       = 0;          // 0 = SwapChain サイズに追従
	uint32_t    height      = 0;
	PixelFormat format      = PixelFormat::D32_FLOAT;
	uint32_t    sampleCount = 1;
	float       clearDepth    = 1.0f;
	uint8_t     clearStencil  = 0;
	const char* debugName   = nullptr;
};

// SwapChain 作成パラメータ (ウィンドウ情報のみ。RT/DS を持たない)
struct GfxSwapChainDesc {
	void*    nativeWindowHandle = nullptr;
	uint32_t width              = 1280;
	uint32_t height             = 720;
	bool     vsync              = true;
	PixelFormat format          = PixelFormat::BGRA8_UNORM;
	uint32_t bufferCount        = 2;      // ダブル/トリプルバッファ
};

// デバイス初期化パラメータ
struct GfxDeviceConfig {
	GraphicsAPI api        = GraphicsAPI::Auto;
	bool        validation = false;
	char        appName[128] = "GraphicsApp";
};

// バッファ / テクスチャ / シェーダー記述子
struct GfxBufferDesc {
	uint64_t    size        = 0;
	BufferUsage usage       = BufferUsage::Vertex;
	const void* initialData = nullptr;
	uint32_t    stride      = 0;   // 頂点バッファのストライド (バイト)。0 のとき size から推定
};
struct GfxTextureDesc {
	uint32_t    width       = 1;
	uint32_t    height      = 1;
	uint32_t    mips        = 1;
	PixelFormat format      = PixelFormat::RGBA8_UNORM;
	const void* initialData = nullptr;   // ピクセルデータの先頭ポインタ
	uint32_t    rowPitch    = 0;         // 1 行のバイト数。0 のとき width * 4 と仮定
	const char* debugName   = nullptr;
	bool        allowUAV    = false;     // true: UAV として書き込み可能
};

// ============================================================
//  サンプラー
// ============================================================

enum class FilterMode : uint8_t
{
	Nearest    = 0,  // 点フィルタリング
	Linear     = 1,  // 線形フィルタリング
	Aniso      = 2,  // 異方性フィルタリング
	Comparison = 3,  // 比較サンプラー (PCF シャドウ用)
};

enum class AddressMode : uint8_t
{
	Wrap   = 0,
	Clamp  = 1,
	Mirror = 2,
	Border = 3,
};

struct GfxSamplerDesc
{
	FilterMode  filter     = FilterMode::Linear;
	AddressMode addressU   = AddressMode::Wrap;
	AddressMode addressV   = AddressMode::Wrap;
	AddressMode addressW   = AddressMode::Wrap;
	float       mipLodBias = 0.0f;
	uint32_t    maxAniso   = 4;       // Aniso のとき有効
	float       minLod     = 0.0f;
	float       maxLod     = 1000.0f;
	const char* debugName  = nullptr;
};

struct SamplerHandle { uint64_t id = 0; bool valid() const { return id != 0; } };

// ============================================================
//  Pipeline
// ============================================================

struct PipelineHandle { uint64_t id = 0; bool valid() const { return id != 0; } };
struct BLASHandle     { uint64_t id = 0; bool valid() const { return id != 0; } };
struct TLASHandle     { uint64_t id = 0; bool valid() const { return id != 0; } };

// ============================================================
//  Raytracing パイプライン
// ============================================================

struct RTPipelineHandle { uint64_t id = 0; bool valid() const { return id != 0; } };

// RT パイプライン構築パラメータ
// raygen / miss / closesthit の HLSL ソースをまとめて渡す
struct GfxRTPipelineDesc
{
	const char* raygenSource     = nullptr;  // Ray Generation シェーダーのソース
	const char* missSource       = nullptr;  // Miss シェーダーのソース
	const char* closestHitSource = nullptr;  // Closest Hit シェーダーのソース
	uint32_t    maxPayloadSize   = 32;       // ペイロードの最大サイズ (bytes)
	uint32_t    maxAttributeSize = 8;        // 属性の最大サイズ (bytes)
	uint32_t    maxRecursionDepth = 1;       // 最大再帰深度
	const char* debugName        = nullptr;
};


enum class BlendMode : uint8_t
{
	Opaque      = 0,  // アルファブレンドなし
	Alpha       = 1,  // src*a + dst*(1-a)
	Additive    = 2,  // src + dst
};

enum class CullMode : uint8_t
{
	None  = 0,
	Back  = 1,
	Front = 2,
};

enum class FillMode : uint8_t
{
	Solid     = 0,
	Wireframe = 1,
};

enum class Topology : uint8_t
{
	TriangleList = 0,
	LineList     = 1,
};

// ============================================================
//  GfxRootSignatureDesc
//  パイプラインごとのルートシグネチャ構成を記述する。
//  未指定（デフォルト構成）の場合は以下の設定が使われる:
//    SRV  : t0-t3  (Pixel 可視、4枚)
//    Sampler: s0   (Pixel 可視、1個)
//    CBV  : b0     (VS/PS 可視、1個)
// ============================================================

// ルートパラメータの種別
enum class RootParamType : uint8_t
{
	SRVTable    = 0,  // Descriptor Table (SRV)
	SamplerTable= 1,  // Descriptor Table (Sampler)
	CBV         = 2,  // Root Descriptor (CBV)
	SRVTableVS  = 3,  // Descriptor Table (SRV, Vertex Shader 可視)
};

// シェーダーステージの可視性
enum class ShaderVisibility : uint8_t
{
	All    = 0,  // VS + PS
	Vertex = 1,  // VS のみ
	Pixel  = 2,  // PS のみ
};

// 1つのルートパラメータの記述
struct GfxRootParam
{
	RootParamType   type            = RootParamType::SRVTable;
	ShaderVisibility visibility     = ShaderVisibility::Pixel;
	uint32_t        shaderRegister  = 0;   // t0, s0, b0 などのレジスタ番号
	uint32_t        registerSpace   = 0;
	uint32_t        descriptorCount = 1;   // SRVTable / SamplerTable のみ有効
};

// ルートシグネチャ全体の記述
struct GfxRootSignatureDesc
{
	static constexpr uint32_t MAX_PARAMS = 8;
	GfxRootParam params[MAX_PARAMS] = {};
	uint32_t     paramCount         = 0;   // 0 = デフォルト構成を使用

	// ヘルパー: パラメータを追加する
	GfxRootSignatureDesc& add(
		RootParamType    type,
		ShaderVisibility visibility,
		uint32_t         shaderRegister,
		uint32_t         descriptorCount = 1,
		uint32_t         registerSpace   = 0)
	{
		if (paramCount < MAX_PARAMS)
		{
			auto& p          = params[paramCount++];
			p.type           = type;
			p.visibility     = visibility;
			p.shaderRegister = shaderRegister;
			p.registerSpace  = registerSpace;
			p.descriptorCount = descriptorCount;
		}
		return *this;
	}

	// ヘルパー: よく使うデフォルト構成を返す
	// SRV×4(t0-t3, Pixel) + Sampler×1(s0, Pixel) + CBV×1(b0, All)
	static GfxRootSignatureDesc defaultLayout()
	{
		GfxRootSignatureDesc d;
		d.add(RootParamType::SRVTable,     ShaderVisibility::Pixel, 0, 4)
		 .add(RootParamType::SamplerTable, ShaderVisibility::Pixel, 0, 1)
		 .add(RootParamType::CBV,          ShaderVisibility::All,   0);
		return d;
	}

	// ヘルパー: SRV×1 の軽量構成 (フォワード / G-Buffer パス用)
	static GfxRootSignatureDesc singleTexture()
	{
		GfxRootSignatureDesc d;
		d.add(RootParamType::SRVTable,     ShaderVisibility::Pixel, 0, 1)
		 .add(RootParamType::SamplerTable, ShaderVisibility::Pixel, 0, 1)
		 .add(RootParamType::CBV,          ShaderVisibility::All,   0);
		return d;
	}

	// ヘルパー: per-frame(b0) + per-object(b1) の2 CBV 構成
	static GfxRootSignatureDesc perFramePerObject()
	{
		GfxRootSignatureDesc d;
		d.add(RootParamType::SRVTable,     ShaderVisibility::Pixel, 0, 4)
		 .add(RootParamType::SamplerTable, ShaderVisibility::Pixel, 0, 1)
		 .add(RootParamType::CBV,          ShaderVisibility::All,   0)   // b0: per-frame
		 .add(RootParamType::CBV,          ShaderVisibility::All,   1);  // b1: per-object
		return d;
	}
};

// Pipeline 作成記述子
// RT フォーマット・深度・ブレンドなどシェーダー以外の描画ステートも含む

// ============================================================
//  Raytracing 加速構造
// ============================================================

// BLAS（Bottom-Level Acceleration Structure）の構築パラメータ
struct GfxBLASDesc
{
	BufferHandle vertexBuffer;
	uint32_t     vertexCount  = 0;
	uint32_t     vertexStride = 0;   // bytes
	uint32_t     positionOffset = 0; // 頂点内の Position のバイトオフセット
	BufferHandle indexBuffer;
	uint32_t     indexCount   = 0;
	const char*  debugName    = nullptr;
};

// TLAS インスタンス (1オブジェクト = 1インスタンス)
struct GfxTLASInstance
{
	BLASHandle blas;
	float      transform[12] = {        // 3x4 行優先変換行列
		1,0,0,0,
		0,1,0,0,
		0,0,1,0
	};
	uint32_t   instanceID    = 0;
	uint32_t   hitGroupIndex = 0;
};

struct GfxPipelineDesc
{
	ShaderHandle    vs               = {};
	ShaderHandle    ps               = {};

	// RT / DS フォーマット (最大 8 枚)
	static constexpr uint32_t MAX_RTV = 8;
	PixelFormat     rtvFormats[MAX_RTV] = {};
	uint32_t        rtvCount         = 0;
	PixelFormat     dsvFormat        = PixelFormat::Unknown;

	// 描画ステート
	BlendMode       blendMode        = BlendMode::Opaque;
	CullMode        cullMode         = CullMode::Back;
	FillMode        fillMode         = FillMode::Solid;
	Topology        topology         = Topology::TriangleList;
	bool            depthEnable      = true;
	bool            depthWrite       = true;

	// 頂点入力レイアウト (VS の InputLayout)
	GfxVertexLayout vertexLayout     = {};

	// ルートシグネチャの構成
	// paramCount == 0 の場合はエンジンのデフォルト構成を使用する
	GfxRootSignatureDesc rootSignature = {};

	const char*     debugName        = nullptr;

	// MSAA のサンプル数 (RT / DS の sampleCount と一致させる)
	uint32_t        sampleCount      = 1;
};

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

// ============================================================
//  v1 → v2 互換レイヤー
//  GfxPipelineDesc (v1) を GfxGraphicsPipelineDesc + GfxBindingLayoutDesc に変換する。
//  outPipeline.bindingLayout は呼び出し側が outLayout から作成して設定する。
// ============================================================

// PixelFormat (v1) → GfxFormat
inline GfxFormat gfxToFormat(PixelFormat fmt)
{
	switch (fmt)
	{
	case PixelFormat::RGBA8_UNORM:       return GfxFormat::R8G8B8A8_Unorm;
	case PixelFormat::BGRA8_UNORM:       return GfxFormat::B8G8R8A8_Unorm;
	case PixelFormat::RGBA16_FLOAT:      return GfxFormat::R16G16B16A16_Float;
	case PixelFormat::R32_FLOAT:         return GfxFormat::R32_Float;
	case PixelFormat::D32_FLOAT:         return GfxFormat::D32_Float;
	case PixelFormat::D24_UNORM_S8_UINT: return GfxFormat::D24_Unorm_S8_Uint;
	case PixelFormat::D16_UNORM:         return GfxFormat::D16_Unorm;
	default:                             return GfxFormat::Unknown;
	}
}

// GfxVertexLayout (v1) → GfxInputLayoutDesc
//  v1 は R32_FLOAT 指定をセマンティクスごとの既定の幅 (float2/3/4) に展開する。
inline GfxInputLayoutDesc gfxConvertVertexLayout(const GfxVertexLayout& layout)
{
	GfxInputLayoutDesc out;
	if (layout.stride == 0)   // stride = 0 は InputLayout なし
		return out;

	for (uint32_t i = 0; i < layout.elementCount; ++i)
	{
		const GfxVertexElement& e = layout.elements[i];
		const char* sem      = "POSITION";
		uint32_t    semIndex = 0;
		GfxFormat   wide     = GfxFormat::R32G32B32_Float;
		switch (e.attribute)
		{
		case VertexAttribute::Position:  sem = "POSITION";                wide = GfxFormat::R32G32B32_Float;    break;
		case VertexAttribute::Normal:    sem = "NORMAL";                  wide = GfxFormat::R32G32B32_Float;    break;
		case VertexAttribute::Tangent:   sem = "TANGENT";                 wide = GfxFormat::R32G32B32A32_Float; break;
		case VertexAttribute::TexCoord0: sem = "TEXCOORD";                wide = GfxFormat::R32G32_Float;       break;
		case VertexAttribute::TexCoord1: sem = "TEXCOORD"; semIndex = 1;  wide = GfxFormat::R32G32_Float;       break;
		case VertexAttribute::Color0:    sem = "COLOR";                   wide = GfxFormat::R32G32B32_Float;    break;
		}
		const GfxFormat fmt = (e.format == PixelFormat::R32_FLOAT) ? wide : gfxToFormat(e.format);
		out.add(sem, semIndex, fmt, 0, e.offset);
	}
	return out;
}

// GfxRootSignatureDesc (v1) → GfxBindingLayoutDesc
inline GfxBindingLayoutDesc gfxConvertRootSignature(const GfxRootSignatureDesc& rs)
{
	auto toVis = [](ShaderVisibility v)
	{
		switch (v)
		{
		case ShaderVisibility::Vertex: return GfxShaderVisibility::Vertex;
		case ShaderVisibility::Pixel:  return GfxShaderVisibility::Pixel;
		default:                       return GfxShaderVisibility::All;
		}
	};

	GfxBindingLayoutDesc out;
	out.flags = GfxRootSignatureFlags::AllowInputAssemblerInputLayout;
	for (uint32_t i = 0; i < rs.paramCount && i < GfxRootSignatureDesc::MAX_PARAMS; ++i)
	{
		const GfxRootParam& p = rs.params[i];
		switch (p.type)
		{
		case RootParamType::CBV:
			out.addRootDescriptor(GfxRootParameterType::CBV, p.shaderRegister, p.registerSpace, toVis(p.visibility));
			break;
		case RootParamType::SamplerTable:
			out.beginTable(toVis(p.visibility))
			   .addRange(GfxDescriptorRangeType::Sampler, p.descriptorCount, p.shaderRegister, p.registerSpace);
			break;
		case RootParamType::SRVTable:
		case RootParamType::SRVTableVS:
			out.beginTable(p.type == RootParamType::SRVTableVS ? GfxShaderVisibility::Vertex : toVis(p.visibility))
			   .addRange(GfxDescriptorRangeType::SRV, p.descriptorCount, p.shaderRegister, p.registerSpace,
						 GfxDescriptorRangeFlags::DescriptorsVolatile);
			break;
		}
	}
	return out;
}

// GfxPipelineDesc (v1) → GfxGraphicsPipelineDesc
inline GfxGraphicsPipelineDesc gfxConvertPipelineDesc(const GfxPipelineDesc& v1)
{
	GfxGraphicsPipelineDesc out;
	out.vs = v1.vs;
	out.ps = v1.ps;

	GfxRenderTargetBlendDesc& rt0 = out.blendState.renderTarget[0];
	if (v1.blendMode == BlendMode::Alpha)
	{
		rt0.blendEnable = true;
		rt0.srcBlend    = GfxBlend::SrcAlpha;
		rt0.destBlend   = GfxBlend::InvSrcAlpha;
	}
	else if (v1.blendMode == BlendMode::Additive)
	{
		rt0.blendEnable = true;
		rt0.srcBlend    = GfxBlend::One;
		rt0.destBlend   = GfxBlend::One;
	}

	out.rasterizerState.fillMode = (v1.fillMode == FillMode::Wireframe) ? GfxFillMode::Wireframe : GfxFillMode::Solid;
	out.rasterizerState.cullMode = (v1.cullMode == CullMode::Back)  ? GfxCullMode::Back
	                             : (v1.cullMode == CullMode::Front) ? GfxCullMode::Front
	                             :                                    GfxCullMode::None;

	// v1 は DSV がないときは深度ステートを使わない
	out.dsvFormat = gfxToFormat(v1.dsvFormat);
	if (out.dsvFormat != GfxFormat::Unknown)
	{
		out.depthStencilState.depthEnable    = v1.depthEnable;
		out.depthStencilState.depthWriteMask = v1.depthWrite ? GfxDepthWriteMask::All : GfxDepthWriteMask::Zero;
	}
	else
	{
		out.depthStencilState.depthEnable    = false;
		out.depthStencilState.depthWriteMask = GfxDepthWriteMask::Zero;
	}

	out.inputLayout = gfxConvertVertexLayout(v1.vertexLayout);

	if (v1.topology == Topology::LineList)
	{
		out.primitiveTopologyType = GfxPrimitiveTopologyType::Line;
		out.primitiveTopology     = GfxPrimitiveTopology::LineList;
	}

	out.numRenderTargets = v1.rtvCount < GFX_MAX_RENDER_TARGETS ? v1.rtvCount : GFX_MAX_RENDER_TARGETS;
	for (uint32_t i = 0; i < out.numRenderTargets; ++i)
		out.rtvFormats[i] = gfxToFormat(v1.rtvFormats[i]);

	out.sampleCount = v1.sampleCount > 0 ? v1.sampleCount : 1;
	out.rasterizerState.multisampleEnable = out.sampleCount > 1;
	out.debugName = v1.debugName;
	return out;
}

// ============================================================
//  所有権のルール (全インターフェース共通)
//
//  IPipeline / ICommandBuffer / IRenderTarget / IDepthStencil / ISwapChain の
//  ラッパー構造体はバックエンド DLL 内の実装オブジェクトが所有する。
//  アプリ (EXE) 側で delete してはいけない。
//  解放は必ず destroy() / release() (= DLL 内の vtable 関数) を経由する。
//  これにより EXE と DLL の CRT が異なっても安全に解放できる。
//
//  GPU が使用中のリソースはバックエンドが遅延解放するため、
//  destroy() は記録済み・実行中のコマンドがあっても呼んでよい。
// ============================================================

// IPipeline vtable
struct IPipelineVTable
{
	void (*destroy)(void* self);
};

struct IPipeline
{
	const IPipelineVTable* vtable = nullptr;
	void*                  self   = nullptr;

	// 実装とこのラッパーを DLL 内で解放する。呼び出し後このポインタは無効。
	void destroy() { vtable->destroy(self); }
};

struct GfxShaderDesc {
	ShaderStage      stage;
	const char*      source         = nullptr;  // HLSL ソース (優先)
	const char*      entryPoint     = "main";
	const void*      bytecode       = nullptr;  // コンパイル済み DXBC/DXIL
	size_t           bytecodeSize   = 0;
};
struct GfxDrawCmd {
	BufferHandle vertexBuffer;
	BufferHandle indexBuffer;
	uint32_t     indexCount    = 0;
	uint32_t     vertexCount   = 0;   // indexBuffer なし時に使用
	uint32_t     instanceCount = 1;
	uint32_t     firstIndex    = 0;
	int32_t      vertexOffset  = 0;
};

// ============================================================
//  IRenderTargetVTable
// ============================================================
struct IRenderTargetVTable {
	void     (*destroy)    (void* self);
	bool     (*resize)     (void* self, uint32_t w, uint32_t h);
	uint32_t (*getWidth)   (const void* self);
	uint32_t (*getHeight)  (const void* self);
	PixelFormat (*getFormat)(const void* self);
	// SRV としてシェーダーから読み取るためのテクスチャハンドルを返す
	// (ポストエフェクト、Deferred Lighting パスで使用)
	TextureHandle (*asTexture)(void* self);
};

struct IRenderTarget {
	const IRenderTargetVTable* vtable = nullptr;
	void*                      self   = nullptr;

	void          destroy()                      { vtable->destroy(self); }
	bool          resize(uint32_t w, uint32_t h) { return vtable->resize(self, w, h); }
	uint32_t      getWidth()  const              { return vtable->getWidth(self);  }
	uint32_t      getHeight() const              { return vtable->getHeight(self); }
	PixelFormat   getFormat() const              { return vtable->getFormat(self); }
	TextureHandle asTexture()                    { return vtable->asTexture(self); }
};

// ============================================================
//  IDepthStencilVTable
// ============================================================
struct IDepthStencilVTable {
	void     (*destroy)   (void* self);
	bool     (*resize)    (void* self, uint32_t w, uint32_t h);
	uint32_t (*getWidth)  (const void* self);
	uint32_t (*getHeight) (const void* self);
	PixelFormat (*getFormat)(const void* self);
	// デプス値をシェーダーから読むためのテクスチャハンドル (SSAO 等)
	TextureHandle (*asTexture)(void* self);
};

struct IDepthStencil {
	const IDepthStencilVTable* vtable = nullptr;
	void*                      self   = nullptr;

	void          destroy()                      { vtable->destroy(self); }
	bool          resize(uint32_t w, uint32_t h) { return vtable->resize(self, w, h); }
	uint32_t      getWidth()  const              { return vtable->getWidth(self);  }
	uint32_t      getHeight() const              { return vtable->getHeight(self); }
	PixelFormat   getFormat() const              { return vtable->getFormat(self); }
	TextureHandle asTexture()                    { return vtable->asTexture(self); }
};

// ============================================================
//  ISwapChainVTable  (Present 専用。RT/DS を持たない)
// ============================================================
struct ISwapChainVTable {
	void (*destroy)(void* self);
	bool (*resize) (void* self, uint32_t w, uint32_t h);

	// acquireNextBuffer: 次フレームのバックバッファ index を確定し
	//                    同期オブジェクト (semaphore / fence) を待機する。
	//                    beginFrame 相当。
	bool (*acquireNextBuffer)(void* self);

	// getRenderTarget: 現フレームのバックバッファを IRenderTarget* として返す。
	// 返した IRenderTarget* の寿命は次の acquireNextBuffer まで有効。
	// アプリはこれを setRenderTargets() に渡す。
	IRenderTarget* (*getRenderTarget)(void* self);

	// present: バックバッファを画面に表示する。
	void (*present)(void* self);

	uint32_t    (*getWidth) (const void* self);
	uint32_t    (*getHeight)(const void* self);
	PixelFormat (*getFormat)(const void* self);
};

struct ISwapChain {
	const ISwapChainVTable* vtable = nullptr;
	void*                   self   = nullptr;

	void           destroy()                      { vtable->destroy(self); }
	bool           resize(uint32_t w, uint32_t h) { return vtable->resize(self, w, h); }
	bool           acquireNextBuffer()             { return vtable->acquireNextBuffer(self); }
	IRenderTarget* getRenderTarget()               { return vtable->getRenderTarget(self); }
	void           present()                       { vtable->present(self); }
	uint32_t       getWidth()  const               { return vtable->getWidth(self);  }
	uint32_t       getHeight() const               { return vtable->getHeight(self); }
	PixelFormat    getFormat() const               { return vtable->getFormat(self); }
};

// ============================================================
//  ICommandBuffer — 外部から保持・記録できるコマンドバッファ
//
//  ライフサイクル:
//    ICommandBuffer* cb = device->allocCommandBuffer(type);
//    cb->begin();
//    ... draw calls ...
//    cb->end();
//    device->submitCommandBuffers(&cb, 1);
//    // フレーム終了後に解放
//    cb->release();
//
//  用途別に type を使い分ける:
//    Direct   … GPU に直接サブミットできる主コマンドバッファ
//    Bundle   … Direct に記録して繰り返し再利用 (DX12 Bundle / Vk Secondary)
// ============================================================
enum class CommandBufferType : uint32_t {
	Direct = 0,   // 主コマンドバッファ。サブミット可能。
	Bundle = 1,   // セカンダリ / バンドル。Direct から実行する。
};

struct ICommandBufferVTable {
	// 記録開始 / 終了
	void (*begin) (void* self);
	void (*end)   (void* self);

	// このコマンドバッファを Direct に埋め込む (Bundle/Secondary 用)
	void (*executeBundle) (void* self, void* parentSelf);

	// DLL 内部で確保されたオブジェクトを解放する
	void (*release) (void* self);

	// ---- コマンドバッファへの描画コマンド記録 ----
	// setRenderTargets
	void (*setRenderTargets)(void* self,
							 uint32_t       rtCount,
							 IRenderTarget* const* rts,
							 IDepthStencil* ds,
							 bool clearRT, bool clearDS);
	void (*bindPipeline)      (void* self, IPipeline*    p);
	void (*bindTexture)       (void* self, TextureHandle h, uint32_t slot);
	void (*bindSampler)       (void* self, SamplerHandle h, uint32_t slot);
	void (*bindUniformBuffer) (void* self, BufferHandle  h, uint32_t slot);
	void (*setTopology)       (void* self, Topology t);
	void (*bindUAV)           (void* self, TextureHandle h, uint32_t slot);
	void (*dispatchRays)      (void* self, RTPipelineHandle pipe, TLASHandle tlas, TextureHandle output, TextureHandle depthTex, BufferHandle rtCB, uint32_t w, uint32_t h);
	void (*transitionToUAV)   (void* self, TextureHandle h);
	void (*draw)              (void* self, const GfxDrawCmd* cmd);
	void (*drawFullscreen)    (void* self);

	// クエリ
	CommandBufferType (*getType)(const void* self);

	// ---- ABI v2: 低レベルなバインド ----
	// rootIndex はバインディングレイアウトのパラメータの位置。
	// bindPipeline で Compute パイプラインを選んでいれば Compute 側のルートに設定する。
	void (*setRootConstants)  (void* self, uint32_t rootIndex, uint32_t num32BitValues, const void* data, uint32_t destOffsetIn32BitValues);
	// ルート CBV / SRV / UAV (種類はレイアウトのパラメータから決まる)。offset はバイト数
	void (*setRootDescriptor) (void* self, uint32_t rootIndex, BufferHandle buffer, uint64_t offset);
	// ディスクリプタを一時領域へ連続して書き込み、テーブルとして設定する
	void (*setDescriptorTable)(void* self, uint32_t rootIndex, const GfxDescriptor* descriptors, uint32_t count);
	void (*setVertexBuffers)  (void* self, uint32_t startSlot, uint32_t count, const GfxVertexBufferBinding* bindings);
	void (*setIndexBuffer)    (void* self, const GfxIndexBufferBinding* binding);

	// ---- ABI v2: 描画 / Compute ----
	void (*drawInstanced)       (void* self, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance);
	void (*drawIndexedInstanced)(void* self, uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t baseVertex, uint32_t firstInstance);
	void (*drawIndirect)        (void* self, const GfxIndirectDesc* desc);
	void (*drawIndexedIndirect) (void* self, const GfxIndirectDesc* desc);
	void (*dispatch)            (void* self, uint32_t x, uint32_t y, uint32_t z);
	void (*dispatchIndirect)    (void* self, const GfxIndirectDesc* desc);

	// ---- ABI v2: バリア / 動的ステート ----
	void (*barrier)       (void* self, const GfxBarrierDesc* barriers, uint32_t count);
	void (*setViewports)  (void* self, uint32_t count, const GfxViewport* viewports);
	void (*setScissors)   (void* self, uint32_t count, const GfxRect* rects);
	void (*setStencilRef) (void* self, uint32_t ref);
	void (*setBlendFactor)(void* self, const float* rgba);

	// ---- ABI v2: 転送 / クリア ----
	void (*copyBuffer) (void* self, BufferHandle dst, uint64_t dstOffset, BufferHandle src, uint64_t srcOffset, uint64_t size);
	// srcBox = nullptr ならサブリソース全体
	void (*copyTexture)(void* self, GfxResourceRef dst, uint32_t dstSubresource, uint32_t dstX, uint32_t dstY, uint32_t dstZ,
	                    GfxResourceRef src, uint32_t srcSubresource, const GfxBox* srcBox);
	// rectCount = 0 なら全体
	void (*clearRenderTarget)(void* self, IRenderTarget* rt, const float* rgba, uint32_t rectCount, const GfxRect* rects);
	void (*clearDepthStencil)(void* self, IDepthStencil* ds, GfxClearFlags flags, float depth, uint8_t stencil,
	                          uint32_t rectCount, const GfxRect* rects);

	// ---- ABI v2: マーカー / クエリ ----
	void (*beginEvent)    (void* self, const char* name);
	void (*endEvent)      (void* self);
	void (*writeTimestamp)(void* self, QueryPoolHandle pool, uint32_t index);
	void (*beginQuery)    (void* self, QueryPoolHandle pool, uint32_t index);
	void (*endQuery)      (void* self, QueryPoolHandle pool, uint32_t index);
	// [first, first + count) の結果を読み出し用の領域へ書き出す。readQueryResults で読む
	void (*resolveQueries)(void* self, QueryPoolHandle pool, uint32_t first, uint32_t count);

	// ---- ABI v2 (Phase 2b-2): ビュー / MSAA ----
	// RTV / DSV のビューを設定する (クリアはしない)。ビューポートとシザーは最初のビューの mip のサイズに合わせる
	void (*setRenderTargetViews) (void* self, uint32_t count, const ViewHandle* rtvs, ViewHandle dsv);
	void (*clearRenderTargetView)(void* self, ViewHandle rtv, const float* rgba, uint32_t rectCount, const GfxRect* rects);
	void (*clearDepthStencilView)(void* self, ViewHandle dsv, GfxClearFlags flags, float depth, uint8_t stencil,
	                              uint32_t rectCount, const GfxRect* rects);
	// MSAA のサブリソースを解決する (ResolveSubresource)。format = Unknown なら dst のフォーマット
	void (*resolve)(void* self, GfxResourceRef dst, uint32_t dstSubresource, GfxResourceRef src, uint32_t srcSubresource, GfxFormat format);
};

struct ICommandBuffer {
	const ICommandBufferVTable* vtable = nullptr;
	void*                       self   = nullptr;

	void begin()  { vtable->begin(self);  }
	void end()    { vtable->end(self);    }
	// impl とこのラッパーを DLL 内で解放 (またはプールへ返却) する。
	// 呼び出し後このポインタは無効。
	void release() { vtable->release(self); }

	void executeBundle(ICommandBuffer* parent) {
		vtable->executeBundle(self, parent ? parent->self : nullptr);
	}

	void setRenderTargets(uint32_t count, IRenderTarget* const* rts,
						  IDepthStencil* ds,
						  bool clearRT = true, bool clearDS = true) {
		vtable->setRenderTargets(self, count, rts, ds, clearRT, clearDS);
	}
	void setTopology       (Topology t)                   { vtable->setTopology(self, t);                 }
	void bindUAV           (TextureHandle h, uint32_t s)  { vtable->bindUAV(self, h, s);                  }
	void dispatchRays      (RTPipelineHandle pipe, TLASHandle tlas, TextureHandle output,
	                        TextureHandle depthTex, BufferHandle rtCB, uint32_t w, uint32_t h)
	                       { vtable->dispatchRays(self, pipe, tlas, output, depthTex, rtCB, w, h); }
	void transitionToUAV   (TextureHandle h) { vtable->transitionToUAV(self, h); }
	void bindPipeline      (IPipeline*    p)              { vtable->bindPipeline(self, p);                }
	void bindPipeline      (PipelineHandle h)
	{
		// PipelineHandle.id には IPipeline* が直接格納されている
		if (h.valid())
			vtable->bindPipeline(self, reinterpret_cast<IPipeline*>(h.id));
	}
	void bindTexture       (TextureHandle h, uint32_t s)  { vtable->bindTexture(self, h, s);              }
	void bindSampler       (SamplerHandle h, uint32_t s)  { vtable->bindSampler(self, h, s);              }
	void bindUniformBuffer (BufferHandle  h, uint32_t s)  { vtable->bindUniformBuffer(self, h, s);        }
	void draw              (const GfxDrawCmd& cmd)        { vtable->draw(self, &cmd);                     }
	void drawFullscreen    ()                             { vtable->drawFullscreen(self);                  }

	CommandBufferType getType() const { return vtable->getType(self); }

	// ---- ABI v2 ----
	void setRootConstants  (uint32_t rootIndex, uint32_t num32, const void* data, uint32_t destOffset = 0)
	                       { vtable->setRootConstants(self, rootIndex, num32, data, destOffset); }
	void setRootDescriptor (uint32_t rootIndex, BufferHandle buf, uint64_t offset = 0) { vtable->setRootDescriptor(self, rootIndex, buf, offset); }
	void setDescriptorTable(uint32_t rootIndex, const GfxDescriptor* descs, uint32_t count) { vtable->setDescriptorTable(self, rootIndex, descs, count); }
	void setVertexBuffers  (uint32_t startSlot, uint32_t count, const GfxVertexBufferBinding* b) { vtable->setVertexBuffers(self, startSlot, count, b); }
	void setIndexBuffer    (const GfxIndexBufferBinding& b) { vtable->setIndexBuffer(self, &b); }

	void drawInstanced       (uint32_t vtx, uint32_t inst = 1, uint32_t firstVtx = 0, uint32_t firstInst = 0)
	                         { vtable->drawInstanced(self, vtx, inst, firstVtx, firstInst); }
	void drawIndexedInstanced(uint32_t idx, uint32_t inst = 1, uint32_t firstIdx = 0, int32_t baseVtx = 0, uint32_t firstInst = 0)
	                         { vtable->drawIndexedInstanced(self, idx, inst, firstIdx, baseVtx, firstInst); }
	void drawIndirect        (const GfxIndirectDesc& d) { vtable->drawIndirect(self, &d);        }
	void drawIndexedIndirect (const GfxIndirectDesc& d) { vtable->drawIndexedIndirect(self, &d); }
	void dispatch            (uint32_t x, uint32_t y = 1, uint32_t z = 1) { vtable->dispatch(self, x, y, z); }
	void dispatchIndirect    (const GfxIndirectDesc& d) { vtable->dispatchIndirect(self, &d);    }

	void barrier       (const GfxBarrierDesc* b, uint32_t count) { vtable->barrier(self, b, count); }
	void barrier       (const GfxBarrierDesc& b)                 { vtable->barrier(self, &b, 1);    }
	void setViewports  (uint32_t count, const GfxViewport* v)    { vtable->setViewports(self, count, v); }
	void setScissors   (uint32_t count, const GfxRect* r)        { vtable->setScissors(self, count, r);  }
	void setStencilRef (uint32_t ref)                            { vtable->setStencilRef(self, ref);     }
	void setBlendFactor(const float rgba[4])                     { vtable->setBlendFactor(self, rgba);   }

	void copyBuffer (BufferHandle dst, uint64_t dstOffset, BufferHandle src, uint64_t srcOffset, uint64_t size)
	                { vtable->copyBuffer(self, dst, dstOffset, src, srcOffset, size); }
	void copyTexture(GfxResourceRef dst, uint32_t dstSub, uint32_t dstX, uint32_t dstY, uint32_t dstZ,
	                 GfxResourceRef src, uint32_t srcSub, const GfxBox* srcBox = nullptr)
	                { vtable->copyTexture(self, dst, dstSub, dstX, dstY, dstZ, src, srcSub, srcBox); }
	void clearRenderTarget(IRenderTarget* rt, const float rgba[4], uint32_t rectCount = 0, const GfxRect* rects = nullptr)
	                { vtable->clearRenderTarget(self, rt, rgba, rectCount, rects); }
	void clearDepthStencil(IDepthStencil* ds, GfxClearFlags flags, float depth, uint8_t stencil,
	                       uint32_t rectCount = 0, const GfxRect* rects = nullptr)
	                { vtable->clearDepthStencil(self, ds, flags, depth, stencil, rectCount, rects); }

	void beginEvent    (const char* name)                    { vtable->beginEvent(self, name);          }
	void endEvent      ()                                    { vtable->endEvent(self);                  }
	void writeTimestamp(QueryPoolHandle pool, uint32_t index) { vtable->writeTimestamp(self, pool, index); }
	void beginQuery    (QueryPoolHandle pool, uint32_t index) { vtable->beginQuery(self, pool, index);     }
	void endQuery      (QueryPoolHandle pool, uint32_t index) { vtable->endQuery(self, pool, index);       }
	void resolveQueries(QueryPoolHandle pool, uint32_t first, uint32_t count) { vtable->resolveQueries(self, pool, first, count); }

	void setRenderTargetViews (uint32_t count, const ViewHandle* rtvs, ViewHandle dsv = {}) { vtable->setRenderTargetViews(self, count, rtvs, dsv); }
	void clearRenderTargetView(ViewHandle rtv, const float rgba[4], uint32_t rectCount = 0, const GfxRect* rects = nullptr)
	                          { vtable->clearRenderTargetView(self, rtv, rgba, rectCount, rects); }
	void clearDepthStencilView(ViewHandle dsv, GfxClearFlags flags, float depth, uint8_t stencil,
	                           uint32_t rectCount = 0, const GfxRect* rects = nullptr)
	                          { vtable->clearDepthStencilView(self, dsv, flags, depth, stencil, rectCount, rects); }
	void resolve(GfxResourceRef dst, uint32_t dstSub, GfxResourceRef src, uint32_t srcSub, GfxFormat format = GfxFormat::Unknown)
	            { vtable->resolve(self, dst, dstSub, src, srcSub, format); }
};

// ============================================================
//  GfxDeviceVTable
// ============================================================
struct GfxDeviceVTable {
	bool (*init)    (void* self, const GfxDeviceConfig* cfg);
	void (*shutdown)(void* self);

	// ---- SwapChain / RT / DS 作成 ----
	ISwapChain*    (*createSwapChain)   (void* self, const GfxSwapChainDesc*    desc);
	IRenderTarget* (*createRenderTarget)(void* self, const GfxRenderTargetDesc* desc);
	IDepthStencil* (*createDepthStencil)(void* self, const GfxDepthStencilDesc* desc);

	// ---- コマンドバッファ管理 ----
	// allocCommandBuffer: アプリが任意のタイミングで確保できる
	// → release() で解放するまで有効
	ICommandBuffer* (*allocCommandBuffer)(void* self, CommandBufferType type);

	// submitCommandBuffers: Direct コマンドバッファを GPU キューにサブミット
	// cbs[count] の配列を渡す。記録済み(end済み)であること。
	void (*submitCommandBuffers)(void* self,
								 ICommandBuffer* const* cbs,
								 uint32_t count);

	// ---- GPU リソース ----
	BufferHandle  (*createBuffer)  (void* self, const GfxBufferDesc*  desc);
	TextureHandle (*createTexture) (void* self, const GfxTextureDesc* desc);
	ShaderHandle  (*compileShader) (void* self, const GfxShaderDesc*  desc);
	SamplerHandle (*createSampler) (void* self, const GfxSamplerDesc* desc);
	void (*destroyBuffer)  (void* self, BufferHandle  h);
	void (*destroyTexture) (void* self, TextureHandle h);
	void (*destroyShader)  (void* self, ShaderHandle  h);
	void (*destroySampler) (void* self, SamplerHandle h);

	// UPLOAD ヒープのバッファに CPU からデータを書き込む
	// Uniform / Staging バッファ用。size は書き込むバイト数。
	bool (*updateBuffer)   (void* self, BufferHandle h, const void* data, uint64_t size);

	// ---- 描画 (現在アクティブなコマンドバッファへ記録) ----
	void (*bindTexture)    (void* self, TextureHandle h, uint32_t slot);
	void (*draw)           (void* self, const GfxDrawCmd* cmd);
	void (*drawFullscreen) (void* self);

	// ---- クエリ ----
	GraphicsAPI (*getAPI)        (const void* self);
	const char* (*getAdapterName)(const void* self);
	uint64_t    (*getVRAMUsageMB)(const void* self);

	// ---- Pipeline ----
	PipelineHandle (*createPipeline)  (void* self, const GfxPipelineDesc* desc);
	void           (*destroyPipeline) (void* self, PipelineHandle h);
	BLASHandle     (*createBLAS)      (void* self, const GfxBLASDesc* desc);
	void           (*destroyBLAS)     (void* self, BLASHandle h);
	TLASHandle     (*createTLAS)      (void* self, const GfxTLASInstance* instances, uint32_t count);
	void           (*destroyTLAS)     (void* self, TLASHandle h);
	void           (*updateTLAS)      (void* self, TLASHandle h, const GfxTLASInstance* instances, uint32_t count);
	RTPipelineHandle (*createRTPipeline)  (void* self, const GfxRTPipelineDesc* desc);
	void             (*destroyRTPipeline) (void* self, RTPipelineHandle h);

	// ---- ABI v2: バインディングレイアウト / パイプラインステート ----
	// desc->structSize までしか読まない。作成したパイプラインは destroyPipeline で破棄する。
	// バインディングレイアウトは、それを使うパイプラインより先に破棄してもよい
	// (パイプラインはルートシグネチャへの参照を自分で保持する)。
	BindingLayoutHandle (*createBindingLayout)   (void* self, const GfxBindingLayoutDesc* desc);
	void                (*destroyBindingLayout)  (void* self, BindingLayoutHandle h);
	PipelineHandle      (*createGraphicsPipeline)(void* self, const GfxGraphicsPipelineDesc* desc);

	// ---- ABI v2 (Phase 2b): Compute パイプライン / クエリ ----
	PipelineHandle  (*createComputePipeline)(void* self, const GfxComputePipelineDesc* desc);
	QueryPoolHandle (*createQueryPool)      (void* self, const GfxQueryPoolDesc* desc);
	void            (*destroyQueryPool)     (void* self, QueryPoolHandle h);
	// resolveQueries で書き出した結果を読む。対象の resolve を含む submit が GPU で
	// 完了していなければ false を返す (待たない)。
	bool            (*readQueryResults)     (void* self, QueryPoolHandle h, uint32_t first, uint32_t count, uint64_t* out);
	// タイムスタンプの 1 秒あたりのティック数
	uint64_t        (*getTimestampFrequency)(const void* self);

	// ---- ABI v2 (Phase 2b-2): リソース / ビュー / サンプラー ----
	// 破棄は destroyTexture / destroySampler。ビューはリソースへの参照を持つので、
	// リソースを先に破棄してもビューが残っている間は解放されない。
	TextureHandle (*createTextureResource)(void* self, const GfxTextureResourceDesc* desc);
	ViewHandle    (*createView)           (void* self, const GfxViewDesc* desc);
	void          (*destroyView)          (void* self, ViewHandle h);
	SamplerHandle (*createSamplerState)   (void* self, const GfxSamplerStateDesc* desc);
};

// ============================================================
//  IBackendPlugin
// ============================================================
struct IBackendPlugin {
	const GfxDeviceVTable* vtable = nullptr;
	void*                  self   = nullptr;

	bool init    (const GfxDeviceConfig& c)  { return vtable->init(self, &c); }
	void shutdown()                           { vtable->shutdown(self); }

	ISwapChain*    createSwapChain   (const GfxSwapChainDesc& d)    { return vtable->createSwapChain(self, &d);    }
	IRenderTarget* createRenderTarget(const GfxRenderTargetDesc& d) { return vtable->createRenderTarget(self, &d); }
	IDepthStencil* createDepthStencil(const GfxDepthStencilDesc& d) { return vtable->createDepthStencil(self, &d); }

	ICommandBuffer* allocCommandBuffer(CommandBufferType type = CommandBufferType::Direct) {
		return vtable->allocCommandBuffer(self, type);
	}
	void submitCommandBuffers(ICommandBuffer* const* cbs, uint32_t count) {
		vtable->submitCommandBuffers(self, cbs, count);
	}

	BufferHandle  createBuffer  (const GfxBufferDesc& d)   { return vtable->createBuffer(self, &d);   }
	TextureHandle createTexture (const GfxTextureDesc& d)  { return vtable->createTexture(self, &d);  }
	ShaderHandle  compileShader (const GfxShaderDesc& d)   { return vtable->compileShader(self, &d);  }
	SamplerHandle createSampler (const GfxSamplerDesc& d)  { return vtable->createSampler(self, &d);  }
	void destroyBuffer (BufferHandle  h) { vtable->destroyBuffer(self, h);  }
	void destroyTexture(TextureHandle h) { vtable->destroyTexture(self, h); }
	void destroyShader (ShaderHandle  h) { vtable->destroyShader(self, h);  }
	void destroySampler(SamplerHandle h) { vtable->destroySampler(self, h); }
	bool updateBuffer  (BufferHandle h, const void* data, uint64_t size)
	{
		return vtable->updateBuffer(self, h, data, size);
	}
	PipelineHandle createPipeline (const GfxPipelineDesc& d) { return vtable->createPipeline(self, &d); }
	void destroyPipeline(PipelineHandle h)                    { vtable->destroyPipeline(self, h);       }
	BLASHandle createBLAS  (const GfxBLASDesc& d) { return vtable->createBLAS(self, &d); }
	void       destroyBLAS (BLASHandle h)          { vtable->destroyBLAS(self, h);        }
	TLASHandle createTLAS  (const GfxTLASInstance* inst, uint32_t n) { return vtable->createTLAS(self, inst, n); }
	void       destroyTLAS (TLASHandle h)                            { vtable->destroyTLAS(self, h);             }
	void       updateTLAS  (TLASHandle h, const GfxTLASInstance* inst, uint32_t n) { vtable->updateTLAS(self, h, inst, n); }
	RTPipelineHandle createRTPipeline  (const GfxRTPipelineDesc& d) { return vtable->createRTPipeline(self, &d); }
	void             destroyRTPipeline (RTPipelineHandle h)          { vtable->destroyRTPipeline(self, h);       }

	BindingLayoutHandle createBindingLayout (const GfxBindingLayoutDesc& d)    { return vtable->createBindingLayout(self, &d);    }
	void                destroyBindingLayout(BindingLayoutHandle h)            { vtable->destroyBindingLayout(self, h);           }
	PipelineHandle      createPipeline      (const GfxGraphicsPipelineDesc& d) { return vtable->createGraphicsPipeline(self, &d); }
	PipelineHandle      createPipeline      (const GfxComputePipelineDesc& d)  { return vtable->createComputePipeline(self, &d);  }
	QueryPoolHandle     createQueryPool     (const GfxQueryPoolDesc& d)        { return vtable->createQueryPool(self, &d);        }
	void                destroyQueryPool    (QueryPoolHandle h)                { vtable->destroyQueryPool(self, h);               }
	bool                readQueryResults    (QueryPoolHandle h, uint32_t first, uint32_t count, uint64_t* out)
	                                        { return vtable->readQueryResults(self, h, first, count, out); }
	uint64_t            getTimestampFrequency() const                         { return vtable->getTimestampFrequency(self);      }
	TextureHandle       createTexture       (const GfxTextureResourceDesc& d)  { return vtable->createTextureResource(self, &d);  }
	ViewHandle          createView          (const GfxViewDesc& d)             { return vtable->createView(self, &d);             }
	void                destroyView         (ViewHandle h)                     { vtable->destroyView(self, h);                    }
	SamplerHandle       createSampler       (const GfxSamplerStateDesc& d)     { return vtable->createSamplerState(self, &d);     }

	void bindTexture   (TextureHandle h, uint32_t s)  { vtable->bindTexture(self, h, s);  }
	void draw          (const GfxDrawCmd& cmd)        { vtable->draw(self, &cmd);         }
	void drawFullscreen()                              { vtable->drawFullscreen(self);      }

	GraphicsAPI getAPI()         const { return vtable->getAPI(self);         }
	const char* getAdapterName() const { return vtable->getAdapterName(self); }
	uint64_t    getVRAMUsageMB() const { return vtable->getVRAMUsageMB(self); }
};

// ============================================================
//  DLL エクスポート
// ============================================================
#if defined(_WIN32)
  #define GFX_EXPORT __declspec(dllexport)
  #define GFX_IMPORT __declspec(dllimport)
#else
  #define GFX_EXPORT __attribute__((visibility("default")))
  #define GFX_IMPORT
#endif

// ============================================================
//  ABI バージョン
//  ABI (vtable の並び・構造体レイアウト) を変更したら必ず上げること。
//  PluginLoader はバージョンと各構造体サイズが一致しない DLL を拒否する。
// ============================================================
//  3: ABI v2 のパイプラインステート / バインディングレイアウト (createBindingLayout / createGraphicsPipeline)
//  4: Phase 2b-1 (低レベルなバインド、明示的なコマンド、Compute パイプライン、クエリ)
//  5: Phase 2b-2 (テクスチャ / ビュー / サンプラーの v2 記述子、MSAA、resolve。GfxPipelineDesc に sampleCount)
#define GFX_ABI_VERSION 5u

struct GfxABIInfo
{
	uint32_t abiVersion;
	uint32_t sizeofDeviceVTable;
	uint32_t sizeofCommandBufferVTable;
	uint32_t sizeofSwapChainVTable;
	uint32_t sizeofRenderTargetVTable;
	uint32_t sizeofDepthStencilVTable;
	uint32_t sizeofPipelineDesc;
	uint32_t sizeofDrawCmd;
};

// この翻訳単位 (EXE / DLL それぞれ) でコンパイルされた ABI 情報を返す
inline GfxABIInfo gfxCurrentABIInfo()
{
	return {
		GFX_ABI_VERSION,
		static_cast<uint32_t>(sizeof(GfxDeviceVTable)),
		static_cast<uint32_t>(sizeof(ICommandBufferVTable)),
		static_cast<uint32_t>(sizeof(ISwapChainVTable)),
		static_cast<uint32_t>(sizeof(IRenderTargetVTable)),
		static_cast<uint32_t>(sizeof(IDepthStencilVTable)),
		static_cast<uint32_t>(sizeof(GfxPipelineDesc)),
		static_cast<uint32_t>(sizeof(GfxDrawCmd)),
	};
}

inline bool operator==(const GfxABIInfo& a, const GfxABIInfo& b)
{
	return a.abiVersion                == b.abiVersion
		&& a.sizeofDeviceVTable        == b.sizeofDeviceVTable
		&& a.sizeofCommandBufferVTable == b.sizeofCommandBufferVTable
		&& a.sizeofSwapChainVTable     == b.sizeofSwapChainVTable
		&& a.sizeofRenderTargetVTable  == b.sizeofRenderTargetVTable
		&& a.sizeofDepthStencilVTable  == b.sizeofDepthStencilVTable
		&& a.sizeofPipelineDesc        == b.sizeofPipelineDesc
		&& a.sizeofDrawCmd             == b.sizeofDrawCmd;
}

using PFN_GFX_CreateBackend  = IBackendPlugin* (*)();
using PFN_GFX_DestroyBackend = void (*)(IBackendPlugin*);
using PFN_GFX_GetABIInfo     = void (*)(GfxABIInfo*);

#define GFX_PLUGIN_EXPORT_FUNCTIONS(DeviceClass)                        \
	extern "C" GFX_EXPORT void GFX_GetABIInfo(GfxABIInfo* out) {        \
		if (out) *out = gfxCurrentABIInfo();                             \
	}                                                                    \
	extern "C" GFX_EXPORT IBackendPlugin* GFX_CreateBackend() {         \
		auto* obj  = new DeviceClass();                                  \
		auto* plug = new IBackendPlugin();                               \
		plug->vtable = DeviceClass::getVTable();                         \
		plug->self   = obj;                                              \
		return plug;                                                     \
	}                                                                    \
	extern "C" GFX_EXPORT void GFX_DestroyBackend(IBackendPlugin* p) {  \
		if (!p) return;                                                  \
		static_cast<DeviceClass*>(p->self)->shutdown();                  \
		delete static_cast<DeviceClass*>(p->self);                       \
		delete p;                                                        \
	}
