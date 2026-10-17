#pragma once
// ============================================================
//  gfx/rendergraph/GraphDesc.h
//
//  レンダーグラフのデータ (グラフのアセット graphs/*.json と 1 対 1 に対応する)。
//  設計: docs/design/editor-architecture.md の「Phase 3」「M3 の詳細設計」「表せる描画の流れ」
//
//    パラメーター : 品質設定などの値。パスの when (条件) と repeat (繰り返しの数) で使う
//    リソース     : 一時的なテクスチャ (2D / 配列 / キューブ / 3D、mip、MSAA) / バッファ、
//                   履歴 (history: 前フレームの内容を name@prev で読む)、
//                   インポート ($ で始まる名前。実行時にアプリが渡す。$backbuffer など)
//    パス         : 入力 (読む) と出力 (書く) のリソース (mip / スライス / ビューのフォーマットを指定できる)、
//                   パイプラインのアセット、描く内容、ビュー (カメラ)、ビューポート、繰り返し、条件
//
//  パスの宣言順が「意味の順序」になる (後のパスは、前のパスが書いた内容を読む)。
//  コンパイル (Compiler.h) は、データの依存を保ったまま order の小さい順に並べ替え、
//  使われないパスを除き、リソースの寿命とバリアを (サブリソース単位で) 決める。
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
// ---- パラメーター ------------------------------------------

// bool か整数。when ("ssao" / "!ssao") と repeat.count ("cascades") から名前で参照する
struct Parameter
{
	std::string name;
	int64_t     value  = 0;
	bool        isBool = false;

	bool operator==(const Parameter&) const = default;
};

// ---- リソース ----------------------------------------------

enum class ResourceKind : uint32_t
{
	Texture,
	Buffer,
	Imported,   // 実行時にアプリが渡す ($ で始まる名前は宣言しなくてもインポートとして扱う)
};

enum class TextureDimension : uint32_t
{
	Tex2D,   // 2D (arraySize > 1 なら配列)
	Cube,    // キューブ (スライスは 6 * arraySize 枚)
	Tex3D,   // 3D (depth 枚。スライスはサブリソースではない)
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
	std::string      name;
	ResourceKind     kind        = ResourceKind::Texture;
	GfxFormat        format      = GfxFormat::Unknown;    // Texture
	TextureDimension dimension   = TextureDimension::Tex2D;
	SizeSpec         size;                                 // Texture
	uint32_t         arraySize   = 1;                      // Texture (Cube はキューブの数)
	uint32_t         depth       = 1;                      // Tex3D
	uint32_t         mipLevels   = 1;                      // Texture
	uint32_t         sampleCount = 1;                      // Texture
	uint64_t         byteSize    = 0;                      // Buffer
	bool             history     = false;                  // 前フレームの内容を name@prev で読める
	ClearSpec        clear;

	// サブリソースの数 (D3D12 の番号は mip + slice * mipLevels)
	uint32_t layers() const
	{
		if (kind != ResourceKind::Texture) return 1;
		if (dimension == TextureDimension::Cube) return 6 * arraySize;
		if (dimension == TextureDimension::Tex3D) return 1;
		return arraySize;
	}
	uint32_t mips() const { return kind == ResourceKind::Texture ? mipLevels : 1; }

	bool operator==(const ResourceDesc&) const = default;
};

inline bool isImportedName(const std::string& name) { return !name.empty() && name[0] == '$'; }

// "taaColor@prev" → 前フレームの内容。base に "taaColor" を入れて true を返す
inline bool isPrevName(const std::string& name, std::string* base = nullptr)
{
	static const char suffix[] = "@prev";
	const size_t n = sizeof(suffix) - 1;
	if (name.size() <= n || name.compare(name.size() - n, n, suffix) != 0)
		return false;
	if (base) *base = name.substr(0, name.size() - n);
	return true;
}

// ---- パス --------------------------------------------------

enum class PassType : uint32_t
{
	Raster,    // ラスタライズ (RTV / DSV に描く)
	Compute,   // Compute シェーダー (UAV に書く)
	Copy,      // コピー / MSAA の解決 (CopySrc → CopyDst、ResolveSrc → ResolveDst)
	// RayTrace / Subgraph は名前と意味だけを決めてある (設計ドキュメント)。まだ読み込めない
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
	IndirectArgs,   // 間接描画 / dispatch の引数 (入力)
	ResolveSrc,     // MSAA の解決の元 (入力)
	ResolveDst,     // MSAA の解決の先 (出力)
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

// mip / スライスの番号。数値か、repeat の変数を使った式 ("i" / "i+1" / "i-1")
//   value < 0 かつ var が空 = すべて
struct IndexValue
{
	int32_t     value = -1;
	std::string var;
	int32_t     add   = 0;

	bool all()      const { return var.empty() && value < 0; }
	bool usesVar()  const { return !var.empty(); }
	static IndexValue fixed(int32_t v) { IndexValue x; x.value = v; return x; }
	bool operator==(const IndexValue&) const = default;
};

struct PassResourceRef
{
	std::string res;                           // "name" / "name@prev" / "$imported"
	Access      as         = Access::SRV;
	uint32_t    rtvIndex   = 0;               // as = RTV のとき
	LoadOp      load       = LoadOp::Load;    // 出力の RTV / DSV / UAV のとき
	IndexValue  mip;                           // 省略 = すべて
	uint32_t    mipCount   = 1;               // mip を指定したときの数
	IndexValue  slice;                         // 配列 / キューブのスライス。省略 = すべて
	uint32_t    sliceCount = 1;
	GfxFormat   viewFormat = GfxFormat::Unknown;   // ビューのフォーマット (Unknown = リソースのまま)
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

// 間接描画 / dispatch の引数
struct IndirectSpec
{
	std::string args;              // 引数のバッファ (パスの入力に IndirectArgs で書くこと)
	uint64_t    offset   = 0;      // バイト
	std::string count;             // 描画の数を持つバッファ (drawIndirect のみ。省略可)
	uint32_t    maxCount = 1;

	bool operator==(const IndirectSpec&) const = default;
};

enum class ContentKind : uint32_t
{
	None,
	DrawList,           // シーンのオブジェクトを描く
	Fullscreen,         // 全画面の三角形を 1 つ描く
	Dispatch,           // Compute を dispatch(x, y, z)
	DrawIndirect,       // 引数バッファで描く
	DispatchIndirect,   // 引数バッファで dispatch
	Custom,             // アプリが名前で登録した処理
};

struct PassContent
{
	ContentKind   kind = ContentKind::None;
	DrawListQuery drawList;
	uint32_t      dispatch[3] = { 1, 1, 1 };
	IndirectSpec  indirect;
	std::string   custom;

	bool operator==(const PassContent&) const = default;
};

// ビューポート / シザー。既定は出力の大きさに対する比率 (relative = false でピクセル)
struct Region
{
	bool  set      = false;
	bool  relative = true;
	float x = 0, y = 0, width = 1, height = 1;

	bool operator==(const Region&) const = default;
};

// パスを繰り返す (mip ごと、カスケードごとなど)。count か countParam のどちらか
struct RepeatSpec
{
	bool        set        = false;
	uint32_t    count      = 0;
	std::string countParam;        // パラメーターの名前
	std::string var        = "i";  // mip / slice / view の中で使う変数
	int32_t     start      = 0;    // 変数の最初の値

	bool operator==(const RepeatSpec&) const = default;
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
	std::string                  view;          // ビュー (カメラ) の名前。アプリが解決する ("cascade{i}" のように変数を使える)
	Region                       viewport;
	Region                       scissor;
	RepeatSpec                   repeat;
	std::string                  when;          // 条件 (パラメーターの名前。"!name" で否定)
	int32_t                      order   = 0;       // データの依存がないパスどうしは、小さいほど先
	std::vector<std::string>     after;             // 明示的な依存 (これらのパスのあとに実行する)
	bool                         keep    = false;   // 出力が使われなくても除かない
	bool                         enabled = true;    // false なら、なかったものとしてコンパイルする

	bool operator==(const PassDesc&) const = default;
};

struct GraphDesc
{
	uint32_t                  version = 1;
	std::vector<Parameter>    parameters;
	std::vector<ResourceDesc> resources;
	std::vector<PassDesc>     passes;
	// エディターの情報 (ノードの位置など) の JSON (オブジェクト)。エンジンは使わず、読み書きで保つだけ。空ならなし
	std::string               editor;

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
	const Parameter* findParameter(const std::string& name) const
	{
		for (const auto& p : parameters)
			if (p.name == name)
				return &p;
		return nullptr;
	}

	bool operator==(const GraphDesc&) const = default;
};

// ---- 名前 (JSON と診断のメッセージで使う) ----
// ※ doctest などが値の文字列化に toString を探すので、別の名前にしている
const char* nameOf(ResourceKind v);
const char* nameOf(TextureDimension v);
const char* nameOf(PassType v);
const char* nameOf(Access v);
const char* nameOf(LoadOp v);
const char* nameOf(ContentKind v);
bool parse(const std::string& s, PassType& out);
bool parse(const std::string& s, Access& out);
bool parse(const std::string& s, LoadOp& out);
bool parse(const std::string& s, TextureDimension& out);

// "i" / "i+1" / "i-1" / "3" の形の式 (JSON の文字列) を読む / 書く
bool        parseIndexExpr(const std::string& s, IndexValue& out);
std::string formatIndexExpr(const IndexValue& v);
} // namespace gfx::rg
