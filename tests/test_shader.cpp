// ============================================================
//  シェーダーのテスト (DXC、リフレクション、#include / マクロ、エラーの通知)
//  デバイスが必要なので GPU または WARP で実行する (gfx_tests_render に含める)
// ============================================================
#include <doctest/doctest.h>
#include "TestCommon.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>

using namespace gfxtest;
namespace fs = std::filesystem;

namespace
{
const GfxShaderVariable* findVariable(const GfxShaderReflection& r, const char* name)
{
	for (uint32_t i = 0; i < r.variableCount; ++i)
		if (std::strcmp(r.variables[i].name, name) == 0)
			return &r.variables[i];
	return nullptr;
}

// テスト用の一時フォルダ (終了時に削除する)
struct TempDir
{
	fs::path path;
	TempDir()
	{
		path = fs::temp_directory_path() / ("gfx_tests_shader_" + std::to_string(std::random_device{}()));
		fs::remove_all(path);
		fs::create_directories(path);
	}
	~TempDir()
	{
		std::error_code ec;
		fs::remove_all(path, ec);
	}
	fs::path write(const fs::path& rel, const std::string& text) const
	{
		const fs::path p = path / rel;
		fs::create_directories(p.parent_path());
		std::ofstream(p, std::ios::binary) << text;
		return p;
	}
};

const char* k_ReflectPS = R"hlsl(
struct Light
{
	float3 dir;
	float  intensity;
	float4 color;
};
cbuffer Params : register(b1, space2)
{
	float4x4 world;
	Light    light;
	float4   values[4];
};
Texture2D<float4>            gAlbedo  : register(t3);
SamplerState                 gSampler : register(s1);
RWStructuredBuffer<uint>     gCounter : register(u1);

struct Out
{
	float4 color  : SV_Target0;
	float4 normal : SV_Target1;
};

Out main(float4 pos : SV_Position, float2 uv : TEXCOORD0)
{
	Out o;
	float4 a = gAlbedo.Sample(gSampler, uv);
	o.color  = a * light.color * light.intensity + mul(world, values[2]);
	o.normal = float4(light.dir, 0);
	InterlockedAdd(gCounter[0], 1);
	return o;
}
)hlsl";

const char* k_ReflectVS = R"hlsl(
struct VSIn { float3 pos : POSITION; float2 uv : TEXCOORD0; uint id : SV_VertexID; };
float4 main(VSIn i) : SV_Position { return float4(i.pos + i.uv.xyx + i.id, 1); }
)hlsl";

const char* k_ReflectCS = R"hlsl(
RWByteAddressBuffer gOut : register(u0);
[numthreads(8, 4, 1)]
void main(uint3 id : SV_DispatchThreadID) { gOut.Store(id.x * 4, id.y); }
)hlsl";
} // namespace

TEST_CASE("シェーダー: DXC でコンパイルし、リフレクションを取得できる")
{
	MessageLog     log;
	GraphicsEngine engine;
	REQUIRE_MESSAGE(engine.init(makeConfig(log, "gfx_tests_shader")), engine.lastError());

	ShaderDesc psDesc; psDesc.stage = ShaderStage::Fragment; psDesc.source = k_ReflectPS; psDesc.sourceName = "reflect.ps.hlsl";
	ShaderDesc vsDesc; vsDesc.stage = ShaderStage::Vertex;   vsDesc.source = k_ReflectVS;
	ShaderDesc csDesc; csDesc.stage = ShaderStage::Compute;  csDesc.source = k_ReflectCS; csDesc.targetProfile = "cs_6_2";
	const ShaderHandle ps = engine.compileShader(psDesc);
	const ShaderHandle vs = engine.compileShader(vsDesc);
	const ShaderHandle cs = engine.compileShader(csDesc);
	REQUIRE(ps.valid());
	REQUIRE(vs.valid());
	REQUIRE(cs.valid());

	auto r = std::make_unique<GfxShaderReflection>();

	SUBCASE("ピクセルシェーダー: バインディングと定数バッファのレイアウト")
	{
		REQUIRE(engine.getShaderReflection(ps, *r));
		CHECK(r->shaderModel == 60);
		CHECK_FALSE(r->truncated);

		const GfxShaderBinding* tex = r->findBinding("gAlbedo");
		REQUIRE(tex != nullptr);
		CHECK(tex->type == GfxShaderInputType::Texture);
		CHECK(tex->dimension == GfxShaderResourceDimension::Texture2D);
		CHECK(tex->bindPoint == 3);

		const GfxShaderBinding* sam = r->findBinding("gSampler");
		REQUIRE(sam != nullptr);
		CHECK(sam->type == GfxShaderInputType::Sampler);
		CHECK(sam->bindPoint == 1);

		const GfxShaderBinding* uav = r->findBinding("gCounter");
		REQUIRE(uav != nullptr);
		CHECK(uav->type == GfxShaderInputType::UAVRWStructured);
		CHECK(uav->bindPoint == 1);

		const GfxShaderBinding* cb = r->findBinding("Params");
		REQUIRE(cb != nullptr);
		CHECK(cb->type == GfxShaderInputType::CBuffer);
		CHECK(cb->bindPoint == 1);
		CHECK(cb->space == 2);
		REQUIRE(cb->cbufferIndex < r->cbufferCount);

		const GfxShaderCBuffer& params = r->cbuffers[cb->cbufferIndex];
		CHECK(std::string(params.name) == "Params");
		CHECK(params.size == 160);
		CHECK(params.bindPoint == 1);
		CHECK(params.space == 2);

		const GfxShaderVariable* world = findVariable(*r, "world");
		REQUIRE(world != nullptr);
		CHECK(world->offset == 0);
		CHECK(world->size == 64);
		CHECK(world->variableClass == GfxShaderVariableClass::MatrixColumns);
		CHECK(world->rows == 4);
		CHECK(world->columns == 4);

		const GfxShaderVariable* light = findVariable(*r, "light");
		REQUIRE(light != nullptr);
		CHECK(light->variableClass == GfxShaderVariableClass::Struct);
		CHECK(light->offset == 64);

		const GfxShaderVariable* dir = findVariable(*r, "light.dir");
		REQUIRE(dir != nullptr);
		CHECK(dir->offset == 64);
		CHECK(dir->columns == 3);
		CHECK(dir->depth == 1);
		const GfxShaderVariable* intensity = findVariable(*r, "light.intensity");
		REQUIRE(intensity != nullptr);
		CHECK(intensity->offset == 76);
		CHECK(intensity->variableClass == GfxShaderVariableClass::Scalar);
		const GfxShaderVariable* color = findVariable(*r, "light.color");
		REQUIRE(color != nullptr);
		CHECK(color->offset == 80);
		CHECK(color->size == 16);

		const GfxShaderVariable* values = findVariable(*r, "values");
		REQUIRE(values != nullptr);
		CHECK(values->offset == 96);
		CHECK(values->elements == 4);
		CHECK(values->size == 64);

		REQUIRE(r->outputCount == 2);
		CHECK(r->outputs[0].systemValue == GfxSystemValue::Target);
		CHECK(r->outputs[1].registerIndex == 1);
		CHECK(r->inputCount >= 2);
	}

	SUBCASE("頂点シェーダー: 入力シグネチャ")
	{
		REQUIRE(engine.getShaderReflection(vs, *r));
		REQUIRE(r->inputCount == 3);
		CHECK(std::string(r->inputs[0].semanticName) == "POSITION");
		CHECK(r->inputs[0].mask == 0x7);
		CHECK(std::string(r->inputs[1].semanticName) == "TEXCOORD");
		CHECK(r->inputs[2].systemValue == GfxSystemValue::VertexId);
		CHECK(r->inputs[2].componentType == GfxRegisterComponentType::UInt32);
		REQUIRE(r->outputCount == 1);
		CHECK(r->outputs[0].systemValue == GfxSystemValue::Position);
	}

	SUBCASE("Compute シェーダー: スレッドグループのサイズとプロファイルの指定")
	{
		REQUIRE(engine.getShaderReflection(cs, *r));
		CHECK(r->shaderModel == 62);
		CHECK(r->threadGroupSize[0] == 8);
		CHECK(r->threadGroupSize[1] == 4);
		CHECK(r->threadGroupSize[2] == 1);
		const GfxShaderBinding* out = r->findBinding("gOut");
		REQUIRE(out != nullptr);
		CHECK(out->type == GfxShaderInputType::UAVRWByteAddress);
	}

	SUBCASE("古いヘッダー (小さい structSize) には、その大きさまでしか書かない")
	{
		std::memset(r.get(), 0xCD, sizeof(GfxShaderReflection));
		r->structSize = offsetof(GfxShaderReflection, bindings);
		auto* p = engine.device();
		REQUIRE(p != nullptr);
		REQUIRE(p->getShaderReflection(ps, *r));
		CHECK(r->structSize == offsetof(GfxShaderReflection, bindings));
		CHECK(r->shaderModel == 60);
		CHECK(r->bindingCount == 0xCDCDCDCDu);   // 範囲外は書き換えない
	}

	engine.destroyShader(ps);
	engine.destroyShader(vs);
	engine.destroyShader(cs);
	engine.shutdown();
	CHECK(log.problems().empty());
}

TEST_CASE("シェーダー: コンパイルエラーはデバッグメッセージで通知され、無効なハンドルを返す")
{
	MessageLog     log;
	GraphicsEngine engine;
	REQUIRE_MESSAGE(engine.init(makeConfig(log, "gfx_tests_shader")), engine.lastError());

	ShaderDesc d;
	d.stage      = ShaderStage::Fragment;
	d.source     = "float4 main() : SV_Target { return undefinedSymbol; }";
	d.sourceName = "broken.ps.hlsl";
	const ShaderHandle h = engine.compileShader(d);
	CHECK_FALSE(h.valid());

	const auto msgs = log.withId(GFX_MESSAGE_ID_SHADER_COMPILE);
	REQUIRE(msgs.size() == 1);
	CHECK(msgs[0].find("broken.ps.hlsl") != std::string::npos);
	CHECK(msgs[0].find("undefinedSymbol") != std::string::npos);

	engine.shutdown();
	CHECK(log.problems({ GFX_MESSAGE_ID_SHADER_COMPILE }).empty());
}

TEST_CASE("シェーダー: #include とマクロ定義、依存ファイルの記録")
{
	TempDir dir;
	const fs::path mainPath = dir.write("src/main.ps.hlsl",
		"#include \"common.hlsli\"\n"
		"#include \"shared.hlsli\"\n"
		"float4 main() : SV_Target { return tint() * SHARED_SCALE; }\n");
	const fs::path common = dir.write("src/common.hlsli",
		"float4 tint() { return float4(VALUE, 0, 0, 1); }\n");
	const fs::path shared = dir.write("include/shared.hlsli",
		"#define SHARED_SCALE 2.0\n");

	MessageLog     log;
	GraphicsEngine engine;
	REQUIRE_MESSAGE(engine.init(makeConfig(log, "gfx_tests_shader")), engine.lastError());

	std::ifstream in(mainPath, std::ios::binary);
	const std::string source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	const std::string mainStr    = mainPath.string();
	const std::string includeDir = (dir.path / "include").string();
	const char*       includeDirs[] = { includeDir.c_str() };

	SUBCASE("マクロがないとエラーになる")
	{
		ShaderDesc d;
		d.stage = ShaderStage::Fragment; d.source = source.c_str(); d.sourceName = mainStr.c_str();
		d.includeDirs = includeDirs; d.includeDirCount = 1;
		CHECK_FALSE(engine.compileShader(d).valid());
		CHECK(log.withId(GFX_MESSAGE_ID_SHADER_COMPILE).size() == 1);
	}

	SUBCASE("マクロと検索パスを指定すると成功し、読み込んだファイルが記録される")
	{
		const GfxShaderDefine defines[] = { { "VALUE", "0.5" } };
		ShaderDesc d;
		d.stage = ShaderStage::Fragment; d.source = source.c_str(); d.sourceName = mainStr.c_str();
		d.defines = defines; d.defineCount = 1;
		d.includeDirs = includeDirs; d.includeDirCount = 1;
		d.flags = GfxShaderCompileFlags::Debug | GfxShaderCompileFlags::WarningsAsErrors;
		const ShaderHandle h = engine.compileShader(d);
		REQUIRE(h.valid());

		const auto deps = engine.shaderDependencies(h);
		REQUIRE(deps.size() == 2);
		auto same = [](const std::string& a, const fs::path& b) {
			return fs::equivalent(fs::path(a), b);
		};
		CHECK(std::any_of(deps.begin(), deps.end(), [&](const std::string& p) { return same(p, common); }));
		CHECK(std::any_of(deps.begin(), deps.end(), [&](const std::string& p) { return same(p, shared); }));
		engine.destroyShader(h);
	}

	engine.shutdown();
	CHECK(log.problems({ GFX_MESSAGE_ID_SHADER_COMPILE }).empty());
}
