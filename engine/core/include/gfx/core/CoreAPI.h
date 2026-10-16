#pragma once
// ============================================================
//  gfx/core/CoreAPI.h
//
//  エンジン本体 (SunsetGraphicsEngine.dll) の C ABI。
//
//  本体 DLL の役割は次の 4 つだけ:
//    ・バックエンド DLL (gfx_dx12 など) のロードと ABI の照合
//    ・デバイス (IBackendPlugin) の寿命管理
//    ・ウィンドウ (スワップチェーン) の管理
//    ・バックエンドの実行時切り替え (switchAPI)
//  リソースの作成や描画コマンドは、gfxEngineGetDevice() が返すデバイスの
//  vtable (gfx/abi) を直接呼ぶ。
//
//  DLL の境界を越えるのは POD・関数ポインタ・不透明ハンドルだけにする。
//  これにより、アプリと本体 DLL のコンパイラ / CRT / Debug・Release が違っても動く。
//  C++ から使う場合は gfx/core/GraphicsEngine.h (ヘッダーのみのラッパー) を使う。
// ============================================================
#include "gfx/abi/GfxBackendABI.h"
#include <cstdint>

#if defined(_WIN32)
  #if defined(GFX_CORE_BUILDING_DLL)
    #define GFX_CORE_API __declspec(dllexport)
  #else
    #define GFX_CORE_API __declspec(dllimport)
  #endif
#else
  #define GFX_CORE_API __attribute__((visibility("default")))
#endif

// 不透明ハンドル (実体は本体 DLL 内にある)
struct GfxEngine;
struct GfxWindow;

// switchAPI の前後に呼ばれるコールバック。user は gfxEngineSwitchAPI に渡した値
using PFN_GfxSwapCallback = void (*)(void* user, GraphicsAPI oldApi, GraphicsAPI newApi);

extern "C" {

// ---- ABI の照合 ----
// 本体 DLL がビルドされたときの ABI 情報 (gfx/abi の GfxABIInfo) を返す。
// ラッパーは自分のヘッダーの gfxCurrentABIInfo() と比較し、一致しなければ初期化しない。
GFX_CORE_API void gfxCoreGetABIInfo(GfxABIInfo* out);

// ---- エンジン ----
GFX_CORE_API GfxEngine* gfxEngineCreate(void);
GFX_CORE_API void       gfxEngineDestroy(GfxEngine* engine);   // shutdown も行う

// バックエンド DLL の探索パスを追加する (UTF-8)。既定は「実行ファイルのディレクトリ」
// 「本体 DLL のディレクトリ」「実行ファイルの ../lib」
GFX_CORE_API void gfxEngineAddPluginSearchPath(GfxEngine* engine, const char* dirUtf8);

GFX_CORE_API bool gfxEngineInit    (GfxEngine* engine, const GfxDeviceConfig* config);
GFX_CORE_API void gfxEngineShutdown(GfxEngine* engine);       // ウィンドウもすべて破棄する
GFX_CORE_API bool gfxEngineIsInitialized(const GfxEngine* engine);

// 現在のデバイス。未初期化 / switchAPI 失敗後は nullptr。
// switchAPI のたびに変わるので、保持せずに毎回取得すること。
GFX_CORE_API IBackendPlugin* gfxEngineGetDevice(GfxEngine* engine);

// 直近のエラーメッセージ (UTF-8)。次の呼び出しまで有効。エラーがなければ ""
GFX_CORE_API const char* gfxEngineGetLastError(const GfxEngine* engine);

// バックエンドを切り替える。pre / post は nullptr でもよい。
// pre の中でアプリは旧バックエンドのリソースをすべて破棄すること (ウィンドウは本体が作り直す)。
// 失敗した場合 false を返し、エンジンは未初期化状態になる。
GFX_CORE_API bool gfxEngineSwitchAPI(GfxEngine* engine, GraphicsAPI newApi,
                                     PFN_GfxSwapCallback pre, PFN_GfxSwapCallback post, void* user);

// ---- ウィンドウ (スワップチェーン) ----
// name は UTF-8 (ログ用)。失敗時は nullptr
GFX_CORE_API GfxWindow* gfxEngineCreateWindow (GfxEngine* engine, const GfxSwapChainDesc* desc, const char* nameUtf8);
GFX_CORE_API void       gfxEngineDestroyWindow(GfxEngine* engine, GfxWindow* window);

// ウィンドウの現在のスワップチェーン。switchAPI のたびに変わるので、保持せずに毎回取得すること。
// switchAPI に失敗した後は nullptr。
GFX_CORE_API ISwapChain* gfxWindowGetSwapChain(GfxWindow* window);

// リサイズ。最小化 (0x0) は無視して true を返す。サイズは switchAPI での作り直しにも使われる
GFX_CORE_API bool gfxWindowResize(GfxWindow* window, uint32_t width, uint32_t height);

} // extern "C"
