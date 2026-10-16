#pragma once
// ============================================================
//  gfx/abi/ShaderReflection.h
//  ABI 7: シェーダーのリフレクション (バインディング、定数バッファのレイアウト、入出力シグネチャ)
//  ※ GfxBackendABI.h の一部。単独で include してもよい (前のパートを読み込む)
//
//  ・D3D12_SHADER_DESC / D3D12_SHADER_INPUT_BIND_DESC / D3D12_SHADER_BUFFER_DESC /
//    D3D12_SHADER_VARIABLE_DESC / D3D12_SIGNATURE_PARAMETER_DESC に対応する。
//    列挙値も D3D の値と同じ (名前は API 非依存)。
//  ・ほかの記述子と同じく X マクロで定義するので、スキーマ (GfxSchema.h) から
//    JSON への変換やエディターの表示がそのまま使える。
//  ・可変長のデータは固定長配列 + 個数で持つ。上限を超えた分は切り捨て、truncated を true にする。
//  ・大きい (約 35 KB) ので、スタックではなくヒープに置くこと。
// ============================================================

#include "Compat.h"

constexpr uint32_t GFX_MAX_SHADER_NAME       = 64;    // 終端の '\0' を含む
constexpr uint32_t GFX_MAX_SHADER_BINDINGS   = 64;
constexpr uint32_t GFX_MAX_SHADER_CBUFFERS   = 16;
constexpr uint32_t GFX_MAX_SHADER_VARIABLES  = 256;
constexpr uint32_t GFX_MAX_SHADER_SIGNATURE  = 32;

constexpr uint32_t GFX_SHADER_REFLECTION_VERSION = 1u;

// ---- 列挙型 ------------------------------------------------

// D3D_SHADER_INPUT_TYPE
#define GFX_ENUM_GfxShaderInputType(E) \
	E(CBuffer, 0) E(TBuffer, 1) E(Texture, 2) E(Sampler, 3) E(UAVRWTyped, 4) E(Structured, 5) \
	E(UAVRWStructured, 6) E(ByteAddress, 7) E(UAVRWByteAddress, 8) E(UAVAppendStructured, 9) \
	E(UAVConsumeStructured, 10) E(UAVRWStructuredWithCounter, 11) E(RTAccelerationStructure, 12) \
	E(UAVFeedbackTexture, 13)
GFX_DECLARE_ENUM(GfxShaderInputType, uint32_t)

// D3D_SRV_DIMENSION
#define GFX_ENUM_GfxShaderResourceDimension(E) \
	E(Unknown, 0) E(Buffer, 1) E(Texture1D, 2) E(Texture1DArray, 3) E(Texture2D, 4) E(Texture2DArray, 5) \
	E(Texture2DMS, 6) E(Texture2DMSArray, 7) E(Texture3D, 8) E(TextureCube, 9) E(TextureCubeArray, 10) \
	E(BufferEx, 11)
GFX_DECLARE_ENUM(GfxShaderResourceDimension, uint32_t)

// D3D_SHADER_VARIABLE_CLASS
#define GFX_ENUM_GfxShaderVariableClass(E) \
	E(Scalar, 0) E(Vector, 1) E(MatrixRows, 2) E(MatrixColumns, 3) E(Object, 4) E(Struct, 5) \
	E(InterfaceClass, 6) E(InterfacePointer, 7)
GFX_DECLARE_ENUM(GfxShaderVariableClass, uint32_t)

// D3D_SHADER_VARIABLE_TYPE (定数バッファに置けるもの)
#define GFX_ENUM_GfxShaderVariableType(E) \
	E(Void, 0) E(Bool, 1) E(Int, 2) E(Float, 3) E(UInt, 19) E(UInt8, 20) E(Double, 39) \
	E(Min8Float, 55) E(Min10Float, 56) E(Min16Float, 57) E(Min12Int, 58) E(Min16Int, 59) E(Min16UInt, 60) \
	E(Int16, 61) E(UInt16, 62) E(Float16, 63) E(Int64, 64) E(UInt64, 65)
GFX_DECLARE_ENUM(GfxShaderVariableType, uint32_t)

// D3D_REGISTER_COMPONENT_TYPE
#define GFX_ENUM_GfxRegisterComponentType(E) \
	E(Unknown, 0) E(UInt32, 1) E(SInt32, 2) E(Float32, 3) E(UInt16, 4) E(SInt16, 5) E(Float16, 6) \
	E(UInt64, 7) E(SInt64, 8) E(Float64, 9)
GFX_DECLARE_ENUM(GfxRegisterComponentType, uint32_t)

// D3D_NAME (システム値のセマンティクス)
#define GFX_ENUM_GfxSystemValue(E) \
	E(Undefined, 0) E(Position, 1) E(ClipDistance, 2) E(CullDistance, 3) E(RenderTargetArrayIndex, 4) \
	E(ViewportArrayIndex, 5) E(VertexId, 6) E(PrimitiveId, 7) E(InstanceId, 8) E(IsFrontFace, 9) \
	E(SampleIndex, 10) E(FinalQuadEdgeTessFactor, 11) E(FinalQuadInsideTessFactor, 12) \
	E(FinalTriEdgeTessFactor, 13) E(FinalTriInsideTessFactor, 14) E(FinalLineDetailTessFactor, 15) \
	E(FinalLineDensityTessFactor, 16) E(Barycentrics, 23) E(ShadingRate, 24) E(CullPrimitive, 25) \
	E(Target, 64) E(Depth, 65) E(Coverage, 66) E(DepthGreaterEqual, 67) E(DepthLessEqual, 68) \
	E(StencilRef, 69) E(InnerCoverage, 70)
GFX_DECLARE_ENUM(GfxSystemValue, uint32_t)

// ---- 構造体 ------------------------------------------------

// D3D12_SHADER_INPUT_BIND_DESC (t / s / u / b レジスタへのバインド)
#define GFX_FIELDS_GfxShaderBinding(F, FA) \
	FA(Char,  char,                       name,         GFX_MAX_SHADER_NAME,                          "D3D12_SHADER_INPUT_BIND_DESC::Name") \
	F(Enum,   GfxShaderInputType,         type,         GfxShaderInputType::CBuffer,      0, 0,       "D3D12_SHADER_INPUT_BIND_DESC::Type") \
	F(Enum,   GfxShaderResourceDimension, dimension,    GfxShaderResourceDimension::Unknown, 0, 0,    "D3D12_SHADER_INPUT_BIND_DESC::Dimension") \
	F(UInt32, uint32_t,                   bindPoint,    0, 0, 0xFFFFFFFFu,                            "D3D12_SHADER_INPUT_BIND_DESC::BindPoint (レジスタ番号)") \
	F(UInt32, uint32_t,                   bindCount,    1, 0, 0xFFFFFFFFu,                            "D3D12_SHADER_INPUT_BIND_DESC::BindCount (配列の要素数。0 = 境界なし)") \
	F(UInt32, uint32_t,                   space,        0, 0, 0xFFFFFFFFu,                            "D3D12_SHADER_INPUT_BIND_DESC::Space") \
	F(UInt32, uint32_t,                   cbufferIndex, 0xFFFFFFFFu, 0, 0xFFFFFFFFu,                  "type が CBuffer のとき cbuffers の位置 (それ以外は 0xFFFFFFFF)")
struct GfxShaderBinding
{
	GFX_FIELDS_GfxShaderBinding(GFX_DECLARE_FIELD, GFX_DECLARE_ARRAY)
};

// D3D12_SHADER_BUFFER_DESC (定数バッファ)
#define GFX_FIELDS_GfxShaderCBuffer(F, FA) \
	FA(Char,  char,     name,          GFX_MAX_SHADER_NAME,             "D3D12_SHADER_BUFFER_DESC::Name") \
	F(UInt32, uint32_t, size,          0, 0, 0xFFFFFFFFu,               "D3D12_SHADER_BUFFER_DESC::Size (バイト)") \
	F(UInt32, uint32_t, bindPoint,     0, 0, 0xFFFFFFFFu,               "b レジスタの番号") \
	F(UInt32, uint32_t, space,         0, 0, 0xFFFFFFFFu,               "レジスタの space") \
	F(UInt32, uint32_t, firstVariable, 0, 0, GFX_MAX_SHADER_VARIABLES,  "variables の先頭の位置") \
	F(UInt32, uint32_t, variableCount, 0, 0, GFX_MAX_SHADER_VARIABLES,  "この定数バッファの変数の数 (構造体のメンバーは展開して数える)")
struct GfxShaderCBuffer
{
	GFX_FIELDS_GfxShaderCBuffer(GFX_DECLARE_FIELD, GFX_DECLARE_ARRAY)
};

// D3D12_SHADER_VARIABLE_DESC + D3D12_SHADER_TYPE_DESC
// 構造体のメンバーは "light.color" のように . でつないだ名前に展開する (offset は定数バッファの先頭から)
#define GFX_FIELDS_GfxShaderVariable(F, FA) \
	FA(Char,  char,                   name,          GFX_MAX_SHADER_NAME,                       "変数名 (構造体のメンバーは parent.member)") \
	F(UInt32, uint32_t,               cbufferIndex,  0, 0, GFX_MAX_SHADER_CBUFFERS,             "属する定数バッファの位置") \
	F(UInt32, uint32_t,               offset,        0, 0, 0xFFFFFFFFu,                         "D3D12_SHADER_VARIABLE_DESC::StartOffset (定数バッファの先頭からのバイト数)") \
	F(UInt32, uint32_t,               size,          0, 0, 0xFFFFFFFFu,                         "D3D12_SHADER_VARIABLE_DESC::Size (バイト)") \
	F(Enum,   GfxShaderVariableClass, variableClass, GfxShaderVariableClass::Scalar, 0, 0,      "D3D12_SHADER_TYPE_DESC::Class") \
	F(Enum,   GfxShaderVariableType,  variableType,  GfxShaderVariableType::Float,   0, 0,      "D3D12_SHADER_TYPE_DESC::Type") \
	F(UInt32, uint32_t,               rows,          1, 0, 4,                                   "D3D12_SHADER_TYPE_DESC::Rows") \
	F(UInt32, uint32_t,               columns,       1, 0, 4,                                   "D3D12_SHADER_TYPE_DESC::Columns") \
	F(UInt32, uint32_t,               elements,      0, 0, 0xFFFFFFFFu,                         "D3D12_SHADER_TYPE_DESC::Elements (配列の要素数。0 = 配列ではない)") \
	F(UInt32, uint32_t,               depth,         0, 0, 16,                                  "構造体の入れ子の深さ (0 = 定数バッファの直下)")
struct GfxShaderVariable
{
	GFX_FIELDS_GfxShaderVariable(GFX_DECLARE_FIELD, GFX_DECLARE_ARRAY)
};

// D3D12_SIGNATURE_PARAMETER_DESC (入力 / 出力のシグネチャ)
#define GFX_FIELDS_GfxShaderSignatureParam(F, FA) \
	FA(Char,  char,                     semanticName,  GFX_MAX_SEMANTIC_NAME,                          "D3D12_SIGNATURE_PARAMETER_DESC::SemanticName") \
	F(UInt32, uint32_t,                 semanticIndex, 0, 0, 0xFFFFFFFFu,                               "D3D12_SIGNATURE_PARAMETER_DESC::SemanticIndex") \
	F(UInt32, uint32_t,                 registerIndex, 0, 0, 0xFFFFFFFFu,                               "D3D12_SIGNATURE_PARAMETER_DESC::Register") \
	F(Enum,   GfxSystemValue,           systemValue,   GfxSystemValue::Undefined,           0, 0,       "D3D12_SIGNATURE_PARAMETER_DESC::SystemValueType") \
	F(Enum,   GfxRegisterComponentType, componentType, GfxRegisterComponentType::Float32,   0, 0,       "D3D12_SIGNATURE_PARAMETER_DESC::ComponentType") \
	F(UInt8,  uint8_t,                  mask,          0, 0, 15,                                        "D3D12_SIGNATURE_PARAMETER_DESC::Mask (xyzw = 1/2/4/8)") \
	F(UInt8,  uint8_t,                  readWriteMask, 0, 0, 15,                                        "D3D12_SIGNATURE_PARAMETER_DESC::ReadWriteMask")
struct GfxShaderSignatureParam
{
	GFX_FIELDS_GfxShaderSignatureParam(GFX_DECLARE_FIELD, GFX_DECLARE_ARRAY)
};

// D3D12_SHADER_DESC と上の配列をまとめたもの
#define GFX_FIELDS_GfxShaderReflection(F, FA) \
	F(UInt32, uint32_t,                structSize,    sizeof(GfxShaderReflection),   0, 0xFFFFFFFFu,              "構造体のバイト数 (sizeof)。バックエンドはここまでしか書かない") \
	F(UInt32, uint32_t,                version,       GFX_SHADER_REFLECTION_VERSION, 0, 0xFFFFFFFFu,              "構造体のバージョン") \
	F(UInt32, uint32_t,                shaderModel,   0, 0, 0xFFFFFFFFu,                                          "シェーダーモデル (6.0 なら 60)") \
	FA(UInt32, uint32_t,               threadGroupSize, 3,                                                        "[numthreads(x, y, z)] (Compute のみ)") \
	F(UInt32, uint32_t,                instructionCount, 0, 0, 0xFFFFFFFFu,                                       "D3D12_SHADER_DESC::InstructionCount") \
	F(Bool,   bool,                    truncated,     false, 0, 1,                                                "配列の上限を超えて切り捨てたものがある") \
	FA(Struct, GfxShaderBinding,       bindings,      GFX_MAX_SHADER_BINDINGS,                                    "D3D12_SHADER_DESC::BoundResources") \
	F(UInt32, uint32_t,                bindingCount,  0, 0, GFX_MAX_SHADER_BINDINGS,                              "bindings の個数") \
	FA(Struct, GfxShaderCBuffer,       cbuffers,      GFX_MAX_SHADER_CBUFFERS,                                    "D3D12_SHADER_DESC::ConstantBuffers") \
	F(UInt32, uint32_t,                cbufferCount,  0, 0, GFX_MAX_SHADER_CBUFFERS,                              "cbuffers の個数") \
	FA(Struct, GfxShaderVariable,      variables,     GFX_MAX_SHADER_VARIABLES,                                   "定数バッファの変数 (全定数バッファ分)") \
	F(UInt32, uint32_t,                variableCount, 0, 0, GFX_MAX_SHADER_VARIABLES,                             "variables の個数") \
	FA(Struct, GfxShaderSignatureParam, inputs,       GFX_MAX_SHADER_SIGNATURE,                                   "D3D12_SHADER_DESC::InputParameters") \
	F(UInt32, uint32_t,                inputCount,    0, 0, GFX_MAX_SHADER_SIGNATURE,                             "inputs の個数") \
	FA(Struct, GfxShaderSignatureParam, outputs,      GFX_MAX_SHADER_SIGNATURE,                                   "D3D12_SHADER_DESC::OutputParameters") \
	F(UInt32, uint32_t,                outputCount,   0, 0, GFX_MAX_SHADER_SIGNATURE,                             "outputs の個数")
struct GfxShaderReflection
{
	GFX_FIELDS_GfxShaderReflection(GFX_DECLARE_FIELD, GFX_DECLARE_ARRAY)

	// 名前でバインディングを探す (見つからなければ nullptr)
	const GfxShaderBinding* findBinding(const char* n) const
	{
		for (uint32_t i = 0; i < bindingCount && i < GFX_MAX_SHADER_BINDINGS; ++i)
		{
			uint32_t k = 0;
			while (n && n[k] && n[k] == bindings[i].name[k]) ++k;
			if (n && n[k] == '\0' && bindings[i].name[k] == '\0')
				return &bindings[i];
		}
		return nullptr;
	}
};
