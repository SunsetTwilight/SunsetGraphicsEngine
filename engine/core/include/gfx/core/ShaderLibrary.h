#pragma once
// ============================================================
//  gfx/core/ShaderLibrary.h
//
//  HLSL ファイルからシェーダーを作り、ファイルが変わったら作り直す (ホットリロード)。
//  ヘッダーのみ。GraphicsEngine の公開 API (compileShader / updateShader / shaderDependencies) だけを使う。
//
//  ・load() はファイルを読んでコンパイルし、ハンドルを返す。
//  ・poll() を毎フレーム呼ぶと、本体と #include したファイルの更新時刻を調べ、
//    変わったシェーダーを updateShader で作り直す。ハンドルは変わらず、
//    そのシェーダーを使うパイプラインもエンジンが作り直す。
//  ・コンパイルに失敗したときは元のシェーダーのまま描画を続ける (エラーはデバッグメッセージ)。
//    ファイルを直して保存すれば、もう一度作り直す。
//  ・エディターは保存の途中のファイルを読むことがあるので、更新時刻が変わってから
//    debounce の間 (既定 100ms) 変化がなくなるのを待ってから読む。
//
//  使い方:
//    ShaderLibrary shaders(engine);
//    shaders.addSearchPath("D:/project/shaders");
//    ShaderHandle ps = shaders.load("lighting.ps.hlsl", ShaderStage::Fragment);
//    // フレームループ
//    shaders.poll();
//
//  エンジンと同じスレッドから呼ぶこと。switchAPI の前に clear() し、後で load し直すこと。
// ============================================================

#include "gfx/core/GraphicsEngine.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

class ShaderLibrary
{
public:
	struct Options
	{
		std::string                                      entryPoint = "main";
		std::string                                      profile;              // 空なら <stage>_6_0
		std::vector<std::pair<std::string, std::string>> defines;              // { 名前, 値 }
		std::vector<std::string>                         includeDirs;          // 追加の #include 検索パス
		GfxShaderCompileFlags                            flags = GfxShaderCompileFlags::None;
	};

	struct PollResult
	{
		uint32_t                 reloaded = 0;   // 作り直せた数
		uint32_t                 failed   = 0;   // 作り直せなかった数 (元のまま)
		std::vector<ShaderHandle> handles;       // 作り直しを試みたシェーダー
	};

	// ok = false のときは元のシェーダーのまま
	using ReloadCallback = std::function<void(ShaderHandle handle, const std::string& path, bool ok)>;

	explicit ShaderLibrary(GraphicsEngine& engine) : m_engine(engine) {}
	~ShaderLibrary() { clear(); }
	ShaderLibrary(const ShaderLibrary&)            = delete;
	ShaderLibrary& operator=(const ShaderLibrary&) = delete;

	// 相対パスを探すフォルダ (追加した順に探す)
	void addSearchPath(const std::filesystem::path& dir) { m_searchPaths.push_back(dir); }

	// ファイルからシェーダーを作る。失敗したら無効なハンドル (理由はデバッグメッセージ)。
	// 同じファイル / ステージ / オプションなら、作成済みのハンドルを返す。
	ShaderHandle load(const std::filesystem::path& file, ShaderStage stage, const Options& opts = {})
	{
		const std::filesystem::path path = resolve(file);
		if (path.empty())
		{
			m_lastError = "シェーダーファイルが見つかりません: " + pathString(file);
			return {};
		}
		const std::string key = makeKey(path, stage, opts);
		for (const auto& e : m_entries)
			if (e->key == key)
				return e->handle;

		auto entry = std::make_unique<Entry>();
		entry->key   = key;
		entry->path  = path;
		entry->stage = stage;
		entry->opts  = opts;

		std::string source;
		if (!readFile(path, source))
		{
			m_lastError = "シェーダーファイルを読めません: " + pathString(path);
			return {};
		}
		CompileArgs args(*entry, source);
		entry->handle = m_engine.compileShader(args.desc);
		if (!entry->handle.valid())
		{
			m_lastError = "シェーダーのコンパイルに失敗しました: " + pathString(path);
			return {};
		}
		refreshWatches(*entry);
		const ShaderHandle h = entry->handle;
		m_entries.push_back(std::move(entry));
		return h;
	}

	// 更新されたファイルを調べて作り直す。毎フレーム呼んでよい (minInterval より短い間隔の呼び出しは何もしない)
	PollResult poll()
	{
		PollResult result;
		const auto now = Clock::now();
		if (now - m_lastPoll < m_minInterval)
			return result;
		m_lastPoll = now;

		for (auto& e : m_entries)
		{
			bool changed = false;
			for (auto& w : e->watches)
			{
				const auto t = writeTime(w.path);
				if (t != w.seen)
				{
					// 保存の途中かもしれないので、変化が止まるのを待つ
					w.seen      = t;
					w.changedAt = now;
					w.pending   = true;
				}
				if (w.pending && now - w.changedAt >= m_debounce)
					changed = true;
			}
			if (!changed)
				continue;

			result.handles.push_back(e->handle);
			if (reloadEntry(*e))
				++result.reloaded;
			else
				++result.failed;
		}
		return result;
	}

	// ファイルの更新に関係なく作り直す
	bool reload(ShaderHandle h)
	{
		for (auto& e : m_entries)
			if (e->handle.id == h.id)
				return reloadEntry(*e);
		return false;
	}

	// 作ったシェーダーをすべて破棄する
	void clear()
	{
		for (auto& e : m_entries)
			m_engine.destroyShader(e->handle);
		m_entries.clear();
	}

	void setOnReload(ReloadCallback cb)                     { m_onReload = std::move(cb); }
	void setPollInterval(std::chrono::milliseconds interval) { m_minInterval = interval; }
	void setDebounce(std::chrono::milliseconds debounce)     { m_debounce = debounce; }

	// シェーダーの元のファイル (見つからなければ空)
	std::filesystem::path pathOf(ShaderHandle h) const
	{
		for (const auto& e : m_entries)
			if (e->handle.id == h.id)
				return e->path;
		return {};
	}
	// 監視しているファイル (本体と #include したもの)
	std::vector<std::filesystem::path> watchedFiles(ShaderHandle h) const
	{
		std::vector<std::filesystem::path> out;
		for (const auto& e : m_entries)
			if (e->handle.id == h.id)
				for (const auto& w : e->watches)
					out.push_back(w.path);
		return out;
	}
	size_t             size()      const { return m_entries.size(); }
	const std::string& lastError() const { return m_lastError; }

private:
	using Clock    = std::chrono::steady_clock;
	using FileTime = std::filesystem::file_time_type;

	struct Watch
	{
		std::filesystem::path path;
		FileTime              seen {};
		Clock::time_point     changedAt {};
		bool                  pending = false;
	};
	struct Entry
	{
		std::string           key;
		std::filesystem::path path;
		ShaderStage           stage = ShaderStage::Vertex;
		Options               opts;
		ShaderHandle          handle;
		std::vector<Watch>    watches;
	};

	// Options → GfxShaderDesc (文字列の寿命をこのオブジェクトが持つ)
	struct CompileArgs
	{
		std::string                  pathUtf8;
		std::vector<GfxShaderDefine> defines;
		std::vector<std::string>     includeStorage;
		std::vector<const char*>     includeDirs;
		GfxShaderDesc                desc {};

		CompileArgs(const Entry& e, const std::string& source)
		{
			pathUtf8 = pathString(e.path);
			for (const auto& d : e.opts.defines)
				defines.push_back({ d.first.c_str(), d.second.empty() ? nullptr : d.second.c_str() });
			includeStorage = e.opts.includeDirs;
			for (const auto& dir : includeStorage)
				includeDirs.push_back(dir.c_str());

			desc.stage           = e.stage;
			desc.source          = source.c_str();
			desc.entryPoint      = e.opts.entryPoint.c_str();
			desc.sourceName      = pathUtf8.c_str();
			desc.targetProfile   = e.opts.profile.empty() ? nullptr : e.opts.profile.c_str();
			desc.defines         = defines.empty() ? nullptr : defines.data();
			desc.defineCount     = static_cast<uint32_t>(defines.size());
			desc.includeDirs     = includeDirs.empty() ? nullptr : includeDirs.data();
			desc.includeDirCount = static_cast<uint32_t>(includeDirs.size());
			desc.flags           = e.opts.flags;
		}
	};

	static std::string pathString(const std::filesystem::path& p)
	{
		const auto u8 = p.u8string();
		return std::string(u8.begin(), u8.end());
	}

	static std::filesystem::path pathFromUtf8(const std::string& s)
	{
#if defined(__cpp_char8_t)
		return std::filesystem::path(std::u8string(s.begin(), s.end()));
#else
		return std::filesystem::u8path(s);
#endif
	}

	static FileTime writeTime(const std::filesystem::path& p)
	{
		std::error_code ec;
		const FileTime t = std::filesystem::last_write_time(p, ec);
		return ec ? FileTime{} : t;
	}

	static bool readFile(const std::filesystem::path& p, std::string& out)
	{
		std::ifstream in(p, std::ios::binary);
		if (!in)
			return false;
		out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
		return true;
	}

	static std::string makeKey(const std::filesystem::path& path, ShaderStage stage, const Options& o)
	{
		std::string k = pathString(path) + "|" + std::to_string(static_cast<uint32_t>(stage)) + "|" + o.entryPoint + "|" + o.profile
			+ "|" + std::to_string(static_cast<uint32_t>(o.flags));
		for (const auto& d : o.defines)     k += "|D" + d.first + "=" + d.second;
		for (const auto& i : o.includeDirs) k += "|I" + i;
		return k;
	}

	std::filesystem::path resolve(const std::filesystem::path& file) const
	{
		std::error_code ec;
		if (file.is_absolute())
			return std::filesystem::exists(file, ec) ? std::filesystem::weakly_canonical(file, ec) : std::filesystem::path();
		for (const auto& dir : m_searchPaths)
		{
			const auto p = dir / file;
			if (std::filesystem::exists(p, ec))
				return std::filesystem::weakly_canonical(p, ec);
		}
		if (std::filesystem::exists(file, ec))
			return std::filesystem::weakly_canonical(file, ec);
		return {};
	}

	// 監視するファイル = 本体 + エンジンが記録した #include のファイル
	void refreshWatches(Entry& e)
	{
		std::vector<std::filesystem::path> files { e.path };
		for (const auto& dep : m_engine.shaderDependencies(e.handle))
			files.push_back(pathFromUtf8(dep));

		std::vector<Watch> watches;
		for (const auto& f : files)
		{
			Watch w;
			w.path = f;
			w.seen = writeTime(f);
			watches.push_back(std::move(w));
		}
		e.watches = std::move(watches);
	}

	bool reloadEntry(Entry& e)
	{
		for (auto& w : e.watches)
			w.pending = false;

		std::string source;
		bool ok = readFile(e.path, source);
		if (ok)
		{
			CompileArgs args(e, source);
			ok = m_engine.updateShader(e.handle, args.desc);
		}
		if (ok)
			refreshWatches(e);   // #include が増減していることがある
		else
			m_lastError = "シェーダーを作り直せませんでした (元のまま): " + pathString(e.path);
		if (m_onReload)
			m_onReload(e.handle, pathString(e.path), ok);
		return ok;
	}

	GraphicsEngine&                     m_engine;
	std::vector<std::filesystem::path>  m_searchPaths;
	std::vector<std::unique_ptr<Entry>> m_entries;
	ReloadCallback                      m_onReload;
	std::string                         m_lastError;
	Clock::time_point                   m_lastPoll {};
	std::chrono::milliseconds           m_minInterval { 100 };
	std::chrono::milliseconds           m_debounce    { 100 };
};
