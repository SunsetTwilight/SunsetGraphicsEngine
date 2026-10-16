// ============================================================
//  editor/src/main.cpp  (Phase 4 までのスタブ)
//
//  外部ライブラリ (ImGui / imnodes / nlohmann_json) とエンジンのライブラリが
//  正しく取得・リンクできることを確認するだけの実行ファイル。
//  エディター本体は docs/design/editor-architecture.md の Phase 4 で実装する。
// ============================================================
#include "gfx/core/GraphicsEngine.h"
#include "gfx/schema/GfxSchema.h"

#include <imgui.h>
#include <imnodes.h>
#include <nlohmann/json.hpp>

#include <iostream>

int main()
{
	ImGui::CreateContext();
	ImNodes::CreateContext();

	nlohmann::json info = {
		{ "imgui",   IMGUI_VERSION },
		{ "json",    { NLOHMANN_JSON_VERSION_MAJOR, NLOHMANN_JSON_VERSION_MINOR, NLOHMANN_JSON_VERSION_PATCH } },
		{ "gfxAbi",  GFX_ABI_VERSION },
	};
	std::cout << "[GfxEditor] stub: " << info.dump() << "\n";

	ImNodes::DestroyContext();
	ImGui::DestroyContext();
	return 0;
}
