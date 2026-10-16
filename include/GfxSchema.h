#pragma once
// ============================================================
//  GfxSchema.h
//
//  ABI v2 記述子のフィールドのメタデータ表 (スキーマ)。
//  GfxBackendABI.h の GFX_FIELDS_<型名> / GFX_ENUM_<型名> の X マクロから生成するので、
//  構造体の定義と表がずれることはない。
//
//  エディターのプロパティインスペクター、JSON シリアライザー、差分表示は
//  この表をたどって汎用的に作る (構造体ごとの手書きのコードは書かない)。
//
//  使い方:
//    const GfxStructInfo* si = gfxStructInfo<GfxGraphicsPipelineDesc>();
//    for (uint32_t i = 0; i < si->fieldCount; ++i) {
//        const GfxFieldInfo& f = si->fields[i];
//        // f.name, f.type, f.offset, f.enumInfo, f.structInfo, f.minValue ...
//    }
// ============================================================

#include "GfxBackendABI.h"
#include <cstddef>
#include <cstring>

enum class GfxFieldType : uint32_t
{
	Bool,      // bool
	UInt8,     // uint8_t
	Int32,     // int32_t
	UInt32,    // uint32_t
	Float,     // float
	Enum,      // 列挙型 (enumInfo に列挙子の表)
	Flags,     // ビットフラグの列挙型 (enumInfo に各ビットの表)
	Struct,    // 入れ子の構造体 (structInfo にフィールドの表)
	Char,      // 固定長の文字列 (char[arrayCount]、'\0' 終端)
	Handle,    // リソースのハンドル (uint64_t id)
	CString,   // const char* (シリアライズしない)
	Pointer,   // const void* (シリアライズしない)
};

struct GfxEnumerator
{
	const char* name;
	uint64_t    value;
};

struct GfxEnumInfo
{
	const char*          name;        // "GfxBlend"
	uint32_t             size;        // 列挙型のバイト数
	bool                 isFlags;     // ビットフラグか
	const GfxEnumerator* values;
	uint32_t             valueCount;
};

struct GfxStructInfo;

struct GfxFieldInfo
{
	const char*          name;        // "depthBias"
	GfxFieldType         type;        // 配列のときは要素の型
	uint32_t             offset;      // offsetof
	uint32_t             size;        // 要素 1 個のバイト数
	uint32_t             arrayCount;  // 1 = スカラー、それ以外は固定長配列の要素数
	const GfxEnumInfo*   enumInfo;    // Enum / Flags のとき
	const GfxStructInfo* structInfo;  // Struct のとき
	double               minValue;    // UI 用の範囲 (数値型のとき)
	double               maxValue;
	const char*          tooltip;     // 対応する D3D12 のメンバー
};

struct GfxStructInfo
{
	const char*         name;         // "GfxRasterizerDesc"
	uint32_t            size;         // sizeof
	const GfxFieldInfo* fields;
	uint32_t            fieldCount;
};

// ---- 型 → メタデータの対応 ---------------------------------
// 列挙型と構造体は下の GFX_SCHEMA_ENUM / GFX_SCHEMA_STRUCT で特殊化する。
template<typename T>
struct GfxTypeInfo
{
	static const GfxEnumInfo*   enumInfo()   { return nullptr; }
	static const GfxStructInfo* structInfo() { return nullptr; }
};

template<typename T> inline const GfxEnumInfo*   gfxEnumInfo()   { return GfxTypeInfo<T>::enumInfo();   }
template<typename T> inline const GfxStructInfo* gfxStructInfo() { return GfxTypeInfo<T>::structInfo(); }

#define GFX_SCHEMA_ENUMERATOR(name, value) { #name, static_cast<uint64_t>(value) },

#define GFX_SCHEMA_ENUM_IMPL(Type, isFlags)                                                     \
	template<> struct GfxTypeInfo<Type>                                                         \
	{                                                                                           \
		static const GfxEnumInfo* enumInfo()                                                    \
		{                                                                                       \
			static const GfxEnumerator values[] = { GFX_ENUM_##Type(GFX_SCHEMA_ENUMERATOR) };   \
			static const GfxEnumInfo   info = { #Type, static_cast<uint32_t>(sizeof(Type)), isFlags, \
				values, static_cast<uint32_t>(sizeof(values) / sizeof(values[0])) };            \
			return &info;                                                                       \
		}                                                                                       \
		static const GfxStructInfo* structInfo() { return nullptr; }                            \
	};
#define GFX_SCHEMA_ENUM(Type)  GFX_SCHEMA_ENUM_IMPL(Type, false)
#define GFX_SCHEMA_FLAGS(Type) GFX_SCHEMA_ENUM_IMPL(Type, true)

#define GFX_SCHEMA_FIELD(kind, type, name, def, mn, mx, tip)                        \
	{ #name, GfxFieldType::kind,                                                    \
	  static_cast<uint32_t>(offsetof(Self, name)), static_cast<uint32_t>(sizeof(type)), 1u, \
	  GfxTypeInfo<type>::enumInfo(), GfxTypeInfo<type>::structInfo(),               \
	  static_cast<double>(mn), static_cast<double>(mx), tip },

#define GFX_SCHEMA_ARRAY(kind, type, name, count, tip)                              \
	{ #name, GfxFieldType::kind,                                                    \
	  static_cast<uint32_t>(offsetof(Self, name)), static_cast<uint32_t>(sizeof(type)), \
	  static_cast<uint32_t>(count),                                                 \
	  GfxTypeInfo<type>::enumInfo(), GfxTypeInfo<type>::structInfo(), 0.0, 0.0, tip },

// 入れ子の構造体は、それを含む構造体より先に GFX_SCHEMA_STRUCT すること
#define GFX_SCHEMA_STRUCT(Type)                                                                  \
	template<> struct GfxTypeInfo<Type>                                                          \
	{                                                                                            \
		static const GfxEnumInfo* enumInfo() { return nullptr; }                                 \
		static const GfxStructInfo* structInfo()                                                 \
		{                                                                                        \
			using Self = Type;                                                                   \
			static const GfxFieldInfo fields[] = {                                               \
				GFX_FIELDS_##Type(GFX_SCHEMA_FIELD, GFX_SCHEMA_ARRAY) };                         \
			static const GfxStructInfo info = { #Type, static_cast<uint32_t>(sizeof(Type)),      \
				fields, static_cast<uint32_t>(sizeof(fields) / sizeof(fields[0])) };             \
			return &info;                                                                        \
		}                                                                                        \
	};

// ---- 列挙型 ------------------------------------------------
GFX_SCHEMA_ENUM (GfxFormat)
GFX_SCHEMA_ENUM (GfxBlend)
GFX_SCHEMA_ENUM (GfxBlendOp)
GFX_SCHEMA_ENUM (GfxLogicOp)
GFX_SCHEMA_FLAGS(GfxColorWriteMask)
GFX_SCHEMA_ENUM (GfxFillMode)
GFX_SCHEMA_ENUM (GfxCullMode)
GFX_SCHEMA_ENUM (GfxConservativeRaster)
GFX_SCHEMA_ENUM (GfxCompareFunc)
GFX_SCHEMA_ENUM (GfxDepthWriteMask)
GFX_SCHEMA_ENUM (GfxStencilOp)
GFX_SCHEMA_ENUM (GfxInputClassification)
GFX_SCHEMA_ENUM (GfxIndexBufferStripCut)
GFX_SCHEMA_ENUM (GfxPrimitiveTopologyType)
GFX_SCHEMA_ENUM (GfxPrimitiveTopology)
GFX_SCHEMA_FLAGS(GfxPipelineStateFlags)
GFX_SCHEMA_ENUM (GfxDescriptorRangeType)
GFX_SCHEMA_FLAGS(GfxDescriptorRangeFlags)
GFX_SCHEMA_ENUM (GfxRootParameterType)
GFX_SCHEMA_ENUM (GfxShaderVisibility)
GFX_SCHEMA_FLAGS(GfxRootDescriptorFlags)
GFX_SCHEMA_FLAGS(GfxRootSignatureFlags)
GFX_SCHEMA_ENUM (GfxFilter)
GFX_SCHEMA_ENUM (GfxTextureAddressMode)
GFX_SCHEMA_ENUM (GfxStaticBorderColor)
GFX_SCHEMA_ENUM (GfxQueryType)
GFX_SCHEMA_ENUM (GfxResourceKind)
GFX_SCHEMA_FLAGS(GfxResourceState)
GFX_SCHEMA_ENUM (GfxBarrierType)
GFX_SCHEMA_FLAGS(GfxClearFlags)
GFX_SCHEMA_ENUM (GfxDescriptorType)
GFX_SCHEMA_ENUM (GfxTextureDimension)
GFX_SCHEMA_FLAGS(GfxTextureUsage)
GFX_SCHEMA_ENUM (GfxViewType)
GFX_SCHEMA_ENUM (GfxViewDimension)
GFX_SCHEMA_FLAGS(GfxDSVFlags)

// ---- 構造体 (入れ子の内側から) -----------------------------
GFX_SCHEMA_STRUCT(GfxRenderTargetBlendDesc)
GFX_SCHEMA_STRUCT(GfxBlendDesc)
GFX_SCHEMA_STRUCT(GfxRasterizerDesc)
GFX_SCHEMA_STRUCT(GfxStencilOpDesc)
GFX_SCHEMA_STRUCT(GfxDepthStencilStateDesc)
GFX_SCHEMA_STRUCT(GfxInputElementDesc)
GFX_SCHEMA_STRUCT(GfxInputLayoutDesc)
GFX_SCHEMA_STRUCT(GfxGraphicsPipelineDesc)
GFX_SCHEMA_STRUCT(GfxDescriptorRange)
GFX_SCHEMA_STRUCT(GfxRootParameter)
GFX_SCHEMA_STRUCT(GfxStaticSamplerDesc)
GFX_SCHEMA_STRUCT(GfxBindingLayoutDesc)
GFX_SCHEMA_STRUCT(GfxComputePipelineDesc)
GFX_SCHEMA_STRUCT(GfxQueryPoolDesc)
GFX_SCHEMA_STRUCT(GfxResourceRef)
GFX_SCHEMA_STRUCT(GfxBarrierDesc)
GFX_SCHEMA_STRUCT(GfxViewport)
GFX_SCHEMA_STRUCT(GfxRect)
GFX_SCHEMA_STRUCT(GfxClearValue)
GFX_SCHEMA_STRUCT(GfxTextureResourceDesc)
GFX_SCHEMA_STRUCT(GfxViewDesc)
GFX_SCHEMA_STRUCT(GfxSamplerStateDesc)

// ---- 汎用アクセス ------------------------------------------

// 名前でフィールドを探す (見つからなければ nullptr)
inline const GfxFieldInfo* gfxFindField(const GfxStructInfo* si, const char* name)
{
	if (!si || !name) return nullptr;
	for (uint32_t i = 0; i < si->fieldCount; ++i)
		if (std::strcmp(si->fields[i].name, name) == 0)
			return &si->fields[i];
	return nullptr;
}

// フィールドの index 番目の要素のアドレス (スカラーは index = 0)
inline void* gfxFieldPtr(void* base, const GfxFieldInfo& f, uint32_t index = 0)
{
	return static_cast<uint8_t*>(base) + f.offset + static_cast<size_t>(f.size) * index;
}
inline const void* gfxFieldPtr(const void* base, const GfxFieldInfo& f, uint32_t index = 0)
{
	return static_cast<const uint8_t*>(base) + f.offset + static_cast<size_t>(f.size) * index;
}

// 整数型 / 列挙型 / Bool の値を符号なし整数として読み書きする (size は 1 / 4 / 8 バイト)
inline uint64_t gfxReadUnsigned(const void* p, uint32_t size)
{
	switch (size)
	{
	case 1: { uint8_t  v; std::memcpy(&v, p, 1); return v; }
	case 2: { uint16_t v; std::memcpy(&v, p, 2); return v; }
	case 4: { uint32_t v; std::memcpy(&v, p, 4); return v; }
	case 8: { uint64_t v; std::memcpy(&v, p, 8); return v; }
	default: return 0;
	}
}
inline void gfxWriteUnsigned(void* p, uint32_t size, uint64_t value)
{
	switch (size)
	{
	case 1: { const uint8_t  v = static_cast<uint8_t >(value); std::memcpy(p, &v, 1); break; }
	case 2: { const uint16_t v = static_cast<uint16_t>(value); std::memcpy(p, &v, 2); break; }
	case 4: { const uint32_t v = static_cast<uint32_t>(value); std::memcpy(p, &v, 4); break; }
	case 8: { std::memcpy(p, &value, 8); break; }
	default: break;
	}
}

// 列挙値 → 名前 (見つからなければ nullptr)
inline const char* gfxEnumName(const GfxEnumInfo* ei, uint64_t value)
{
	if (!ei) return nullptr;
	for (uint32_t i = 0; i < ei->valueCount; ++i)
		if (ei->values[i].value == value)
			return ei->values[i].name;
	return nullptr;
}

// 名前 → 列挙値 (見つかれば true)
inline bool gfxEnumValue(const GfxEnumInfo* ei, const char* name, uint64_t& outValue)
{
	if (!ei || !name) return false;
	for (uint32_t i = 0; i < ei->valueCount; ++i)
		if (std::strcmp(ei->values[i].name, name) == 0)
		{
			outValue = ei->values[i].value;
			return true;
		}
	return false;
}
