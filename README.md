# SunsetGraphicsEngine

DLL プラグイン方式の DirectX 12 / Vulkan グラフィックスエンジン。単独のライブラリとして、ほかのプロジェクトから使えます。

- エンジン本体は `SunsetGraphicsEngine.dll`。DLL の境界は C ABI で、C++ API はヘッダーのみのラッパー
- DX12・Vulkan のバックエンドは、本体が実行時に `LoadLibrary` / `dlopen` でロードする DLL
- アプリと DLL のコンパイラ / CRT / Debug・Release が違っても動く
- **デバイス** と **スワップチェーン / RenderTarget / DepthStencil** を分離した設計
- Deferred Rendering・シャドウマップ・マルチウィンドウ・DXR に対応

> **現在の対応状況**
>
> | バックエンド | 状態 |
> |------------|------|
> | DirectX 12 (Windows) | 動作します |
> | Vulkan | **実験的。現在の ABI と合っておらずビルドできません** (`GFX_BUILD_VULKAN` の既定値は `OFF`) |
>
> Linux / macOS は Vulkan バックエンドが前提のため、現時点では動作しません。
> 今後の設計方針 (ABI v2・レンダーグラフ・エディター) は [docs/design/editor-architecture.md](docs/design/editor-architecture.md) を参照してください。
> エディターは、このエンジンと SunsetMathLib を組み合わせる別リポジトリ SunsetStudio で開発します。

---

## ディレクトリ構成

```
SunsetGraphicsEngine/
├── CMakeLists.txt                  ← ルート (オプションと add_subdirectory のみ)
├── CMakePresets.json               ← Ninja / VS2022 / Linux プリセット
├── cmake/                          ← 共通の警告・定義、DXC の検出、パッケージ設定、配置関数
├── engine/
│   ├── abi/include/gfx/abi/        ← DLL 境界の C ABI (gfx_abi、ヘッダーのみ)
│   │     GfxBackendABI.h           ←   まとめて読み込むヘッダー
│   │     Types / PipelineState / Commands / Resources / Compat / Interfaces / Device / Plugin .h
│   ├── schema/include/gfx/schema/  ← v2 記述子のメタデータ表 (gfx_schema、ヘッダーのみ)
│   ├── core/                       ← 本体 SunsetGraphicsEngine.dll (C ABI) と C++ ラッパー
│   └── rendergraph/                ← Phase 3 (未実装)
├── backends/
│   ├── dx12/                       ← gfx_dx12.dll (責務ごとに分割: Device / CommandBuffer / Resources / ...)
│   └── vulkan/                     ← gfx_vulkan (現在はビルド対象外)
├── samples/deferred/               ← GfxExample (ディファードレンダリングのサンプル)
│   └── shaders/                    ←   HLSL (ビルド時に bin/shaders/ にコピー)
├── shaders/                        ← 共有 HLSL (未使用)
├── assets/                         ← レンダーグラフ / パイプラインの JSON (Phase 3 以降)
├── tests/                          ← 自動テスト (未実装)
├── tools/                          ← 開発用スクリプト (check-sample.ps1)
└── docs/design/                    ← 設計ドキュメント
```

依存の向きは次のとおりです。CMake のターゲットのリンク関係で強制しています。

- `engine/abi` は何にも依存しません。
- `backends/*` は `engine/abi` にだけ依存します (`engine/core` は参照しません)。
- `engine/schema` と `engine/core` は `engine/abi` に依存します。
- `samples/*` (と外部のアプリ) は `engine/core` と `engine/schema` に依存し、バックエンドは本体が実行時にロードします。

公開ヘッダーは `#include "gfx/<モジュール>/<ファイル>.h"` の形で読み込みます (例: `#include "gfx/core/GraphicsEngine.h"`)。

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
| `GFX_BUILD_EXAMPLE` | 単独ビルド時 `ON` / 取り込まれたとき `OFF` | サンプルアプリ `GfxExample` をビルドする |
| `GFX_BUILD_TESTS` | 単独ビルド時 `ON` / 取り込まれたとき `OFF` | 自動テスト (doctest) をビルドする。configure 時に doctest を取得する |
| `GFX_INSTALL` | `ON` | install / export (find_package 用) の規則を生成する |
| `GFX_ENABLE_VALIDATION` | `OFF` | GPU バリデーション / デバッグレイヤーを有効にする |
| `GFX_DXC_DIR` | *(空)* | DXC のルートディレクトリ。空なら公式リリースを自動で取得する (下記) |

---

## DXC の指定 (Windows)

DX12 バックエンドは、すべての HLSL を DirectX Shader Compiler (DXC) で DXIL にコンパイルします
(既定のプロファイルは `<stage>_6_0`。`GfxShaderDesc::targetProfile` で変更できます)。

DXC は configure 時に公式リリース ([microsoft/DirectXShaderCompiler](https://github.com/microsoft/DirectXShaderCompiler)
v1.8.2505) のアーカイブを取得します (バージョンと SHA256 は `cmake/GfxDxc.cmake` で固定)。
実行時には `dxcompiler.dll` と `dxil.dll` (DXIL の署名用) が exe の横に必要で、
`sunset_gfx_deploy_runtime()` がコピーします。

手元の DXC を使う場合は、`inc/dxcapi.h` と `bin/x64/dxcompiler.dll` を含むフォルダを指定します。

```bat
cmake --preset windows-debug -DGFX_DXC_DIR="C:\dxc_2025_05_24"
```

コンパイルのエラーと警告は、デバッグメッセージのコールバック (`DeviceConfig::debugMessage`、
id = `GFX_MESSAGE_ID_SHADER_COMPILE`) で通知されます。コールバックがなければ stderr に出力します。

---

## 成果物

```
build/<プリセット>/bin/
├── GfxExample.exe        ← サンプルアプリ (Windows)
├── shaders/              ← サンプルの HLSL
├── SunsetGraphicsEngine.dll ← エンジン本体
├── dxcompiler.dll        ← DXC (HLSL → DXIL)
├── dxil.dll              ← DXIL の署名
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

エンジン本体の DLL 境界は C ABI (`gfx/core/CoreAPI.h`) です。`GraphicsEngine` / `RenderWindow` はヘッダーのみのラッパーで、
`std::string` などの C++ の型が DLL の境界を越えることはありません。

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

`engine/abi/include/gfx/abi/` の vtable や構造体を変更したら、`GFX_ABI_VERSION` を上げてください。
`PluginLoader` は、ABI のバージョンと構造体のサイズが一致しない DLL を読み込みません。

ABI v2 の記述子 (`GfxGraphicsPipelineDesc` / `GfxBindingLayoutDesc`) は先頭に `structSize` と `version` を持ち、
バックエンドは `structSize` までしか読みません。フィールドは必ず末尾に追加してください。
構造体は `GFX_FIELDS_<型名>` の X マクロで定義し、同じマクロから `GfxSchema.h` がメタデータ表を生成します。
`GfxPipelineDesc` (v1) は互換レイヤーとして残してあり、DLL 内で v2 に変換されます。
`GFX_EXAMPLE_DUMP_SCHEMA=1` でサンプルを実行すると、スキーマ経由で記述子の中身を出力します。

### シェーダー

- HLSL は DXC で DXIL にコンパイルします (上の「DXC の指定」)。`ShaderDesc::sourceName` にファイルのパスを渡すと、
  エラーメッセージにファイル名が出て、相対パスの `#include` もそのファイルのフォルダから探します。
- `GraphicsEngine::getShaderReflection` で、バインディング、定数バッファのレイアウト、入出力シグネチャを取得できます
  (`GfxShaderReflection` は約 35 KB あるので、ヒープに置いてください)。
- `GraphicsEngine::updateShader` / `updatePipeline` は、ハンドルを変えずに中身を作り直します。
  全部作れたときだけ入れ替わり、失敗したら元のまま (理由はデバッグメッセージ) です。
  記録中のコマンドバッファでバインド済みのパイプラインを、その記録中に更新しないでください。

ホットリロードは `gfx/core/ShaderLibrary.h` (ヘッダーのみ) を使います。

```cpp
ShaderLibrary shaders(engine);
shaders.addSearchPath("shaders");
ShaderHandle ps = shaders.load("lighting.ps.hlsl", ShaderStage::Fragment);
// フレームループ
shaders.poll();   // 保存された .hlsl (と #include したファイル) を作り直す。パイプラインも作り直される
```

サンプル (`GfxExample`) は、ビルドツリーで実行するとソースの `samples/deferred/shaders/` を直接読みます。
実行中に `.hlsl` を保存すると、その場で反映されます (`[HotReload]` の行が出ます)。

### レンダーグラフ (M3)

`SunsetGraphicsEngine::rendergraph` (C++ の静的ライブラリ、`#include "gfx/rendergraph/RenderGraph.h"`) に、
描画の手順をデータ (グラフ) で表すレンダーグラフを作っています。M3a では、グラフのデータ、
グラフのアセット (JSON) との相互変換、コンパイル (順序、使われないパスの除外、リソースの寿命、バリア、
シェーダーとのつなぎ方の検証) ができます。実行 (GPU) は M3b で入ります。
詳しくは [engine/rendergraph/README.md](engine/rendergraph/README.md) を参照してください。

### 記述子と JSON

`gfx/schema/GfxJson.h` (ヘッダーのみ、外部依存なし) で、記述子と JSON を相互に変換できます。

```cpp
#include "gfx/schema/GfxJson.h"

std::string text = gfxJsonStringify(gfxToJson(pipelineDesc));   // 既定値と同じフィールドは省く

GfxJson json;
std::string error;
if (gfxJsonParse(text, json, &error))
{
    GfxGraphicsPipelineDesc desc;
    GfxJsonDiagnostics diag;   // エラーと警告 (フィールドのパス付き)
    if (gfxFromJson(json, desc, &diag)) { /* ... */ }
}
```

ハンドル (シェーダーなど) は実行時の値なので、既定では書き出しません。アセットの名前などに置き換えるときは
`GfxJsonWriteOptions::writeHandle` / `GfxJsonReadOptions::readHandle` を使います。

サンプルは 60 フレームごとに `[GPU   N]` の行で、パスごとの GPU 時間 (タイムスタンプのクエリ) と G-Buffer のオクルージョンクエリの結果を出力します。

---

## 自動テスト

doctest と CTest による自動テストがあります。内容は [tests/README.md](tests/README.md) を参照してください。

```bat
ctest --test-dir build/windows-debug --output-on-failure
```

GitHub Actions (`.github/workflows/ci.yml`) で、push と pull request のたびに Debug / Release のビルドとテストを実行します。ランナーには GPU がないため、描画テストは WARP (ソフトウェアレンダラー) で実行します。

---

## 動作確認 (サンプルの自動テスト)

`GfxExample` は、環境変数で自動テスト用の動作に切り替えられます。

| 環境変数 | 内容 |
|---------|------|
| `GFX_EXAMPLE_FRAMES=N` | N フレーム描画したら自動で終了する |
| `GFX_EXAMPLE_RESIZE_TEST=1` | リサイズ、最小化、復帰を自動で繰り返す |

`GFX_ENABLE_VALIDATION=ON` でビルドすると、D3D12 デバッグレイヤーのエラーと警告が stderr にも出力されます。

通常の実行とリサイズテストをまとめて確認するには、次のスクリプトを使います (D3D12 のメッセージが 0 件なら成功)。

```powershell
powershell -ExecutionPolicy Bypass -File tools/check-sample.ps1 -BinDir build/windows-debug/bin
```

---

## インストール

```bash
cmake --install build/linux-release --prefix /usr/local
```

| インストール先 | 内容 |
|---------------|------|
| `<prefix>/include/gfx/` | ヘッダファイル (`gfx/abi`、`gfx/schema`、`gfx/core`) |
| `<prefix>/bin/` | `SunsetGraphicsEngine.dll`、`gfx_dx12.dll`、`dxcompiler.dll`、`dxil.dll` (Windows) |
| `<prefix>/lib/` | インポートライブラリ (Windows)、共有ライブラリ (Linux / macOS) |
| `<prefix>/lib/cmake/SunsetGraphicsEngine/` | `find_package(SunsetGraphicsEngine)` 用の設定ファイル |

本体は、実行ファイルのディレクトリ、本体 DLL のディレクトリ、`<exe>/../lib` の順にバックエンド DLL を探します。

---

## ほかのプロジェクトから使う

ターゲット名は、`find_package` で使う場合も FetchContent / `add_subdirectory` で取り込む場合も同じです。

| ターゲット | 内容 |
|-----------|------|
| `SunsetGraphicsEngine::core` | 本体 DLL と C++ API (`gfx/core/GraphicsEngine.h`)。通常はこれだけをリンクする |
| `SunsetGraphicsEngine::schema` | 記述子のメタデータ表 (エディターのインスペクター / JSON 用) |
| `SunsetGraphicsEngine::abi` | DLL 境界の C ABI (`core` から自動で参照される) |
| `SunsetGraphicsEngine::dx12` | DX12 バックエンド。リンクはせず、配置にだけ使う |

アプリの実行ファイルの横に DLL を配置するには、`sunset_gfx_deploy_runtime(<ターゲット>)` を呼びます。

```cmake
# インストール済みのパッケージを使う場合
find_package(SunsetGraphicsEngine 0.1 REQUIRED)

# または FetchContent で取り込む場合 (取り込まれたときはサンプルをビルドしない)
# FetchContent_Declare(SunsetGraphicsEngine GIT_REPOSITORY ... GIT_TAG ...)
# FetchContent_MakeAvailable(SunsetGraphicsEngine)

add_executable(MyApp main.cpp)
target_link_libraries(MyApp PRIVATE SunsetGraphicsEngine::core)
sunset_gfx_deploy_runtime(MyApp)   # SunsetGraphicsEngine.dll / gfx_dx12.dll / dxcompiler.dll / dxil.dll をコピー
```

```cpp
#include "gfx/core/GraphicsEngine.h"

GraphicsEngine engine;
if (!engine.init(DeviceConfig{}))   // ヘッダーと本体 DLL の ABI が一致しない場合も false
    std::cerr << engine.lastError() << "
";
```
