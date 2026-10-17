#pragma once
// ============================================================
//  gfx/rendergraph/RenderGraph.h
//  レンダーグラフの公開ヘッダーをまとめて読み込む (名前空間 gfx::rg)
//
//    GraphDesc.h  グラフのデータ (リソースとパス、つなぎ方 bind)
//    GraphJson.h  グラフのアセット (graphs/*.json) との相互変換
//    Compiler.h   コンパイル (順序、カリング、寿命、バリア、bind の検証)
//
//  実行 (GPU、本体 DLL を使う) は別に読み込む:
//    PipelineLibrary.h  パイプラインのアセット (pipelines/*.json)
//    Executor.h         一時リソース、バリア、パスの記録、パスごとの GPU 時間
// ============================================================

#include "gfx/rendergraph/GraphDesc.h"
#include "gfx/rendergraph/GraphJson.h"
#include "gfx/rendergraph/Compiler.h"
