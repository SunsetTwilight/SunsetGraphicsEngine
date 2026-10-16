// ============================================================
//  engine/core/src/Engine.cpp
//  エンジン本体 DLL (SunsetGraphicsEngine.dll) の C ABI の実装。
//  公開している関数は gfx/core/CoreAPI.h を参照。
// ============================================================
#include "gfx/core/CoreAPI.h"
#include "PluginLoader.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#elif defined(__APPLE__)
  #include <mach-o/dyld.h>
  #include <dlfcn.h>
#else
  #include <dlfcn.h>
#endif

namespace fs = std::filesystem;

// ============================================================
//  内部の実体
// ============================================================
struct GfxWindow
{
	GfxEngine*       owner     = nullptr;
	ISwapChain*      swapChain = nullptr;
	GfxSwapChainDesc desc      = {};   // switchAPI で作り直すために保持する (サイズは resize で更新)
	std::string      name;
};

struct GfxEngine
{
	PluginLoader                            loader;
	IBackendPlugin*                         plugin      = nullptr;
	GfxDeviceConfig                         config      = {};
	bool                                    initialized = false;
	std::vector<std::unique_ptr<GfxWindow>> windows;
	std::vector<std::string>                searchPaths;
	std::string                             lastError;
};

// ============================================================
//  パスのヘルパー
// ============================================================

// 実行ファイルが存在するディレクトリ (取得できなければ ".")
static std::string executableDir()
{
#if defined(_WIN32)
	wchar_t buf[MAX_PATH];
	GetModuleFileNameW(nullptr, buf, MAX_PATH);
	return fs::path(buf).parent_path().string();
#elif defined(__APPLE__)
	uint32_t size = 0;
	_NSGetExecutablePath(nullptr, &size);
	std::string buf(size, '\0');
	if (_NSGetExecutablePath(buf.data(), &size) != 0)
		return ".";
	std::error_code ec;
	const fs::path p = fs::canonical(fs::path(buf.c_str()), ec);
	return ec ? std::string(".") : p.parent_path().string();
#else
	std::error_code ec;
	const fs::path p = fs::canonical("/proc/self/exe", ec);
	return ec ? std::string(".") : p.parent_path().string();
#endif
}

// この DLL (本体) が存在するディレクトリ。バックエンド DLL は通常ここに並べて配置する
static std::string coreModuleDir()
{
#if defined(_WIN32)
	HMODULE mod = nullptr;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&gfxEngineCreate), &mod))
		return {};
	wchar_t buf[MAX_PATH];
	GetModuleFileNameW(mod, buf, MAX_PATH);
	return fs::path(buf).parent_path().string();
#else
	Dl_info info = {};
	if (dladdr(reinterpret_cast<void*>(&gfxEngineCreate), &info) == 0 || !info.dli_fname)
		return {};
	return fs::path(info.dli_fname).parent_path().string();
#endif
}

static const char* dllBaseName(GraphicsAPI api)
{
	return api == GraphicsAPI::DirectX12 ? "gfx_dx12" : "gfx_vulkan";
}

// DLL の実フルパス (拡張子なし) を探索パスから探して返す。
// 見つからなければ base をそのまま返す (LoadLibrary / dlopen の既定の探索に任せる)
static std::string resolveDllPath(const GfxEngine* e, const std::string& base)
{
#if defined(_WIN32)
	constexpr std::string_view ext = ".dll";
#elif defined(__APPLE__)
	constexpr std::string_view ext = ".dylib";
#else
	constexpr std::string_view ext = ".so";
#endif

	for (const auto& dir : e->searchPaths)
	{
		std::error_code ec;
		if (fs::exists(fs::path(dir) / (base + std::string(ext)), ec))
			return (fs::path(dir) / base).string();
	}
	return base;
}

// ============================================================
//  プラグインのロード / アンロード
// ============================================================
static bool loadPlugin(GfxEngine* e, GraphicsAPI api)
{
	if (!e->loader.load(resolveDllPath(e, dllBaseName(api))))
	{
		e->lastError = e->loader.lastError();
		return false;
	}
	e->plugin = e->loader.createPlugin();
	if (!e->plugin)
	{
		e->lastError = e->loader.lastError();
		return false;
	}
	return true;
}

static void unloadPlugin(GfxEngine* e)
{
	if (e->plugin)
	{
		e->loader.destroyPlugin(e->plugin);
		e->plugin = nullptr;
	}
	e->loader.unload();
}

static void destroySwapChain(GfxWindow* w)
{
	if (w->swapChain)
	{
		w->swapChain->destroy();   // ラッパーごとバックエンド DLL 内で解放される
		w->swapChain = nullptr;
	}
}

// ============================================================
//  C ABI
// ============================================================
extern "C" {

GFX_CORE_API void gfxCoreGetABIInfo(GfxABIInfo* out)
{
	if (out)
		*out = gfxCurrentABIInfo();
}

GFX_CORE_API GfxEngine* gfxEngineCreate(void)
{
	auto* e = new GfxEngine();
	const std::string exeDir = executableDir();
	e->searchPaths.push_back(exeDir);
	const std::string coreDir = coreModuleDir();
	if (!coreDir.empty() && coreDir != exeDir)
		e->searchPaths.push_back(coreDir);
	// インストール配置 (<prefix>/bin/exe と <prefix>/lib/*.so) にも対応する
	e->searchPaths.push_back((fs::path(exeDir) / ".." / "lib").string());
	return e;
}

GFX_CORE_API void gfxEngineDestroy(GfxEngine* engine)
{
	if (!engine)
		return;
	gfxEngineShutdown(engine);
	delete engine;
}

GFX_CORE_API void gfxEngineAddPluginSearchPath(GfxEngine* engine, const char* dirUtf8)
{
	if (engine && dirUtf8 && *dirUtf8)
		engine->searchPaths.emplace_back(dirUtf8);
}

GFX_CORE_API bool gfxEngineInit(GfxEngine* engine, const GfxDeviceConfig* config)
{
	if (!engine || !config)
		return false;
	if (engine->initialized)
		gfxEngineShutdown(engine);

	engine->lastError.clear();
	engine->config = *config;

	GraphicsAPI api = config->api;
	if (api == GraphicsAPI::Auto)
	{
#if defined(_WIN32)
		api = GraphicsAPI::DirectX12;
#else
		api = GraphicsAPI::Vulkan;
#endif
	}

	if (!loadPlugin(engine, api))
	{
		std::cerr << "[Engine] " << engine->lastError << "\n";
		unloadPlugin(engine);
		return false;
	}

	GfxDeviceConfig pod = engine->config;
	pod.api = api;
	if (!engine->plugin->init(pod))
	{
		engine->lastError = "backend init failed";
		unloadPlugin(engine);
		return false;
	}

	engine->config.api  = api;
	engine->initialized = true;
	std::cout << "[Engine] " << engine->plugin->getAdapterName()
			  << " / " << (api == GraphicsAPI::DirectX12 ? "DX12" : "Vulkan") << "\n";
	return true;
}

GFX_CORE_API void gfxEngineShutdown(GfxEngine* engine)
{
	if (!engine)
		return;
	for (auto& w : engine->windows)
		destroySwapChain(w.get());
	engine->windows.clear();
	unloadPlugin(engine);
	engine->initialized = false;
}

GFX_CORE_API bool gfxEngineIsInitialized(const GfxEngine* engine)
{
	return engine && engine->initialized;
}

GFX_CORE_API IBackendPlugin* gfxEngineGetDevice(GfxEngine* engine)
{
	return engine ? engine->plugin : nullptr;
}

GFX_CORE_API const char* gfxEngineGetLastError(const GfxEngine* engine)
{
	return engine ? engine->lastError.c_str() : "";
}

GFX_CORE_API bool gfxEngineSwitchAPI(GfxEngine* engine, GraphicsAPI newApi,
                                     PFN_GfxSwapCallback pre, PFN_GfxSwapCallback post, void* user)
{
	if (!engine || !engine->initialized || !engine->plugin)
		return false;
	if (newApi == GraphicsAPI::Auto || engine->plugin->getAPI() == newApi)
		return true;

	const GraphicsAPI old = engine->plugin->getAPI();

	// アプリはここで旧バックエンドのリソース (RT / DS / Buffer / Pipeline 等) を破棄する
	if (pre)
		pre(user, old, newApi);

	// スワップチェーンは旧 DLL のオブジェクトなので、DLL をアンロードする前に破棄する
	for (auto& w : engine->windows)
		destroySwapChain(w.get());
	unloadPlugin(engine);

	auto fail = [engine](const char* what) {
		engine->lastError = std::string("switchAPI failed: ") + what + " " + engine->loader.lastError();
		std::cerr << "[Engine] " << engine->lastError << "\n";
		unloadPlugin(engine);
		engine->initialized = false;
		return false;
	};

	if (!loadPlugin(engine, newApi))
		return fail("loadPlugin");
	GfxDeviceConfig pod = engine->config;
	pod.api = newApi;
	if (!engine->plugin->init(pod))
		return fail("init");
	engine->config.api = newApi;

	// ウィンドウのスワップチェーンを新しいバックエンドで作り直す (直近のサイズを使う)
	for (auto& w : engine->windows)
	{
		w->swapChain = engine->plugin->createSwapChain(w->desc);
		if (!w->swapChain)
			std::cerr << "[Engine] switchAPI: createSwapChain failed (" << w->name << ")\n";
	}

	if (post)
		post(user, old, newApi);
	return true;
}

GFX_CORE_API GfxWindow* gfxEngineCreateWindow(GfxEngine* engine, const GfxSwapChainDesc* desc, const char* nameUtf8)
{
	if (!engine || !engine->initialized || !engine->plugin || !desc)
		return nullptr;

	const char* name = nameUtf8 ? nameUtf8 : "";
	ISwapChain* sc = engine->plugin->createSwapChain(*desc);
	if (!sc)
	{
		engine->lastError = std::string("createSwapChain failed (") + name + ")";
		std::cerr << "[Engine] " << engine->lastError << "\n";
		return nullptr;
	}

	auto w = std::make_unique<GfxWindow>();
	w->owner     = engine;
	w->swapChain = sc;
	w->desc      = *desc;
	w->name      = name;
	GfxWindow* raw = w.get();
	engine->windows.push_back(std::move(w));
	std::cout << "[Engine] Window \"" << name << "\" " << desc->width << "x" << desc->height << "\n";
	return raw;
}

GFX_CORE_API void gfxEngineDestroyWindow(GfxEngine* engine, GfxWindow* window)
{
	if (!engine || !window)
		return;
	auto it = std::find_if(engine->windows.begin(), engine->windows.end(),
		[window](const std::unique_ptr<GfxWindow>& p) { return p.get() == window; });
	if (it == engine->windows.end())
		return;
	destroySwapChain(window);
	engine->windows.erase(it);
}

GFX_CORE_API ISwapChain* gfxWindowGetSwapChain(GfxWindow* window)
{
	return window ? window->swapChain : nullptr;
}

GFX_CORE_API bool gfxWindowResize(GfxWindow* window, uint32_t width, uint32_t height)
{
	if (!window || !window->swapChain)
		return false;
	if (!window->swapChain->resize(width, height))
		return false;
	// 最小化 (0x0) はスワップチェーン側で無視されるため、有効なサイズだけ記録する
	if (width > 0 && height > 0)
	{
		window->desc.width  = width;
		window->desc.height = height;
	}
	return true;
}

} // extern "C"
