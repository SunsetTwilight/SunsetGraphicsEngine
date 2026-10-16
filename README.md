# GraphicsEngine

DLL プラグイン方式の DirectX 12 / Vulkan グラフィックスエンジン。

- **デバイス** と **スワップチェーン / RenderTarget / DepthStencil** を分離した設計
- DX12・Vulkan は実行時に `LoadLibrary` / `dlopen` でロードする DLL
- Deferred Rendering・シャドウマップ・マルチウィンドウに対応

> **現在の対応状況**
>
> | バックエンド | 状態 |
> |------------|------|
> | DirectX 12 (Windows) | 動作します |
> | Vulkan | **実験的。現在の ABI と合っておらずビルドできません** (`GFX_BUILD_VULKAN` の既定値は `OFF`) |
>
> Linux / macOS は Vulkan バックエンドが前提のため、現時点では動作しません。
> 今後の設計方針 (ABI v2・レンダーグラフ・エディター) は [docs/design/editor-architecture.md](docs/design/editor-architecture.md) を参照してください。

---

## ディレクトリ構成

```
GraphicsEngine/
├── CMakeLists.txt              ← ルート (単一ファイル構成)
├── CMakePresets.json           ← Ninja / VS2022 / Linux プリセット
├── include/
│   ├── GfxBackendABI.h         ← DLL 境界 C ABI (SwapChain / RT / DS 分離、v2 記述子)
│   ├── GfxSchema.h             ← v2 記述子のフィールドのメタデータ表 (X マクロから生成)
│   ├── GraphicsEngine.h        ← アプリ向け統合 API
│   └── PluginLoader.h          ← LoadLibrary / dlopen ラッパー
├── src/
│   └── GraphicsEngine.cpp      ← エンジン本体 (スタティックライブラリ)
├── plugins/
│   ├── dx12/
│   │   └── DX12Backend.cpp     ← gfx_dx12.dll (Windows 専用)
│   └── vulkan/
│       └── VulkanBackend.cpp   ← gfx_vulkan.dll / .so / .dylib
└── examples/
    └── main.cpp                ← 使用例 (Forward / Deferred / Shadow)
```

---

## 前提条件

| 環境    | 必要なもの |
|---------|-----------|
| Windows | Visual Studio 2022 (C++ デスクトップ開発ワークロード)、Windows SDK 10.0.22000+ |
| Linux   | GCC 12+ または Clang 15+、libx11-dev または libxcb-dev |
| macOS   | Xcode 14+、Command Line Tools |
| 共通    | CMake 3.22+、Ninja、[Vulkan SDK](https://vulkan.lunarg.com/) |

---

## ビルド方法

### CMake Presets を使う (推奨)

```bat
:: Windows — Ninja + Debug (DX12 & Vulkan、バリデーション ON)
cmake --preset windows-debug
cmake --build --preset windows-debug

:: Windows — Visual Studio 2022 ソリューション生成
cmake --preset windows-vs2022
cmake --build --preset windows-vs2022-debug
cmake --build --preset windows-vs2022-release
```

```bash
# Linux — Ninja + Debug (Vulkan のみ、バリデーション ON)
cmake --preset linux-debug
cmake --build --preset linux-debug

# Linux — Release
cmake --preset linux-release
cmake --build --preset linux-release
```

成果物は `build/<プリセット名>/bin/` に出力されます。

---

### 手動ビルド

```bat
:: Windows (Ninja + Debug)
cmake -B build\debug -G Ninja ^
      -DCMAKE_BUILD_TYPE=Debug ^
      -DGFX_BUILD_DX12=ON ^
      -DGFX_BUILD_VULKAN=ON ^
      -DGFX_ENABLE_VALIDATION=ON
cmake --build build\debug
```

```bash
# Linux (Ninja + Release)
cmake -B build/release -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DGFX_BUILD_DX12=OFF \
      -DGFX_BUILD_VULKAN=ON
cmake --build build/release
```

---

## CMake オプション

| 変数 | 既定値 | 説明 |
|------|--------|------|
| `GFX_BUILD_DX12` | `WIN32` | DirectX 12 バックエンド DLL をビルドする |
| `GFX_BUILD_VULKAN` | `OFF` | Vulkan バックエンド DLL をビルドする (現在はビルド不可。上記「対応状況」を参照) |
| `GFX_BUILD_EXAMPLE` | `ON` | サンプルアプリ `GfxExample` をビルドする |
| `GFX_ENABLE_VALIDATION` | `OFF` | GPU バリデーション / デバッグレイヤーを有効にする |
| `GFX_DXC_DIR` | *(空)* | DXC のルートディレクトリ (HLSL → DXIL コンパイルを有効化) |

---

## DXC の指定 (Windows)

HLSL シェーダーのコンパイルには DirectX Shader Compiler (DXC) が必要です。  
省略時はシェーダーコンパイルがスタブになります。

```bat
:: Vulkan SDK に同梱の DXC を使う場合
cmake --preset windows-debug ^
      -DGFX_DXC_DIR="%VULKAN_SDK%"

:: Windows SDK に同梱の DXC を使う場合
cmake --preset windows-debug ^
      -DGFX_DXC_DIR="C:\Program Files (x86)\Windows Kits\10\bin\10.0.22621.0\x64"
```

---

## 成果物

```
build/<プリセット>/bin/
├── GfxExample.exe        ← サンプルアプリ (Windows)
├── GfxExample            ← サンプルアプリ (Linux)
├── gfx_dx12.dll          ← DX12 バックエンド (Windows)
├── gfx_vulkan.dll        ← Vulkan バックエンド (Windows)
├── gfx_vulkan.so         ← Vulkan バックエンド (Linux)
└── gfx_vulkan.dylib      ← Vulkan バックエンド (macOS)
```

`GfxExample` と DLL はすべて同じ `bin/` に出力されるため、  
`LoadLibrary` / `dlopen` がパス指定なしで DLL を発見できます。

---

## Visual Studio でのデバッグ

`cmake --preset windows-vs2022` で生成した `.sln` を Visual Studio で開き、  
`GfxExample` をスタートアッププロジェクトに設定して **F5** を押します。  
デバッグ作業ディレクトリは `build/vs2022/bin/` に自動設定済みです。

---

## API 利用時のルール

### 所有権 (DLL 境界)

`IRenderTarget` / `IDepthStencil` / `ISwapChain` / `IPipeline` / `ICommandBuffer` は
バックエンド DLL 内のオブジェクトが所有します。アプリ側で `delete` してはいけません。
必ず `destroy()` / `release()` (または `GraphicsEngine::destroy*()`) で解放してください。
これにより EXE と DLL の CRT (Debug / Release) が異なっても安全に解放できます。

`RenderWindow::getRenderTarget()` が返すバックバッファはスワップチェーンが所有しているため、破棄できません。

### GPU との同期

- `destroy*()` は GPU が使用中のリソースでも呼び出せます。バックエンドが GPU の完了を待ってから解放します。
- Uniform バッファの `updateBuffer()` は、GPU が読み取り中のデータを上書きしません。内部で複数のバージョンを切り替えて使います。
  同じバッファの更新は submit 1 回につき 3 回までにしてください。
- 複数のコマンドバッファを、提出順と異なる順序や並列で記録してもかまいません。
  リソース状態は submit の順序で確定し、足りないバリアは自動で補われます。
- Bundle の中では `bindTexture` / `bindSampler` は使えません。親の Direct コマンドバッファ側でバインドしてください。
- MSAA は `GfxRenderTargetDesc` / `GfxDepthStencilDesc` / `GfxTextureResourceDesc` の `sampleCount` で指定します。PSO の `sampleCount` と合わせ、`ICommandBuffer::resolve` で 1x に解決します。対応していないサンプル数は作成時に失敗します。

### ABI バージョン

`GfxBackendABI.h` の vtable や構造体を変更したら、`GFX_ABI_VERSION` を上げてください。
`PluginLoader` は、ABI のバージョンと構造体のサイズが一致しない DLL を読み込みません。

ABI v2 の記述子 (`GfxGraphicsPipelineDesc` / `GfxBindingLayoutDesc`) は先頭に `structSize` と `version` を持ち、
バックエンドは `structSize` までしか読みません。フィールドは必ず末尾に追加してください。
構造体は `GFX_FIELDS_<型名>` の X マクロで定義し、同じマクロから `GfxSchema.h` がメタデータ表を生成します。
`GfxPipelineDesc` (v1) は互換レイヤーとして残してあり、DLL 内で v2 に変換されます。
`GFX_EXAMPLE_DUMP_SCHEMA=1` でサンプルを実行すると、スキーマ経由で記述子の中身を出力します。

サンプルは 60 フレームごとに `[GPU   N]` の行で、パスごとの GPU 時間 (タイムスタンプのクエリ) と G-Buffer のオクルージョンクエリの結果を出力します。

---

## 動作確認 (サンプルの自動テスト)

`GfxExample` は、環境変数で自動テスト用の動作に切り替えられます。

| 環境変数 | 内容 |
|---------|------|
| `GFX_EXAMPLE_FRAMES=N` | N フレーム描画したら自動で終了する |
| `GFX_EXAMPLE_RESIZE_TEST=1` | リサイズ、最小化、復帰を自動で繰り返す |

`GFX_ENABLE_VALIDATION=ON` でビルドすると、D3D12 デバッグレイヤーのエラーと警告が stderr にも出力されます。

---

## インストール

```bash
cmake --install build/linux-release --prefix /usr/local
```

| インストール先 | 内容 |
|---------------|------|
| `<prefix>/lib/` | `libGraphicsEngine.a` |
| `<prefix>/include/GraphicsEngine/` | ヘッダファイル |
| `<prefix>/bin/` | DLL (Windows) |
| `<prefix>/lib/` | 共有ライブラリ (Linux / macOS)。エンジンは実行ファイルと同じディレクトリと `<exe>/../lib` を探索します |
