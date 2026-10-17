#pragma once
// ============================================================
//  gfx/rendergraph/GraphAsset.h
//
//  グラフのアセット (graphs/*.json) のファイル。読み込み、コンパイル、変更の監視、保存をまとめる。
//  GPU は使わない (コンパイルの bind の検証に PipelineLibrary のレイアウトを渡すのはアプリ)。
//
//    ・load / poll / set は、読み込みかコンパイルに失敗したら前のグラフのまま (valid のまま) にして、
//      理由を errors() に入れる。ファイルを直して保存すれば、もう一度読み直す。
//    ・切り替えるたびに revision() が増える (アプリは変わったことをこれで知る)。
//    ・save は一時ファイルに書いてから置き換える (監視している側が書きかけを読まないように)。
//
//  使い方:
//    gfx::rg::GraphAsset graph(root / "graphs/deferred.json");
//    graph.load(options);
//    // フレームごと
//    if (graph.poll(options)) { /* 新しいグラフに切り替わった */ }
//    executor.record(cb, graph.desc(), graph.compiled(), in);
// ============================================================

#include "gfx/rendergraph/Compiler.h"
#include "gfx/rendergraph/FileWatch.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace gfx::rg
{
// テキストファイルの読み書き (UTF-8)。書き込みは一時ファイル経由で置き換える
bool readTextFile(const std::filesystem::path& path, std::string& out);
bool writeTextFileAtomic(const std::filesystem::path& path, const std::string& text, std::string* error = nullptr);
// UTF-8 の文字列からパスを作る
std::filesystem::path pathFromUtf8(const std::string& s);

class GraphAsset
{
public:
	explicit GraphAsset(std::filesystem::path file);

	// ファイルを読んでコンパイルする。失敗したら前のグラフのまま false
	bool load(const CompileOptions& options = {});
	// ファイルが変わっていたら load する。新しいグラフに切り替えたら true
	bool poll(const CompileOptions& options = {});
	// 同じグラフをコンパイルし直す (パイプラインのレイアウトが変わったときなど)。失敗したら前のまま false
	bool recompile(const CompileOptions& options = {});
	// エディターで編集したグラフに切り替える (ファイルには書かない)。失敗したら前のまま false
	bool set(GraphDesc desc, const CompileOptions& options = {});

	// 今のグラフをファイルに書く。書いた内容は変更として扱わない
	bool save();
	bool saveAs(const std::filesystem::path& file);

	bool                 valid()    const { return m_valid; }      // 一度でも読めてコンパイルできた
	const GraphDesc&     desc()     const { return m_desc; }
	const CompiledGraph& compiled() const { return m_compiled; }
	uint64_t             revision() const { return m_revision; }
	const std::filesystem::path& file() const { return m_watch.path(); }

	// 最後の load / poll / recompile / set / save の問題 (エラーと警告。失敗したときはエラーがある)
	const std::vector<std::string>& messages() const { return m_messages; }
	bool                            lastFailed() const { return m_lastFailed; }
	// messages / lastFailed を消す (表示し終えたとき)
	void                            clearStatus()      { m_messages.clear(); m_lastFailed = false; }

	void setDebounce(std::chrono::milliseconds d)     { m_debounce = d; }
	void setPollInterval(std::chrono::milliseconds d) { m_pollInterval = d; }

private:
	bool apply(GraphDesc desc, const CompileOptions& options, const char* what);

	FileWatch                 m_watch;
	GraphDesc                 m_desc;
	CompiledGraph             m_compiled;
	bool                      m_valid      = false;
	bool                      m_lastFailed = false;
	uint64_t                  m_revision   = 0;
	std::vector<std::string>  m_messages;
	std::chrono::milliseconds m_debounce{ 100 };
	std::chrono::milliseconds m_pollInterval{ 250 };
	FileWatch::Clock::time_point m_lastPoll{};
};
} // namespace gfx::rg
