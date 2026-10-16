// ============================================================
//  samples/deferred/ShaderFiles.cpp
// ============================================================
#include "ShaderFiles.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

// 実行ファイルのあるディレクトリ (取得できなければカレントディレクトリ)
static fs::path executableDir()
{
#if defined(_WIN32)
	wchar_t buf[MAX_PATH] = {};
	if (GetModuleFileNameW(nullptr, buf, MAX_PATH) > 0)
		return fs::path(buf).parent_path();
#endif
	return fs::current_path();
}

std::string loadShaderSource(const char* name)
{
	const fs::path path = executableDir() / "shaders" / name;
	std::ifstream in(path, std::ios::binary);
	if (!in)
		throw std::runtime_error("[Error] シェーダーファイルが見つかりません: " + path.string());

	std::ostringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

std::vector<fs::path> shaderSearchPaths()
{
	std::vector<fs::path> dirs;
#ifdef GFX_SAMPLE_SHADER_SOURCE_DIR
	std::error_code ec;
	const std::string utf8   = GFX_SAMPLE_SHADER_SOURCE_DIR;   // CMake のパス (UTF-8)
	const fs::path    source = fs::path(std::u8string(utf8.begin(), utf8.end()));
	if (fs::is_directory(source, ec))
		dirs.push_back(source);
#endif
	dirs.push_back(executableDir() / "shaders");
	return dirs;
}
