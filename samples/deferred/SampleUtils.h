#pragma once
// ============================================================
//  samples/deferred/SampleUtils.h
//  サンプル用のエラーチェックと、スキーマによる記述子のダンプ
// ============================================================
#include "gfx/core/GraphicsEngine.h"
#include "gfx/schema/GfxSchema.h"
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

// ------------------------------------------------------------
//  エラーチェックヘルパー
//  ポインタが nullptr なら例外を投げる
// ------------------------------------------------------------
template <typename T>
T* check(T* ptr, const char* msg)
{
	if (!ptr)
		throw std::runtime_error(std::string("[Error] ") + msg);
	return ptr;
}

// BufferHandle / ShaderHandle / TextureHandle / SamplerHandle のバリデーション
inline void checkHandle(BufferHandle  h, const char* msg)
{
	if (!h.valid())
		throw std::runtime_error(std::string("[Error] ") + msg);
}
inline void checkHandle(ShaderHandle  h, const char* msg)
{
	if (!h.valid())
		throw std::runtime_error(std::string("[Error] ") + msg);
}
inline void checkHandle(TextureHandle h, const char* msg)
{
	if (!h.valid())
		throw std::runtime_error(std::string("[Error] ") + msg);
}
inline void checkHandle(SamplerHandle h, const char* msg)
{
	if (!h.valid())
		throw std::runtime_error(std::string("[Error] ") + msg);
}
inline void checkHandle(BindingLayoutHandle h, const char* msg)
{
	if (!h.valid())
		throw std::runtime_error(std::string("[Error] ") + msg);
}

// ---------------------------------------------------
//  GFX_EXAMPLE_DUMP_SCHEMA=1 のとき、v2 記述子の値をスキーマ (X マクロで生成した
//  メタデータ表) だけを使って出力する。インスペクター / JSON シリアライザーの土台の確認用。
//  配列は先頭の 2 要素だけを出力する。
// ---------------------------------------------------
inline void dumpBySchema(const GfxStructInfo* si, const void* obj, int indent)
{
	const std::string pad(static_cast<size_t>(indent) * 2, ' ');
	for (uint32_t i = 0; i < si->fieldCount; ++i)
	{
		const GfxFieldInfo& f = si->fields[i];
		const uint32_t count  = (f.type == GfxFieldType::Char) ? 1u : f.arrayCount;
		const uint32_t shown  = count < 2u ? count : 2u;
		for (uint32_t k = 0; k < shown; ++k)
		{
			const void* p = gfxFieldPtr(obj, f, k);
			std::cout << pad << f.name;
			if (count > 1)
				std::cout << "[" << k << "]";
			std::cout << " (+" << (f.offset + f.size * k) << ")";

			switch (f.type)
			{
			case GfxFieldType::Struct:
				std::cout << " : " << f.structInfo->name << "\n";
				dumpBySchema(f.structInfo, p, indent + 1);
				continue;
			case GfxFieldType::Bool:
				std::cout << " = " << (gfxReadUnsigned(p, f.size) ? "true" : "false");
				break;
			case GfxFieldType::UInt8:
			case GfxFieldType::UInt32:
			case GfxFieldType::Handle:
				std::cout << " = " << gfxReadUnsigned(p, f.size);
				break;
			case GfxFieldType::Int32:
			{
				int32_t v = 0;
				std::memcpy(&v, p, sizeof(v));
				std::cout << " = " << v;
				break;
			}
			case GfxFieldType::Float:
			{
				float v = 0.0f;
				std::memcpy(&v, p, sizeof(v));
				std::cout << " = " << v;
				break;
			}
			case GfxFieldType::Enum:
			{
				const uint64_t v  = gfxReadUnsigned(p, f.size);
				const char*    nm = gfxEnumName(f.enumInfo, v);
				std::cout << " = " << f.enumInfo->name << "::" << (nm ? nm : "?");
				break;
			}
			case GfxFieldType::Flags:
			{
				const uint64_t v = gfxReadUnsigned(p, f.size);
				std::cout << " = " << f.enumInfo->name << "(";
				bool first = true;
				for (uint32_t b = 0; b < f.enumInfo->valueCount; ++b)
				{
					const GfxEnumerator& e = f.enumInfo->values[b];
					// 単一ビットの列挙子だけを出す (All などの合成値は除く)
					const bool singleBit = e.value != 0 && (e.value & (e.value - 1)) == 0;
					if (singleBit && (v & e.value) == e.value)
					{
						std::cout << (first ? "" : "|") << e.name;
						first = false;
					}
				}
				std::cout << ")";
				break;
			}
			case GfxFieldType::Char:
				std::cout << " = \"" << static_cast<const char*>(p) << "\"";
				break;
			case GfxFieldType::Pointer:
				std::cout << " = (pointer)";
				break;
			case GfxFieldType::CString:
			{
				const char* s = nullptr;
				std::memcpy(&s, p, sizeof(s));
				std::cout << " = " << (s ? s : "(null)");
				break;
			}
			}
			std::cout << "\n";
		}
		if (count > shown)
			std::cout << pad << f.name << "[" << shown << ".." << (count - 1) << "] ...\n";
	}
}
inline void checkHandle(PipelineHandle h, const char* msg)
{
	if (!h.valid())
		throw std::runtime_error(std::string("[Error] ") + msg);
}
