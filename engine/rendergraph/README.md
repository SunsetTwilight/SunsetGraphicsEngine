# engine/rendergraph (Phase 3 / M3)

ノード型のレンダーグラフです。ターゲットは `SunsetGraphicsEngine::rendergraph` (C++ の静的ライブラリ、名前空間 `gfx::rg`)。

設計は [docs/design/editor-architecture.md](../../docs/design/editor-architecture.md) の「Phase 3」と「M3 の詳細設計」を参照してください。

| 段階 | 内容 | 状態 |
|------|------|------|
| M3a | `GraphDesc` (データ)、`GraphJson` (JSON との相互変換)、`compile()` (順序、カリング、寿命、バリア、bind の検証) | 完了 |
| M3b | `AssetLibrary` (パイプライン / シェーダーのアセット)、`Executor` (一時リソース、バリアの発行、パスの記録、パスごとの GPU 時間) | 予定 |
| M3c | アセットの保存とホットリロード | 予定 |

- 依存できるのは `engine/core` と `engine/schema` です。バックエンド (`backends/*`) は参照しません。
- 公開ヘッダーは `include/gfx/rendergraph/` に置きます (`#include "gfx/rendergraph/RenderGraph.h"`)。
- パスの入力とシェーダーのつなぎ方 (`bind: { param, offset }`) はグラフのデータとして明示的に持ちます。実行時はこの指定だけを使います。
- テストは `tests/test_rendergraph.cpp` (GPU 不要。`gfx_tests_unit` に含まれる)。

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
