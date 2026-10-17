#pragma once
// ============================================================
//  gfx/rendergraph/GraphJson.h
//
//  グラフ (GraphDesc) と JSON (グラフのアセット graphs/*.json) の相互変換。
//  JSON は gfx/schema/GfxJson.h (外部依存なし) を使う。
//
//  形式 (例):
//    {
//      "version": 1,
//      "resources": {
//        "gbAlbedo": { "format": "R8G8B8A8_Unorm", "size": { "relative": 1.0 } },
//        "depth":    { "format": "D32_Float", "size": { "width": 1024, "height": 1024 }, "clear": { "depth": 1.0 } },
//        "lights":   { "kind": "buffer", "bytes": 4096 }
//      },
//      "passes": [
//        { "name": "GBuffer", "type": "Raster", "pipeline": "pipelines/gbuffer.json",
//          "constants": [ { "source": "frame", "bind": { "param": 0 } } ],
//          "outputs": [ { "res": "gbAlbedo", "as": "RTV0", "load": "clear" },
//                       { "res": "depth", "as": "DSV", "load": "clear" } ],
//          "content": { "drawList": { "layer": "Opaque", "sort": "frontToBack" } } },
//        ...
//      ]
//    }
//
//  ・as は SRV / UAV / RTV0〜RTV7 / DSV / DSVRead / CopySrc / CopyDst / IndirectArgs
//  ・load は load / clear / dontCare (省略時 load)
//  ・content は drawList / fullscreen: true / dispatch: [x, y, z] / custom: "名前" のどれか 1 つ
//  ・既定値と同じ項目は書かない (order: 0、keep: false、enabled: true など)
//  ・読むときの知らないキーは警告 (新しい形式のファイルを古いエンジンで読めるように)、型の違いはエラー。
//    診断のパスは "passes[GBuffer].outputs[0].as" のような形
// ============================================================

#include "gfx/rendergraph/GraphDesc.h"
#include "gfx/schema/GfxJson.h"

#include <string>
#include <string_view>

namespace gfx::rg
{
GfxJson graphToJson(const GraphDesc& graph);

// 失敗 (エラーがある) なら false。out は既定値から読み込んだ結果 (エラーのあった項目以外は入る)
bool graphFromJson(const GfxJson& json, GraphDesc& out, GfxJsonDiagnostics* diag = nullptr);

// 文字列との変換 (ファイルの読み書きはアプリが行う)
std::string graphToString(const GraphDesc& graph, int indent = 2);
bool        graphFromString(std::string_view text, GraphDesc& out, GfxJsonDiagnostics* diag = nullptr);
} // namespace gfx::rg
