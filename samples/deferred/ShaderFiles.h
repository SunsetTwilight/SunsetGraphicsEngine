#pragma once
// ============================================================
//  samples/deferred/ShaderFiles.h
//  実行ファイルの横の shaders/ から HLSL ソースを読み込む。
//  (ビルド時に samples/deferred/shaders/ が bin/shaders/ にコピーされる)
// ============================================================
#include <string>

// shaders/<name> の内容を返す。見つからなければ std::runtime_error を投げる。
std::string loadShaderSource(const char* name);
