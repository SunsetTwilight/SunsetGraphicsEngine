// ============================================================
//  backends/dx12/DX12ShaderCompiler.cpp
// ============================================================

#include "DX12ShaderCompiler.h"
#include <atomic>

namespace
{
	std::wstring widen(const char* s)
	{
		if (!s || !*s)
			return {};
		const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
		std::wstring w(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
		if (n > 1)
			MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
		return w;
	}

	std::string narrow(const wchar_t* w)
	{
		if (!w || !*w)
			return {};
		const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
		std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
		if (n > 1)
			WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
		return s;
	}

	// 既定の include ハンドラーに処理を任せ、読み込んだファイルを記録する
	class RecordingIncludeHandler final : public IDxcIncludeHandler
	{
	public:
		RecordingIncludeHandler(IDxcIncludeHandler* inner, std::vector<std::string>& deps)
			: m_inner(inner), m_deps(deps) {}

		HRESULT STDMETHODCALLTYPE LoadSource(LPCWSTR fileName, IDxcBlob** out) override
		{
			const HRESULT hr = m_inner->LoadSource(fileName, out);
			if (SUCCEEDED(hr))
			{
				wchar_t full[MAX_PATH * 2] = {};
				const DWORD n = GetFullPathNameW(fileName, static_cast<DWORD>(std::size(full)), full, nullptr);
				const std::string path = narrow(n > 0 && n < std::size(full) ? full : fileName);
				if (std::find(m_deps.begin(), m_deps.end(), path) == m_deps.end())
					m_deps.push_back(path);
			}
			return hr;
		}

		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
		{
			if (riid == __uuidof(IDxcIncludeHandler) || riid == __uuidof(IUnknown))
			{
				*ppv = static_cast<IDxcIncludeHandler*>(this);
				AddRef();
				return S_OK;
			}
			*ppv = nullptr;
			return E_NOINTERFACE;
		}
		ULONG STDMETHODCALLTYPE AddRef() override  { return ++m_ref; }
		ULONG STDMETHODCALLTYPE Release() override { return --m_ref; }   // スタック上で使うので delete しない

	private:
		std::atomic<ULONG>        m_ref { 1 };
		IDxcIncludeHandler*       m_inner;
		std::vector<std::string>& m_deps;
	};

	// gfx_dx12.dll のあるフォルダ (末尾の区切り文字付き)
	std::wstring thisModuleDir()
	{
		HMODULE self = nullptr;
		if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&dx12DefaultShaderProfile), &self))
			return {};
		wchar_t path[MAX_PATH * 2] = {};
		const DWORD n = GetModuleFileNameW(self, path, static_cast<DWORD>(std::size(path)));
		if (n == 0 || n >= std::size(path))
			return {};
		std::wstring dir(path, n);
		const size_t pos = dir.find_last_of(L"\\/");
		return pos == std::wstring::npos ? std::wstring() : dir.substr(0, pos + 1);
	}
}

const char* dx12DefaultShaderProfile(ShaderStage stage)
{
	switch (stage)
	{
	case ShaderStage::Vertex:   return "vs_6_0";
	case ShaderStage::Fragment: return "ps_6_0";
	case ShaderStage::Compute:  return "cs_6_0";
	case ShaderStage::Geometry: return "gs_6_0";
	case ShaderStage::Hull:     return "hs_6_0";
	case ShaderStage::Domain:   return "ds_6_0";
	default:                    return nullptr;
	}
}

bool DX12ShaderCompiler::ensureLoaded()
{
	if (m_compiler)
		return true;
	if (m_triedLoad)
		return false;
	m_triedLoad = true;

	// 同じフォルダの dxil.dll が署名に使われるよう、gfx_dx12.dll の横のものを優先する。
	// モジュールはプロセスの終了まで解放しない (DXC のオブジェクトがどこかに残っていても安全なように)
	const std::wstring dir = thisModuleDir();
	if (!dir.empty())
		m_module = LoadLibraryExW((dir + L"dxcompiler.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!m_module)
		m_module = LoadLibraryW(L"dxcompiler.dll");
	if (!m_module)
	{
		m_loadError = "dxcompiler.dll が見つかりません (実行ファイルか gfx_dx12.dll の横に dxcompiler.dll と dxil.dll を置いてください)";
		return false;
	}

	using PFN_DxcCreateInstance = HRESULT (WINAPI*)(REFCLSID, REFIID, LPVOID*);
	auto create = reinterpret_cast<PFN_DxcCreateInstance>(GetProcAddress(m_module, "DxcCreateInstance"));
	if (!create
	 || FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&m_compiler)))
	 || FAILED(create(CLSID_DxcUtils,    IID_PPV_ARGS(&m_utils)))
	 || FAILED(m_utils->CreateDefaultIncludeHandler(&m_defaultInclude)))
	{
		m_compiler.Reset();
		m_utils.Reset();
		m_loadError = "dxcompiler.dll の初期化に失敗しました";
		return false;
	}
	return true;
}

bool DX12ShaderCompiler::available()
{
	return ensureLoaded();
}

bool DX12ShaderCompiler::compile(const GfxShaderDesc& desc, const char* profile, DX12ShaderOutput& out)
{
	out = {};
	if (!ensureLoaded())
	{
		out.messages = m_loadError;
		return false;
	}
	if (!desc.source || !profile)
	{
		out.messages = "HLSL のソースかプロファイルがありません";
		return false;
	}

	// 引数 (wstring の寿命を args より長く保つ)
	std::vector<std::wstring> storage;
	storage.reserve(16 + desc.defineCount * 2 + desc.includeDirCount * 2);
	auto push = [&](std::wstring s) { storage.push_back(std::move(s)); };

	push(desc.sourceName && *desc.sourceName ? widen(desc.sourceName) : std::wstring(L"shader.hlsl"));
	if (desc.entryPoint && *desc.entryPoint)
	{
		push(L"-E");
		push(widen(desc.entryPoint));
	}
	push(L"-T");
	push(widen(profile));
	for (uint32_t i = 0; i < desc.defineCount && desc.defines; ++i)
	{
		const GfxShaderDefine& d = desc.defines[i];
		if (!d.name || !*d.name)
			continue;
		push(L"-D");
		push(widen(d.name) + L"=" + (d.value ? widen(d.value) : std::wstring(L"1")));
	}
	for (uint32_t i = 0; i < desc.includeDirCount && desc.includeDirs; ++i)
	{
		if (!desc.includeDirs[i] || !*desc.includeDirs[i])
			continue;
		push(L"-I");
		push(widen(desc.includeDirs[i]));
	}
	if (gfxHasFlag(desc.flags, GfxShaderCompileFlags::Debug))
	{
		push(L"-Zi");
		push(L"-Qembed_debug");
	}
	if (gfxHasFlag(desc.flags, GfxShaderCompileFlags::SkipOptimization))
		push(L"-Od");
	if (gfxHasFlag(desc.flags, GfxShaderCompileFlags::WarningsAsErrors))
		push(L"-WX");
	// リフレクションは別のパート (DXC_OUT_REFLECTION) で受け取り、PSO に渡すバイトコードからは外す
	push(L"-Qstrip_reflect");

	std::vector<LPCWSTR> args;
	args.reserve(storage.size());
	for (const auto& s : storage)
		args.push_back(s.c_str());

	DxcBuffer src = {};
	src.Ptr      = desc.source;
	src.Size     = strlen(desc.source);
	src.Encoding = DXC_CP_UTF8;

	RecordingIncludeHandler include(m_defaultInclude.Get(), out.dependencies);
	ComPtr<IDxcResult> result;
	HRESULT hr = m_compiler->Compile(&src, args.data(), static_cast<UINT32>(args.size()), &include, IID_PPV_ARGS(&result));
	if (FAILED(hr) || !result)
	{
		out.messages = "DXC の呼び出しに失敗しました";
		return false;
	}

	ComPtr<IDxcBlobUtf8> errors;
	if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr)) && errors && errors->GetStringLength() > 0)
		out.messages.assign(errors->GetStringPointer(), errors->GetStringLength());

	HRESULT status = E_FAIL;
	result->GetStatus(&status);
	if (FAILED(status))
		return false;

	ComPtr<IDxcBlob> object;
	if (FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr)) || !object || object->GetBufferSize() == 0)
	{
		if (out.messages.empty())
			out.messages = "DXC がバイトコードを出力しませんでした";
		return false;
	}
	const uint8_t* p = static_cast<const uint8_t*>(object->GetBufferPointer());
	out.bytecode.assign(p, p + object->GetBufferSize());

	if (result->HasOutput(DXC_OUT_REFLECTION))
		result->GetOutput(DXC_OUT_REFLECTION, IID_PPV_ARGS(&out.reflection), nullptr);
	return true;
}

bool DX12ShaderCompiler::compileLibrary(const char* source, const char* profile, DX12ShaderOutput& out)
{
	GfxShaderDesc d = {};
	d.stage      = ShaderStage::Compute;   // 使わない
	d.source     = source;
	d.entryPoint = nullptr;
	d.sourceName = "raytracing.hlsl";
	return compile(d, profile, out);
}

ComPtr<ID3D12ShaderReflection> DX12ShaderCompiler::createReflection(const void* data, size_t size)
{
	if (!data || size == 0 || !ensureLoaded())
		return nullptr;
	DxcBuffer buf = {};
	buf.Ptr  = data;
	buf.Size = size;
	ComPtr<ID3D12ShaderReflection> refl;
	if (FAILED(m_utils->CreateReflection(&buf, IID_PPV_ARGS(&refl))))
		return nullptr;
	return refl;
}
