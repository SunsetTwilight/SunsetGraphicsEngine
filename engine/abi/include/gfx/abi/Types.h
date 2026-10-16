#pragma once
// ============================================================
//  gfx/abi/Types.h
//  共通型と v1 記述子 (フォーマット、頂点レイアウト、ハンドル、RT/DS/Buffer/Texture/Sampler、v1 パイプライン、ルートシグネチャ、加速構造)
//  ※ GfxBackendABI.h の一部。単独で include してもよい (前のパートを読み込む)
// ============================================================
#include <cstdint>

// ============================================================
//  共通型
// ============================================================
enum class GraphicsAPI : uint32_t { DirectX12 = 0, Vulkan = 1, Auto = 0xFFFF };
// Fragment = ピクセルシェーダー。Geometry / Hull / Domain は ABI 7 で追加 (値は末尾に追加する)
enum class ShaderStage : uint32_t { Vertex = 0, Fragment = 1, Compute = 2, Geometry = 3, Hull = 4, Domain = 5 };

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
// 使うアダプター
enum class GfxAdapterType : uint32_t {
	Hardware = 0,   // GPU (ソフトウェアアダプターは除く)
	Software = 1,   // ソフトウェアレンダラー (DX12 では WARP)。GPU のない CI などで使う
};

// デバッグメッセージ (D3D12 デバッグレイヤーなど) の重要度
enum class GfxMessageSeverity : uint32_t {
	Corruption = 0,
	Error      = 1,
	Warning    = 2,
	Info       = 3,   // 情報 (例: デバッグレイヤーを有効にできなかった)
};

// デバッグメッセージの id。0 以上は D3D12_MESSAGE_ID (デバッグレイヤー)、負の値はエンジン自身のメッセージ
constexpr int32_t GFX_MESSAGE_ID_ENGINE         = -1;   // 一般的な情報
constexpr int32_t GFX_MESSAGE_ID_SHADER_COMPILE = -2;   // シェーダーのコンパイルエラー / 警告 (validation に関係なく通知する)
constexpr int32_t GFX_MESSAGE_ID_PIPELINE       = -3;   // パイプラインの作成 / 作り直しの失敗

// デバッグメッセージを受け取るコールバック。
// メッセージを発生させた API を呼んだスレッドから呼ばれる。text は呼び出し中のみ有効 (UTF-8)
using PFN_GfxDebugMessage = void (*)(void* user, GfxMessageSeverity severity, int32_t id, const char* text);

struct GfxDeviceConfig {
	GraphicsAPI api        = GraphicsAPI::Auto;
	bool        validation = false;   // デバッグレイヤー (Debug ビルドのバックエンドのみ有効)
	char        appName[128] = "GraphicsApp";

	GfxAdapterType      adapterType   = GfxAdapterType::Hardware;
	// メッセージの通知先 (デバッグレイヤーのメッセージは validation 有効時のみ)。nullptr なら stderr に出力する
	PFN_GfxDebugMessage debugCallback = nullptr;
	void*               debugUser     = nullptr;
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
