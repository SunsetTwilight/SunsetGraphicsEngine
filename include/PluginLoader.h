#pragma once
// ============================================================
//  PluginLoader.h
//  DLL の動的ロード / アンロードをプラットフォーム非依存に扱う
// ============================================================

#include "GfxBackendABI.h"
#include <string>
#include <stdexcept>
#include <iostream>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  using NativeHandle = HMODULE;
  #define DLL_EXT ".dll"
#elif defined(__APPLE__)
  #include <dlfcn.h>
  using NativeHandle = void*;
  #define DLL_EXT ".dylib"
#else
  #include <dlfcn.h>
  using NativeHandle = void*;
  #define DLL_EXT ".so"
#endif

// ============================================================
//  PluginLoader
//  DLL のロード・シンボル取得・アンロードをまとめたクラス。
//  コピー不可、ムーブ可。
// ============================================================
class PluginLoader {
public:
	PluginLoader() = default;

	// DLL をロードし、エクスポート関数を取得する
	// dllPath: 拡張子なし ("gfx_dx12" → "gfx_dx12.dll" / ".so")
	bool load(const std::string& dllBaseName) {
		unload(); // 既存をアンロード

		std::string path = dllBaseName + DLL_EXT;

#if defined(_WIN32)
		m_handle = LoadLibraryA(path.c_str());
		if (!m_handle) {
			DWORD err = GetLastError();
			char msg[256];
			FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, nullptr, err, 0, msg, 256, nullptr);
			m_lastError = std::string("LoadLibrary(") + path + ") failed: " + msg;
			return false;
		}
		m_pfnCreate  = reinterpret_cast<PFN_GFX_CreateBackend >(
						   GetProcAddress(m_handle, "GFX_CreateBackend"));
		m_pfnDestroy = reinterpret_cast<PFN_GFX_DestroyBackend>(
						   GetProcAddress(m_handle, "GFX_DestroyBackend"));
#else
		// RTLD_LOCAL: シンボルを他の DLL に漏らさない
		// RTLD_NOW:   ロード時に未解決シンボルをエラーにする
		m_handle = dlopen(path.c_str(), RTLD_LOCAL | RTLD_NOW);
		if (!m_handle) {
			m_lastError = std::string("dlopen(") + path + ") failed: " + dlerror();
			return false;
		}
		m_pfnCreate  = reinterpret_cast<PFN_GFX_CreateBackend >(
						   dlsym(m_handle, "GFX_CreateBackend"));
		m_pfnDestroy = reinterpret_cast<PFN_GFX_DestroyBackend>(
						   dlsym(m_handle, "GFX_DestroyBackend"));
#endif

		if (!m_pfnCreate || !m_pfnDestroy) {
			m_lastError = path + ": GFX_CreateBackend / GFX_DestroyBackend が見つかりません";
			unload();
			return false;
		}

		// ABI 互換性チェック: バージョンと vtable / 構造体サイズが一致しない DLL は拒否する
		{
#if defined(_WIN32)
			auto pfnInfo = reinterpret_cast<PFN_GFX_GetABIInfo>(
							   GetProcAddress(m_handle, "GFX_GetABIInfo"));
#else
			auto pfnInfo = reinterpret_cast<PFN_GFX_GetABIInfo>(
							   dlsym(m_handle, "GFX_GetABIInfo"));
#endif
			GfxABIInfo dllInfo = {};
			if (pfnInfo) pfnInfo(&dllInfo);
			const GfxABIInfo hostInfo = gfxCurrentABIInfo();
			if (!pfnInfo || !(dllInfo == hostInfo)) {
				m_lastError = path + ": ABI が一致しません (DLL abi="
							+ std::to_string(dllInfo.abiVersion) + ", host abi="
							+ std::to_string(hostInfo.abiVersion)
							+ ")。エンジンと同じヘッダでビルドし直してください";
				unload();
				return false;
			}
		}

		m_dllName = path;
		std::cout << "[PluginLoader] Loaded: " << path << "\n";
		return true;
	}

	// DLL からバックエンドプラグインを生成する
	// 呼び出し元は releasePlugin() か unload() まで有効
	IBackendPlugin* createPlugin() {
		if (!m_pfnCreate) return nullptr;
		IBackendPlugin* p = m_pfnCreate();
		if (!p) m_lastError = "GFX_CreateBackend() returned nullptr";
		return p;
	}

	// プラグインを破棄する (DLL 内部の delete を呼ぶ)
	void destroyPlugin(IBackendPlugin* p) {
		if (p && m_pfnDestroy) m_pfnDestroy(p);
	}

	// DLL をアンロードする
	// ★ プラグインを全て destroy してから呼ぶこと
	void unload() {
		if (!m_handle) return;
#if defined(_WIN32)
		FreeLibrary(m_handle);
#else
		dlclose(m_handle);
#endif
		m_handle     = nullptr;
		m_pfnCreate  = nullptr;
		m_pfnDestroy = nullptr;
		std::cout << "[PluginLoader] Unloaded: " << m_dllName << "\n";
		m_dllName.clear();
	}

	~PluginLoader() { unload(); }

	// ムーブ
	PluginLoader(PluginLoader&& o) noexcept
		: m_handle(o.m_handle), m_pfnCreate(o.m_pfnCreate),
		  m_pfnDestroy(o.m_pfnDestroy), m_dllName(std::move(o.m_dllName)) {
		o.m_handle = nullptr; o.m_pfnCreate = nullptr; o.m_pfnDestroy = nullptr;
	}
	PluginLoader& operator=(PluginLoader&& o) noexcept {
		if (this != &o) { unload(); std::swap(m_handle, o.m_handle);
			std::swap(m_pfnCreate, o.m_pfnCreate); std::swap(m_pfnDestroy, o.m_pfnDestroy);
			m_dllName = std::move(o.m_dllName); }
		return *this;
	}
	PluginLoader(const PluginLoader&)            = delete;
	PluginLoader& operator=(const PluginLoader&) = delete;

	bool        isLoaded()   const { return m_handle != nullptr; }
	std::string lastError()  const { return m_lastError; }
	std::string dllName()    const { return m_dllName; }

private:
	NativeHandle           m_handle     = nullptr;
	PFN_GFX_CreateBackend  m_pfnCreate  = nullptr;
	PFN_GFX_DestroyBackend m_pfnDestroy = nullptr;
	std::string            m_dllName;
	std::string            m_lastError;
};
