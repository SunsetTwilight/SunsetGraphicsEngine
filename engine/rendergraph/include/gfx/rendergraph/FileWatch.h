#pragma once
// ============================================================
//  gfx/rendergraph/FileWatch.h
//
//  ファイルの更新時刻を見て、変更を知らせる小さな部品 (GraphAsset / PipelineLibrary が使う)。
//  エディターは保存の途中のファイルを読むことがあるので、更新時刻が変わってから
//  debounce の間、変化がなくなるのを待ってから「変わった」と返す (ShaderLibrary と同じ考え方)。
// ============================================================

#include <chrono>
#include <filesystem>
#include <system_error>

namespace gfx::rg
{
class FileWatch
{
public:
	using Clock    = std::chrono::steady_clock;
	using FileTime = std::filesystem::file_time_type;

	FileWatch() = default;
	explicit FileWatch(std::filesystem::path path) : m_path(std::move(path)) { m_seen = writeTime(m_path); }

	const std::filesystem::path& path() const { return m_path; }

	// 今の更新時刻を「見た」ことにする (自分で保存したときなど)
	void reset()
	{
		m_seen    = writeTime(m_path);
		m_pending = false;
	}

	// 変わってから debounce の間、変化が止まっていれば true (true は 1 回だけ返す)
	bool changed(Clock::time_point now, std::chrono::milliseconds debounce)
	{
		const FileTime t = writeTime(m_path);
		if (t != m_seen)
		{
			m_seen      = t;
			m_changedAt = now;
			m_pending   = true;
		}
		if (m_pending && now - m_changedAt >= debounce)
		{
			m_pending = false;
			return true;
		}
		return false;
	}

	static FileTime writeTime(const std::filesystem::path& p)
	{
		std::error_code ec;
		const FileTime t = std::filesystem::last_write_time(p, ec);
		return ec ? FileTime::min() : t;
	}

private:
	std::filesystem::path m_path;
	FileTime              m_seen      = FileTime::min();
	Clock::time_point     m_changedAt = {};
	bool                  m_pending   = false;
};
} // namespace gfx::rg
