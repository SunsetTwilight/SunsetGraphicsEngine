#pragma once
// ============================================================
//  gfx/rendergraph/PipelineLibrary.h
//
//  パイプラインのアセット (pipelines/*.json) を読み、GPU のオブジェクトを作って再利用する。
//  3D モデルやテクスチャは扱わない (それはアプリ側の仕事。設計ドキュメントの「M3 の詳細設計」)。
//
//  アセットの形:
//    {
//      "version": 1,
//      "type": "graphics",                       // "graphics" (既定) / "compute"
//      "shaders": {
//        "vs": { "file": "shaders/gbuffer.vs.hlsl" },
//        "ps": { "file": "shaders/gbuffer.ps.hlsl", "entry": "main", "profile": "ps_6_0",
//                "defines": { "USE_NORMAL_MAP": "1" } }
//      },                                        // compute は "cs"
//      "layout": { ... GfxBindingLayoutDesc の JSON ... },
//      "state":  { ... GfxGraphicsPipelineDesc の JSON (ハンドルは書かない) ... }
//    }
//
//  ・パスはプロジェクトのフォルダ (setRoot) からの相対パス。区切りは "/"。
//  ・RTV / DSV のフォーマット、枚数、サンプル数は、パイプラインを使うパスの出力から決める
//    (PipelineTargets)。state に書いても上書きする。出力が違えば別の PSO を作る。
//  ・シェーダーは ShaderLibrary で作るので、.hlsl の変更は ShaderLibrary::poll() で反映される。
//    JSON の変更の反映は M3c。
//  ・エンジンと同じスレッドから呼ぶこと。
// ============================================================

#include "gfx/core/ShaderLibrary.h"
#include "gfx/schema/GfxJson.h"

#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace gfx::rg
{
// パスの出力から決まる、パイプラインのフォーマット
struct PipelineTargets
{
	uint32_t  numRenderTargets = 0;
	GfxFormat rtvFormats[GFX_MAX_RENDER_TARGETS] = {};
	GfxFormat dsvFormat   = GfxFormat::Unknown;
	uint32_t  sampleCount = 1;

	bool operator==(const PipelineTargets&) const = default;
};

struct ShaderStageAsset
{
	std::string                                      file;      // 空 = このステージは使わない
	std::string                                      entry = "main";
	std::string                                      profile;   // 空 = <stage>_6_0
	std::vector<std::pair<std::string, std::string>> defines;

	bool used() const { return !file.empty(); }
	bool operator==(const ShaderStageAsset&) const = default;
};

struct PipelineAsset
{
	bool                    compute = false;
	ShaderStageAsset        vs, ps, cs;
	GfxBindingLayoutDesc    layout;
	GfxGraphicsPipelineDesc state;   // ハンドル、debugName、出力のフォーマットは使わない
};

// JSON との相互変換。diag のパスは "shaders.vs.file"、"layout.parameters[0]"、"state.rasterizerState.cullMode" の形
bool    pipelineAssetFromJson(const GfxJson& json, PipelineAsset& out, GfxJsonDiagnostics* diag = nullptr);
GfxJson pipelineAssetToJson(const PipelineAsset& asset);

class PipelineLibrary
{
public:
	PipelineLibrary(GraphicsEngine& engine, ShaderLibrary& shaders);
	~PipelineLibrary();
	PipelineLibrary(const PipelineLibrary&)            = delete;
	PipelineLibrary& operator=(const PipelineLibrary&) = delete;

	// プロジェクトのフォルダ。ShaderLibrary の検索パスにも加える
	void setRoot(const std::filesystem::path& root);
	const std::filesystem::path& root() const { return m_root; }

	// アセットを読む (GPU は使わない)。読めなければ nullptr (理由は errors())
	const PipelineAsset*        asset(const std::string& path);
	// コンパイルの bind の検証用 (CompileOptions::bindingLayout に渡す)
	const GfxBindingLayoutDesc* bindingLayout(const std::string& path);

	// GPU のオブジェクト。作れなければ無効なハンドル (理由は errors())。同じものは作り直さない
	BindingLayoutHandle layoutHandle(const std::string& path);
	PipelineHandle      graphics(const std::string& path, const PipelineTargets& targets);
	PipelineHandle      compute(const std::string& path);

	// 作ったものをすべて破棄する (switchAPI の前など)
	void clear();

	// 失敗の記録 (同じ失敗は 1 回だけ記録する)
	const std::vector<std::string>& errors() const { return m_errors; }
	void                            clearErrors()  { m_errors.clear(); }

private:
	struct Variant
	{
		PipelineTargets targets;
		bool            computeVariant = false;
		PipelineHandle  handle;
		bool            failed = false;
	};
	struct Entry
	{
		std::string          path;
		bool                 loaded = false;   // JSON を読んだ (失敗も含む)
		bool                 ok     = false;
		PipelineAsset        asset;
		BindingLayoutHandle  layout;
		bool                 layoutFailed = false;
		std::vector<Variant> variants;
		std::string          debugName;
	};

	Entry*       entry(const std::string& path);
	ShaderHandle loadShader(Entry& e, const ShaderStageAsset& s, ShaderStage stage, const char* key);
	void         error(const std::string& text);

	GraphicsEngine&                     m_engine;
	ShaderLibrary&                      m_shaders;
	std::filesystem::path               m_root;
	std::vector<std::unique_ptr<Entry>> m_entries;
	std::vector<std::string>            m_errors;
};
} // namespace gfx::rg
