# 描画エディターに向けたアーキテクチャ設計

## 目的

このエンジンの上に **描画エディター** を作る。エディターでは次のものを細かく制御できるようにする。

- **パイプラインステート**:D3D12 の構造体のメンバー単位(ブレンド、ラスタライザー、深度・ステンシル、入力レイアウト、ルートシグネチャなど)
- **描画順**:パスの実行順と、パスの中のドローの並べ方
- **合成方法**:G-Buffer の構成、ライティング、ポストエフェクトなど、ディファードレンダリングの各段の結果をどう組み合わせるか

方針は次の 2 つ。

1. **記述子は API 非依存のまま、DX と同じ粒度で表現する。** DX12 で設定できることはすべてデータとして書けるようにする。Vulkan を将来戻す余地も残す。
2. **描画順と合成はノード型のレンダーグラフで表現する。** バリアやリソースの寿命はエンジンが自動で解決する。必要な場合だけ、ユーザーが手動で上書きできる。

---

## レイヤー構成

```
┌──────────────────────────────────────────────────────────┐
│ Editor (Phase 4)                                           │
│   Web UI (webview) + ノードエディター / インスペクター       │
├──────────────────────────────────────────────────────────┤
│ Render Graph (Phase 3)  ── JSON でシリアライズ               │
│   パス / リソースのノード、コンパイル (順序・バリア・寿命)   │
├──────────────────────────────────────────────────────────┤
│ GraphicsEngine (本体 DLL、C ABI + C++ ラッパー)             │
│   プラグインのロード、ウィンドウ、スキーマ (リフレクション)  │
├──────────────────── DLL 境界 (C ABI) ─────────────────────┤
│ Backend ABI v2 (Phase 2)                                   │
│   全メンバーを持つ POD 記述子 + 明示的なコマンド             │
├──────────────────────────────────────────────────────────┤
│ gfx_dx12.dll            │ gfx_vulkan.dll (将来)            │
└──────────────────────────────────────────────────────────┘
```

### リポジトリの構成

エンジンと数学ライブラリは、それぞれ単独で使えるライブラリにする。エディターなど両方を組み合わせるソフトは、別のリポジトリにする。

| リポジトリ | 内容 | 依存 |
|-----------|------|------|
| SunsetGraphicsEngine | このエンジン (本体 DLL、バックエンド DLL、サンプル) | なし (MathLib にも依存しない) |
| SunsetMathLib | ヘッダーのみの数学ライブラリ (`MathLib::MathLib`) | なし |
| SunsetStudio | エディター (Phase 4) など、両ライブラリを組み合わせるアプリ | 両ライブラリ、webview / json (設計: SunsetStudio の docs/design/editor-shell.md) |

- エンジンの公開 API は POD (float 配列など) で表し、数学ライブラリの型を使わない。数学はアプリ側で MathLib を使う。
- SunsetStudio は両ライブラリを FetchContent で取得する。開発用のプリセットでは、同じワークスペースの隣のフォルダを直接使う (`FETCHCONTENT_SOURCE_DIR_<名前>`)。

### エンジン内のディレクトリと CMake ターゲットの対応

| レイヤー | ディレクトリ | ターゲット | 依存してよいもの |
|---------|-------------|-----------|-----------------|
| Render Graph | `engine/rendergraph/` | (Phase 3) | core、schema |
| GraphicsEngine (本体 DLL) | `engine/core/` | `SunsetGraphicsEngine::core` | abi |
| スキーマ | `engine/schema/` | `SunsetGraphicsEngine::schema` | abi |
| Backend ABI | `engine/abi/` | `SunsetGraphicsEngine::abi` | なし |
| バックエンド | `backends/dx12/`、`backends/vulkan/` | `SunsetGraphicsEngine::dx12`、`::vulkan` | abi のみ |
| サンプル | `samples/<名前>/` | `GfxExample` など | core、schema |

- 公開ヘッダーは各モジュールの `include/gfx/<モジュール>/` に置き、`#include "gfx/<モジュール>/..."` で読み込む。
- エンジン本体は DLL で、境界は C ABI (`gfx/core/CoreAPI.h`)。C++ API はヘッダーのみのラッパー (`gfx/core/GraphicsEngine.h`)。
- バックエンドとエディターが直接依存しないことが、ABI の網羅性を保つための前提になる。エディターで必要な機能は、必ず ABI に追加してから使う。
- シェーダーは `.hlsl` ファイルとして置く(共有するものは `shaders/`、サンプル固有のものは `samples/<名前>/shaders/`)。ビルド時に `bin/shaders/` にコピーし、実行時に読み込む。エディターのホットリロードはこの仕組みの上に作る。

---

## Phase 1:土台の修正(完了)

エディターでは、リソースや PSO を対話的に作り直したり破棄したりすることが前提になる。そのため、次の問題を先に直した。

| 領域 | 内容 |
|------|------|
| 所有権 | ABI のラッパー(`IRenderTarget` など)は DLL 内の実装が所有する。EXE 側で `delete` しない |
| ABI チェック | `GFX_ABI_VERSION` と vtable・構造体のサイズを `PluginLoader` で照合する |
| 遅延解放 | `destroy*` は submit フェンスが完了してから実際に解放する(`deferRelease`) |
| 定数バッファ | Uniform は 4 つのバージョンを持つリング。実行中のフレームのデータは上書きしない |
| ディスクリプタ | シェーダー可視ヒープを「静的領域」と「フェンス付き一時リング」に分けた |
| 状態追跡 | CB ごとのローカル状態と、submit 順で確定するグローバル状態の 2 段で管理する。submit 時に足りないバリアを補う |

Phase 1 の静的ディスクリプタ領域は、Phase 2 でそのまま**バインドレスのテーブル**として使える。シェーダー可視ヒープ上の番号と CPU ヒープ上の番号は同じにしてある。

---

## Phase 2:ABI v2(DX と同じ粒度の記述子)

### Phase 2a の実装状況(完了)

| 項目 | 内容 |
|------|------|
| 記述子 | `GfxGraphicsPipelineDesc`(`D3D12_GRAPHICS_PIPELINE_STATE_DESC`)と `GfxBindingLayoutDesc`(`D3D12_ROOT_SIGNATURE_DESC1`)。入れ子の構造体と列挙型はすべて `gfx/abi/PipelineState.h` |
| ABI | `createBindingLayout` / `destroyBindingLayout` / `createGraphicsPipeline` を vtable の末尾に追加。`GFX_ABI_VERSION` は 3 |
| 未対応のメンバー | `StreamOutput`、`CachedPSO`(必要になったら末尾に追加する) |
| 可変長の配列 | 固定長配列 + 個数(入力要素 16、ルートパラメータ 16、レンジ 32、静的サンプラー 16)。テーブルは `ranges[]` を `firstRange` / `numRanges` で参照する |
| 寿命 | パイプラインはルートシグネチャへの参照を自分で持つので、バインディングレイアウトは先に破棄してよい |
| 既存コマンドとの対応 | `bindTexture` / `bindSampler` は SRV だけ / Sampler だけのレンジを持つ最初のテーブルに、`bindUniformBuffer(reg)` は space 0 のルート CBV に対応付ける。ルート定数、ルート SRV/UAV、混在テーブルを設定するコマンドは Phase 2b |
| v1 | `GfxPipelineDesc` は互換レイヤーとして残した。`gfxConvertPipelineDesc` / `gfxConvertRootSignature`(ヘッダー内)で v2 に変換し、DLL 内で v2 の経路で作成する |
| スキーマ | `gfx/schema/GfxSchema.h`。`GFX_FIELDS_<型名>` / `GFX_ENUM_<型名>` の X マクロから、構造体の定義と同じ情報源でフィールド表と列挙子の表を生成する(2c の「スキーマの X マクロ」を先に入れた) |
| 値の対応 | 列挙値は D3D12 の値と同じ。DX12 バックエンドは `static_cast` で渡し、代表値を `static_assert` で照合している |

### Phase 2b の実装状況(完了)

2b-1(バインド、明示的なコマンド、タイムスタンプ)と 2b-2(リソースとビュー、MSAA)に分けて実装した。`GFX_ABI_VERSION` は 5。

| 項目 | 内容 |
|------|------|
| 低レベルなバインド | `setRootConstants` / `setRootDescriptor`(種類はレイアウトから決まる) / `setDescriptorTable`(一時リングに連続して書き込む) / `setVertexBuffers` / `setIndexBuffer`。`setDescriptorTable` で設定したテーブルは、draw 時の互換バインドの自動 flush で上書きしない |
| Compute | `GfxComputePipelineDesc`。`bindPipeline` で Compute を選ぶと、ルートの設定先が Compute 側に切り替わる |
| 描画 / 間接実行 | `drawInstanced` / `drawIndexedInstanced` / `drawIndirect` / `drawIndexedIndirect` / `dispatch` / `dispatchIndirect`(引数だけのコマンドシグネチャ、count バッファ対応) |
| バリア | `GfxBarrierDesc`(Transition / UAV)。`before = Tracked` なら自動追跡と同じ。`before` を明示すると、追跡を before に合わせてから before→after を記録し、追跡を after に上書きする。サブリソース指定のバリアは追跡を更新しない(元に戻すのは利用者の責任)。Aliasing と split barrier はプレースドリソースの対応後 |
| バッファの状態 | DEFAULT ヒープのバッファは CB の中だけで追跡する(開始時 COMMON、`end()` で COMMON に戻す)。UPLOAD ヒープは GENERIC_READ 固定。`BufferUsage::Storage` は UAV 可能な DEFAULT バッファ |
| 動的ステート / 転送 | `setViewports` / `setScissors` / `setStencilRef` / `setBlendFactor`、`copyBuffer` / `copyTexture`、範囲指定の `clearRenderTarget` / `clearDepthStencil`(ビュー版もあり) |
| マーカー / クエリ | `beginEvent` / `endEvent`(PIX)。`GfxQueryPoolDesc`(Timestamp / Occlusion / BinaryOcclusion)、`writeTimestamp` / `beginQuery` / `endQuery` / `resolveQueries`、`readQueryResults`(resolve を含む submit が GPU で完了していなければ false。待たない) |
| リソース | `GfxTextureResourceDesc`(1D/2D/3D/Cube、配列数、mip、サンプル数、使用フラグ、最適化クリア値、初期状態)。`TextureHandle` は v1 と同じハンドル空間で、既定の SRV / UAV を作る。深度フォーマットを SRV でも読むときは自動で Typeless にする |
| ビュー | `GfxViewDesc`(SRV/UAV/RTV/DSV、サブリソースの範囲、フォーマットの再解釈、バッファの構造化 / raw、読み取り専用 DSV)。v1 の RT / DS からも作れる。ビューはリソースへの参照を持つ |
| サンプラー | `GfxSamplerStateDesc`(D3D12_SAMPLER_DESC の全メンバー) |
| MSAA | v1 の RT / DS と v2 のテクスチャでサンプル数を指定できる(`CheckFeatureSupport` で確認)。MS 用の SRV / RTV / DSV、`resolve` コマンド、v1 の `GfxPipelineDesc::sampleCount` |
| スキーマ | 上の記述子と列挙型をすべて `GfxSchema.h` に追加した |

サンプル: パスごとの GPU 時間とオクルージョンをログに出す。グリッドは 4x MSAA の RT に描いて resolve し、乗算済みアルファで合成する(drawIndirect、v2 のビュー / サンプラー / setDescriptorTable を使用)。起動時に Compute / dispatchIndirect / 手動バリア / コピー / 範囲クリアの確認を 1 回実行する。

### Phase 2c の実装状況(完了、M1)

`GFX_ABI_VERSION` は 7。

| 項目 | 内容 |
|------|------|
| DXC への統一 | D3DCompile (FXC) を削除し、すべての HLSL を DXC で DXIL にコンパイルする(既定のプロファイルは `<stage>_6_0`)。DXC は公式リリース v1.8.2505 を configure 時に取得する(`cmake/GfxDxc.cmake`、`GFX_DXC_DIR` で上書き可)。`dxcompiler.dll` と `dxil.dll` を配置する |
| シェーダーの記述子 | `GfxShaderDesc` に `sourceName`(エラーメッセージと相対 `#include` の基準)、`targetProfile`、`defines`、`includeDirs`、`flags`(Debug / SkipOptimization / WarningsAsErrors)。`ShaderStage` に Geometry / Hull / Domain |
| エラーの通知 | コンパイルのエラーと警告は、デバッグメッセージのコールバックに `GFX_MESSAGE_ID_SHADER_COMPILE` で通知する(validation に関係なく)。パイプラインの作成失敗は `GFX_MESSAGE_ID_PIPELINE` |
| リフレクション | `GfxShaderReflection`(`gfx/abi/ShaderReflection.h`):バインディング(名前、種類、次元、register、space、数)、定数バッファ(構造体のメンバーを `parent.member` に展開し、オフセットとサイズ、型、行列の向き)、入出力シグネチャ、スレッドグループのサイズ、シェーダーモデル。X マクロで定義したのでスキーマと JSON にもそのまま載る。`getShaderReflection` は呼び出し側の `structSize` までだけを書く |
| 依存ファイル | `#include` で読み込んだファイルを記録し、`getShaderDependencies` で返す(ホットリロードの監視対象) |
| JSON | `gfx/schema/GfxJson.h`(外部依存なし)。スキーマの表をたどる汎用の変換 `gfxToJson` / `gfxFromJson`。列挙型は名前、フラグは名前の配列、既定値と同じフィールドは省く、float は最短表記、ハンドルは変換関数でアセットの参照に置き換える、エラーはフィールドのパス付き。`gfxDescribeStruct` / `gfxDescribeEnum` はエディターのフォーム生成用の型の説明 |
| 作り直し | `updateShader`(再コンパイルし、そのシェーダーを使うパイプラインも作り直す)、`updatePipeline`(記述子を変えて PSO を作り直す)、`getPipelineDesc`。ハンドルはそのままで、全部作れたときだけ入れ替える。古い PSO は遅延解放する |
| ホットリロード | `gfx/core/ShaderLibrary.h`(ヘッダーのみ)。ファイルからシェーダーを作り、`poll()` で本体と `#include` の更新時刻を見て作り直す。保存の途中を読まないよう、変化が止まるまで待つ。失敗したら元のシェーダーのまま描画を続ける。サンプルはソースの `shaders/` を直接読み、保存するとすぐ反映される |

完了条件の確認(テスト):記述子を JSON にしてハンドルを名前に置き換え、読み戻した記述子で作ったパイプラインの記述子が元と一致し、描画できる(`tests/test_reload.cpp`)。シェーダーのファイルと `#include` を書き換えると作り直され、壊れた変更では元のまま描画を続ける(同上、サンプルでも確認)。

### 記述子の共通ルール

- すべて C の POD にする。先頭に `uint32_t structSize; uint32_t version;` を置く。
  - DLL 境界を越えても安全に渡せる。
  - フィールドを末尾に追加しても後方互換を保てる。古い DLL は `structSize` を見て、知らないフィールドを無視する。
  - エディターがそのままシリアライズできる。
- 列挙値は D3D12 の値と 1 対 1 で対応させる(名前は API 非依存にする)。Vulkan への変換表はバックエンド側に持つ。

### パイプラインステート

`D3D12_GRAPHICS_PIPELINE_STATE_DESC` のメンバーを 1 対 1 で表現する。

```cpp
struct GfxRenderTargetBlendDesc {
    bool        blendEnable;      bool        logicOpEnable;
    GfxBlend    srcBlend, destBlend;       GfxBlendOp blendOp;
    GfxBlend    srcBlendAlpha, destBlendAlpha; GfxBlendOp blendOpAlpha;
    GfxLogicOp  logicOp;          uint8_t     writeMask;
};
struct GfxBlendDesc {
    bool alphaToCoverage; bool independentBlend;
    GfxRenderTargetBlendDesc rt[8];
};
struct GfxRasterizerDesc {
    GfxFillMode fill; GfxCullMode cull; bool frontCCW;
    int32_t depthBias; float depthBiasClamp; float slopeScaledDepthBias;
    bool depthClip; bool multisample; bool antialiasedLine;
    uint32_t forcedSampleCount; GfxConservativeRaster conservative;
};
struct GfxStencilOpDesc { GfxStencilOp fail, depthFail, pass; GfxCompareFunc func; };
struct GfxDepthStencilStateDesc {
    bool depthEnable; GfxDepthWriteMask depthWrite; GfxCompareFunc depthFunc;
    bool stencilEnable; uint8_t stencilReadMask, stencilWriteMask;
    GfxStencilOpDesc front, back;
};
```

- 入力レイアウトは、明示的な頂点フォーマット(`R32G32B32_FLOAT` など)、入力スロット、インスタンスのステップレートを持つ。セマンティクスは文字列で、名前とインデックスの組にする。
- トポロジは、種類(Triangle/Line/Point/Patch)と実際のトポロジ(List/Strip/Adj)の両方を持つ。サンプル数と品質も持つ。
- Compute パイプラインとメッシュシェーダーのパイプラインも同じ形式で記述する。

### バインディングレイアウト(ルートシグネチャ)

- テーブルは複数のレンジを持てる(SRV/UAV/CBV/Sampler を混在可、レンジごとに space とオフセットを指定)。
- ルート定数、ルート CBV/SRV/UAV、静的サンプラー(全メンバー)、フラグを持つ。
- Vulkan の対応:テーブルは DescriptorSetLayout、ルート定数は push constants に対応付ける。

### リソースとビュー

- テクスチャ:次元(1D/2D/3D/Cube)、配列数、mip、サンプル数、使用フラグ(RT/DS/UAV/SRV)、最適化クリア値、初期状態。
- ビュー:SRV/RTV/DSV/UAV を別々に作る。サブリソースの範囲(mip と配列スライス)とフォーマットを再解釈できる。
- サンプラー:全メンバー(border color、比較関数、異方性)。
- MSAA:サンプル数を持つリソース、MS 用のビュー、`resolve` コマンドに対応する。

### 明示的なコマンド

| コマンド | 用途 |
|---------|------|
| `barrier(res, before, after, subresource)` / `uavBarrier` / `aliasingBarrier` | 自動の状態追跡を上書きする |
| `setViewports` / `setScissors` / `setStencilRef` / `setBlendFactor` | 動的ステート |
| `setRootConstants` / `setRootDescriptor` / `setDescriptorTable` | 低レベルなバインド |
| `dispatch` / `dispatchIndirect` / `drawIndirect` | Compute と間接描画 |
| `copyBuffer` / `copyTexture` / `resolve` / `clear(rect)` | 転送 |
| `beginQuery` / `endQuery` / `writeTimestamp` | パスごとの GPU 時間の計測 |
| `beginEvent` / `endEvent` | PIX / RenderDoc のマーカー |

既存の `dispatchRays` は、RT シャドウ用に固定されたルートシグネチャを使っている。これは汎用の RT パイプライン(シェーダーテーブルとローカルルートシグネチャを記述子で指定する形)に置き換える。

### シェーダー

- DXC に統一する(DXIL を出力し、Vulkan 向けには `-spirv` で SPIR-V を出力する)。DXIL は 2c で実装済み。
- **リフレクション**を ABI で返す。返す内容は、バインディング(名前、種類、register、space、数)、cbuffer のレイアウト(メンバー名、型、オフセット)、入力シグネチャ。
  - エディターでは、マテリアルのパラメータ UI を自動生成したり、バインド漏れを検出したりするのに使う。

### スキーマ(リフレクション用メタデータ)

各 POD 記述子に、次のようなフィールド記述の表を用意する。

```cpp
struct GfxFieldInfo {
    const char* name;      // "depthBias"
    GfxFieldType type;     // Int32 / Float / Bool / Enum / Struct / Array
    uint32_t offset;       // offsetof
    const GfxEnumInfo* enumInfo;   // 列挙子の名前と値
    const GfxStructInfo* structInfo;
    float minValue, maxValue;      // UI 用の範囲
    const char* tooltip;           // D3D12 ドキュメントへの対応
};
```

- エディターのプロパティインスペクター、JSON シリアライザー、差分表示は、すべてこの表から汎用的に作る。手書きの UI は作らない。
- 表は X マクロで生成し、構造体の定義とずれないようにする。

---

## Phase 3:レンダーグラフ

### データモデル

```
Graph
 ├─ Resources
 │    ├─ Transient Texture  { format, size = 絶対値 | 出力解像度に対する比率, mips, samples, clear }
 │    ├─ Transient Buffer   { size, usage }
 │    └─ Imported           { バックバッファ, 前フレームの結果 (TAA 履歴など), 外部テクスチャ }
 └─ Passes
      ├─ type      : Raster | Compute | Copy | RayTrace | Present
      ├─ inputs    : [{ resource, usage: SRV | UAV(read) | CopySrc | IndirectArgs }]
      ├─ outputs   : [{ resource, usage: RTV[n] | DSV | UAV | CopyDst, load/store/clear }]
      ├─ pipeline  : パイプラインアセットの参照 (Phase 2 の記述子)
      ├─ content   : DrawList(query) | Fullscreen | Dispatch(x,y,z) | Custom(コールバック)
      ├─ order     : 優先度 / 明示的な依存 (データ依存がない場合の順序指定)
      └─ overrides : 手動バリア、ビューポート、動的ステート
```

- **DrawList のクエリ**:シーン中のどのオブジェクトをどう描くかを指定する。
  - 例:`layer == "Opaque"`、マテリアルのタグによるフィルター、ソート方法(front-to-back、back-to-front、マテリアル順、ユーザー定義キー)。
- **ディファードレンダリングの設計はすべてグラフ上のデータになる。**
  - G-Buffer の枚数とフォーマット:G-Buffer パスの出力 RTV 群。
  - ライティング方式(フルスクリーン、タイル、クラスター):ライティングパスの種類と Compute の構成。
  - 合成方法:合成パスのブレンドステートと入力の組み合わせ。

### コンパイル

1. **依存の解析**:リソースの読み書きから DAG を作り、トポロジカルソートする。データ依存がない場合はユーザーの `order` に従う。
2. **カリング**:出力が最終結果に届かないパスを除く(エディターでは灰色で表示する)。
3. **寿命の解析**:一時リソースが最初に使われるパスと最後に使われるパスを求める。将来はメモリエイリアシングに使う。
4. **バリアの生成**:エッジの用途から before/after を決め、パスの境界にまとめて発行する。`overrides` の手動バリアを優先する。
   - インポートしたリソースは、Phase 1 の CB 単位の状態追跡で外部の状態と合わせる。
5. **実行**:パスごとにコマンドバッファを割り当てる(並列記録に対応)。タイムスタンプのクエリを挿入する。

### 保存形式

- グラフとパイプラインのアセットは JSON で保存する。スキーマのバージョンを持たせる。
- ホットリロードに対応する。ファイルを監視し、変更されたパスだけを作り直す。PSO も差分だけ作り直す。

例(ディファードの一部):

```json
{
  "resources": {
    "gbAlbedo": { "format": "RGBA8_UNORM",  "size": { "relative": 1.0 } },
    "gbNormal": { "format": "RGBA16_FLOAT", "size": { "relative": 1.0 } },
    "depth":    { "format": "D32_FLOAT",    "size": { "relative": 1.0 }, "clear": { "depth": 1.0 } }
  },
  "passes": [
    { "name": "GBuffer", "type": "Raster", "pipeline": "pipelines/gbuffer.json",
      "outputs": [ { "res": "gbAlbedo", "as": "RTV0", "load": "clear" },
                   { "res": "gbNormal", "as": "RTV1", "load": "clear" },
                   { "res": "depth",    "as": "DSV",  "load": "clear" } ],
      "content": { "drawList": { "layer": "Opaque", "sort": "frontToBack" } } },
    { "name": "Lighting", "type": "Raster", "pipeline": "pipelines/deferred_light.json",
      "inputs":  [ { "res": "gbAlbedo", "as": "SRV" }, { "res": "gbNormal", "as": "SRV" },
                   { "res": "depth", "as": "SRV" } ],
      "outputs": [ { "res": "$backbuffer", "as": "RTV0", "load": "dontCare" } ],
      "content": { "fullscreen": true } }
  ]
}
```

### M3 の詳細設計 (2026-10-17 決定)

#### 決定事項

| 論点 | 決定 | 内容 |
|------|------|------|
| シーンの表現 | **コールバック方式** | パスの `content.drawList` は問い合わせ (レイヤー、タグによる絞り込み、ソート方法) だけを持つ。実際の描画は、アプリが渡す `IDrawListProvider` が行う。エンジンはシーンのデータ構造を持たないので、ECS にするかリストにするかはアプリ側で後から決められる |
| 入力とシェーダーのつなぎ方 | **ファイルに明示的に保存する (読み書きできる)** | パスの入力と定数ごとに、バインディングレイアウトのどこにつなぐか (`bind: { param, offset }`) をグラフの JSON に書く。実行時はこの指定だけを使う。エディターはシェーダーのリフレクションで候補を出したり自動で埋めたりして、その結果をファイルに書き込む。`shaderName` を書いておくと、リフレクションと照合し、違っていれば警告する |
| アセットの参照 | **ファイルのパス** | プロジェクトのフォルダからの相対パス (区切りは `/`)。例: `"pipelines/gbuffer.json"`。名前を変えたら参照も直す (エディターでの一括置換は M4) |
| 統計 (M2d) | **M3b に含める** | パスごとの GPU 時間は、レンダーグラフの実行がパスの境界にタイムスタンプを入れて測るのが自然なので、M3b で入れる |

#### アセット

- **シェーダー**: `.hlsl` のファイル。エントリーポイントとマクロはパイプラインのアセットで指定する。
- **パイプライン** (`pipelines/*.json`): シェーダーのファイル、バインディングレイアウト (`GfxBindingLayoutDesc` の JSON)、ステート (`GfxGraphicsPipelineDesc` の JSON。ハンドルは含めない)。
  RTV / DSV のフォーマットと枚数は、それを使うパスの出力から決める (G-Buffer のフォーマットをグラフで変えても、パイプラインを直さなくてよい)。
- **グラフ** (`graphs/*.json`): リソースとパス。

```json
{
  "version": 1,
  "resources": {
    "gbAlbedo": { "format": "R8G8B8A8_Unorm",     "size": { "relative": 1.0 } },
    "gbNormal": { "format": "R16G16B16A16_Float", "size": { "relative": 1.0 } },
    "depth":    { "format": "D32_Float",          "size": { "relative": 1.0 }, "clear": { "depth": 1.0 } }
  },
  "passes": [
    { "name": "GBuffer", "type": "Raster", "pipeline": "pipelines/gbuffer.json",
      "constants": [ { "source": "frame", "bind": { "param": 0 } } ],
      "outputs": [ { "res": "gbAlbedo", "as": "RTV0", "load": "clear" },
                   { "res": "gbNormal", "as": "RTV1", "load": "clear" },
                   { "res": "depth",    "as": "DSV",  "load": "clear" } ],
      "content": { "drawList": { "layer": "Opaque", "sort": "frontToBack" } } },
    { "name": "Lighting", "type": "Raster", "pipeline": "pipelines/deferred_light.json",
      "constants": [ { "source": "frame", "bind": { "param": 0 } } ],
      "inputs":  [ { "res": "gbAlbedo", "as": "SRV", "bind": { "param": 1, "offset": 0 }, "shaderName": "gAlbedo" },
                   { "res": "gbNormal", "as": "SRV", "bind": { "param": 1, "offset": 1 }, "shaderName": "gNormal" },
                   { "res": "depth",    "as": "SRV", "bind": { "param": 1, "offset": 2 }, "shaderName": "gDepth" } ],
      "outputs": [ { "res": "$backbuffer", "as": "RTV0", "load": "dontCare" } ],
      "content": { "fullscreen": true } }
  ]
}
```

- 同じ `param` (ディスクリプタテーブル) につながる入力は、`offset` の位置に並べた 1 つのテーブルとして設定する。
- `constants.source` は、アプリが名前で渡す定数バッファ (`frame` など) か、JSON に書いた値 (`inline`)。
- `$backbuffer` などの `$` で始まる名前は、実行時にアプリが渡すインポートのリソース。

#### 表せる描画の流れ (2026-10-17 決定)

ディファード専用にはしない。一般的な描画手法を、グラフのデータとして表せることを目標にする。

| 手法 | 表し方 |
|------|--------|
| フォワード / Forward+ | Compute でライトを分類してバッファに書き、フォワードのパスが SRV で読む |
| ディファード (G-Buffer の構成は自由)、タイル / クラスターのライティング | 複数の RTV、Compute で UAV のテクスチャに書く |
| シャドウマップ / カスケードシャドウ / キューブマップの影と反射プローブ | 配列 / キューブのテクスチャ、出力の `slice`、`repeat` でスライスごとにパスを並べ、`view` でカメラを差し替える |
| シャドウアトラス、部分的な描画 | パスの `viewport` / `scissor` |
| ブルームの縮小 / 拡大、mip の生成 | mip を持つテクスチャ、入出力の `mip`、`repeat` で mip ごとにパスを並べる (読む mip と書く mip が違えば、同じテクスチャを同じパスで読み書きしてよい) |
| TAA、SSR / ボリュメトリックの時間方向の積み上げ | `history: true` のリソース。`name@prev` で前フレームの内容を読む |
| 半透明、デカール、アウトライン | 深度を `DSVRead` で読みながら RTV に重ねて描く (`load: load`) |
| MSAA | サンプル数を持つテクスチャ、Copy のパスの `ResolveSrc` / `ResolveDst` |
| GPU 主導の描画 | `content` の `drawIndirect` / `dispatchIndirect` (引数バッファは入力の `IndirectArgs`) |
| フォーマットの読み替え (sRGB、深度を R32 で読む) | 入出力の `format` (ビューのフォーマット) |
| 品質設定、機能のオン / オフ | グラフの `parameters` と、パスの `when` |
| 複数のビュー (分割画面、エディターの複数ビューポート) | 同じグラフを、別のカメラと別の `$` 出力で複数回実行する |
| レイトレーシング (影、反射、GI) | `type: "RayTrace"` (**名前と意味だけ決める**。エンジンの汎用の RT パイプラインのあとで実装) |
| ブルームなどの部品化と再利用 | `type: "Subgraph"` と `graph` (別のグラフのアセットを、入出力をつないで埋め込む。**名前と意味だけ決める**。M4 のエディターと合わせて実装) |
| 非同期 Compute | パスの `queue: "compute"` (**名前と意味だけ決める**。エンジンが複数のキューに対応したら実装) |

追加する項目の JSON:

```json
{
  "parameters": { "ssao": true, "cascades": 4 },
  "resources": {
    "shadowMap": { "format": "D32_Float", "size": { "width": 2048, "height": 2048 }, "array": 4 },
    "envProbe":  { "format": "R16G16B16A16_Float", "dimension": "cube", "size": { "width": 256, "height": 256 }, "mips": 5 },
    "fogVolume": { "format": "R16G16B16A16_Float", "dimension": "3d", "size": { "width": 160, "height": 90 }, "depth": 64 },
    "bloom":     { "format": "R11G11B10_Float", "size": { "relative": 0.5 }, "mips": 5 },
    "taaColor":  { "format": "R16G16B16A16_Float", "size": { "relative": 1.0 }, "history": true },
    "sceneMS":   { "format": "R16G16B16A16_Float", "size": { "relative": 1.0 }, "samples": 4 }
  },
  "passes": [
    { "name": "Shadow", "type": "Raster", "pipeline": "pipelines/shadow.json",
      "repeat": { "count": "cascades", "var": "i" }, "view": "cascade{i}",
      "outputs": [ { "res": "shadowMap", "as": "DSV", "slice": "i", "load": "clear" } ],
      "content": { "drawList": { "layer": "ShadowCaster" } } },
    { "name": "BloomDown", "type": "Raster", "pipeline": "pipelines/bloom_down.json",
      "repeat": { "count": 4, "var": "i", "start": 1 },
      "inputs":  [ { "res": "bloom", "as": "SRV", "mip": "i-1", "bind": { "param": 1 } } ],
      "outputs": [ { "res": "bloom", "as": "RTV0", "mip": "i", "load": "dontCare" } ],
      "content": { "fullscreen": true } },
    { "name": "TAA", "type": "Compute", "pipeline": "pipelines/taa.json",
      "inputs":  [ { "res": "taaColor@prev", "as": "SRV", "bind": { "param": 1, "offset": 1 } } ],
      "outputs": [ { "res": "taaColor", "as": "UAV", "load": "dontCare", "bind": { "param": 2 } } ],
      "content": { "dispatch": [ 120, 68, 1 ] } },
    { "name": "SSAO", "when": "ssao", "type": "Compute", "...": "..." },
    { "name": "Resolve", "type": "Copy",
      "inputs":  [ { "res": "sceneMS", "as": "ResolveSrc" } ],
      "outputs": [ { "res": "sceneColor", "as": "ResolveDst" } ] },
    { "name": "Cull", "type": "Compute", "pipeline": "pipelines/cull.json",
      "inputs": [ { "res": "dispatchArgs", "as": "IndirectArgs" } ],
      "content": { "dispatchIndirect": { "args": "dispatchArgs", "offset": 0 } } },
    { "name": "Atlas", "type": "Raster", "viewport": { "x": 0.5, "y": 0, "width": 0.5, "height": 0.5 }, "...": "..." }
  ]
}
```

- `mip` / `slice` は数値か、`repeat` の変数を使った `"i"` / `"i+1"` / `"i-1"`。省略するとすべて。範囲は `mipCount` / `sliceCount` (省略すると 1)。
- `repeat.count` は数値か、グラフのパラメーターの名前。繰り返したパスの名前は `Shadow[0]`、`Shadow[1]` … になる。`after` に元の名前を書くと、すべての繰り返しのあとになる。`view` の `{i}` は値に置き換える。
- `when` はパラメーターの名前 (`"!ssao"` で否定)。偽ならパスはなかったものとしてコンパイルする。パラメーターの値は、コンパイルのときにアプリが上書きできる。
- バリアと依存はサブリソース (mip とスライス) 単位で扱う。インポートしたリソースのサブリソースの指定と、`history` のリソースの mip / 配列は、当面は使えない (エラー)。
- `viewport` / `scissor` は既定で出力の大きさに対する比率 (`"relative": false` でピクセル)。

#### 構成 (エンジンの `engine/rendergraph/`、ターゲット `SunsetGraphicsEngine::rendergraph`)

C++ の静的ライブラリ。エンジンの公開 API (C++ ラッパー) だけを使うので、DLL の境界の外にあり、ABI の網羅性を確かめる利用者にもなる。

| 部品 | 役割 | GPU |
|------|------|-----|
| `GraphDesc` | グラフのデータ。JSON と相互に変換する (M1 の `GfxJson` を使う) | 不要 |
| `compile()` | 順序 (トポロジカルソートと `order`)、カリング、寿命の解析、バリアの計画、`bind` の検証 | 不要 (単体テストできる) |
| `PipelineLibrary` | パイプライン / シェーダーのアセットを読み込み、作成済みのものを再利用する。ファイルの変更で作り直す (`ShaderLibrary` を使う)。3D モデルやテクスチャは扱わない (後で別のリポジトリ SunsetAssets にする) | 必要 |
| `Executor` | 一時リソースのプール (大きさとフォーマットで再利用)、バリアの発行、パスの記録、パスごとのタイムスタンプ | 必要 |
| `IDrawListProvider` | アプリが実装する。`draw(query, ctx)` で描画を記録し、`constants(name)` で定数バッファを返す | — |

#### 段階と完了条件

| 段階 | 場所 | 内容 | 完了条件 |
|------|------|------|---------|
| **M3a** コア | エンジン | `GraphDesc`、`compile()`。上の「表せる描画の流れ」の項目 (RayTrace / Subgraph / queue を除く) | GPU を使わない単体テストで、順序、カリング、寿命、バリアの計画、`bind` の検証が正しい。カスケードシャドウ、ブルームの mip の連鎖、TAA の履歴、Forward+、MSAA の解決、間接 dispatch、条件付きのパスの例がコンパイルできる |
| **M3b** 実行 | エンジン | `PipelineLibrary`、`Executor` (Raster / Fullscreen / Compute)、パスごとの GPU 時間 (M2d) | サンプルのディファード (G-Buffer → ライティング → 合成) をグラフで描け、D3D12 のメッセージが 0 件 |
| **M3c** 保存とホットリロード | エンジン | グラフとパイプラインのアセットの JSON の読み書き、ファイルの変更での作り直し | JSON を編集すると、サンプルの描画がその場で変わる |
| **M3d** エディター | Studio | ビューポートをグラフで描く。アウトライナーにパスの一覧と GPU 時間、インスペクターでパスのパイプラインとつなぎ方を編集する | エディターで G-Buffer の構成や合成のブレンドを変え、すぐ反映される |

ノードエディター (React Flow でグラフを線でつないで編集する UI) は M4。

#### パイプラインのアセット (2026-10-17 決定)

`PipelineLibrary` が読む `pipelines/*.json` の形。`layout` と `state` は M1 の記述子の JSON (`gfxToJson`) と同じ。

```json
{
  "version": 1,
  "type": "graphics",
  "shaders": {
    "vs": { "file": "shaders/gbuffer.vs.hlsl" },
    "ps": { "file": "shaders/gbuffer.ps.hlsl", "entry": "main", "profile": "ps_6_0", "defines": { "NAME": "1" } }
  },
  "layout": { "parameters": [ { "parameterType": "CBV", "shaderRegister": 0 } ], "numParameters": 1 },
  "state":  { "rasterizerState": { "cullMode": "None" } }
}
```

- `type: "compute"` のときは `shaders.cs` だけを使い、`state` は使わない。
- RTV / DSV のフォーマットと枚数、サンプル数は書かない (書いても、パスの出力の値で上書きする)。出力が違うパスで使うと、別の PSO を作る。
- シェーダーのファイルは、プロジェクトのフォルダからの相対パス。`#include` はシェーダーのファイルからの相対パスで探す。
- 名前で渡す定数 (`constants.source` が `inline` 以外) は、`IDrawListProvider::constants(name)` が返す定数バッファをつなぐ。無効なハンドルを返すと Executor はつながず、アプリが `draw` の中で `DrawContext::paramOf(name)` の場所に設定する (オブジェクトごとの定数など)。

#### M3b の実装状況 (完了)

| 項目 | 内容 |
|------|------|
| `PipelineLibrary` | `pipelines/*.json` を読み、バインディングレイアウトとパイプラインを作る。同じアセットと同じ出力のフォーマットなら作成済みのものを返す。シェーダーは `ShaderLibrary` で作るので、`.hlsl` の保存で作り直される。コンパイルの bind の検証にもレイアウトを渡す (`bindingLayout(path)`) |
| `Executor` | 一時リソースのプール (大きさ / フォーマット / 使い方が同じものを再利用し、3 フレーム使わなければ破棄)、バリアの発行 (一時リソースは毎フレーム最初に作成時の状態へ戻す)、出力の設定とクリア、ビューポート / シザー、bind どおりのテーブル / ルート CBV / ルート定数、内容 (drawList / fullscreen / dispatch / drawIndirect / dispatchIndirect / custom / コピー / MSAA の解決)、パスごとのイベントとタイムスタンプ (4 フレームのリングで読み出す) |
| アプリとの境界 | `FrameInputs` (基準の大きさ、`$` のリソース、`IDrawListProvider`)。`$backbuffer` などは `ImportedResource` (IRenderTarget / IDepthStencil / v2 テクスチャ / バッファ) で渡す |
| 制限 | サブリソース単位のバリアになるグラフ、`history` / `@prev`、IRenderTarget で渡した出力とグラフのテクスチャを同じパスに混ぜること、Sampler のテーブル (静的サンプラーを使う) は、まだ実行できない (`record` がエラーを返す)。必要になった時点 (ブルームや TAA を使うとき) で対応する |
| サンプル | `samples/rendergraph` (`GfxGraphSample`): `assets/graphs/deferred.json` (G-Buffer → ライティング → 合成) を描く。Debug + デバッグレイヤー + GBV で D3D12 のメッセージ 0 件 (通常 / リサイズ / 最小化 / 復帰)。GPU 時間を 120 フレームごとに出力する (RTX 3060 Laptop の Release で 3 パス合計 約 0.17 ms) |
| テスト | `tests/test_rendergraph_exec.cpp` (`gfx_tests_render`、GPU / WARP): パイプラインのアセットの JSON と診断、Compute (ルート定数 + UAV) → MRT と深度 → 4 枚を SRV で読んで合成、一時リソースの再利用と大きさの変更、出力のフォーマットごとのパイプライン、GPU 時間、渡していないインポートと読めないアセットのエラー、D3D12 のメッセージ 0 件 |

#### M3c の実装状況 (完了)

| 項目 | 内容 |
|------|------|
| `GraphAsset` | グラフのファイル 1 つ。`load` (読んでコンパイル)、`poll` (変わっていたら load)、`recompile` (パイプラインのレイアウトが変わったとき)、`set` (エディターの編集)、`save` / `saveAs`。読めない / コンパイルできない変更は、前のグラフのまま `messages()` に理由を入れる。切り替えるたびに `revision()` が増える。GPU は使わない |
| `PipelineLibrary` | `poll` で読んだアセットのファイルの変更を調べて作り直す。作成済みのパイプラインは**ハンドルのまま** `updatePipeline` で作り直し、レイアウトが変わったら新しいレイアウトに付け替える。1 つでも作れなければ、作り直したものを元に戻して前のままにする。`set` (エディターの編集)、`save` |
| 監視 | `FileWatch`: 更新時刻が変わってから既定 100 ms 変化がなくなるのを待つ (保存の途中を読まない)。`poll` の間隔は既定 250 ms。保存は一時ファイルに書いてから置き換え、自分で書いた内容は変更として扱わない |
| 反映の順 | アプリはフレームごとに `ShaderLibrary::poll` → `PipelineLibrary::poll` → `GraphAsset::poll` (パイプラインを作り直したら `recompile`) の順に呼ぶ。Executor は毎フレーム `PipelineLibrary` からハンドルとレイアウトを引くので、何もしなくてよい |
| サンプル | `GfxGraphSample` の実行中に `pipelines/gbuffer.json` に `"fillMode": "Wireframe"` を足すとワイヤーフレームになり、`graphs/deferred.json` の `sceneColor` のフォーマットを変えると読み直す。ライティングを無効にする (合成の入力が書かれない) 変更や壊れた JSON では、理由を出して前のグラフのまま描き続ける。D3D12 のメッセージ 0 件 |
| テスト | `tests/test_rendergraph.cpp` (GPU 不要): グラフのファイルの読み直し、壊れた JSON / コンパイルのエラーで前のまま、`set` / `save` / `saveAs` / `recompile`。`tests/test_rendergraph_exec.cpp` (GPU / WARP): パイプラインのステートとレイアウトの変更をハンドルのまま反映、壊れた JSON と作れないシェーダーで前のまま、`save` |

#### M3a の実装状況 (完了)

| 項目 | 内容 |
|------|------|
| ターゲット | `SunsetGraphicsEngine::rendergraph` (C++ の静的ライブラリ、名前空間 `gfx::rg`)。依存は `schema` だけ (M3a は GPU を使わない)。install / `find_package` に含める |
| データ | `GraphDesc.h`: パラメーター (bool / 整数)、リソース (テクスチャ / バッファ / インポート。テクスチャは 2D / キューブ / 3D、配列、mip、MSAA、`history`)、パス (Raster / Compute / Copy)、入出力 (`Access` (`ResolveSrc` / `ResolveDst` を含む)、`rtvIndex`、`LoadOp`、`bind`、`shaderName`、`mip` / `slice` とその数、ビューの `format`)、定数 (`source`、`bind`、inline の値)、内容 (drawList / fullscreen / dispatch / drawIndirect / dispatchIndirect / custom)、`view`、`viewport` / `scissor`、`repeat`、`when`、`order` / `after` / `keep` / `enabled` |
| JSON | `GraphJson.h`: 上の形式のとおり。既定値の項目は書かない。読むときは型の違いをエラー、知らないキーを警告にし、診断に `passes[Lighting].inputs[0].bind` のようなパスを付ける。書き出しは安定している (読み直して書くと同じ文字列) |
| コンパイル | `Compiler.h`: 展開 (パラメーターを `CompileOptions::parameters` で上書きし、`when` を評価して、`repeat` を `Name[k]` に展開する) → 検証 → 依存 (サブリソースごとの RAW / WAR / WAW と after) → カリング (インポートへの出力と keep から RAW をさかのぼる。`name@prev` を読むパスが残るなら `name` を書くパスも残す) → 順序 (order と宣言順で並べるトポロジカルソート。循環はエラー) → 寿命 → バリア (サブリソース単位。全体が同じ状態ならまとめる)。一時リソースは最初に使う状態で作る前提で最初のバリアを出さず、インポートと履歴は `Tracked` からの遷移を出す。UAV に続けて書くときは UAV バリアを出す。実行しないパスは理由 (無効 / 条件 / カリング) と一緒に `skipped` に入る |
| 検証 | 名前の重複、宣言していないリソース、使い方とパスの種類 / リソースの種類 / フォーマット (RTV に深度など)、RTV の番号の重複、DSV は 1 つ、同じパスで読みながら書く、書いていないリソースを読む (エラー)、まだ書かれていない内容を load する (警告)、bind がない / 重なる、mip / スライスの範囲 (インポートとバッファには指定できない、RTV / DSV は mip を 1 つ)、repeat の変数 (repeat のないパスで使う、負になる)、`@prev` (history のリソースだけ、読むだけ)、解決 (Copy のパスだけ、MSAA から MSAA でないものへ)、間接描画の引数 (入力に `IndirectArgs` で書く)、ビューポート / シザーの範囲、キューブ / 3D / MSAA のリソースの制限。`CompileOptions::bindingLayout` を渡すと、bind をバインディングレイアウトと照合する (パラメーターの有無、テーブルの範囲と種類、ルート定数に入る数、ルートの SRV / UAV はバッファだけ) |
| テスト | `tests/test_rendergraph.cpp` (`gfx_tests_unit`、GPU 不要): JSON の往復と診断、ディファードの順序 / 寿命 / バリア、カリングと keep / enabled、order と after と循環、WAR / WAW、UAV バリア、検証のエラー、bind とレイアウトの照合。手法の例として、カスケードシャドウ、ブルームの mip の連鎖、TAA の履歴、Forward+、MSAA の解決、間接 dispatch、パラメーターと when の切り替え、ビューポートとサブリソースの検証 |

---

## Phase 4:エディター

エディターは SunsetStudio リポジトリの SunsetEditor として作る。UI は webview/webview の上の Web UI (保持モード) で、3D ビューポートはエンジンが描画するネイティブの子ウィンドウを重ねる。骨格 (ウィンドウ、スレッド、C++ と JS の橋渡し、ビューポート) の詳細は SunsetStudio の [docs/design/editor-shell.md](https://github.com/SunsetTwilight/SunsetStudio/blob/main/docs/design/editor-shell.md) にまとめた。

| 機能 | 内容 |
|------|------|
| グラフエディター | React Flow (`@xyflow/react`) を使う。パスとリソースのノード、エッジの用途、カリングされたパスを表示する |
| インスペクター | スキーマ (JSON で UI に渡す) からフォームを自動生成する。D3D12 の構造体メンバー単位で編集でき、変更はすぐ PSO の再作成に反映される |
| リソースビューアー | 任意の中間テクスチャを表示する。チャンネルの選択、mip とスライス、深度の線形化、HDR の露出、ピクセルの値を確認できる (ビューポートと同じ子ウィンドウ方式。ピクセル値はリードバックで取得する) |
| プロファイラー | パスごとの GPU 時間(タイムスタンプ)と、PSO の作成やキャッシュミスを記録する |
| デバッグ連携 | PIX と RenderDoc のキャプチャをボタンから実行する。パスの名前をマーカーとして埋め込む |
| Undo/Redo | シリアライズしたグラフに対するコマンドパターン(変更前と変更後の JSON の差分)で実装する |

エディターはエンジンの通常の利用者として、公開 API (C ABI と C++ ラッパー) だけを使う。これにより、エディター自体が ABI の網羅性を確かめるテストにもなる。

### エンジン側への要望

- **スレッドの前提を明記する**:エンジンの API はスレッドセーフではない。エディターは描画スレッドからだけ呼ぶ。
- **リードバック API**:レンダーターゲットやテクスチャを CPU に読み戻す。ビューポートを隠す間の静止画、リソースビューアーのピクセル値、テストでのピクセル検証に使う。
- **ログのコールバック**:ABI 6 の `debugCallback` で対応済み。

---

## 実装順序と完了条件

M0 (テスト、CI、v0.1.0 のタグ)、M1 (Phase 2c、v0.2.0) と M2a〜M2c (エディターの骨格、ビューポート、インスペクター) は完了した。以降はマイルストーン (M) 単位で進める。

| マイルストーン | 内容 | 完了条件 |
|---------------|------|---------|
| ~~M0~~ | doctest のテスト、GitHub Actions (WARP)、v0.1.0 のタグ | 完了 |
| ~~M1 (Phase 2c)~~ | DXC への統一、リフレクション、スキーマの X マクロ、記述子の JSON の往復、シェーダーのホットリロード | 完了 (v0.2.0、ABI 7) |
| ~~M2 (Studio)~~ | エディターの骨格 (webview、Bridge、ビューポート、インスペクター)。詳細は editor-shell.md | M2a〜M2c は完了。M2d (統計) は M3b で入れた (パスごとの GPU 時間) |
| M3 (Phase 3) | レンダーグラフ。段階 M3a〜M3d は上の「M3 の詳細設計」。M3a (データ、JSON、コンパイル)、M3b (PipelineLibrary、Executor、GPU 時間)、M3c (保存とホットリロード) は完了 | グラフからサンプルと同じ画が出る。バリアが自動で生成される。エディターでパスのパイプラインとつなぎ方を編集できる |
| M4 | エディターの MVP(グラフ編集、リソースビューアー、Undo/Redo) | ディファードの合成方法を UI だけで変更できる |
| 並行 | Vulkan バックエンドを ABI v2 で作り直す (macOS / Linux のビューポートの検証もここで行う) | |

完了済みの Phase:1、2a、2b、2c。

## 未解決の論点

- ~~**シーンの表現**~~:コールバック方式に決めた (M3 の詳細設計)。シーンのデータ構造 (ECS かリストか) はアプリ側で決める。
- **マテリアルシステム**:パイプラインアセットとマテリアルパラメータをどう分けるか、インスタンス化の粒度をどうするか。
- ~~**アセットの参照方式**~~:プロジェクトのフォルダからの相対パスに決めた (M3 の詳細設計)。
- ~~**ABI v2 への移行方式**~~:v1 の API は v2 の上の互換レイヤーとして残すことにした(Phase 2a)。リソースとビュー(2b)も同じ方式にする予定。
