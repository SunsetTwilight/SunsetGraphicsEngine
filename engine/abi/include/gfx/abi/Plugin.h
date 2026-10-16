#pragma once
// ============================================================
//  gfx/abi/Plugin.h
//  DLL エクスポート、ABI バージョン、プラグインのエクスポートマクロ
//  ※ GfxBackendABI.h の一部。単独で include してもよい (前のパートを読み込む)
// ============================================================

#include "Device.h"
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
//  6: GfxDeviceConfig にアダプターの種類 (WARP) とデバッグメッセージのコールバックを追加
#define GFX_ABI_VERSION 6u

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
	uint32_t sizeofDeviceConfig;
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
		static_cast<uint32_t>(sizeof(GfxDeviceConfig)),
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
		&& a.sizeofDrawCmd             == b.sizeofDrawCmd
		&& a.sizeofDeviceConfig        == b.sizeofDeviceConfig;
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
