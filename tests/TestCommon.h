#pragma once
// ============================================================
//  GPU を使うテストの共通部分
//    ・環境変数 GFX_TEST_ADAPTER=software で WARP (ソフトウェアレンダラー) を使う
//    ・デバッグメッセージを重大度で分けて記録する
// ============================================================
#include "gfx/core/GraphicsEngine.h"

#include <cstdlib>
#include <string>
#include <vector>

namespace gfxtest
{
inline bool useSoftwareAdapter()
{
	char   buf[32] = {};
	size_t len     = 0;
	return getenv_s(&len, buf, sizeof(buf), "GFX_TEST_ADAPTER") == 0 && len > 0
		&& std::string(buf) == "software";
}

// デバッグメッセージの記録
struct MessageLog
{
	struct Entry
	{
		GfxMessageSeverity severity;
		int32_t            id;
		std::string        text;
	};
	std::vector<Entry> entries;

	// Corruption / Error / Warning (ただし除外する id は数えない)
	std::vector<std::string> problems(std::initializer_list<int32_t> ignoreIds = {}) const
	{
		std::vector<std::string> out;
		for (const auto& e : entries)
		{
			if (e.severity > GfxMessageSeverity::Warning)
				continue;
			bool ignored = false;
			for (int32_t id : ignoreIds)
				ignored |= (e.id == id);
			if (!ignored)
				out.push_back("#" + std::to_string(e.id) + " " + e.text);
		}
		return out;
	}
	std::vector<std::string> infos() const
	{
		std::vector<std::string> out;
		for (const auto& e : entries)
			if (e.severity > GfxMessageSeverity::Warning)
				out.push_back("#" + std::to_string(e.id) + " " + e.text);
		return out;
	}
	// 指定した id のメッセージ
	std::vector<std::string> withId(int32_t id) const
	{
		std::vector<std::string> out;
		for (const auto& e : entries)
			if (e.id == id)
				out.push_back(e.text);
		return out;
	}
};

inline DeviceConfig makeConfig(MessageLog& log, const char* appName)
{
	DeviceConfig cfg;
	cfg.appName    = appName;
	cfg.validation = true;
	cfg.adapter    = useSoftwareAdapter() ? GfxAdapterType::Software : GfxAdapterType::Hardware;
	cfg.debugMessage = [&log](GfxMessageSeverity sev, int32_t id, const char* text) {
		log.entries.push_back({ sev, id, text ? text : "" });
	};
	return cfg;
}
} // namespace gfxtest
