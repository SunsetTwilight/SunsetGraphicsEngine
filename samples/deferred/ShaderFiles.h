#pragma once
// ============================================================
//  samples/deferred/ShaderFiles.h
//  実行ファイルの横の shaders/ から HLSL ソースを読み込む。
//  (ビルド時に samples/deferred/shaders/ が bin/shaders/ にコピーされる)
// ============================================================
#include <filesystem>
#include <string>
#include <vector>

// shaders/<name> の内容を返す。見つからなければ std::runtime_error を投げる。
std::string loadShaderSource(const char* name);

// シェーダーを探すフォルダ (ホットリロード用)。
// ビルドツリーではソースのフォルダ (GFX_SAMPLE_SHADER_SOURCE_DIR) を先にし、編集をそのまま反映する。
std::vector<std::filesystem::path> shaderSearchPaths();
