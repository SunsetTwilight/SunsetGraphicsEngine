# tools

開発用のスクリプトを置く場所です。

| スクリプト | 内容 |
|-----------|------|
| [check-sample.ps1](check-sample.ps1) | ビルド済みの `GfxExample` を自動テストモードで実行し、D3D12 のバリデーションメッセージが 0 件であることを確認します |

```powershell
powershell -ExecutionPolicy Bypass -File tools/check-sample.ps1 -BinDir build/windows-debug/bin
```
