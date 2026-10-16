// ============================================================
//  GPU を使わないテスト: JSON のパース / 文字列化と、記述子 ⇔ JSON の往復
// ============================================================
#include <doctest/doctest.h>
#include "gfx/abi/GfxBackendABI.h"
#include "gfx/schema/GfxJson.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace
{
// 文字列 → JSON → 記述子 (失敗したらテストを止める)
template<typename T>
T roundTrip(const T& in, const GfxJsonWriteOptions& wopts = {}, const GfxJsonReadOptions& ropts = {})
{
	const std::string text = gfxJsonStringify(gfxToJson(in, wopts));
	GfxJson parsed;
	std::string err;
	REQUIRE_MESSAGE(gfxJsonParse(text, parsed, &err), err);
	T out{};
	GfxJsonDiagnostics diag;
	const bool ok = gfxFromJson(parsed, out, &diag, ropts);
	for (const auto& e : diag.errors)   MESSAGE("error: ", e);
	for (const auto& w : diag.warnings) MESSAGE("warning: ", w);
	REQUIRE(ok);
	return out;
}

template<typename T>
bool sameDesc(const T& a, const T& b)
{
	return gfx_json_detail::structEquals(gfxStructInfo<T>(), &a, &b);
}

GfxGraphicsPipelineDesc makeNonDefaultPipeline()
{
	GfxGraphicsPipelineDesc d;
	d.blendState.independentBlendEnable = true;
	d.blendState.renderTarget[0].blendEnable = true;
	d.blendState.renderTarget[0].srcBlend    = GfxBlend::SrcAlpha;
	d.blendState.renderTarget[0].destBlend   = GfxBlend::InvSrcAlpha;
	d.blendState.renderTarget[2].renderTargetWriteMask = GfxColorWriteMask::Red | GfxColorWriteMask::Blue;
	d.rasterizerState.cullMode             = GfxCullMode::None;
	d.rasterizerState.depthBias            = -12;
	d.rasterizerState.slopeScaledDepthBias = 1.5f;
	d.rasterizerState.depthBiasClamp       = 0.1f;
	d.depthStencilState.depthFunc          = GfxCompareFunc::GreaterEqual;
	d.depthStencilState.stencilEnable      = true;
	d.depthStencilState.stencilWriteMask   = 0x0F;
	d.depthStencilState.backFace.stencilPassOp = GfxStencilOp::IncrSat;
	d.inputLayout.add("POSITION", 0, GfxFormat::R32G32B32_Float)
	             .add("TEXCOORD", 1, GfxFormat::R16G16_Float, 1, 0, GfxInputClassification::PerInstance, 1);
	d.primitiveTopologyType = GfxPrimitiveTopologyType::Line;
	d.primitiveTopology     = GfxPrimitiveTopology::LineList;
	d.numRenderTargets      = 3;
	d.rtvFormats[0]         = GfxFormat::R16G16B16A16_Float;
	d.rtvFormats[2]         = GfxFormat::R8G8B8A8_Unorm;
	d.dsvFormat             = GfxFormat::D32_Float;
	d.sampleMask            = 0x0000FFFFu;
	d.flags                 = GfxPipelineStateFlags::ToolDebug;
	return d;
}
} // namespace

TEST_CASE("JSON: パースと文字列化")
{
	GfxJson v;
	std::string err;
	// \" \\ \n と \u エスケープ (サロゲートペアを含む)。MSVC はマクロの引数の raw 文字列の \ を扱えないので変数に置く
	const std::string input = std::string(R"({ "a": [1, 2.5, -3e2, true, null], "s": "x\"y\\z\n)")
		+ "\\" + "u00e9" + "\\" + "ud83d" + "\\" + "ude00" + R"(", "o": {} })";
	const bool parsed = gfxJsonParse(input, v, &err);
	REQUIRE_MESSAGE(parsed, err);
	REQUIRE(v.isObject());
	CHECK(v["a"].size() == 5);
	CHECK(v["a"].at(1).asNumber() == 2.5);
	CHECK(v["a"].at(2).asNumber() == -300.0);
	CHECK(v["a"].at(3).asBool());
	CHECK(v["a"].at(4).isNull());
	CHECK(v["s"].asString() == "x\"y\\z\n\xC3\xA9\xF0\x9F\x98\x80");
	CHECK(v["o"].isObject());
	CHECK(v["missing"].isNull());

	// 文字列化してパースし直すと同じ内容になる
	GfxJson again;
	REQUIRE(gfxJsonParse(gfxJsonStringify(v), again, &err));
	CHECK(gfxJsonStringify(again, 0) == gfxJsonStringify(v, 0));
	const std::string expected = std::string(R"({"a":[1,2.5,-300,true,null],"s":"x\"y\\z\n)") + "\xC3\xA9\xF0\x9F\x98\x80" + R"(","o":{}})";
	CHECK(gfxJsonStringify(v, 0) == expected);

	SUBCASE("エラーは行と列を返す")
	{
		CHECK_FALSE(gfxJsonParse("{\n  \"a\": [1, 2,]\n}", v, &err));
		CHECK(err.rfind("2:", 0) == 0);
		CHECK_FALSE(gfxJsonParse("{\"a\": 1} x", v, &err));
		CHECK_FALSE(gfxJsonParse("\"open", v, &err));
		CHECK_FALSE(gfxJsonParse("01x", v, &err));
	}
}

TEST_CASE("JSON: 既定値の記述子は空のオブジェクトになる")
{
	CHECK(gfxJsonStringify(gfxToJson(GfxGraphicsPipelineDesc{}), 0) == "{}");
	CHECK(gfxJsonStringify(gfxToJson(GfxBindingLayoutDesc{}), 0) == "{}");

	// writeDefaults なら全フィールド (structSize と debugName とハンドルは除く)
	const GfxJson all = gfxToJson(GfxRasterizerDesc{}, GfxJsonWriteOptions{ true, {} });
	CHECK(all["cullMode"].asString() == "Back");
	CHECK(all["depthClipEnable"].asBool());
	const GfxJson pipe = gfxToJson(GfxGraphicsPipelineDesc{}, GfxJsonWriteOptions{ true, {} });
	CHECK(pipe.find("structSize") == nullptr);
	CHECK(pipe.find("debugName")  == nullptr);
	CHECK(pipe.find("vs")         == nullptr);
	CHECK(pipe["version"].asNumber() == GFX_GRAPHICS_PIPELINE_DESC_VERSION);
	CHECK(pipe["rtvFormats"].size() == GFX_MAX_RENDER_TARGETS);
}

TEST_CASE("JSON: グラフィックスパイプラインの記述子を往復できる")
{
	const GfxGraphicsPipelineDesc in = makeNonDefaultPipeline();
	const GfxJson json = gfxToJson(in);

	// 変更したフィールドだけが書かれる
	CHECK(json["rasterizerState"]["cullMode"].asString() == "None");
	CHECK(json["rasterizerState"]["depthBias"].asNumber() == -12);
	CHECK(json["rasterizerState"]["depthBiasClamp"].asNumber() == 0.1);   // float の最短表記
	CHECK(json["rasterizerState"].find("fillMode") == nullptr);
	CHECK(json["blendState"]["renderTarget"].size() == 3);                // 末尾の既定値は省く
	CHECK(json["blendState"]["renderTarget"].at(1).size() == 0);
	CHECK(gfxJsonStringify(json["blendState"]["renderTarget"].at(2)["renderTargetWriteMask"], 0) == R"(["Red","Blue"])");
	CHECK(json["rtvFormats"].size() == 3);
	CHECK(json["inputLayout"]["elements"].at(1)["semanticName"].asString() == "TEXCOORD");
	CHECK(gfxJsonStringify(json["flags"], 0) == R"(["ToolDebug"])");

	const GfxGraphicsPipelineDesc out = roundTrip(in);
	CHECK(sameDesc(in, out));
	CHECK(out.structSize == sizeof(GfxGraphicsPipelineDesc));
	CHECK(out.rasterizerState.depthBiasClamp == 0.1f);
	CHECK(std::strcmp(out.inputLayout.elements[1].semanticName, "TEXCOORD") == 0);

	// writeDefaults でも同じ結果になる
	CHECK(sameDesc(in, roundTrip(in, GfxJsonWriteOptions{ true, {} })));
}

TEST_CASE("JSON: バインディングレイアウトを往復できる")
{
	GfxBindingLayoutDesc in;
	in.beginTable(GfxShaderVisibility::Pixel)
	  .addRange(GfxDescriptorRangeType::SRV, 4, 0)
	  .addRange(GfxDescriptorRangeType::UAV, 1, 2, 1);
	in.addRootDescriptor(GfxRootParameterType::CBV, 0, 0, GfxShaderVisibility::All);
	in.addConstants(1, 4, 0, GfxShaderVisibility::Vertex);
	GfxStaticSamplerDesc sampler;
	sampler.addressU       = GfxTextureAddressMode::Clamp;
	sampler.shaderRegister = 2;
	in.addStaticSampler(sampler);
	in.flags = GfxRootSignatureFlags::AllowInputAssemblerInputLayout;
	CHECK(sameDesc(in, roundTrip(in)));
}

TEST_CASE("JSON: 登録されたすべての型が既定値で往復できる")
{
	uint32_t count = 0;
	const GfxStructInfo* const* list = gfxAllStructInfos(count);
	REQUIRE(count > 20);
	for (uint32_t i = 0; i < count; ++i)
	{
		const GfxStructInfo* si = list[i];
		const std::string name = si->name;
		CAPTURE(name);
		REQUIRE(si->defaultValue != nullptr);
		CHECK(gfxFindStructInfo(si->name) == si);

		const GfxJson json = gfxStructToJson(si, si->defaultValue, GfxJsonWriteOptions{ true, {} });
		GfxJson parsed;
		REQUIRE(gfxJsonParse(gfxJsonStringify(json), parsed));

		std::vector<uint8_t> buf(si->size);
		std::memcpy(buf.data(), si->defaultValue, si->size);
		GfxJsonDiagnostics diag;
		CHECK(gfxStructFromJson(si, parsed, buf.data(), &diag));
		for (const auto& e : diag.errors)
			MESSAGE(name, ": ", e);
		CHECK(diag.errors.empty());
		CHECK(gfx_json_detail::structEquals(si, buf.data(), si->defaultValue));

		// 既定値なら何も書かない
		CHECK(gfxStructToJson(si, si->defaultValue).size() == 0);
	}
}

TEST_CASE("JSON: 読み込みのエラーと警告")
{
	auto load = [](const char* text, GfxGraphicsPipelineDesc& out, GfxJsonDiagnostics& diag, const GfxJsonReadOptions& opts = {}) {
		GfxJson json;
		std::string err;
		REQUIRE_MESSAGE(gfxJsonParse(text, json, &err), err);
		return gfxFromJson(json, out, &diag, opts);
	};

	SUBCASE("列挙子の名前の間違いはエラー。パスが分かる")
	{
		GfxGraphicsPipelineDesc d;
		d.numRenderTargets = 7;
		GfxJsonDiagnostics diag;
		CHECK_FALSE(load(R"({ "numRenderTargets": 1, "rasterizerState": { "cullMode": "Bak" } })", d, diag));
		REQUIRE(diag.errors.size() == 1);
		CHECK(diag.errors[0].rfind("rasterizerState.cullMode:", 0) == 0);
		CHECK(d.numRenderTargets == 7);   // 失敗したら out は変えない
	}
	SUBCASE("型の違い / 配列の長さ / 文字列の長さ / 範囲外")
	{
		GfxGraphicsPipelineDesc d;
		GfxJsonDiagnostics diag;
		CHECK_FALSE(load(R"({ "sampleMask": "all" })", d, diag));
		CHECK_FALSE(load(R"({ "rtvFormats": [1,2,3,4,5,6,7,8,9] })", d, diag));
		CHECK_FALSE(load(R"({ "inputLayout": { "elements": [ { "semanticName": "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789" } ] } })", d, diag));
		CHECK_FALSE(load(R"({ "rasterizerState": { "depthBias": 1.5 } })", d, diag));
		CHECK_FALSE(load(R"({ "depthStencilState": { "stencilReadMask": 256 } })", d, diag));
		CHECK(diag.errors.size() == 5);
		CHECK(diag.errors[1].rfind("rtvFormats:", 0) == 0);
	}
	SUBCASE("知らないフィールドは警告 (unknownFieldIsError ならエラー)")
	{
		GfxGraphicsPipelineDesc d;
		GfxJsonDiagnostics diag;
		CHECK(load(R"({ "futureField": 1, "numRenderTargets": 2 })", d, diag));
		CHECK(d.numRenderTargets == 2);
		CHECK(diag.warnings.size() == 1);

		GfxJsonReadOptions strict;
		strict.unknownFieldIsError = true;
		GfxJsonDiagnostics diag2;
		CHECK_FALSE(load(R"({ "futureField": 1 })", d, diag2, strict));
	}
	SUBCASE("推奨範囲の外は警告")
	{
		GfxGraphicsPipelineDesc d;
		GfxJsonDiagnostics diag;
		CHECK(load(R"({ "sampleCount": 64 })", d, diag));
		CHECK(diag.warnings.size() == 1);
	}
	SUBCASE("フラグは数値や 'A|B' の文字列でもよい。列挙型は数値でもよい")
	{
		GfxGraphicsPipelineDesc d;
		GfxJsonDiagnostics diag;
		REQUIRE(load(R"({ "blendState": { "renderTarget": [ { "renderTargetWriteMask": "Red | Green" }, { "renderTargetWriteMask": 8 } ] },
		                 "rasterizerState": { "cullMode": 2 } })", d, diag));
		CHECK(d.blendState.renderTarget[0].renderTargetWriteMask == (GfxColorWriteMask::Red | GfxColorWriteMask::Green));
		CHECK(d.blendState.renderTarget[1].renderTargetWriteMask == GfxColorWriteMask::Alpha);
		CHECK(d.rasterizerState.cullMode == GfxCullMode::Front);
	}
}

TEST_CASE("JSON: float の特殊値と、ハンドルの変換")
{
	GfxSamplerStateDesc s;
	s.maxLOD     = std::numeric_limits<float>::infinity();
	s.mipLODBias = -0.25f;
	s.borderColor[3] = 1.0f;
	const GfxJson json = gfxToJson(s);
	CHECK(json["maxLOD"].asString() == "inf");
	CHECK(gfxJsonStringify(json["borderColor"], 0) == "[0,0,0,1]");
	const GfxSamplerStateDesc back = roundTrip(s);
	CHECK(std::isinf(back.maxLOD));
	CHECK(back.mipLODBias == -0.25f);
	CHECK(back.borderColor[3] == 1.0f);

	// ハンドルは既定では書かない。変換関数を渡すと名前などに置き換えられる
	GfxGraphicsPipelineDesc p;
	p.vs = ShaderHandle{ 42 };
	p.ps = ShaderHandle{ 43 };
	CHECK(gfxToJson(p).find("vs") == nullptr);

	GfxJsonWriteOptions w;
	w.writeHandle = [](const GfxFieldInfo&, uint64_t id) -> GfxJson {
		return id ? GfxJson("shader_" + std::to_string(id)) : GfxJson();
	};
	GfxJsonReadOptions r;
	r.readHandle = [](const GfxFieldInfo&, const GfxJson& v, uint64_t& id) {
		if (!v.isString() || v.asString().rfind("shader_", 0) != 0) return false;
		id = std::stoull(v.asString().substr(7));
		return true;
	};
	const GfxJson withHandles = gfxToJson(p, w);
	CHECK(withHandles["vs"].asString() == "shader_42");
	CHECK(withHandles.find("gs") == nullptr);   // 0 (無効) は書かない
	const GfxGraphicsPipelineDesc q = roundTrip(p, w, r);
	CHECK(q.vs.id == 42);
	CHECK(q.ps.id == 43);
}

TEST_CASE("スキーマの説明 (エディターのフォーム用)")
{
	const GfxJson d = gfxDescribeStruct(gfxStructInfo<GfxRasterizerDesc>());
	CHECK(d["name"].asString() == "GfxRasterizerDesc");
	const GfxJson* cull = nullptr;
	for (const auto& f : d["fields"].items())
		if (f["name"].asString() == "cullMode")
			cull = &f;
	REQUIRE(cull != nullptr);
	CHECK((*cull)["type"].asString() == "enum");
	CHECK((*cull)["enum"].asString() == "GfxCullMode");
	CHECK((*cull)["default"].asString() == "Back");
	CHECK((*cull)["tooltip"].asString() == "D3D12_RASTERIZER_DESC::CullMode");

	const GfxJson e = gfxDescribeEnum(gfxFindEnumInfo("GfxColorWriteMask"));
	CHECK(e["flags"].asBool());
	CHECK(e["values"].size() == 6);
}
