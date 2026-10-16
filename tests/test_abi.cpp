// ============================================================
//  GPU を使わないテスト: ABI 情報、フラグ判定、スキーマの表
// ============================================================
#include <doctest/doctest.h>
#include "gfx/abi/GfxBackendABI.h"
#include "gfx/schema/GfxSchema.h"

#include <cstring>

TEST_CASE("ABI 情報はヘッダーの値と一致する")
{
	const GfxABIInfo info = gfxCurrentABIInfo();
	CHECK(info.abiVersion         == GFX_ABI_VERSION);
	CHECK(info.sizeofDeviceVTable == sizeof(GfxDeviceVTable));
	CHECK(info.sizeofDeviceConfig == sizeof(GfxDeviceConfig));
	CHECK(info == gfxCurrentABIInfo());

	GfxABIInfo other = info;
	other.abiVersion += 1;
	CHECK_FALSE(other == info);
}

TEST_CASE("BufferUsage のフラグ判定")
{
	const BufferUsage u = BufferUsage::Vertex | BufferUsage::Uniform;
	CHECK(hasUsage(u, BufferUsage::Vertex));
	CHECK(hasUsage(u, BufferUsage::Uniform));
	CHECK_FALSE(hasUsage(u, BufferUsage::Index));
	CHECK((u & BufferUsage::Uniform) == BufferUsage::Uniform);
}

TEST_CASE("スキーマ: グラフィックスパイプラインの記述子のフィールド表")
{
	const GfxStructInfo* si = gfxStructInfo<GfxGraphicsPipelineDesc>();
	REQUIRE(si != nullptr);
	CHECK(si->fieldCount > 0);

	// X マクロから生成した表に、構造体のメンバーと同じオフセットで載っていること
	const GfxFieldInfo* found = nullptr;
	for (uint32_t i = 0; i < si->fieldCount; ++i)
		if (std::strcmp(si->fields[i].name, "numRenderTargets") == 0)
			found = &si->fields[i];
	REQUIRE(found != nullptr);
	CHECK(found->offset == offsetof(GfxGraphicsPipelineDesc, numRenderTargets));
}
