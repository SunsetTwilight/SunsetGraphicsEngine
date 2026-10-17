// ============================================================
//  engine/rendergraph/src/GraphAsset.cpp
// ============================================================
#include "gfx/rendergraph/GraphAsset.h"
#include "gfx/rendergraph/GraphJson.h"

#include <fstream>
#include <sstream>

namespace gfx::rg
{
// ============================================================
//  ファイル
// ============================================================
std::filesystem::path pathFromUtf8(const std::string& s)
{
	return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

bool readTextFile(const std::filesystem::path& path, std::string& out)
{
	std::ifstream f(path, std::ios::binary);
	if (!f)
		return false;
	std::ostringstream ss;
	ss << f.rdbuf();
	out = ss.str();
	return true;
}

bool writeTextFileAtomic(const std::filesystem::path& path, const std::string& text, std::string* error)
{
	std::error_code ec;
	if (path.has_parent_path())
		std::filesystem::create_directories(path.parent_path(), ec);
	std::filesystem::path tmp = path;
	tmp += ".tmp";
	{
		std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
		if (!f)
		{
			if (error) *error = "書き込めません: " + tmp.string();
			return false;
		}
		f << text;
		if (!f.good())
		{
			if (error) *error = "書き込みに失敗しました: " + tmp.string();
			return false;
		}
	}
	std::filesystem::rename(tmp, path, ec);   // 既存のファイルは置き換える
	if (ec)
	{
		std::filesystem::remove(tmp, ec);
		if (error) *error = "置き換えに失敗しました: " + path.string();
		return false;
	}
	return true;
}

// ============================================================
//  GraphAsset
// ============================================================
GraphAsset::GraphAsset(std::filesystem::path file) : m_watch(std::move(file))
{
}

bool GraphAsset::apply(GraphDesc desc, const CompileOptions& options, const char* what)
{
	CompiledGraph compiled = compile(desc, options);
	for (const auto& d : compiled.diagnostics)
		m_messages.push_back(std::string(d.severity == Diagnostic::Severity::Error ? "error: " : "warning: ") + d.text());
	if (!compiled.ok)
	{
		m_messages.push_back(std::string(what) + ": コンパイルのエラーがあるので、前のグラフのままにします");
		m_lastFailed = true;
		return false;
	}
	m_desc     = std::move(desc);
	m_compiled = std::move(compiled);
	m_valid    = true;
	++m_revision;
	return true;
}

bool GraphAsset::load(const CompileOptions& options)
{
	m_messages.clear();
	m_lastFailed = false;
	m_watch.reset();

	std::string text;
	if (!readTextFile(m_watch.path(), text))
	{
		m_messages.push_back("error: グラフのファイルを読めません: " + m_watch.path().string());
		m_lastFailed = true;
		return false;
	}
	GraphDesc desc;
	GfxJsonDiagnostics diag;
	const bool ok = graphFromString(text, desc, &diag);
	for (const auto& e : diag.errors)   m_messages.push_back("error: " + e);
	for (const auto& w : diag.warnings) m_messages.push_back("warning: " + w);
	if (!ok)
	{
		m_messages.push_back("JSON のエラーがあるので、前のグラフのままにします");
		m_lastFailed = true;
		return false;
	}
	return apply(std::move(desc), options, "読み込み");
}

bool GraphAsset::poll(const CompileOptions& options)
{
	const auto now = FileWatch::Clock::now();
	if (now - m_lastPoll < m_pollInterval)
		return false;
	m_lastPoll = now;
	if (!m_watch.changed(now, m_debounce))
		return false;
	return load(options);
}

bool GraphAsset::recompile(const CompileOptions& options)
{
	m_messages.clear();
	m_lastFailed = false;
	if (!m_valid)
		return false;
	return apply(m_desc, options, "コンパイルし直し");
}

bool GraphAsset::set(GraphDesc desc, const CompileOptions& options)
{
	m_messages.clear();
	m_lastFailed = false;
	return apply(std::move(desc), options, "編集");
}

bool GraphAsset::save()
{
	return saveAs(m_watch.path());
}

bool GraphAsset::saveAs(const std::filesystem::path& file)
{
	m_messages.clear();
	m_lastFailed = false;
	if (!m_valid)
	{
		m_messages.push_back("error: 保存するグラフがありません");
		m_lastFailed = true;
		return false;
	}
	std::string error;
	if (!writeTextFileAtomic(file, graphToString(m_desc), &error))
	{
		m_messages.push_back("error: " + error);
		m_lastFailed = true;
		return false;
	}
	if (file != m_watch.path())
		m_watch = FileWatch(file);   // 以後は新しいファイルを監視する
	else
		m_watch.reset();             // 自分で書いた内容は変更として扱わない
	return true;
}
} // namespace gfx::rg
