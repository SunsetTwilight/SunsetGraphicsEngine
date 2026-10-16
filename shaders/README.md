# shaders

複数のサンプルやエディターで共有する HLSL を置く場所です(フルスクリーン描画、ImGui の描画、デバッグ表示など)。まだファイルはありません。

サンプル固有のシェーダーは、各サンプルの `shaders/` に置きます(例: `samples/deferred/shaders/`)。ビルドすると `bin/shaders/` にコピーされます。

ファイル名は `<名前>.<ステージ>.hlsl` とします。ステージは `vs` / `ps` / `cs` / `raygen` / `miss` / `closesthit` のいずれかです。
