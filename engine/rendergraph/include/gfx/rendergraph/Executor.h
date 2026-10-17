#pragma once
// ============================================================
//  gfx/rendergraph/Executor.h
//
//  コンパイルしたグラフ (CompiledGraph) をコマンドバッファに記録する。
//
//    ・一時リソース: 宣言の大きさ / フォーマット / 使い方が同じテクスチャとバッファを、
//      フレームをまたいで再利用する (しばらく使わなければ破棄する)
//    ・バリア: コンパイルが計画したものを発行する。一時リソースは、最初に使う前に
//      作ったときの状態 (initialState) へ戻す (前のフレームの最後の状態から)
//    ・パス: 出力の設定とクリア、ビューポート / シザー、パイプライン (PipelineLibrary)、
//      bind の指定どおりのバインド、内容 (drawList / fullscreen / dispatch / 間接 / custom / コピー / 解決)
//    ・計測: パスごとにタイムスタンプを書き、数フレーム後に GPU 時間として読む
//
//  M3b の制限 (使うと record がエラーを返す):
//    ・サブリソース単位のバリア (mip / スライスごとに状態が変わるグラフ)
//    ・history / name@prev
//    ・アプリが IRenderTarget / IDepthStencil で渡した出力と、グラフのテクスチャを同じパスの出力に混ぜること
//    ・Sampler のディスクリプタテーブル (サンプラーはバインディングレイアウトの静的サンプラーを使う)
//
//  エンジンと同じスレッドから呼ぶこと。
// ============================================================

#include "gfx/rendergraph/Compiler.h"
#include "gfx/rendergraph/PipelineLibrary.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace gfx::rg
{
// アプリが渡すリソース ($ で始まる名前)。使い方に合うものを 1 つ以上入れる
struct ImportedResource
{
	IRenderTarget* renderTarget = nullptr;   // RTV、SRV (asTexture)。バックバッファなど
	IDepthStencil* depthStencil = nullptr;   // DSV、SRV (asTexture)
	TextureHandle  texture;                  // v2 のテクスチャ (SRV / UAV / RTV / DSV のビューを作れる)
	BufferHandle   buffer;                   // バッファ

	static ImportedResource fromRenderTarget(IRenderTarget* rt) { ImportedResource r; r.renderTarget = rt; return r; }
	static ImportedResource fromDepthStencil(IDepthStencil* ds) { ImportedResource r; r.depthStencil = ds; return r; }
	static ImportedResource fromTexture(TextureHandle t)        { ImportedResource r; r.texture = t; return r; }
	static ImportedResource fromBuffer(BufferHandle b)          { ImportedResource r; r.buffer = b; return r; }
};

// パスの記録中の情報 (IDrawListProvider に渡す)
struct DrawContext
{
	ICommandBuffer*     cb     = nullptr;
	const CompiledPass* pass   = nullptr;
	uint32_t            width  = 0;   // 出力の大きさ (出力がなければ基準の大きさ)
	uint32_t            height = 0;

	const std::string& passName() const { return pass->name; }
	const std::string& view()     const { return pass->desc.view; }
	// 定数の出どころ (source) をつなぐルートパラメーターの番号。なければ -1
	int32_t paramOf(const std::string& source) const
	{
		for (const auto& c : pass->desc.constants)
			if (c.source == source)
				return c.bind.param;
		return -1;
	}
};

// アプリが実装する: シーンの描画と、名前で参照する定数バッファ
class IDrawListProvider
{
public:
	virtual ~IDrawListProvider() = default;

	// content.drawList のパスで呼ぶ。パイプライン、出力、bind した入力と定数は設定済み。
	// 頂点 / インデックスバッファと、オブジェクトごとの定数を設定して描く
	virtual void draw(const DrawListQuery& query, const DrawContext& ctx) = 0;

	// constants の source (inline 以外) に対応する定数バッファ。無効なハンドルを返したら、
	// Executor はバインドしない (draw の中でアプリが ctx.paramOf(source) に設定する)
	virtual BufferHandle constants(const std::string& source, const DrawContext& ctx) { (void)source; (void)ctx; return {}; }

	// content.custom のパスで呼ぶ
	virtual void custom(const std::string& name, const DrawContext& ctx) { (void)name; (void)ctx; }
};

struct FrameInputs
{
	uint32_t                                width  = 0;   // 比率で指定したテクスチャの基準の大きさ
	uint32_t                                height = 0;
	std::map<std::string, ImportedResource> imports;      // "$backbuffer" → ...
	IDrawListProvider*                      provider = nullptr;
};

struct PassTiming
{
	std::string name;
	double      gpuMs = 0.0;
};

class Executor
{
public:
	Executor(GraphicsEngine& engine, PipelineLibrary& pipelines);
	~Executor();
	Executor(const Executor&)            = delete;
	Executor& operator=(const Executor&) = delete;

	// graph をコンパイルした結果 compiled を cb (begin 済み) に記録する。
	// 記録できない内容があれば、何も記録せずに false を返す (理由は errors())
	bool record(ICommandBuffer* cb, const GraphDesc& graph, const CompiledGraph& compiled, const FrameInputs& in);

	// 最後に読めたフレームのパスごとの GPU 時間 (数フレーム遅れる)。全体は totalGpuMs()
	const std::vector<PassTiming>& timings() const { return m_timings; }
	double                         totalGpuMs() const { return m_totalMs; }

	// 一時リソースとビューをすべて破棄する (リサイズの後は自動で作り直すので不要)
	void clear();

	// 一時リソースの数 (テスト / 統計用)
	size_t pooledTextures() const;
	size_t pooledBuffers() const;

	// エラーの記録 (同じものは 1 回だけ)
	const std::vector<std::string>& errors() const { return m_errors; }
	void                            clearErrors()  { m_errors.clear(); m_errorSet.clear(); }

private:
	struct Impl;
	std::unique_ptr<Impl>    m_impl;
	std::vector<PassTiming>  m_timings;
	double                   m_totalMs = 0.0;
	std::vector<std::string> m_errors;
	std::vector<std::string> m_errorSet;

	friend struct Impl;
	void error(const std::string& text);
};
} // namespace gfx::rg
