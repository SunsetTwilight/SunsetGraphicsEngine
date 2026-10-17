# tests

[doctest](https://github.com/doctest/doctest) と CTest による自動テストです。単独でビルドしたとき (`GFX_BUILD_TESTS` の既定は ON) にビルドされます。doctest は configure 時に取得します。

| テスト | 実行ファイル | 内容 | GPU |
|-------|-------------|------|-----|
| `unit` | `gfx_tests_unit` | ABI 情報、`BufferUsage` の判定、スキーマの表、JSON (パース / 文字列化、記述子との往復、エラーと警告)、レンダーグラフ (JSON、コンパイル、検証) | 不要 |
| `render` | `gfx_tests_render` | ウィンドウなしでオフスクリーンに描画し、D3D12 デバッグレイヤーのエラー / 警告が 0 件であることと、タイムスタンプで GPU がコマンドを実行したことを確認する。Uniform バッファのバージョン切り替えと、使用中リソースの遅延解放の経路も通る。シェーダー (DXC、リフレクション、`#include` とマクロ、エラーの通知)、作り直し (`updateShader` / `updatePipeline`、JSON から作ったパイプライン、`ShaderLibrary` のホットリロード) | 必要 (WARP でも可) |
| `reject` | `gfx_tests_reject` | ABI のバージョンが違うバックエンド DLL が拒否されることを確認する (専用のディレクトリに偽の `gfx_dx12.dll` を置く) | 不要 |

## 実行

```bat
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --test-dir build/windows-debug --output-on-failure
```

GPU のない環境 (CI など) では、環境変数で WARP (ソフトウェアレンダラー) を使います。

```bat
set GFX_TEST_ADAPTER=software
ctest --test-dir build/windows-debug --output-on-failure
```

## 注意

- `render` のメッセージ確認は、デバッグレイヤー (Windows の「グラフィックス ツール」) が入っている環境の Debug ビルドでのみ有効です。入っていない場合は、テストの出力に `info: D3D12 debug layer is not available` と表示され、描画とタイムスタンプの確認だけを行います。
- 描画結果のピクセルを読み戻す API はまだないため、色の検証は行っていません。
