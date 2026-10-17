#pragma once
// ============================================================
//  gfx/rendergraph/Compiler.h
//
//  グラフ (GraphDesc) のコンパイル。GPU を使わない (単体テストできる)。
//
//    1. 検証      : 名前、リソースの宣言、使い方とパスの種類、フォーマット、同じパスでの読み書き、bind
//    2. 依存      : パスの宣言順で、各リソースを誰が書き誰が読むかをたどり、
//                   書いた → 読む (RAW)、読む → 書く (WAR)、書く → 書く (WAW) の依存を作る。after も依存に加える
//    3. カリング  : インポート ($ で始まる名前) に書くパスと keep のパスから、RAW の依存をさかのぼって
//                   必要なパスを決める。それ以外は除く (culled。エディターでは灰色で表示する)
//    4. 順序      : 依存を保ったまま、order の小さい順 (同じなら宣言順) に並べる (トポロジカルソート)
//    5. 寿命      : 一時リソースを最初と最後に使うパス
//    6. バリア    : パスの前に必要な状態遷移。一時リソースは最初に使う状態で作るので、最初はバリアなし。
//                   インポートは最初に before = Tracked (エンジンの状態追跡の値) からの遷移を出す。
//                   UAV に続けて書くときは UAV バリアを出す
//
//  bind の検証: options.bindingLayout にパイプライン → バインディングレイアウトを引く関数を渡すと、
//  パラメーターの番号、種類 (SRV / UAV / CBV)、テーブルの中の位置まで確かめる。渡さなければ、
//  bind があること、同じパスで重なっていないことだけを確かめる。
// ============================================================

#include "gfx/rendergraph/GraphDesc.h"

#include <functional>
#include <string>
#include <vector>

namespace gfx::rg
{
struct Diagnostic
{
	enum class Severity { Error, Warning };
	Severity    severity = Severity::Error;
	std::string path;      // "passes[Lighting].inputs[0].bind" など
	std::string message;

	std::string text() const { return path.empty() ? message : path + ": " + message; }
};

// パスの前に記録するバリア
struct BarrierOp
{
	uint32_t         resource = 0;   // CompiledGraph::resources の位置
	GfxBarrierType   type     = GfxBarrierType::Transition;
	GfxResourceState before   = GfxResourceState::Common;   // Tracked = エンジンの状態追跡の値 (インポートの最初)
	GfxResourceState after    = GfxResourceState::Common;
};

struct CompiledPass
{
	uint32_t               passIndex = 0;   // GraphDesc::passes の位置
	std::string            name;
	std::vector<BarrierOp> barriers;        // このパスの前に記録する
};

struct ResourceInfo
{
	std::string      name;
	ResourceKind     kind      = ResourceKind::Texture;
	int32_t          descIndex = -1;    // GraphDesc::resources の位置 (宣言していないインポートは -1)
	int32_t          firstPass = -1;    // 最初に使う CompiledGraph::passes の位置 (使わなければ -1)
	int32_t          lastPass  = -1;
	GfxResourceState initialState = GfxResourceState::Common;   // 一時リソースを作るときの状態 (最初に使う状態)
	GfxResourceState finalState   = GfxResourceState::Common;   // 最後のパスのあとの状態
	bool             written      = false;

	bool used() const { return firstPass >= 0; }
	bool imported() const { return kind == ResourceKind::Imported; }
};

struct CompiledGraph
{
	bool                      ok = false;   // エラーがなければ true (警告はあってよい)
	std::vector<CompiledPass> passes;       // 実行する順
	std::vector<uint32_t>     culled;       // 除いたパス (GraphDesc::passes の位置)
	std::vector<uint32_t>     disabled;     // enabled = false のパス
	std::vector<ResourceInfo> resources;    // 宣言したリソース + 参照されたインポート
	std::vector<Diagnostic>   diagnostics;

	std::vector<Diagnostic> errors() const;
	std::vector<Diagnostic> warnings() const;
	const ResourceInfo*     findResource(const std::string& name) const;
	int32_t                 indexOfPass(const std::string& name) const;   // passes の位置 (なければ -1)
	bool                    isCulled(uint32_t passIndex) const;
};

struct CompileOptions
{
	// パイプラインのアセット → バインディングレイアウト。nullptr を返したら、そのパスの bind は詳しく確かめない
	std::function<const GfxBindingLayoutDesc*(const std::string& pipeline)> bindingLayout;
};

CompiledGraph compile(const GraphDesc& graph, const CompileOptions& options = {});

// パスの種類と使い方から、必要なリソースの状態を返す
GfxResourceState requiredState(PassType type, Access access);
bool             isDepthFormat(GfxFormat format);
} // namespace gfx::rg
