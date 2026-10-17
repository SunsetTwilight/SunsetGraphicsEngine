#pragma once
// ============================================================
//  gfx/rendergraph/GraphDesc.h
//
//  レンダーグラフのデータ (グラフのアセット graphs/*.json と 1 対 1 に対応する)。
//  設計: docs/design/editor-architecture.md の「Phase 3」「M3 の詳細設計」
//
//    リソース : 一時的なテクスチャ / バッファ (グラフが確保する) と、インポート ($ で始まる名前。
//               実行時にアプリが渡す。$backbuffer など)
//    パス     : 入力 (読む) と出力 (書く) のリソース、パイプラインのアセット、描く内容
//
//  パスの宣言順が「意味の順序」になる (後のパスは、前のパスが書いた内容を読む)。
//  コンパイル (Compiler.h) は、データの依存を保ったまま order の小さい順に並べ替え、
//  使われないパスを除き、リソースの寿命とバリアを決める。
//
//  入力とシェーダーのつなぎ方 (bind) はデータとして明示的に持つ (決定事項)。
//  実行時は bind の指定だけを使う。エディターはリフレクションで候補を出して書き込む。
// ============================================================

#include "gfx/abi/GfxBackendABI.h"

#include <cstdint>
#include <string>
#include <vector>

namespace gfx::rg
{
// ---- リソース ----------------------------------------------

enum class ResourceKind : uint32_t
{
	Texture,
	Buffer,
	Imported,   // 実行時にアプリが渡す ($ で始まる名前は宣言しなくてもインポートとして扱う)
};

// テクスチャの大きさ: 出力 (バックバッファ) に対する比率か、絶対値
struct SizeSpec
{
	bool     relative = true;
	float    scale    = 1.0f;   // relative のとき
	uint32_t width    = 0;      // !relative のとき
	uint32_t height   = 0;

	bool operator==(const SizeSpec&) const = default;
};

// 出力の load = Clear のときの値。指定がなければ色は 0、深度は 1、ステンシルは 0
struct ClearSpec
{
	bool    hasColor = false;
	float   color[4] = { 0, 0, 0, 0 };
	bool    hasDepth = false;
	float   depth    = 1.0f;
	uint8_t stencil  = 0;

	bool operator==(const ClearSpec&) const = default;
};

struct ResourceDesc
{
	std::string  name;
	ResourceKind kind        = ResourceKind::Texture;
	GfxFormat    format      = GfxFormat::Unknown;   // Texture
	SizeSpec     size;                                // Texture
	uint32_t     mipLevels   = 1;                     // Texture
	uint32_t     sampleCount = 1;                     // Texture
	uint64_t     byteSize    = 0;                     // Buffer
	ClearSpec    clear;

	bool operator==(const ResourceDesc&) const = default;
};

inline bool isImportedName(const std::string& name) { return !name.empty() && name[0] == '$'; }

// ---- パス --------------------------------------------------

enum class PassType : uint32_t
{
	Raster,    // ラスタライズ (RTV / DSV に描く)
	Compute,   // Compute シェーダー (UAV に書く)
	Copy,      // コピー (CopySrc → CopyDst)
};

// リソースの使い方 (バリアの状態はここから決まる)
enum class Access : uint32_t
{
	SRV,            // シェーダーで読む
	UAV,            // シェーダーで読み書きする (入力に書くと読むだけ、出力に書くと書く)
	RTV,            // レンダーターゲット (出力。rtvIndex で何番目か)
	DSV,            // 深度ステンシルに書く (出力)
	DSVRead,        // 深度テストに使うが書かない (入力。読み取り専用の DSV)
	CopySrc,        // コピー元 (入力)
	CopyDst,        // コピー先 (出力)
	IndirectArgs,   // 間接描画の引数 (入力)
};

enum class LoadOp : uint32_t
{
	Load,       // 前の内容を残す
	Clear,      // クリアする (値は ResourceDesc::clear)
	DontCare,   // 前の内容は使わない (全面を描く場合)
};

// バインディングレイアウト (ルートシグネチャ) のどこにつなぐか
//   param  : ルートパラメーターの番号 (-1 = 未指定)
//   offset : ディスクリプタテーブルの中の位置 (テーブル以外では 0)
struct Binding
{
	int32_t  param  = -1;
	uint32_t offset = 0;

	bool valid() const { return param >= 0; }
	bool operator==(const Binding&) const = default;
};

struct PassResourceRef
{
	std::string res;
	Access      as         = Access::SRV;
	uint32_t    rtvIndex   = 0;               // as = RTV のとき
	LoadOp      load       = LoadOp::Load;    // 出力の RTV / DSV / UAV のとき
	Binding     bind;                          // シェーダーから参照するとき (SRV / UAV)
	std::string shaderName;                    // シェーダー側の名前 (リフレクションとの照合用。省略可)

	bool operator==(const PassResourceRef&) const = default;
};

// 定数バッファ: アプリが名前で渡すもの (source = "frame" など) か、JSON に書いた値 (source = "inline")
struct ConstantRef
{
	std::string        source;
	Binding            bind;
	std::vector<float> values;   // source = "inline" のとき (32bit の値として送る)

	bool operator==(const ConstantRef&) const = default;
};

// 描画リストの問い合わせ (実際の描画は IDrawListProvider が行う)
struct DrawListQuery
{
	std::string              layer;
	std::vector<std::string> tags;   // すべてのタグを持つオブジェクトだけ
	std::string              sort;   // "frontToBack" / "backToFront" / "material" / "" (指定なし)

	bool operator==(const DrawListQuery&) const = default;
};

enum class ContentKind : uint32_t
{
	None,
	DrawList,     // シーンのオブジェクトを描く
	Fullscreen,   // 全画面の三角形を 1 つ描く
	Dispatch,     // Compute を dispatch(x, y, z)
	Custom,       // アプリが名前で登録した処理
};

struct PassContent
{
	ContentKind   kind = ContentKind::None;
	DrawListQuery drawList;
	uint32_t      dispatch[3] = { 1, 1, 1 };
	std::string   custom;

	bool operator==(const PassContent&) const = default;
};

struct PassDesc
{
	std::string                  name;
	PassType                     type = PassType::Raster;
	std::string                  pipeline;      // パイプラインのアセット (プロジェクトからの相対パス)
	std::vector<PassResourceRef> inputs;
	std::vector<PassResourceRef> outputs;
	std::vector<ConstantRef>     constants;
	PassContent                  content;
	int32_t                      order   = 0;       // データの依存がないパスどうしは、小さいほど先
	std::vector<std::string>     after;             // 明示的な依存 (これらのパスのあとに実行する)
	bool                         keep    = false;   // 出力が使われなくても除かない
	bool                         enabled = true;    // false なら、なかったものとしてコンパイルする

	bool operator==(const PassDesc&) const = default;
};

struct GraphDesc
{
	uint32_t                  version = 1;
	std::vector<ResourceDesc> resources;
	std::vector<PassDesc>     passes;

	const ResourceDesc* findResource(const std::string& name) const
	{
		for (const auto& r : resources)
			if (r.name == name)
				return &r;
		return nullptr;
	}
	const PassDesc* findPass(const std::string& name) const
	{
		for (const auto& p : passes)
			if (p.name == name)
				return &p;
		return nullptr;
	}

	bool operator==(const GraphDesc&) const = default;
};

// ---- 名前 (JSON と診断のメッセージで使う) ----
const char* nameOf(ResourceKind v);
const char* nameOf(PassType v);
const char* nameOf(Access v);
const char* nameOf(LoadOp v);
const char* nameOf(ContentKind v);
bool parse(const std::string& s, PassType& out);
bool parse(const std::string& s, Access& out);
bool parse(const std::string& s, LoadOp& out);
} // namespace gfx::rg
