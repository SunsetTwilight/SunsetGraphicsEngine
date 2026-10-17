// ============================================================
//  レンダーグラフの実行 (M3b) とアセットの変更の反映 (M3c) のテスト: PipelineLibrary と Executor
//
//  一時フォルダにパイプラインのアセットとシェーダーを書き、グラフ (Compute で UAV に書く →
//  MRT と深度に描く → 全部を SRV で読んでオフスクリーンの RT に合成する) を数フレーム実行して、
//  D3D12 のエラー / 警告が 0 件であること、一時リソースの再利用、GPU 時間の計測を確かめる。
//  環境変数 GFX_TEST_ADAPTER=software で WARP を使う。
// ============================================================
#include <doctest/doctest.h>
#include "gfx/rendergraph/Executor.h"
#include "gfx/rendergraph/GraphAsset.h"
#include "gfx/rendergraph/GraphJson.h"

#include "TestCommon.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using namespace gfxtest;
namespace rg = gfx::rg;

namespace
{
void writeFile(const std::filesystem::path& p, const std::string& text)
{
	std::filesystem::create_directories(p.parent_path());
	std::ofstream f(p, std::ios::binary);
	f << text;
}

const char* k_FillCS = R"hlsl(
cbuffer Fill : register(b0) { float4 gColor; };
RWTexture2D<float4> gOut : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) { gOut[id.xy] = gColor; }
)hlsl";

const char* k_FullscreenVS = R"hlsl(
float4 main(uint id : SV_VertexID) : SV_Position
{
	float2 uv = float2((id << 1) & 2, id & 2);
	return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
)hlsl";

const char* k_MrtPS = R"hlsl(
struct MRT { float4 a : SV_Target0; float4 b : SV_Target1; };
MRT main() { MRT o; o.a = float4(0, 1, 0, 1); o.b = float4(0, 0, 1, 1); return o; }
)hlsl";

const char* k_ShowPS = R"hlsl(
Texture2D        gImg   : register(t0);
Texture2D        gA     : register(t1);
Texture2D        gB     : register(t2);
Texture2D<float> gDepth : register(t3);
float4 main(float4 pos : SV_Position) : SV_Target
{
	int3 p = int3(pos.xy, 0);
	return gImg.Load(p) * 0.25 + gA.Load(p) * 0.25 + gB.Load(p) * 0.25 + gDepth.Load(p) * 0.25;
}
)hlsl";

const char* k_FillPipeline = R"json({
  "type": "compute",
  "shaders": { "cs": { "file": "shaders/fill.cs.hlsl" } },
  "layout": {
    "parameters": [ { "parameterType": "Constants32Bit", "shaderRegister": 0, "num32BitValues": 4 },
                    { "parameterType": "DescriptorTable", "firstRange": 0, "numRanges": 1 } ],
    "numParameters": 2,
    "ranges": [ { "rangeType": "UAV", "numDescriptors": 1 } ],
    "numRanges": 1
  }
})json";

const char* k_MrtPipeline = R"json({
  "shaders": { "vs": { "file": "shaders/fullscreen.vs.hlsl" }, "ps": { "file": "shaders/mrt.ps.hlsl" } },
  "layout": {},
  "state": { "rasterizerState": { "cullMode": "None" } }
})json";

const char* k_ShowPipeline = R"json({
  "shaders": { "vs": { "file": "shaders/fullscreen.vs.hlsl" }, "ps": { "file": "shaders/show.ps.hlsl" } },
  "layout": {
    "parameters": [ { "parameterType": "DescriptorTable", "shaderVisibility": "Pixel", "firstRange": 0, "numRanges": 1 } ],
    "numParameters": 1,
    "ranges": [ { "rangeType": "SRV", "numDescriptors": 4, "flags": [ "DescriptorsVolatile" ] } ],
    "numRanges": 1
  },
  "state": { "rasterizerState": { "cullMode": "None" }, "depthStencilState": { "depthEnable": false, "depthWriteMask": "Zero" } }
})json";

const char* k_Graph = R"json({
  "resources": {
    "img":   { "format": "R16G16B16A16_Float", "size": { "width": 256, "height": 256 } },
    "a":     { "format": "R8G8B8A8_Unorm",     "size": { "relative": 1.0 }, "clear": { "color": [ 1, 0, 0, 1 ] } },
    "b":     { "format": "R16G16B16A16_Float", "size": { "relative": 1.0 } },
    "depth": { "format": "D32_Float",          "size": { "relative": 1.0 }, "clear": { "depth": 1.0 } }
  },
  "passes": [
    { "name": "Fill", "type": "Compute", "pipeline": "pipelines/fill.json",
      "constants": [ { "source": "inline", "values": [ 1, 0.5, 0.25, 1 ], "bind": { "param": 0 } } ],
      "outputs": [ { "res": "img", "as": "UAV", "load": "dontCare", "bind": { "param": 1 } } ],
      "content": { "dispatch": [ 32, 32, 1 ] } },
    { "name": "MRT", "type": "Raster", "pipeline": "pipelines/mrt.json",
      "outputs": [ { "res": "a", "as": "RTV0", "load": "clear" },
                   { "res": "b", "as": "RTV1", "load": "clear" },
                   { "res": "depth", "as": "DSV", "load": "clear" } ],
      "content": { "fullscreen": true } },
    { "name": "Show", "type": "Raster", "pipeline": "pipelines/show.json",
      "inputs": [ { "res": "img",   "as": "SRV", "bind": { "param": 0, "offset": 0 } },
                  { "res": "a",     "as": "SRV", "bind": { "param": 0, "offset": 1 } },
                  { "res": "b",     "as": "SRV", "bind": { "param": 0, "offset": 2 } },
                  { "res": "depth", "as": "SRV", "bind": { "param": 0, "offset": 3 } } ],
      "outputs": [ { "res": "$out", "as": "RTV0", "load": "dontCare" } ],
      "content": { "fullscreen": true } }
  ]
})json";

const char* k_DepthOnlyPipeline = R"json({
  "shaders": { "vs": { "file": "shaders/fullscreen.vs.hlsl" } },
  "layout": {},
  "state": { "rasterizerState": { "cullMode": "None" } }
})json";

const char* k_OverlayPipeline = R"json({
  "shaders": { "vs": { "file": "shaders/fullscreen.vs.hlsl" }, "ps": { "file": "shaders/solid.ps.hlsl" } },
  "layout": {},
  "state": { "rasterizerState": { "cullMode": "None" }, "depthStencilState": { "depthFunc": "LessEqual", "depthWriteMask": "Zero" } }
})json";

// 深度を DSV と DSVRead だけで使う (SRV で読まない) グラフ
const char* k_DepthReadGraph = R"json({
  "resources": { "depth": { "format": "D32_Float", "size": { "relative": 1.0 }, "clear": { "depth": 1.0 } } },
  "passes": [
    { "name": "Z", "type": "Raster", "pipeline": "pipelines/depthonly.json",
      "outputs": [ { "res": "depth", "as": "DSV", "load": "clear" } ], "content": { "fullscreen": true } },
    { "name": "Overlay", "type": "Raster", "pipeline": "pipelines/overlay.json",
      "inputs": [ { "res": "depth", "as": "DSVRead" } ],
      "outputs": [ { "res": "$out", "as": "RTV0", "load": "dontCare" } ], "content": { "fullscreen": true } }
  ]
})json";

std::filesystem::path writeAssets()
{
	const auto root = std::filesystem::temp_directory_path() / "gfx_tests_rendergraph_exec";
	std::filesystem::remove_all(root);
	writeFile(root / "shaders" / "fill.cs.hlsl", k_FillCS);
	writeFile(root / "shaders" / "fullscreen.vs.hlsl", k_FullscreenVS);
	writeFile(root / "shaders" / "mrt.ps.hlsl", k_MrtPS);
	writeFile(root / "shaders" / "show.ps.hlsl", k_ShowPS);
	writeFile(root / "pipelines" / "fill.json", k_FillPipeline);
	writeFile(root / "pipelines" / "mrt.json", k_MrtPipeline);
	writeFile(root / "pipelines" / "show.json", k_ShowPipeline);
	writeFile(root / "shaders" / "solid.ps.hlsl", "float4 main() : SV_Target { return float4(1, 1, 1, 1); }");
	writeFile(root / "pipelines" / "depthonly.json", k_DepthOnlyPipeline);
	writeFile(root / "pipelines" / "overlay.json", k_OverlayPipeline);
	writeFile(root / "pipelines" / "broken.json", "{ \"shaders\": { \"vs\": { \"file\": 3 } }, \"state\": { \"rasterizerState\": { \"cullMode\": \"Sideways\" } } }");
	return root;
}

void dumpErrors(const std::vector<std::string>& errors, const char* prefix)
{
	for (const auto& e : errors)
	{
		const std::string line = std::string(prefix) + e;
		MESSAGE(line);
	}
}
} // namespace

TEST_CASE("パイプラインのアセット: JSON の読み書きと診断")
{
	GfxJson json;
	REQUIRE(gfxJsonParse(k_ShowPipeline, json));
	rg::PipelineAsset a;
	GfxJsonDiagnostics diag;
	REQUIRE(rg::pipelineAssetFromJson(json, a, &diag));
	CHECK(diag.errors.empty());
	CHECK_FALSE(a.compute);
	CHECK(a.vs.file == "shaders/fullscreen.vs.hlsl");
	CHECK(a.layout.numParameters == 1);
	CHECK(a.layout.ranges[0].numDescriptors == 4);
	CHECK(a.state.depthStencilState.depthEnable == false);

	// 書き出して読み直すと同じになる
	rg::PipelineAsset again;
	REQUIRE(rg::pipelineAssetFromJson(rg::pipelineAssetToJson(a), again));
	CHECK(again.vs == a.vs);
	CHECK(again.ps == a.ps);
	CHECK(gfxJsonStringify(gfxToJson(again.layout)) == gfxJsonStringify(gfxToJson(a.layout)));

	// 間違い: ステージの種類、ファイルの型、列挙の名前
	const char* badText = R"({ "type": "compute", "shaders": { "vs": { "file": "x.hlsl" } }, "layout": { "numParameters": "two" } })";
	GfxJson bad;
	REQUIRE(gfxJsonParse(badText, bad));
	GfxJsonDiagnostics d2;
	CHECK_FALSE(rg::pipelineAssetFromJson(bad, a, &d2));
	auto has = [&](const char* prefix) {
		for (const auto& e : d2.errors) if (e.rfind(prefix, 0) == 0) return true;
		return false;
	};
	CHECK(has("shaders.vs"));
	CHECK(has("shaders.cs"));
	CHECK(has("layout.numParameters"));
}

TEST_CASE("レンダーグラフの実行: Compute / MRT / 合成を記録して D3D12 のエラーが出ない")
{
	MessageLog     log;
	GraphicsEngine engine;
	REQUIRE_MESSAGE(engine.init(makeConfig(log, "gfx_tests_rendergraph_exec")), engine.lastError());

	const auto root = writeAssets();
	{
		ShaderLibrary       shaders(engine);
		rg::PipelineLibrary pipelines(engine, shaders);
		pipelines.setRoot(root);

		rg::GraphDesc graph;
		GfxJsonDiagnostics jd;
		REQUIRE(rg::graphFromString(k_Graph, graph, &jd));

		rg::CompileOptions options;
		options.bindingLayout = [&](const std::string& p) { return pipelines.bindingLayout(p); };
		const rg::CompiledGraph compiled = rg::compile(graph, options);
		for (const auto& d : compiled.diagnostics) MESSAGE(d.text());
		REQUIRE(compiled.ok);
		CHECK(compiled.warnings().empty());

		IRenderTarget* out = engine.createRenderTarget({ 256, 256, PixelFormat::RGBA8_UNORM, 1, false, true, { 0, 0, 0, 1 }, "GraphOut" });
		REQUIRE(out != nullptr);

		rg::Executor executor(engine, pipelines);
		rg::FrameInputs in;
		in.width  = 256;
		in.height = 256;
		in.imports["$out"] = rg::ImportedResource::fromRenderTarget(out);

		auto runFrame = [&]() {
			ICommandBuffer* cb = engine.allocCommandBuffer();
			REQUIRE(cb != nullptr);
			cb->begin();
			const bool ok = executor.record(cb, graph, compiled, in);
			cb->end();
			engine.submitCommandBuffers({ cb });
			cb->release();
			return ok;
		};

		// 同じ大きさなら一時リソースを再利用する
		for (int i = 0; i < 6; ++i)
			CHECK(runFrame());
		CHECK(executor.pooledTextures() == 4);

		// パイプラインは出力のフォーマットごとに 1 つ
		rg::PipelineTargets t;
		t.numRenderTargets = 1;
		t.rtvFormats[0]    = GfxFormat::R8G8B8A8_Unorm;
		const PipelineHandle show1 = pipelines.graphics("pipelines/show.json", t);
		CHECK(show1.valid());
		CHECK(pipelines.graphics("pipelines/show.json", t).id == show1.id);
		t.rtvFormats[0] = GfxFormat::R16G16B16A16_Float;
		CHECK(pipelines.graphics("pipelines/show.json", t).id != show1.id);

		// 大きさが変わると作り直し、古いものはしばらくして破棄する
		in.width = in.height = 128;
		for (int i = 0; i < 8; ++i)
			CHECK(runFrame());
		CHECK(executor.pooledTextures() == 4);

		// GPU 時間 (数フレーム遅れて読める)
		for (int i = 0; i < 200 && executor.timings().empty(); ++i)
			runFrame();
		REQUIRE(executor.timings().size() == 3);
		CHECK(executor.timings()[0].name == "Fill");
		CHECK(executor.timings()[2].name == "Show");
		CHECK(executor.totalGpuMs() >= 0.0);

		dumpErrors(executor.errors(), "executor: ");
		dumpErrors(pipelines.errors(), "pipelines: ");
		CHECK(executor.errors().empty());
		CHECK(pipelines.errors().empty());

		// ---- アセットの変更の反映 (M3c) ----
		pipelines.setDebounce(std::chrono::milliseconds(0));
		pipelines.setPollInterval(std::chrono::milliseconds(0));
		const auto showFile = root / "pipelines" / "show.json";
		int bump = 0;
		auto rewrite = [&](const std::filesystem::path& p, const std::string& text) {
			writeFile(p, text);
			bump += 2;   // 更新時刻を確実に進める
			std::filesystem::last_write_time(p, std::filesystem::last_write_time(p) + std::chrono::seconds(bump));
		};
		CHECK(pipelines.poll().reloaded.empty());

		// ステートとレイアウト (SRV を 4 → 5) を変える: ハンドルはそのまま、描画も続けられる
		rg::PipelineTargets outT;
		outT.numRenderTargets = 1;
		outT.rtvFormats[0]    = GfxFormat::R8G8B8A8_Unorm;
		std::string changed = k_ShowPipeline;
		changed.replace(changed.find("\"numDescriptors\": 4"), 19, "\"numDescriptors\": 5");
		changed.replace(changed.find("\"cullMode\": \"None\""), 18, "\"cullMode\": \"Back\"");
		rewrite(showFile, changed);
		const rg::PipelineLibrary::PollResult pr = pipelines.poll();
		CHECK(pr.reloaded == std::vector<std::string>{ "pipelines/show.json" });
		CHECK(pr.failed.empty());
		CHECK(pipelines.graphics("pipelines/show.json", outT).id == show1.id);
		REQUIRE(pipelines.asset("pipelines/show.json") != nullptr);
		CHECK(pipelines.asset("pipelines/show.json")->layout.ranges[0].numDescriptors == 5);
		CHECK(pipelines.asset("pipelines/show.json")->state.rasterizerState.cullMode == GfxCullMode::Back);
		for (int i = 0; i < 3; ++i)
			CHECK(runFrame());

		// 壊れた JSON / 作れないシェーダー → 前のまま描き続ける
		const size_t errorsBefore = pipelines.errors().size();
		rewrite(showFile, "{ \"shaders\": ");
		CHECK(pipelines.poll().failed == std::vector<std::string>{ "pipelines/show.json" });
		CHECK(pipelines.asset("pipelines/show.json")->layout.ranges[0].numDescriptors == 5);
		CHECK(pipelines.errors().size() > errorsBefore);
		rg::PipelineAsset badShader = *pipelines.asset("pipelines/show.json");
		badShader.ps.file = "shaders/none.ps.hlsl";
		CHECK_FALSE(pipelines.set("pipelines/show.json", badShader));
		CHECK(pipelines.asset("pipelines/show.json")->ps.file == "shaders/show.ps.hlsl");
		for (int i = 0; i < 3; ++i)
			CHECK(runFrame());

		// 保存: 今のアセットを書き、自分で書いた内容は変更として扱わない
		REQUIRE(pipelines.save("pipelines/show.json"));
		CHECK(pipelines.poll().reloaded.empty());
		std::string savedText;
		REQUIRE(rg::readTextFile(showFile, savedText));
		GfxJson savedJson;
		REQUIRE(gfxJsonParse(savedText, savedJson));
		rg::PipelineAsset reread;
		REQUIRE(rg::pipelineAssetFromJson(savedJson, reread));
		CHECK(reread.layout.ranges[0].numDescriptors == 5);
		CHECK(reread.state.rasterizerState.cullMode == GfxCullMode::Back);

		// 深度を SRV で読まないグラフでも、DSVRead (DepthRead) に遷移できる
		{
			rg::GraphDesc g2;
			REQUIRE(rg::graphFromString(k_DepthReadGraph, g2));
			const rg::CompiledGraph c2 = rg::compile(g2, options);
			for (const auto& d : c2.diagnostics) MESSAGE(d.text());
			REQUIRE(c2.ok);
			for (int i = 0; i < 3; ++i)
			{
				ICommandBuffer* cb2 = engine.allocCommandBuffer();
				cb2->begin();
				CHECK(executor.record(cb2, g2, c2, in));
				cb2->end();
				engine.submitCommandBuffers({ cb2 });
				cb2->release();
			}
		}

		// アプリが渡していないインポートは、何も記録せずにエラー
		rg::FrameInputs missing = in;
		missing.imports.clear();
		ICommandBuffer* cb = engine.allocCommandBuffer();
		cb->begin();
		CHECK_FALSE(executor.record(cb, graph, compiled, missing));
		cb->end();
		engine.submitCommandBuffers({ cb });
		cb->release();
		REQUIRE_FALSE(executor.errors().empty());
		CHECK(executor.errors().back().find("$out") != std::string::npos);

		// 読めないアセット
		CHECK(pipelines.bindingLayout("pipelines/broken.json") == nullptr);
		CHECK(pipelines.bindingLayout("pipelines/none.json") == nullptr);
		bool brokenReported = false, missingReported = false;
		for (const auto& e : pipelines.errors())
		{
			brokenReported  |= e.rfind("pipelines/broken.json: shaders.vs.file", 0) == 0;
			missingReported |= e.rfind("pipelines/none.json", 0) == 0;
		}
		CHECK(brokenReported);
		CHECK(missingReported);

		executor.clear();
		pipelines.clear();
		engine.destroyRenderTarget(out);
	}
	engine.shutdown();
	std::filesystem::remove_all(root);

	const auto problems = log.problems();
	for (const auto& s : problems)
		MESSAGE("D3D12: ", s);
	CHECK(problems.empty());
}
