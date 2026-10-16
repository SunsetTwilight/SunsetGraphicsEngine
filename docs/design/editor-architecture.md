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

- DXC に統一する(DXIL を出力し、Vulkan 向けには `-spirv` で SPIR-V を出力する)。
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

M0 (テスト、CI、v0.1.0 のタグ) は完了した。以降はマイルストーン (M) 単位で進める。

| マイルストーン | 内容 | 完了条件 |
|---------------|------|---------|
| ~~M0~~ | doctest のテスト、GitHub Actions (WARP)、v0.1.0 のタグ | 完了 |
| M1 (Phase 2c) | DXC への統一、リフレクション、スキーマの X マクロ、記述子の JSON の往復、シェーダーのホットリロード | 記述子を JSON にして戻すと同じ PSO が作れる。シェーダーを書き換えると再読み込みされる |
| M2 (Studio) | エディターの骨格 (webview、Bridge、ビューポート、インスペクター、統計)。段階 M2a〜M2d は editor-shell.md を参照 | M2a/M2b は M1 と並行できる。M2c は M1 の JSON の往復に依存する |
| M3 (Phase 3a/3b) | レンダーグラフのコア、サンプルのディファードをグラフに移行、JSON の保存と読み込み | グラフからサンプルと同じ画が出る。バリアが自動で生成される |
| M4 | エディターの MVP(グラフ編集、リソースビューアー、Undo/Redo) | ディファードの合成方法を UI だけで変更できる |
| 並行 | Vulkan バックエンドを ABI v2 で作り直す (macOS / Linux のビューポートの検証もここで行う) | |

完了済みの Phase:1、2a、2b。

## 未解決の論点

- **シーンの表現**:DrawList のクエリが参照するシーンのデータ構造(ECS にするか、単純なリストにするか)をまだ決めていない。
- **マテリアルシステム**:パイプラインアセットとマテリアルパラメータをどう分けるか、インスタンス化の粒度をどうするか。
- **アセットの参照方式**:パスで参照するか、GUID で参照するか。
- ~~**ABI v2 への移行方式**~~:v1 の API は v2 の上の互換レイヤーとして残すことにした(Phase 2a)。リソースとビュー(2b)も同じ方式にする予定。
