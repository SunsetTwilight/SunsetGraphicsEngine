#pragma once
// ============================================================
//  backends/dx12/DX12ShaderCompiler.h
//  DXC (dxcompiler.dll) による HLSL → DXIL のコンパイル。
//  dxcompiler.dll は最初のコンパイル時に gfx_dx12.dll と同じフォルダから LoadLibrary する
//  (見つからなければ通常の DLL 検索順)。DXIL の署名には同じ場所の dxil.dll が使われる。
// ============================================================

#include "DX12Common.h"

// コンパイル結果
struct DX12ShaderOutput
{
	std::vector<uint8_t>     bytecode;      // DXIL コンテナ
	ComPtr<IDxcBlob>         reflection;    // リフレクションのパート (DXC_OUT_REFLECTION。ないこともある)
	std::vector<std::string> dependencies;  // #include で読み込んだファイル (重複なし、読み込んだ順)
	std::string              messages;      // エラーと警告 (空なら何もなし)
};

class DX12ShaderCompiler
{
public:
	// HLSL をコンパイルする。失敗したら false (out.messages にエラー)。
	//   profile    : "vs_6_0" / "lib_6_3" など
	//   entryPoint : lib_* のときは nullptr か空文字
	bool compile(const GfxShaderDesc& desc, const char* profile, DX12ShaderOutput& out);

	// RT シェーダーのライブラリ (lib_6_x) をコンパイルする
	bool compileLibrary(const char* source, const char* profile, DX12ShaderOutput& out);

	// DXIL コンテナから ID3D12ShaderReflection を作る (リフレクションのパートがなければ nullptr)
	ComPtr<ID3D12ShaderReflection> createReflection(const void* data, size_t size);

	// dxcompiler.dll をロードできるか (ロード済みなら true)
	bool available();

	// 失敗の理由 (dxcompiler.dll が見つからないなど)
	const std::string& loadError() const { return m_loadError; }

private:
	bool ensureLoaded();

	HMODULE                    m_module = nullptr;
	bool                       m_triedLoad = false;
	std::string                m_loadError;
	ComPtr<IDxcCompiler3>      m_compiler;
	ComPtr<IDxcUtils>          m_utils;
	ComPtr<IDxcIncludeHandler> m_defaultInclude;
};

// ShaderStage と SM 6.0 からプロファイル名 ("vs_6_0" など) を返す
const char* dx12DefaultShaderProfile(ShaderStage stage);
