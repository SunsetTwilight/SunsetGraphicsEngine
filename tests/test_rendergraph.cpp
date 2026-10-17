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
	CHECK(c.culled.empty());

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
	REQUIRE(c.culled.size() == 1);
	CHECK(g.passes[c.culled[0]].name == "Debug");
	CHECK_FALSE(c.findResource("debugTex")->used());
	CHECK(hasDiag(c, Diagnostic::Severity::Warning, "resources.debugTex"));

	g.passes.back().keep = true;
	c = compile(g);
	REQUIRE(c.ok);
	CHECK(c.culled.empty());
	CHECK(c.indexOfPass("Debug") >= 0);

	// 無効にしたパスはなかったものとして扱う
	g.passes.back().enabled = false;
	c = compile(g);
	REQUIRE(c.ok);
	CHECK(c.disabled.size() == 1);
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
