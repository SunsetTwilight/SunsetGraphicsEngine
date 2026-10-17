// ============================================================
//  レンダーグラフ (M3a) のテスト: JSON との相互変換とコンパイル。GPU を使わない
// ============================================================
#include <doctest/doctest.h>
#include "gfx/rendergraph/RenderGraph.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace gfx::rg;

namespace
{
// 設計ドキュメントのディファードの例 (G-Buffer → ライティング)
const char* k_Deferred = R"json({
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
})json";

GraphDesc load(const char* text)
{
	GraphDesc g;
	GfxJsonDiagnostics diag;
	const bool ok = graphFromString(text, g, &diag);
	for (const auto& e : diag.errors) MESSAGE("json error: ", e);
	for (const auto& w : diag.warnings) MESSAGE("json warning: ", w);
	REQUIRE(ok);
	return g;
}

bool hasDiag(const CompiledGraph& c, Diagnostic::Severity sev, const std::string& pathPrefix, const std::string& fragment = {})
{
	for (const auto& d : c.diagnostics)
		if (d.severity == sev && d.path.rfind(pathPrefix, 0) == 0 && (fragment.empty() || d.message.find(fragment) != std::string::npos))
			return true;
	return false;
}

void dump(const CompiledGraph& c)
{
	for (const auto& d : c.diagnostics)
	{
		const std::string line = std::string(d.severity == Diagnostic::Severity::Error ? "error: " : "warning: ") + d.text();
		MESSAGE(line);
	}
}

std::vector<std::string> order(const CompiledGraph& c)
{
	std::vector<std::string> out;
	for (const auto& p : c.passes) out.push_back(p.name);
	return out;
}

const BarrierOp* findBarrier(const CompiledGraph& c, const std::string& pass, const std::string& res)
{
	const int32_t pi = c.indexOfPass(pass);
	if (pi < 0) return nullptr;
	for (const auto& b : c.passes[static_cast<size_t>(pi)].barriers)
		if (c.resources[b.resource].name == res)
			return &b;
	return nullptr;
}

// 小さなグラフを組み立てる
PassResourceRef ref(const char* res, Access as, uint32_t rtv = 0, LoadOp load = LoadOp::Load, Binding bind = {})
{
	PassResourceRef r;
	r.res = res; r.as = as; r.rtvIndex = rtv; r.load = load; r.bind = bind;
	return r;
}
ResourceDesc tex(const char* name, GfxFormat fmt)
{
	ResourceDesc r;
	r.name = name; r.format = fmt;
	return r;
}
PassDesc raster(const char* name, std::vector<PassResourceRef> in, std::vector<PassResourceRef> out)
{
	PassDesc p;
	p.name = name; p.type = PassType::Raster; p.pipeline = std::string("pipelines/") + name + ".json";
	p.inputs = std::move(in); p.outputs = std::move(out);
	p.content.kind = ContentKind::Fullscreen;
	return p;
}
} // namespace

TEST_CASE("レンダーグラフ: JSON を読み、書き出して読み直すと同じになる")
{
	const GraphDesc g = load(k_Deferred);
	REQUIRE(g.resources.size() == 3);
	REQUIRE(g.passes.size() == 2);
	CHECK(g.resources[1].format == GfxFormat::R16G16B16A16_Float);
	CHECK(g.resources[2].clear.hasDepth);
	CHECK(g.passes[0].outputs[1].as == Access::RTV);
	CHECK(g.passes[0].outputs[1].rtvIndex == 1);
	CHECK(g.passes[0].outputs[2].load == LoadOp::Clear);
	CHECK(g.passes[0].content.kind == ContentKind::DrawList);
	CHECK(g.passes[0].content.drawList.sort == "frontToBack");
	CHECK(g.passes[1].inputs[2].bind == Binding{ 1, 2 });
	CHECK(g.passes[1].inputs[2].shaderName == "gDepth");
	CHECK(g.passes[1].constants[0].bind.param == 0);

	const std::string text = graphToString(g);
	GraphDesc again;
	REQUIRE(graphFromString(text, again));
	CHECK(again == g);
	CHECK(graphToString(again) == text);   // 書き出しは安定している
}

TEST_CASE("レンダーグラフ: JSON の型の違いはエラー、知らないキーは警告 (パス付き)")
{
	const char* bad = R"json({
	  "resources": { "a": { "format": "NoSuchFormat" } },
	  "passes": [ { "name": "P", "type": "Rasterize", "future": 1,
	                "outputs": [ { "res": "a", "as": "RTV9" } ],
	                "inputs": [ { "res": "a", "as": "SRV", "bind": { "param": "zero" } } ] } ]
	})json";
	GraphDesc g;
	GfxJsonDiagnostics diag;
	CHECK_FALSE(graphFromString(bad, g, &diag));
	auto has = [&](const std::vector<std::string>& v, const char* prefix) {
		return std::any_of(v.begin(), v.end(), [&](const std::string& s) { return s.rfind(prefix, 0) == 0; });
	};
	CHECK(has(diag.errors, "resources.a.format"));
	CHECK(has(diag.errors, "passes[P].type"));
	CHECK(has(diag.errors, "passes[P].outputs[0].as"));
	CHECK(has(diag.errors, "passes[P].inputs[0].bind.param"));
	CHECK(has(diag.warnings, "passes[P].future"));

	GraphDesc g2;
	GfxJsonDiagnostics d2;
	CHECK_FALSE(graphFromString("{ not json", g2, &d2));
	REQUIRE(d2.errors.size() == 1);
	CHECK(d2.errors[0].rfind("JSON:", 0) == 0);
}

TEST_CASE("レンダーグラフ: ディファードをコンパイルする (順序、寿命、バリア)")
{
	const CompiledGraph c = compile(load(k_Deferred));
	dump(c);
	REQUIRE(c.ok);
	CHECK(order(c) == std::vector<std::string>{ "GBuffer", "Lighting" });
	CHECK(c.skipped.empty());

	// 一時リソースは最初に使う状態で作る (最初はバリアなし)
	const ResourceInfo* albedo = c.findResource("gbAlbedo");
	REQUIRE(albedo != nullptr);
	CHECK(albedo->initialState == GfxResourceState::RenderTarget);
	CHECK(albedo->firstPass == 0);
	CHECK(albedo->lastPass == 1);
	CHECK(findBarrier(c, "GBuffer", "gbAlbedo") == nullptr);
	CHECK(c.findResource("depth")->initialState == GfxResourceState::DepthWrite);

	// ライティングの前に、書いた G-Buffer をシェーダーで読む状態にする
	const BarrierOp* b = findBarrier(c, "Lighting", "gbAlbedo");
	REQUIRE(b != nullptr);
	CHECK(b->before == GfxResourceState::RenderTarget);
	CHECK(b->after == GfxResourceState::AllShaderResource);
	const BarrierOp* d = findBarrier(c, "Lighting", "depth");
	REQUIRE(d != nullptr);
	CHECK(d->before == GfxResourceState::DepthWrite);

	// インポートは、エンジンの状態追跡の値 (Tracked) から遷移する
	const BarrierOp* bb = findBarrier(c, "Lighting", "$backbuffer");
	REQUIRE(bb != nullptr);
	CHECK(bb->before == GfxResourceState::Tracked);
	CHECK(bb->after == GfxResourceState::RenderTarget);
	CHECK(c.findResource("$backbuffer")->imported());
	CHECK(c.findResource("$backbuffer")->finalState == GfxResourceState::RenderTarget);
}

TEST_CASE("レンダーグラフ: 結果に届かないパスを除く (keep で残せる)")
{
	GraphDesc g = load(k_Deferred);
	g.resources.push_back(tex("debugTex", GfxFormat::R8G8B8A8_Unorm));
	PassDesc dbg = raster("Debug", { ref("gbNormal", Access::SRV, 0, LoadOp::Load, { 1, 0 }) },
	                               { ref("debugTex", Access::RTV, 0, LoadOp::Clear) });
	g.passes.push_back(dbg);

	CompiledGraph c = compile(g);
	REQUIRE(c.ok);
	CHECK(order(c) == std::vector<std::string>{ "GBuffer", "Lighting" });
	CHECK(c.skippedNames(SkippedPass::Reason::Culled) == std::vector<std::string>{ "Debug" });
	CHECK_FALSE(c.findResource("debugTex")->used());
	CHECK(hasDiag(c, Diagnostic::Severity::Warning, "resources.debugTex"));

	g.passes.back().keep = true;
	c = compile(g);
	REQUIRE(c.ok);
	CHECK(c.skipped.empty());
	CHECK(c.indexOfPass("Debug") >= 0);

	// 無効にしたパスはなかったものとして扱う
	g.passes.back().enabled = false;
	c = compile(g);
	REQUIRE(c.ok);
	CHECK(c.skippedNames(SkippedPass::Reason::Disabled) == std::vector<std::string>{ "Debug" });
	CHECK(c.indexOfPass("Debug") < 0);
}

TEST_CASE("レンダーグラフ: データの依存がないパスは order の順、after で明示的に依存させる")
{
	GraphDesc g;
	g.resources = { tex("a", GfxFormat::R8G8B8A8_Unorm), tex("b", GfxFormat::R8G8B8A8_Unorm) };
	g.passes.push_back(raster("WriteA", {}, { ref("a", Access::RTV, 0, LoadOp::Clear) }));
	g.passes.push_back(raster("WriteB", {}, { ref("b", Access::RTV, 0, LoadOp::Clear) }));
	g.passes.push_back(raster("Combine", { ref("a", Access::SRV, 0, LoadOp::Load, { 0, 0 }), ref("b", Access::SRV, 0, LoadOp::Load, { 0, 1 }) },
	                                     { ref("$backbuffer", Access::RTV, 0, LoadOp::DontCare) }));

	CompiledGraph c = compile(g);
	REQUIRE(c.ok);
	CHECK(order(c) == std::vector<std::string>{ "WriteA", "WriteB", "Combine" });

	g.passes[1].order = -1;   // B を先に
	c = compile(g);
	REQUIRE(c.ok);
	CHECK(order(c) == std::vector<std::string>{ "WriteB", "WriteA", "Combine" });

	g.passes[1].after = { "WriteA" };   // order より after (依存) が優先
	c = compile(g);
	REQUIRE(c.ok);
	CHECK(order(c) == std::vector<std::string>{ "WriteA", "WriteB", "Combine" });

	g.passes[0].after = { "WriteB" };   // 循環
	c = compile(g);
	CHECK_FALSE(c.ok);
	CHECK(hasDiag(c, Diagnostic::Severity::Error, "passes", "循環"));

	g.passes[0].after = { "NoSuchPass" };
	c = compile(g);
	CHECK_FALSE(c.ok);
	CHECK(hasDiag(c, Diagnostic::Severity::Error, "passes[WriteA].after[0]"));
}

TEST_CASE("レンダーグラフ: 読んだあとに上書きするパス (WAR) と、続けて書くパス (WAW) の順序を保つ")
{
	GraphDesc g;
	g.resources = { tex("a", GfxFormat::R8G8B8A8_Unorm) };
	g.passes.push_back(raster("First", {}, { ref("a", Access::RTV, 0, LoadOp::Clear) }));
	g.passes.push_back(raster("Read", { ref("a", Access::SRV, 0, LoadOp::Load, { 0, 0 }) }, { ref("$out1", Access::RTV) }));
	g.passes.push_back(raster("Overwrite", {}, { ref("a", Access::RTV, 0, LoadOp::Load) }));   // load なので First の結果に重ねる
	g.passes.push_back(raster("Final", { ref("a", Access::SRV, 0, LoadOp::Load, { 0, 0 }) }, { ref("$out2", Access::RTV) }));
	g.passes[2].order = -10;   // 前に出したくても、Read が読み終わるまで待つ

	const CompiledGraph c = compile(g);
	dump(c);
	REQUIRE(c.ok);
	CHECK(order(c) == std::vector<std::string>{ "First", "Read", "Overwrite", "Final" });
	CHECK(findBarrier(c, "Overwrite", "a")->after == GfxResourceState::RenderTarget);
	CHECK(findBarrier(c, "Final", "a")->after == GfxResourceState::AllShaderResource);
}

TEST_CASE("レンダーグラフ: Compute で UAV に続けて書くと UAV バリアを出す")
{
	GraphDesc g;
	ResourceDesc buf;
	buf.name = "particles"; buf.kind = ResourceKind::Buffer; buf.byteSize = 4096;
	g.resources = { buf, tex("image", GfxFormat::R16G16B16A16_Float) };

	PassDesc sim;
	sim.name = "Simulate"; sim.type = PassType::Compute; sim.pipeline = "pipelines/sim.json";
	sim.content.kind = ContentKind::Dispatch;
	sim.outputs = { ref("particles", Access::UAV, 0, LoadOp::DontCare, { 0, 0 }) };
	PassDesc integrate = sim;
	integrate.name = "Integrate";
	integrate.inputs  = { ref("particles", Access::UAV, 0, LoadOp::Load, { 0, 0 }) };
	integrate.outputs = { ref("particles", Access::UAV, 0, LoadOp::Load, { 0, 0 }) };   // 同じ UAV を読み書き
	PassDesc draw = raster("Draw", { ref("particles", Access::SRV, 0, LoadOp::Load, { 0, 0 }) }, { ref("$backbuffer", Access::RTV) });
	g.passes = { sim, integrate, draw };

	const CompiledGraph c = compile(g);
	dump(c);
	REQUIRE(c.ok);
	CHECK(order(c) == std::vector<std::string>{ "Simulate", "Integrate", "Draw" });
	CHECK(c.findResource("particles")->initialState == GfxResourceState::UnorderedAccess);
	const BarrierOp* uav = findBarrier(c, "Integrate", "particles");
	REQUIRE(uav != nullptr);
	CHECK(uav->type == GfxBarrierType::UAV);
	const BarrierOp* toSrv = findBarrier(c, "Draw", "particles");
	REQUIRE(toSrv != nullptr);
	CHECK(toSrv->type == GfxBarrierType::Transition);
	CHECK(toSrv->after == GfxResourceState::AllShaderResource);
	CHECK(hasDiag(c, Diagnostic::Severity::Warning, "resources.image"));   // 使っていない

	// 別のリソースの UAV を同じ場所につなぐのはエラー
	g.passes[1].outputs.push_back(ref("image", Access::UAV, 0, LoadOp::DontCare, { 0, 0 }));
	const CompiledGraph c2 = compile(g);
	CHECK_FALSE(c2.ok);
	CHECK(hasDiag(c2, Diagnostic::Severity::Error, "passes[Integrate].outputs[1].bind", "重なって"));
}

TEST_CASE("レンダーグラフ: 検証のエラー")
{
	SUBCASE("宣言していないリソース / 同じ名前のパス")
	{
		GraphDesc g;
		g.passes.push_back(raster("P", { ref("missing", Access::SRV, 0, LoadOp::Load, { 0, 0 }) }, { ref("$out", Access::RTV) }));
		g.passes.push_back(raster("P", {}, { ref("$out", Access::RTV) }));
		const CompiledGraph c = compile(g);
		CHECK_FALSE(c.ok);
		CHECK(hasDiag(c, Diagnostic::Severity::Error, "passes[P].inputs[0].res", "宣言されていません"));
		CHECK(hasDiag(c, Diagnostic::Severity::Error, "passes[P]", "同じ名前"));
	}
	SUBCASE("フォーマットと使い方")
	{
		GraphDesc g;
		g.resources = { tex("color", GfxFormat::R8G8B8A8_Unorm), tex("depth", GfxFormat::D32_Float) };
		g.passes.push_back(raster("P", {}, { ref("depth", Access::RTV), ref("color", Access::DSV), ref("$out", Access::RTV, 0) }));
		const CompiledGraph c = compile(g);
		CHECK_FALSE(c.ok);
		CHECK(hasDiag(c, Diagnostic::Severity::Error, "passes[P].outputs[0].as", "深度"));
		CHECK(hasDiag(c, Diagnostic::Severity::Error, "passes[P].outputs[1].as", "DSV"));
		CHECK(hasDiag(c, Diagnostic::Severity::Error, "passes[P].outputs[2].as", "重なって"));
	}
	SUBCASE("同じパスで読みながら書く")
	{
		GraphDesc g;
		g.resources = { tex("a", GfxFormat::R8G8B8A8_Unorm) };
		g.passes.push_back(raster("Init", {}, { ref("a", Access::RTV, 0, LoadOp::Clear) }));
		g.passes.push_back(raster("P", { ref("a", Access::SRV, 0, LoadOp::Load, { 0, 0 }) }, { ref("a", Access::RTV) }));
		const CompiledGraph c = compile(g);
		CHECK_FALSE(c.ok);
		CHECK(hasDiag(c, Diagnostic::Severity::Error, "passes[P].outputs[0]", "読みながら書く"));
	}
	SUBCASE("書いていないリソースを読む / bind がない / bind が重なる")
	{
		GraphDesc g;
		g.resources = { tex("a", GfxFormat::R8G8B8A8_Unorm), tex("b", GfxFormat::R8G8B8A8_Unorm) };
		g.passes.push_back(raster("P", { ref("a", Access::SRV), ref("b", Access::SRV, 0, LoadOp::Load, { 2, 0 }) },
		                               { ref("$out", Access::RTV) }));
		ConstantRef frame;
		frame.source = "frame";
		frame.bind   = { 2, 0 };
		g.passes[0].constants.push_back(frame);
		const CompiledGraph c = compile(g);
		CHECK_FALSE(c.ok);
		CHECK(hasDiag(c, Diagnostic::Severity::Error, "passes[P].inputs[0].bind", "bind がありません"));
		CHECK(hasDiag(c, Diagnostic::Severity::Error, "passes[P].inputs[1]", "どのパスも書いていない"));
		CHECK(hasDiag(c, Diagnostic::Severity::Error, "passes[P].constants[0].bind", "重なって"));
	}
	SUBCASE("まだ書かれていない内容を load するのは警告")
	{
		GraphDesc g;
		g.resources = { tex("a", GfxFormat::R8G8B8A8_Unorm) };
		g.passes.push_back(raster("P", {}, { ref("a", Access::RTV, 0, LoadOp::Load) }));
		g.passes.push_back(raster("Q", { ref("a", Access::SRV, 0, LoadOp::Load, { 0, 0 }) }, { ref("$out", Access::RTV) }));
		const CompiledGraph c = compile(g);
		CHECK(c.ok);
		CHECK(hasDiag(c, Diagnostic::Severity::Warning, "passes[P].outputs[0].load", "不定"));
	}
}

TEST_CASE("レンダーグラフ: bind をバインディングレイアウトと照合する")
{
	// [0] ルート CBV b0、[1] テーブル { SRV t0〜t2, Sampler は別テーブルなので UAV u0 }、[2] ルート定数 4 個
	GfxBindingLayoutDesc layout;
	layout.addRootDescriptor(GfxRootParameterType::CBV, 0);
	layout.beginTable(GfxShaderVisibility::Pixel)
	      .addRange(GfxDescriptorRangeType::SRV, 3, 0)
	      .addRange(GfxDescriptorRangeType::UAV, 1, 0);
	layout.addConstants(1, 4);

	CompileOptions opt;
	opt.bindingLayout = [&](const std::string& pipeline) -> const GfxBindingLayoutDesc* {
		return pipeline == "pipelines/deferred_light.json" || pipeline == "pipelines/gbuffer.json" ? &layout : nullptr;
	};

	SUBCASE("正しいつなぎ方")
	{
		const CompiledGraph c = compile(load(k_Deferred), opt);
		dump(c);
		CHECK(c.ok);
		CHECK(c.warnings().empty());
	}
	SUBCASE("範囲外 / 種類の違い / ルート定数")
	{
		GraphDesc g = load(k_Deferred);
		PassDesc& light = g.passes[1];
		light.inputs[0].bind = { 1, 3 };   // テーブルの 3 番目は UAV
		light.inputs[1].bind = { 1, 9 };   // テーブルの範囲外
		light.inputs[2].bind = { 7, 0 };   // パラメーターがない
		ConstantRef inlineValues;
		inlineValues.source = "inline";
		inlineValues.bind   = { 2, 2 };
		inlineValues.values = { 1, 2, 3 };   // 2 + 3 > 4
		light.constants.push_back(inlineValues);
		ConstantRef frameToConstants;
		frameToConstants.source = "camera";
		frameToConstants.bind   = { 2, 0 };
		light.constants.push_back(frameToConstants);

		const CompiledGraph c = compile(g, opt);
		CHECK_FALSE(c.ok);
		CHECK(hasDiag(c, Diagnostic::Severity::Error, "passes[Lighting].inputs[0].bind", "UAV です"));
		CHECK(hasDiag(c, Diagnostic::Severity::Error, "passes[Lighting].inputs[1].bind", "範囲外"));
		CHECK(hasDiag(c, Diagnostic::Severity::Error, "passes[Lighting].inputs[2].bind", "ありません"));
		CHECK(hasDiag(c, Diagnostic::Severity::Error, "passes[Lighting].constants[1].bind", "入りません"));
		CHECK(hasDiag(c, Diagnostic::Severity::Error, "passes[Lighting].constants[2].bind", "inline の値だけ"));
	}
	SUBCASE("レイアウトが分からないパイプラインは警告だけ")
	{
		GraphDesc g = load(k_Deferred);
		g.passes[1].pipeline = "pipelines/unknown.json";
		const CompiledGraph c = compile(g, opt);
		CHECK(c.ok);
		CHECK(hasDiag(c, Diagnostic::Severity::Warning, "passes[Lighting].pipeline", "確かめていません"));
	}
}

// ============================================================
//  いろいろな描画の流れ (設計ドキュメントの「表せる描画の流れ」)
// ============================================================
namespace
{
// パスの前のバリアのうち、リソースに対するものをすべて返す
std::vector<BarrierOp> barriersOf(const CompiledGraph& c, const std::string& pass, const std::string& res)
{
	std::vector<BarrierOp> out;
	const int32_t pi = c.indexOfPass(pass);
	if (pi < 0) return out;
	for (const auto& b : c.passes[static_cast<size_t>(pi)].barriers)
		if (c.resources[b.resource].name == res)
			out.push_back(b);
	return out;
}

// JSON に書き出して読み直しても同じになる
void checkRoundTrip(const GraphDesc& g)
{
	const std::string text = graphToString(g);
	GraphDesc again;
	GfxJsonDiagnostics diag;
	const bool ok = graphFromString(text, again, &diag);
	for (const auto& e : diag.errors) MESSAGE("json error: ", e);
	REQUIRE(ok);
	CHECK(again == g);
	CHECK(graphToString(again) == text);
}

const char* k_Cascades = R"json({
  "parameters": { "cascades": 4 },
  "resources": {
    "shadowMap": { "format": "D32_Float", "size": { "width": 2048, "height": 2048 }, "array": 4, "clear": { "depth": 1.0 } }
  },
  "passes": [
    { "name": "Shadow", "type": "Raster", "pipeline": "pipelines/shadow.json",
      "repeat": { "count": "cascades", "var": "i" }, "view": "cascade{i}",
      "outputs": [ { "res": "shadowMap", "as": "DSV", "slice": "i", "load": "clear" } ],
      "content": { "drawList": { "layer": "ShadowCaster" } } },
    { "name": "Lighting", "type": "Raster", "pipeline": "pipelines/light.json", "after": [ "Shadow" ],
      "inputs":  [ { "res": "shadowMap", "as": "SRV", "bind": { "param": 1 } } ],
      "outputs": [ { "res": "$backbuffer", "as": "RTV0", "load": "dontCare" } ],
      "content": { "fullscreen": true } }
  ]
})json";

const char* k_Bloom = R"json({
  "resources": {
    "bloom": { "format": "R11G11B10_Float", "size": { "relative": 0.5 }, "mips": 5 }
  },
  "passes": [
    { "name": "Prefilter", "type": "Raster", "pipeline": "pipelines/bloom_prefilter.json",
      "inputs":  [ { "res": "$scene", "as": "SRV", "bind": { "param": 1 } } ],
      "outputs": [ { "res": "bloom", "as": "RTV0", "mip": 0, "load": "dontCare" } ],
      "content": { "fullscreen": true } },
    { "name": "BloomDown", "type": "Raster", "pipeline": "pipelines/bloom_down.json",
      "repeat": { "count": 4, "var": "i", "start": 1 },
      "inputs":  [ { "res": "bloom", "as": "SRV", "mip": "i-1", "bind": { "param": 1 } } ],
      "outputs": [ { "res": "bloom", "as": "RTV0", "mip": "i", "load": "dontCare" } ],
      "content": { "fullscreen": true } },
    { "name": "Composite", "type": "Raster", "pipeline": "pipelines/composite.json",
      "inputs":  [ { "res": "$scene", "as": "SRV", "bind": { "param": 1, "offset": 0 } },
                   { "res": "bloom",  "as": "SRV", "bind": { "param": 1, "offset": 1 } } ],
      "outputs": [ { "res": "$backbuffer", "as": "RTV0", "load": "dontCare" } ],
      "content": { "fullscreen": true } }
  ]
})json";

const char* k_Taa = R"json({
  "resources": {
    "taaColor": { "format": "R16G16B16A16_Float", "size": { "relative": 1.0 }, "history": true }
  },
  "passes": [
    { "name": "TAA", "type": "Compute", "pipeline": "pipelines/taa.json",
      "inputs":  [ { "res": "$scene",        "as": "SRV", "bind": { "param": 1, "offset": 0 } },
                   { "res": "taaColor@prev", "as": "SRV", "bind": { "param": 1, "offset": 1 } } ],
      "outputs": [ { "res": "taaColor", "as": "UAV", "load": "dontCare", "bind": { "param": 2 } } ],
      "content": { "dispatch": [ 120, 68, 1 ] } },
    { "name": "Present", "type": "Raster", "pipeline": "pipelines/present.json",
      "inputs":  [ { "res": "taaColor", "as": "SRV", "bind": { "param": 1 } } ],
      "outputs": [ { "res": "$backbuffer", "as": "RTV0", "load": "dontCare" } ],
      "content": { "fullscreen": true } }
  ]
})json";

const char* k_ForwardPlus = R"json({
  "resources": {
    "depth":     { "format": "D32_Float", "size": { "relative": 1.0 } },
    "lightGrid": { "kind": "buffer", "bytes": 65536 }
  },
  "passes": [
    { "name": "DepthPrepass", "type": "Raster", "pipeline": "pipelines/depth.json",
      "outputs": [ { "res": "depth", "as": "DSV", "load": "clear" } ],
      "content": { "drawList": { "layer": "Opaque", "sort": "frontToBack" } } },
    { "name": "LightCull", "type": "Compute", "pipeline": "pipelines/light_cull.json",
      "inputs":  [ { "res": "depth", "as": "SRV", "bind": { "param": 1 } } ],
      "outputs": [ { "res": "lightGrid", "as": "UAV", "load": "dontCare", "bind": { "param": 2 } } ],
      "content": { "dispatch": [ 80, 45, 1 ] } },
    { "name": "Forward", "type": "Raster", "pipeline": "pipelines/forward.json",
      "inputs":  [ { "res": "lightGrid", "as": "SRV", "bind": { "param": 1 } },
                   { "res": "depth",     "as": "DSVRead" } ],
      "outputs": [ { "res": "$backbuffer", "as": "RTV0", "load": "clear" } ],
      "content": { "drawList": { "layer": "Opaque" } } }
  ]
})json";

const char* k_Msaa = R"json({
  "resources": {
    "sceneMS":    { "format": "R16G16B16A16_Float", "size": { "relative": 1.0 }, "samples": 4 },
    "depthMS":    { "format": "D32_Float",          "size": { "relative": 1.0 }, "samples": 4 },
    "sceneColor": { "format": "R16G16B16A16_Float", "size": { "relative": 1.0 } }
  },
  "passes": [
    { "name": "Opaque", "type": "Raster", "pipeline": "pipelines/opaque.json",
      "outputs": [ { "res": "sceneMS", "as": "RTV0", "load": "clear" },
                   { "res": "depthMS", "as": "DSV",  "load": "clear" } ],
      "content": { "drawList": { "layer": "Opaque" } } },
    { "name": "Resolve", "type": "Copy",
      "inputs":  [ { "res": "sceneMS",    "as": "ResolveSrc" } ],
      "outputs": [ { "res": "sceneColor", "as": "ResolveDst" } ] },
    { "name": "Tonemap", "type": "Raster", "pipeline": "pipelines/tonemap.json",
      "inputs":  [ { "res": "sceneColor", "as": "SRV", "bind": { "param": 1 } } ],
      "outputs": [ { "res": "$backbuffer", "as": "RTV0", "load": "dontCare" } ],
      "content": { "fullscreen": true } }
  ]
})json";

const char* k_Indirect = R"json({
  "resources": {
    "dispatchArgs": { "kind": "buffer", "bytes": 64 }
  },
  "passes": [
    { "name": "BuildArgs", "type": "Compute", "pipeline": "pipelines/build_args.json",
      "outputs": [ { "res": "dispatchArgs", "as": "UAV", "load": "dontCare", "bind": { "param": 2 } } ],
      "content": { "dispatch": [ 1, 1, 1 ] } },
    { "name": "Cull", "type": "Compute", "pipeline": "pipelines/cull.json",
      "inputs":  [ { "res": "dispatchArgs", "as": "IndirectArgs" } ],
      "outputs": [ { "res": "$visible", "as": "UAV", "load": "dontCare", "bind": { "param": 2 } } ],
      "content": { "dispatchIndirect": { "args": "dispatchArgs", "offset": 0 } } }
  ]
})json";

const char* k_Conditional = R"json({
  "parameters": { "ssao": true, "blurPasses": 2 },
  "resources": {
    "ao": { "format": "R8_Unorm", "size": { "relative": 0.5 } }
  },
  "passes": [
    { "name": "SSAO", "type": "Compute", "pipeline": "pipelines/ssao.json", "when": "ssao",
      "inputs":  [ { "res": "$depth", "as": "SRV", "bind": { "param": 1 } } ],
      "outputs": [ { "res": "ao", "as": "UAV", "load": "dontCare", "bind": { "param": 2 } } ],
      "content": { "dispatch": [ 60, 34, 1 ] } },
    { "name": "Blur", "type": "Compute", "pipeline": "pipelines/blur.json", "when": "ssao",
      "repeat": { "count": "blurPasses" },
      "inputs":  [ { "res": "ao", "as": "UAV", "bind": { "param": 2 } } ],
      "outputs": [ { "res": "ao", "as": "UAV", "bind": { "param": 2 } } ],
      "content": { "dispatch": [ 60, 34, 1 ] } },
    { "name": "Composite", "type": "Raster", "pipeline": "pipelines/composite_ao.json", "when": "ssao",
      "inputs":  [ { "res": "ao", "as": "SRV", "bind": { "param": 1 } } ],
      "outputs": [ { "res": "$backbuffer", "as": "RTV0", "load": "dontCare" } ],
      "content": { "fullscreen": true } },
    { "name": "CompositeNoAO", "type": "Raster", "pipeline": "pipelines/composite.json", "when": "!ssao",
      "outputs": [ { "res": "$backbuffer", "as": "RTV0", "load": "dontCare" } ],
      "content": { "fullscreen": true } }
  ]
})json";
} // namespace

TEST_CASE("レンダーグラフ: カスケードシャドウ (配列のスライスごとに repeat)")
{
	const GraphDesc g = load(k_Cascades);
	checkRoundTrip(g);
	const CompiledGraph c = compile(g);
	dump(c);
	REQUIRE(c.ok);
	CHECK(order(c) == std::vector<std::string>{ "Shadow[0]", "Shadow[1]", "Shadow[2]", "Shadow[3]", "Lighting" });

	const CompiledPass& s2 = c.passes[2];
	CHECK(s2.iteration == 2);
	CHECK(s2.passIndex == 0);
	CHECK(s2.desc.view == "cascade2");
	CHECK(s2.desc.outputs[0].slice.value == 2);
	CHECK_FALSE(s2.desc.repeat.set);

	const ResourceInfo* sm = c.findResource("shadowMap");
	REQUIRE(sm != nullptr);
	CHECK(sm->layers == 4);
	CHECK(sm->initialState == GfxResourceState::DepthWrite);
	CHECK(barriersOf(c, "Shadow[1]", "shadowMap").empty());   // 作ったときから DepthWrite

	// すべてのスライスが同じ状態なので、1 つのバリアにまとめる
	const auto lb = barriersOf(c, "Lighting", "shadowMap");
	REQUIRE(lb.size() == 1);
	CHECK(lb[0].subresource == GFX_ALL_SUBRESOURCES);
	CHECK(lb[0].before == GfxResourceState::DepthWrite);
	CHECK(lb[0].after == GfxResourceState::AllShaderResource);

	// 配列の範囲を超えるスライス
	CompileOptions opt;
	opt.parameters["cascades"] = 5;
	const CompiledGraph c5 = compile(g, opt);
	CHECK_FALSE(c5.ok);
	CHECK(hasDiag(c5, Diagnostic::Severity::Error, "passes[Shadow[4]].outputs[0].slice", "スライス 4 はありません"));
}

TEST_CASE("レンダーグラフ: ブルームの mip の連鎖 (同じテクスチャの別の mip を読み書きする)")
{
	const GraphDesc g = load(k_Bloom);
	checkRoundTrip(g);
	CHECK(g.passes[1].inputs[0].mip.var == "i");
	CHECK(g.passes[1].inputs[0].mip.add == -1);

	const CompiledGraph c = compile(g);
	dump(c);
	REQUIRE(c.ok);
	CHECK(order(c) == std::vector<std::string>{ "Prefilter", "BloomDown[0]", "BloomDown[1]", "BloomDown[2]", "BloomDown[3]", "Composite" });
	CHECK(c.passes[1].desc.inputs[0].mip.value == 0);
	CHECK(c.passes[1].desc.outputs[0].mip.value == 1);

	const ResourceInfo* bloom = c.findResource("bloom");
	REQUIRE(bloom != nullptr);
	CHECK(bloom->mips == 5);
	CHECK(bloom->initialState == GfxResourceState::RenderTarget);

	// BloomDown[1] は mip 1 を読む状態にする (mip 2 は RenderTarget のまま)
	const auto b1 = barriersOf(c, "BloomDown[1]", "bloom");
	REQUIRE(b1.size() == 1);
	CHECK(b1[0].subresource == 1);
	CHECK(b1[0].after == GfxResourceState::AllShaderResource);

	// 最後に、まだ RenderTarget の mip 4 だけを遷移させる
	const auto bc = barriersOf(c, "Composite", "bloom");
	REQUIRE(bc.size() == 1);
	CHECK(bc[0].subresource == 4);
	CHECK(bloom->finalState == GfxResourceState::AllShaderResource);
	CHECK(bloom->finalUniform);

	SUBCASE("同じ mip を読みながら書くのはエラー")
	{
		GraphDesc bad = g;
		REQUIRE(parseIndexExpr("i", bad.passes[1].inputs[0].mip));
		const CompiledGraph e = compile(bad);
		CHECK_FALSE(e.ok);
		CHECK(hasDiag(e, Diagnostic::Severity::Error, "passes[BloomDown[0]].outputs[0]", "読みながら書く"));
	}
	SUBCASE("mip の範囲外 / RTV に mip を 1 つ指定しない")
	{
		GraphDesc bad = g;
		bad.passes[1].repeat.count = 5;   // i = 5 は範囲外
		bad.passes[0].outputs[0].mip = IndexValue{};
		const CompiledGraph e = compile(bad);
		CHECK_FALSE(e.ok);
		CHECK(hasDiag(e, Diagnostic::Severity::Error, "passes[BloomDown[4]].outputs[0].mip", "mip 5 はありません"));
		CHECK(hasDiag(e, Diagnostic::Severity::Error, "passes[Prefilter].outputs[0].mip", "1 つ"));
	}
	SUBCASE("repeat のないパスで変数を使う / 負になる")
	{
		GraphDesc bad = g;
		REQUIRE(parseIndexExpr("i", bad.passes[0].outputs[0].mip));
		bad.passes[1].repeat.start = 0;   // i-1 = -1
		const CompiledGraph e = compile(bad);
		CHECK_FALSE(e.ok);
		CHECK(hasDiag(e, Diagnostic::Severity::Error, "passes[Prefilter].outputs[0].mip", "repeat"));
		CHECK(hasDiag(e, Diagnostic::Severity::Error, "passes[BloomDown[0]].inputs[0].mip", "負"));
	}
}

TEST_CASE("レンダーグラフ: TAA (前フレームの内容を name@prev で読む)")
{
	const GraphDesc g = load(k_Taa);
	checkRoundTrip(g);
	const CompiledGraph c = compile(g);
	dump(c);
	REQUIRE(c.ok);
	CHECK(order(c) == std::vector<std::string>{ "TAA", "Present" });

	const ResourceInfo* prev = c.findResource("taaColor@prev");
	REQUIRE(prev != nullptr);
	CHECK(prev->isPrev());
	CHECK(c.resources[static_cast<size_t>(prev->historyOf)].name == "taaColor");

	// 履歴のリソースはフレームをまたいで残るので、最初はエンジンの状態追跡の値から遷移する
	const BarrierOp* hp = findBarrier(c, "TAA", "taaColor@prev");
	REQUIRE(hp != nullptr);
	CHECK(hp->before == GfxResourceState::Tracked);
	CHECK(hp->after == GfxResourceState::NonPixelShaderResource);
	const BarrierOp* hc = findBarrier(c, "TAA", "taaColor");
	REQUIRE(hc != nullptr);
	CHECK(hc->before == GfxResourceState::Tracked);
	CHECK(hc->after == GfxResourceState::UnorderedAccess);
	CHECK(findBarrier(c, "Present", "taaColor")->before == GfxResourceState::UnorderedAccess);

	SUBCASE("@prev を読むパスが残るなら、元のリソースを書くパスも残す")
	{
		GraphDesc g2;
		ResourceDesc acc = tex("accum", GfxFormat::R16G16B16A16_Float);
		acc.history = true;
		g2.resources = { acc };
		g2.passes.push_back(raster("Show", { ref("accum@prev", Access::SRV, 0, LoadOp::Load, { 1, 0 }) }, { ref("$backbuffer", Access::RTV) }));
		g2.passes.push_back(raster("Accumulate", {}, { ref("accum", Access::RTV, 0, LoadOp::DontCare) }));
		const CompiledGraph c2 = compile(g2);
		dump(c2);
		REQUIRE(c2.ok);
		CHECK(order(c2) == std::vector<std::string>{ "Show", "Accumulate" });
		CHECK(c2.skipped.empty());
	}
	SUBCASE("history でないリソースの @prev / @prev に書く")
	{
		GraphDesc bad = g;
		bad.resources[0].history = false;
		CompiledGraph e = compile(bad);
		CHECK_FALSE(e.ok);
		CHECK(hasDiag(e, Diagnostic::Severity::Error, "passes[TAA].inputs[1].res", "history"));

		bad = g;
		bad.passes[0].outputs.push_back(ref("taaColor@prev", Access::UAV, 0, LoadOp::DontCare, { 2, 1 }));
		e = compile(bad);
		CHECK_FALSE(e.ok);
		CHECK(hasDiag(e, Diagnostic::Severity::Error, "passes[TAA].outputs[1].res", "@prev"));
	}
}

TEST_CASE("レンダーグラフ: Forward+ (Compute でライトを分類し、フォワードで読む)")
{
	const GraphDesc g = load(k_ForwardPlus);
	checkRoundTrip(g);
	const CompiledGraph c = compile(g);
	dump(c);
	REQUIRE(c.ok);
	CHECK(order(c) == std::vector<std::string>{ "DepthPrepass", "LightCull", "Forward" });
	CHECK(findBarrier(c, "LightCull", "depth")->after == GfxResourceState::NonPixelShaderResource);
	CHECK(findBarrier(c, "Forward", "depth")->after == GfxResourceState::DepthRead);
	CHECK(c.findResource("lightGrid")->initialState == GfxResourceState::UnorderedAccess);
	CHECK(findBarrier(c, "Forward", "lightGrid")->after == GfxResourceState::AllShaderResource);
}

TEST_CASE("レンダーグラフ: MSAA の解決 (Copy のパスの ResolveSrc / ResolveDst)")
{
	const GraphDesc g = load(k_Msaa);
	checkRoundTrip(g);
	const CompiledGraph c = compile(g);
	dump(c);
	REQUIRE(c.ok);
	CHECK(order(c) == std::vector<std::string>{ "Opaque", "Resolve", "Tonemap" });
	const BarrierOp* src = findBarrier(c, "Resolve", "sceneMS");
	REQUIRE(src != nullptr);
	CHECK(src->before == GfxResourceState::RenderTarget);
	CHECK(src->after == GfxResourceState::ResolveSource);
	CHECK(c.findResource("sceneColor")->initialState == GfxResourceState::ResolveDest);
	CHECK(findBarrier(c, "Tonemap", "sceneColor")->before == GfxResourceState::ResolveDest);
	CHECK(hasDiag(c, Diagnostic::Severity::Warning, "resources.depthMS") == false);

	SUBCASE("MSAA でないテクスチャの解決 / MSAA への解決 / MSAA と mip")
	{
		GraphDesc bad = g;
		std::swap(bad.passes[1].inputs[0].res, bad.passes[1].outputs[0].res);
		bad.resources[2].mipLevels = 2;
		bad.resources[0].mipLevels = 2;
		const CompiledGraph e = compile(bad);
		CHECK_FALSE(e.ok);
		CHECK(hasDiag(e, Diagnostic::Severity::Error, "passes[Resolve].inputs[0].as", "ResolveSrc"));
		CHECK(hasDiag(e, Diagnostic::Severity::Error, "passes[Resolve].outputs[0].as", "ResolveDst"));
		CHECK(hasDiag(e, Diagnostic::Severity::Error, "resources.sceneMS.mips", "MSAA"));
	}
}

TEST_CASE("レンダーグラフ: 間接 dispatch (引数のバッファを IndirectArgs で読む)")
{
	const GraphDesc g = load(k_Indirect);
	checkRoundTrip(g);
	const CompiledGraph c = compile(g);
	dump(c);
	REQUIRE(c.ok);
	CHECK(order(c) == std::vector<std::string>{ "BuildArgs", "Cull" });
	const BarrierOp* b = findBarrier(c, "Cull", "dispatchArgs");
	REQUIRE(b != nullptr);
	CHECK(b->before == GfxResourceState::UnorderedAccess);
	CHECK(b->after == GfxResourceState::IndirectArgument);

	SUBCASE("引数のバッファを入力に書いていない / Compute で drawIndirect")
	{
		GraphDesc bad = g;
		bad.passes[1].inputs.clear();
		CompiledGraph e = compile(bad);
		CHECK_FALSE(e.ok);
		CHECK(hasDiag(e, Diagnostic::Severity::Error, "passes[Cull].content.dispatchIndirect.args", "IndirectArgs"));

		bad = g;
		bad.passes[1].content.kind = ContentKind::DrawIndirect;
		e = compile(bad);
		CHECK_FALSE(e.ok);
		CHECK(hasDiag(e, Diagnostic::Severity::Error, "passes[Cull].content", "Compute"));
	}
}

TEST_CASE("レンダーグラフ: パラメーターと when で、パスの有無を切り替える")
{
	const GraphDesc g = load(k_Conditional);
	checkRoundTrip(g);
	REQUIRE(g.parameters.size() == 2);

	CompiledGraph c = compile(g);
	dump(c);
	REQUIRE(c.ok);
	CHECK(order(c) == std::vector<std::string>{ "SSAO", "Blur[0]", "Blur[1]", "Composite" });
	CHECK(c.skippedNames(SkippedPass::Reason::Condition) == std::vector<std::string>{ "CompositeNoAO" });
	REQUIRE(findBarrier(c, "Blur[1]", "ao") != nullptr);
	CHECK(findBarrier(c, "Blur[1]", "ao")->type == GfxBarrierType::UAV);

	// アプリが上書きする
	CompileOptions opt;
	opt.parameters["ssao"] = 0;
	c = compile(g, opt);
	dump(c);
	REQUIRE(c.ok);
	CHECK(order(c) == std::vector<std::string>{ "CompositeNoAO" });
	CHECK(c.skippedNames(SkippedPass::Reason::Condition) == std::vector<std::string>{ "SSAO", "Blur", "Composite" });
	CHECK(c.parameters.at("ssao") == 0);
	CHECK(hasDiag(c, Diagnostic::Severity::Warning, "resources.ao"));   // 使われない

	// 繰り返しの数を 0 にすると、そのパスは実行しない
	opt.parameters = { { "blurPasses", 0 } };
	c = compile(g, opt);
	REQUIRE(c.ok);
	CHECK(order(c) == std::vector<std::string>{ "SSAO", "Composite" });

	// 知らないパラメーターの上書きは警告、ないパラメーターを when で使うとエラー
	opt.parameters = { { "noSuchParam", 1 } };
	c = compile(g, opt);
	CHECK(c.ok);
	CHECK(hasDiag(c, Diagnostic::Severity::Warning, "parameters.noSuchParam"));

	GraphDesc bad = g;
	bad.passes[0].when = "ssaoo";
	c = compile(bad);
	CHECK_FALSE(c.ok);
	CHECK(hasDiag(c, Diagnostic::Severity::Error, "passes[SSAO].when", "ssaoo"));
}

TEST_CASE("レンダーグラフ: ビューポートとサブリソースの検証")
{
	GraphDesc g;
	ResourceDesc atlas = tex("atlas", GfxFormat::D32_Float);
	atlas.size.relative = false; atlas.size.width = 4096; atlas.size.height = 4096;
	g.resources = { atlas };
	PassDesc p = raster("Atlas", {}, { ref("atlas", Access::DSV, 0, LoadOp::Clear) });
	p.keep = true;
	p.viewport.set = true;
	p.viewport.x = 0.5f; p.viewport.y = 0.0f; p.viewport.width = 0.5f; p.viewport.height = 0.5f;
	g.passes = { p };
	checkRoundTrip(g);
	CompiledGraph c = compile(g);
	dump(c);
	CHECK(c.ok);

	g.passes[0].viewport.width = 0.75f;   // 0.5 + 0.75 > 1
	g.passes[0].scissor.set = true;
	g.passes[0].scissor.relative = false;
	g.passes[0].scissor.width = 0;
	c = compile(g);
	CHECK_FALSE(c.ok);
	CHECK(hasDiag(c, Diagnostic::Severity::Error, "passes[Atlas].viewport", "範囲"));
	CHECK(hasDiag(c, Diagnostic::Severity::Error, "passes[Atlas].scissor", "幅と高さ"));

	SUBCASE("インポート / バッファ / 3D のスライス / キューブの大きさ")
	{
		GraphDesc s;
		ResourceDesc vol = tex("vol", GfxFormat::R16G16B16A16_Float);
		vol.dimension = TextureDimension::Tex3D; vol.depth = 32;
		ResourceDesc cube = tex("probe", GfxFormat::R16G16B16A16_Float);
		cube.dimension = TextureDimension::Cube; cube.size.relative = false; cube.size.width = 256; cube.size.height = 128;
		ResourceDesc buf;
		buf.name = "buf"; buf.kind = ResourceKind::Buffer; buf.byteSize = 256;
		s.resources = { vol, cube, buf };

		PassDesc q;
		q.name = "Q"; q.type = PassType::Compute; q.pipeline = "pipelines/q.json"; q.content.kind = ContentKind::Dispatch; q.keep = true;
		PassResourceRef imported = ref("$scene", Access::SRV, 0, LoadOp::Load, { 1, 0 });
		imported.mip = IndexValue::fixed(1);
		PassResourceRef bufRef = ref("buf", Access::UAV, 0, LoadOp::DontCare, { 2, 0 });
		bufRef.mip = IndexValue::fixed(0);
		PassResourceRef volRef = ref("vol", Access::UAV, 0, LoadOp::DontCare, { 2, 1 });
		volRef.slice = IndexValue::fixed(3);
		q.inputs  = { imported };
		q.outputs = { bufRef, volRef };
		s.passes = { q };
		checkRoundTrip(s);

		const CompiledGraph e = compile(s);
		CHECK_FALSE(e.ok);
		CHECK(hasDiag(e, Diagnostic::Severity::Error, "passes[Q].inputs[0]", "インポート"));
		CHECK(hasDiag(e, Diagnostic::Severity::Error, "passes[Q].outputs[0]", "バッファ"));
		CHECK(hasDiag(e, Diagnostic::Severity::Error, "passes[Q].outputs[1].slice", "3D"));
		CHECK(hasDiag(e, Diagnostic::Severity::Error, "resources.probe.size", "キューブ"));
	}
}
