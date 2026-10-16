# engine/rendergraph (Phase 3)

ノード型のレンダーグラフを実装する予定の場所です。まだコードはありません。

- 依存できるのは `engine/core` と `engine/schema` です。バックエンド (`backends/*`) は参照しません。
- 公開ヘッダーは `include/gfx/rendergraph/` に置きます。
- 設計は [docs/design/editor-architecture.md](../../docs/design/editor-architecture.md) の「Phase 3」を参照してください。

実装を始めるときは、`engine/CMakeLists.txt` に `add_subdirectory(rendergraph)` を追加します。
