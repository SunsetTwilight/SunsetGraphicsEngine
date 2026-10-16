// ============================================================
//  作り直しのテスト (GPU または WARP)
//    ・パイプラインの記述子の取得と変更 (updatePipeline / getPipelineDesc)
//    ・シェーダーの作り直し (updateShader) と、それを使うパイプラインの作り直し
//    ・記述子 → JSON → 記述子 で同じパイプラインを作れること
//    ・ShaderLibrary によるファイルのホットリロード
// ============================================================
#include <doctest/doctest.h>
#include "TestCommon.h"
#include "gfx/core/ShaderLibrary.h"
#include "gfx/schema/GfxJson.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>

using namespace gfxtest;
namespace fs = std::filesystem;

namespace
{
const char* k_VS = R"hlsl(
float4 main(uint id : SV_VertexID) : SV_Position
{
	float2 uv = float2((id << 1) & 2, id & 2);
	return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
)hlsl";

const char* k_PS_A = R"hlsl(
cbuffer A : register(b0) { float4 gColor; };
float4 main() : SV_Target { return gColor; }
)hlsl";

const char* k_PS_B = R"hlsl(
cbuffer B : register(b0) { float4 gTint; float gScale; };
float4 main() : SV_Target { return gTint * gScale; }
)hlsl";

// 頂点シェーダーが出力しない TEXCOORD を入力にする (パイプラインを作れない)
const char* k_PS_Incompatible = R"hlsl(
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target { return float4(uv, 0, 1); }
)hlsl";

struct Fixture
{
	MessageLog          log;
	GraphicsEngine      engine;
	IRenderTarget*      rt     = nullptr;
	BindingLayoutHandle layout;
	ShaderHandle        vs, ps;
	PipelineHandle      pipe;
	BufferHandle        cbuf;

	Fixture()
	{
		REQUIRE_MESSAGE(engine.init(makeConfig(log, "gfx_tests_reload")), engine.lastError());
		rt = engine.createRenderTarget({ 64, 64, PixelFormat::RGBA8_UNORM, 1, false, true, { 0, 0, 0, 1 }, "ReloadRT" });
		REQUIRE(rt != nullptr);

		ShaderDesc vsDesc; vsDesc.stage = ShaderStage::Vertex;   vsDesc.source = k_VS;
		ShaderDesc psDesc; psDesc.stage = ShaderStage::Fragment; psDesc.source = k_PS_A; psDesc.sourceName = "ps_a.hlsl";
		vs = engine.compileShader(vsDesc);
		ps = engine.compileShader(psDesc);
		REQUIRE(vs.valid());
		REQUIRE(ps.valid());

		GfxBindingLayoutDesc layoutDesc;
		layoutDesc.addRootDescriptor(GfxRootParameterType::CBV, 0, 0, GfxShaderVisibility::Pixel);
		layout = engine.createBindingLayout(layoutDesc);
		REQUIRE(layout.valid());

		pipe = engine.createPipeline(makeDesc());
		REQUIRE(pipe.valid());

		cbuf = engine.createBuffer({ 256, BufferUsage::Uniform });
		REQUIRE(cbuf.valid());
		const float data[8] = { 1, 0, 0, 1, 0.5f, 0, 0, 0 };
		REQUIRE(engine.updateBuffer(cbuf, data, sizeof(data)));
	}

	~Fixture()
	{
		engine.destroyBuffer(cbuf);
		engine.destroyPipeline(pipe);
		engine.destroyBindingLayout(layout);
		engine.destroyShader(vs);
		engine.destroyShader(ps);
		engine.destroyRenderTarget(rt);
		engine.shutdown();
	}

	GfxGraphicsPipelineDesc makeDesc() const
	{
		GfxGraphicsPipelineDesc d;
		d.bindingLayout                    = layout;
		d.vs                               = vs;
		d.ps                               = ps;
		d.rasterizerState.cullMode         = GfxCullMode::None;
		d.depthStencilState.depthEnable    = false;
		d.depthStencilState.depthWriteMask = GfxDepthWriteMask::Zero;
		d.numRenderTargets                 = 1;
		d.rtvFormats[0]                    = GfxFormat::R8G8B8A8_Unorm;
		d.debugName                        = "ReloadPipeline";
		return d;
	}

	// パイプラインで 1 回描画する (作り直した PSO が使えることの確認)
	void draw(PipelineHandle p)
	{
		ICommandBuffer* cb = engine.allocCommandBuffer();
		REQUIRE(cb != nullptr);
		cb->begin();
		cb->setRenderTargets(1, &rt, nullptr, true, false);
		cb->bindPipeline(p);
		cb->bindUniformBuffer(cbuf, 0);
		cb->drawFullscreen();
		cb->end();
		engine.submitCommandBuffers({ cb });
		cb->release();
	}
};

bool sameDesc(const GfxGraphicsPipelineDesc& a, const GfxGraphicsPipelineDesc& b)
{
	return gfx_json_detail::structEquals(gfxStructInfo<GfxGraphicsPipelineDesc>(), &a, &b);
}

std::string cbufferName(GraphicsEngine& engine, ShaderHandle h)
{
	auto r = std::make_unique<GfxShaderReflection>();
	if (!engine.getShaderReflection(h, *r) || r->cbufferCount == 0)
		return {};
	return r->cbuffers[0].name;
}

struct TempDir
{
	fs::path path;
	TempDir()
	{
		path = fs::temp_directory_path() / ("gfx_tests_reload_" + std::to_string(std::random_device{}()));
		fs::create_directories(path);
	}
	~TempDir()
	{
		std::error_code ec;
		fs::remove_all(path, ec);
	}
	// 書き込み、更新時刻を確実に変える (ファイルシステムの時刻の分解能に依存しないように)
	void write(const fs::path& rel, const std::string& text)
	{
		const fs::path p = path / rel;
		fs::create_directories(p.parent_path());
		{
			std::ofstream(p, std::ios::binary | std::ios::trunc) << text;
		}
		fs::last_write_time(p, fs::file_time_type::clock::now() + std::chrono::seconds(++m_tick));
	}
	int m_tick = 0;
};
} // namespace

TEST_CASE("パイプライン: 記述子を取得し、変更して作り直せる")
{
	Fixture f;
	f.draw(f.pipe);

	GfxGraphicsPipelineDesc current;
	REQUIRE(f.engine.getPipelineDesc(f.pipe, current));
	CHECK(sameDesc(current, f.makeDesc()));
	CHECK(std::string(current.debugName) == "ReloadPipeline");

	// 変更 (名前を渡さなければ今の名前のまま)
	GfxGraphicsPipelineDesc next = current;
	next.rasterizerState.cullMode = GfxCullMode::Back;
	next.rasterizerState.fillMode = GfxFillMode::Wireframe;
	next.blendState.renderTarget[0].renderTargetWriteMask = GfxColorWriteMask::Red | GfxColorWriteMask::Alpha;
	next.debugName = nullptr;
	REQUIRE(f.engine.updatePipeline(f.pipe, next));
	f.draw(f.pipe);

	GfxGraphicsPipelineDesc after;
	REQUIRE(f.engine.getPipelineDesc(f.pipe, after));
	CHECK(after.rasterizerState.fillMode == GfxFillMode::Wireframe);
	CHECK(after.blendState.renderTarget[0].renderTargetWriteMask == (GfxColorWriteMask::Red | GfxColorWriteMask::Alpha));
	CHECK(std::string(after.debugName) == "ReloadPipeline");

	// Compute 用の取得は失敗する / 無効なハンドルも失敗する
	GfxComputePipelineDesc cdesc;
	CHECK_FALSE(f.engine.getPipelineDesc(f.pipe, cdesc));
	CHECK_FALSE(f.engine.getPipelineDesc(PipelineHandle{ 12345 }, current));

	const auto problems = f.log.problems();
	for (const auto& s : problems) MESSAGE("D3D12: ", s);
	CHECK(problems.empty());
}

TEST_CASE("パイプライン: 作れない記述子では何も変えずに失敗する")
{
	Fixture f;
	GfxGraphicsPipelineDesc bad = f.makeDesc();
	bad.ps = f.vs;   // ピクセルシェーダーの場所に頂点シェーダー
	CHECK_FALSE(f.engine.updatePipeline(f.pipe, bad));
	CHECK(f.log.withId(GFX_MESSAGE_ID_PIPELINE).size() == 1);

	GfxGraphicsPipelineDesc current;
	REQUIRE(f.engine.getPipelineDesc(f.pipe, current));
	CHECK(current.ps.id == f.ps.id);
	f.draw(f.pipe);   // 元の PSO で描ける
}

TEST_CASE("シェーダー: 作り直すと、使っているパイプラインも作り直される")
{
	Fixture f;
	CHECK(cbufferName(f.engine, f.ps) == "A");

	ShaderDesc d;
	d.stage = ShaderStage::Fragment; d.source = k_PS_B; d.sourceName = "ps_b.hlsl";
	REQUIRE(f.engine.updateShader(f.ps, d));
	CHECK(cbufferName(f.engine, f.ps) == "B");   // ハンドルはそのままで中身が変わる
	f.draw(f.pipe);

	SUBCASE("コンパイルエラーなら元のまま")
	{
		ShaderDesc broken;
		broken.stage = ShaderStage::Fragment; broken.source = "float4 main() : SV_Target { return nope; }";
		CHECK_FALSE(f.engine.updateShader(f.ps, broken));
		CHECK(cbufferName(f.engine, f.ps) == "B");
		f.draw(f.pipe);
		CHECK(f.log.problems({ GFX_MESSAGE_ID_SHADER_COMPILE }).empty());
	}
	SUBCASE("ステージは変えられない")
	{
		ShaderDesc other;
		other.stage = ShaderStage::Vertex; other.source = k_VS;
		CHECK_FALSE(f.engine.updateShader(f.ps, other));
		CHECK(f.log.problems({ GFX_MESSAGE_ID_SHADER_COMPILE }).empty());
	}
	SUBCASE("問題がなければ D3D12 のメッセージも出ない")
	{
		CHECK(f.log.problems().empty());
	}
}

TEST_CASE("シェーダー: パイプラインを作れない変更は、シェーダーも元のまま")
{
	Fixture f;
	ShaderDesc d;
	d.stage = ShaderStage::Fragment; d.source = k_PS_Incompatible; d.sourceName = "incompatible.hlsl";
	CHECK_FALSE(f.engine.updateShader(f.ps, d));
	CHECK(cbufferName(f.engine, f.ps) == "A");
	CHECK(f.log.withId(GFX_MESSAGE_ID_PIPELINE).size() == 1);
	CHECK(f.log.withId(GFX_MESSAGE_ID_SHADER_COMPILE).size() == 1);
	f.draw(f.pipe);
	// デバッグレイヤーは PSO の作成失敗をエラーとして報告するので、ここではメッセージの件数を確認しない
}

TEST_CASE("記述子 → JSON → 記述子 で同じパイプラインを作れる")
{
	Fixture f;
	GfxGraphicsPipelineDesc original = f.makeDesc();
	original.rasterizerState.depthBias  = 3;
	original.blendState.renderTarget[0].blendEnable = true;
	original.blendState.renderTarget[0].srcBlend    = GfxBlend::SrcAlpha;
	original.blendState.renderTarget[0].destBlend   = GfxBlend::InvSrcAlpha;

	// ハンドルはアセットの名前に置き換える
	GfxJsonWriteOptions w;
	w.writeHandle = [&](const GfxFieldInfo& field, uint64_t id) -> GfxJson {
		if (id == 0) return GfxJson();
		if (std::strcmp(field.name, "bindingLayout") == 0) return GfxJson("layout/main");
		if (id == f.vs.id) return GfxJson("shaders/fullscreen.vs");
		if (id == f.ps.id) return GfxJson("shaders/color.ps");
		return GfxJson();
	};
	GfxJsonReadOptions r;
	r.readHandle = [&](const GfxFieldInfo&, const GfxJson& v, uint64_t& id) {
		if (!v.isString()) return false;
		if (v.asString() == "layout/main")           { id = f.layout.id; return true; }
		if (v.asString() == "shaders/fullscreen.vs") { id = f.vs.id;     return true; }
		if (v.asString() == "shaders/color.ps")      { id = f.ps.id;     return true; }
		return false;
	};

	const std::string text = gfxJsonStringify(gfxToJson(original, w));
	MESSAGE(text);
	GfxJson json;
	std::string err;
	REQUIRE_MESSAGE(gfxJsonParse(text, json, &err), err);
	GfxGraphicsPipelineDesc loaded;
	GfxJsonDiagnostics diag;
	REQUIRE(gfxFromJson(json, loaded, &diag, r));
	CHECK(diag.warnings.empty());

	const PipelineHandle p = f.engine.createPipeline(loaded);
	REQUIRE(p.valid());
	GfxGraphicsPipelineDesc created;
	REQUIRE(f.engine.getPipelineDesc(p, created));
	created.debugName = original.debugName = nullptr;
	CHECK(sameDesc(created, original));
	f.draw(p);
	f.engine.destroyPipeline(p);
	CHECK(f.log.problems().empty());
}

TEST_CASE("ShaderLibrary: ファイルと #include の変更で作り直す")
{
	TempDir dir;
	dir.write("color.hlsli", "float4 baseColor() { return float4(1, 0, 0, 1); }\n");
	dir.write("main.ps.hlsl",
		"#include \"color.hlsli\"\n"
		"cbuffer A : register(b0) { float4 gColor; };\n"
		"float4 main() : SV_Target { return baseColor() * gColor; }\n");

	Fixture f;
	ShaderLibrary lib(f.engine);
	lib.addSearchPath(dir.path);
	lib.setPollInterval(std::chrono::milliseconds(0));
	lib.setDebounce(std::chrono::milliseconds(0));

	int callbacks = 0;
	lib.setOnReload([&](ShaderHandle, const std::string&, bool) { ++callbacks; });

	const ShaderHandle ps = lib.load("main.ps.hlsl", ShaderStage::Fragment);
	REQUIRE_MESSAGE(ps.valid(), lib.lastError());
	CHECK(lib.load("main.ps.hlsl", ShaderStage::Fragment).id == ps.id);   // 同じものは同じハンドル
	CHECK(lib.watchedFiles(ps).size() == 2);
	CHECK(lib.poll().handles.empty());

	GfxGraphicsPipelineDesc d = f.makeDesc();
	d.ps = ps;
	const PipelineHandle p = f.engine.createPipeline(d);
	REQUIRE(p.valid());

	// 更新時刻の変化を見つけた poll では待ち、次の poll で作り直す (debounce = 0 でも 1 回は待つ)
	auto pollUntilDone = [&]() {
		ShaderLibrary::PollResult total;
		for (int i = 0; i < 3 && total.handles.empty(); ++i)
		{
			auto r = lib.poll();
			total.reloaded += r.reloaded;
			total.failed   += r.failed;
			total.handles.insert(total.handles.end(), r.handles.begin(), r.handles.end());
		}
		return total;
	};

	SUBCASE("本体の変更")
	{
		dir.write("main.ps.hlsl",
			"#include \"color.hlsli\"\n"
			"cbuffer Changed : register(b0) { float4 gColor; };\n"
			"float4 main() : SV_Target { return baseColor() + gColor; }\n");
		const auto r = pollUntilDone();
		CHECK(r.reloaded == 1);
		CHECK(cbufferName(f.engine, ps) == "Changed");
		f.draw(p);
	}
	SUBCASE("#include したファイルの変更")
	{
		dir.write("color.hlsli", "float4 baseColor() { return float4(0, 1, 0, 1); }\n");
		CHECK(pollUntilDone().reloaded == 1);
		f.draw(p);
	}
	SUBCASE("壊れた変更は元のまま。直せばまた作り直す")
	{
		dir.write("main.ps.hlsl", "float4 main() : SV_Target { return broken; }\n");
		const auto bad = pollUntilDone();
		CHECK(bad.failed == 1);
		CHECK(cbufferName(f.engine, ps) == "A");
		f.draw(p);

		dir.write("main.ps.hlsl",
			"cbuffer Fixed : register(b0) { float4 gColor; };\n"
			"float4 main() : SV_Target { return gColor; }\n");
		CHECK(pollUntilDone().reloaded == 1);
		CHECK(cbufferName(f.engine, ps) == "Fixed");
		CHECK(lib.watchedFiles(ps).size() == 1);   // #include がなくなった
		f.draw(p);
	}
	CHECK(callbacks >= 1);

	f.engine.destroyPipeline(p);
	lib.clear();
	CHECK(f.log.problems({ GFX_MESSAGE_ID_SHADER_COMPILE }).empty());
}
