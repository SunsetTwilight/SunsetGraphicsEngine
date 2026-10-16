// ============================================================
//  ABI の合わないバックエンド DLL が拒否されることを確認する
//  (実行ファイルと同じディレクトリに、ABI のバージョンだけが違う偽の gfx_dx12.dll がある)
// ============================================================
#include <doctest/doctest.h>
#include "gfx/core/GraphicsEngine.h"

#include <string>

TEST_CASE("ABI のバージョンが違うバックエンド DLL は読み込まれない")
{
	GraphicsEngine engine;
	DeviceConfig   cfg;
	cfg.api = GraphicsAPI::DirectX12;

	CHECK_FALSE(engine.init(cfg));
	CHECK_FALSE(engine.isInitialized());
	CHECK(engine.device() == nullptr);

	const std::string err = engine.lastError();
	MESSAGE("lastError: ", err);
	CHECK(err.find("ABI") != std::string::npos);
}
