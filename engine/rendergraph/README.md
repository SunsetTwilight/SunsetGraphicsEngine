# engine/rendergraph (Phase 3 / M3)

ノード型のレンダーグラフです。ターゲットは `SunsetGraphicsEngine::rendergraph` (C++ の静的ライブラリ、名前空間 `gfx::rg`)。

設計は [docs/design/editor-architecture.md](../../docs/design/editor-architecture.md) の「Phase 3」と「M3 の詳細設計」を参照してください。

| 段階 | 内容 | 状態 |
|------|------|------|
| M3a | `GraphDesc` (データ)、`GraphJson` (JSON との相互変換)、`compile()` (順序、カリング、寿命、バリア、bind の検証) | 完了 |
| M3b | `PipelineLibrary` (パイプライン / シェーダーのアセット)、`Executor` (一時リソース、バリアの発行、パスの記録、パスごとの GPU 時間) | 完了 |
| M3c | アセットの保存とホットリロード | 予定 |

- 依存できるのは `engine/core` と `engine/schema` です。バックエンド (`backends/*`) は参照しません。
- 公開ヘッダーは `include/gfx/rendergraph/` に置きます (`#include "gfx/rendergraph/RenderGraph.h"`)。
- パスの入力とシェーダーのつなぎ方 (`bind: { param, offset }`) はグラフのデータとして明示的に持ちます。実行時はこの指定だけを使います。
- テストは `tests/test_rendergraph.cpp` (GPU 不要。`gfx_tests_unit`) と `tests/test_rendergraph_exec.cpp` (実行。`gfx_tests_render`)。
- データとコンパイル (`RenderGraph.h`) だけを使うなら GPU も本体 DLL も要りません。`PipelineLibrary` / `Executor` は本体 (`SunsetGraphicsEngine::core`) を使います。

## 使い方 (M3a)

```cpp
#include "gfx/rendergraph/RenderGraph.h"

gfx::rg::GraphDesc graph;
GfxJsonDiagnostics diag;
if (!gfx::rg::graphFromString(text, graph, &diag)) { /* diag.errors */ }

gfx::rg::CompileOptions options;
options.bindingLayout = [&](const std::string& pipeline) -> const GfxBindingLayoutDesc* { /* パイプラインのアセットから */ };
const gfx::rg::CompiledGraph compiled = gfx::rg::compile(graph, options);
for (const auto& d : compiled.diagnostics) { /* d.text() */ }
for (const auto& pass : compiled.passes) { /* pass.barriers を記録してからパスを実行する */ }
```

## 使い方 (M3b: 実行)

```cpp
#include "gfx/rendergraph/Executor.h"

ShaderLibrary           shaders(engine);
gfx::rg::PipelineLibrary pipelines(engine, shaders);
pipelines.setRoot("D:/project/assets");                 // pipelines/*.json と shaders/*.hlsl の基準

gfx::rg::CompileOptions options;
options.bindingLayout = [&](const std::string& p) { return pipelines.bindingLayout(p); };
const gfx::rg::CompiledGraph compiled = gfx::rg::compile(graph, options);

gfx::rg::Executor executor(engine, pipelines);
// フレームごと
gfx::rg::FrameInputs in;
in.width    = back->getWidth();
in.height   = back->getHeight();
in.provider = &scene;                                   // IDrawListProvider (drawList と名前の定数)
in.imports["$backbuffer"] = gfx::rg::ImportedResource::fromRenderTarget(back);
cb->begin();
executor.record(cb, graph, compiled, in);               // false なら executor.errors()
cb->end();
engine.submitCommandBuffers({ cb });
// executor.timings() にパスごとの GPU 時間 (数フレーム遅れ)
```

パイプラインのアセットの形と、M3b の実行の制限は、設計ドキュメントの「パイプラインのアセット」「M3b の実装状況」を参照してください。
サンプルは `samples/rendergraph` (`GfxGraphSample`) です。
