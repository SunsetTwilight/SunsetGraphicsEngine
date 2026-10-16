// ============================================================
//  描画テスト (ヘッドレス)
//
//  ウィンドウを作らずにオフスクリーンの RT へ描画し、次を確認する。
//    ・D3D12 デバッグレイヤーのエラー / 警告が 0 件
//    ・タイムスタンプのクエリで、GPU がコマンドを実行したこと
//  あわせて、Uniform バッファのバージョン切り替えと、使用中リソースの遅延解放の経路を通す。
//
//  環境変数 GFX_TEST_ADAPTER=software で WARP (ソフトウェアレンダラー) を使う。
//  デバッグレイヤー (Graphics Tools) がない環境では、メッセージの確認は行えない (情報として表示する)。
// ============================================================
#include <doctest/doctest.h>
#include "gfx/core/GraphicsEngine.h"

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace
{
bool useSoftwareAdapter()
{
	char   buf[32] = {};
	size_t len     = 0;
	return getenv_s(&len, buf, sizeof(buf), "GFX_TEST_ADAPTER") == 0 && len > 0
		&& std::string(buf) == "software";
}

// デバッグメッセージを数える
struct MessageLog
{
	std::vector<std::string> problems;   // Corruption / Error / Warning
	std::vector<std::string> infos;
};

DeviceConfig makeConfig(MessageLog& log)
{
	DeviceConfig cfg;
	cfg.appName    = "gfx_tests_render";
	cfg.validation = true;
	cfg.adapter    = useSoftwareAdapter() ? GfxAdapterType::Software : GfxAdapterType::Hardware;
	cfg.debugMessage = [&log](GfxMessageSeverity sev, int32_t id, const char* text) {
		std::string s = "#" + std::to_string(id) + " " + text;
		if (sev <= GfxMessageSeverity::Warning)
			log.problems.push_back(std::move(s));
		else
			log.infos.push_back(std::move(s));
	};
	return cfg;
}

const char* k_VS = R"hlsl(
float4 main(uint id : SV_VertexID) : SV_Position
{
	float2 uv = float2((id << 1) & 2, id & 2);
	return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
)hlsl";

const char* k_PS = R"hlsl(
cbuffer Color : register(b0) { float4 gColor; };
float4 main() : SV_Target { return gColor; }
)hlsl";
} // namespace

TEST_CASE("ヘッドレスでオフスクリーンに描画しても D3D12 のエラー / 警告が出ない")
{
	MessageLog     log;
	GraphicsEngine engine;
	REQUIRE_MESSAGE(engine.init(makeConfig(log)), engine.lastError());
	MESSAGE("adapter: ", engine.adapterName());
	for (const auto& s : log.infos)
		MESSAGE("info: ", s);

	// ---- リソース ----
	IRenderTarget* rt = engine.createRenderTarget({ 256, 256, PixelFormat::RGBA8_UNORM, 1, false, true, { 0, 0, 0, 1 }, "TestRT" });
	REQUIRE(rt != nullptr);

	ShaderDesc vsDesc; vsDesc.stage = ShaderStage::Vertex;   vsDesc.source = k_VS;
	ShaderDesc psDesc; psDesc.stage = ShaderStage::Fragment; psDesc.source = k_PS;
	const ShaderHandle vs = engine.compileShader(vsDesc);
	const ShaderHandle ps = engine.compileShader(psDesc);
	REQUIRE(vs.valid());
	REQUIRE(ps.valid());

	GfxBindingLayoutDesc layoutDesc;
	layoutDesc.addRootDescriptor(GfxRootParameterType::CBV, 0, 0, GfxShaderVisibility::Pixel);
	layoutDesc.debugName = "TestLayout";
	const BindingLayoutHandle layout = engine.createBindingLayout(layoutDesc);
	REQUIRE(layout.valid());

	GfxGraphicsPipelineDesc pipeDesc;
	pipeDesc.bindingLayout                    = layout;
	pipeDesc.vs                               = vs;
	pipeDesc.ps                               = ps;
	pipeDesc.rasterizerState.cullMode         = GfxCullMode::None;
	pipeDesc.depthStencilState.depthEnable    = false;
	pipeDesc.depthStencilState.depthWriteMask = GfxDepthWriteMask::Zero;
	pipeDesc.numRenderTargets                 = 1;
	pipeDesc.rtvFormats[0]                    = GfxFormat::R8G8B8A8_Unorm;
	pipeDesc.debugName                        = "TestPipeline";
	const PipelineHandle pipe = engine.createPipeline(pipeDesc);
	REQUIRE(pipe.valid());

	const BufferHandle colorBuf = engine.createBuffer({ 16, BufferUsage::Uniform });
	REQUIRE(colorBuf.valid());

	GfxQueryPoolDesc qpDesc;
	qpDesc.type  = GfxQueryType::Timestamp;
	qpDesc.count = 2;
	const QueryPoolHandle queries = engine.createQueryPool(qpDesc);
	REQUIRE(queries.valid());

	// ---- 描画 ----
	constexpr int kFrames = 8;
	for (int frame = 0; frame < kFrames; ++frame)
	{
		// Uniform バッファは毎フレーム更新する (実行中のフレームと別のバージョンに書き込まれる)
		const float color[4] = { frame / float(kFrames), 0.5f, 0.25f, 1.0f };
		REQUIRE(engine.updateBuffer(colorBuf, color, sizeof(color)));

		// 毎フレーム作って、GPU が使っている途中で破棄する (遅延解放の経路)
		const uint32_t pixels[4] = { 0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u, 0xFFFFFFFFu };
		TextureDesc texDesc;
		texDesc.width = 2; texDesc.height = 2; texDesc.initialData = pixels; texDesc.debugName = "TempTexture";
		const TextureHandle tempTex = engine.createTexture(texDesc);
		REQUIRE(tempTex.valid());

		ICommandBuffer* cb = engine.allocCommandBuffer();
		REQUIRE(cb != nullptr);
		cb->begin();
		cb->writeTimestamp(queries, 0);
		cb->setRenderTargets(1, &rt, nullptr, true, false);
		cb->bindPipeline(pipe);
		cb->bindUniformBuffer(colorBuf, 0);
		cb->drawFullscreen();
		cb->writeTimestamp(queries, 1);
		cb->resolveQueries(queries, 0, 2);
		cb->end();
		engine.submitCommandBuffers({ cb });
		cb->release();

		engine.destroyTexture(tempTex);
	}

	// ---- GPU が実行したことをタイムスタンプで確認する (完了まで最大 10 秒待つ) ----
	uint64_t ts[2] = {};
	bool     resolved = false;
	for (int i = 0; i < 1000 && !resolved; ++i)
	{
		resolved = engine.readQueryResults(queries, 0, 2, ts);
		if (!resolved)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	CHECK(resolved);
	CHECK(ts[1] >= ts[0]);
	CHECK(engine.getTimestampFrequency() > 0);

	// ---- 後片付け ----
	engine.destroyQueryPool(queries);
	engine.destroyBuffer(colorBuf);
	engine.destroyPipeline(pipe);
	engine.destroyBindingLayout(layout);
	engine.destroyShader(vs);
	engine.destroyShader(ps);
	engine.destroyRenderTarget(rt);
	engine.shutdown();

	for (const auto& s : log.problems)
		MESSAGE("D3D12: ", s);
	CHECK(log.problems.empty());
}
