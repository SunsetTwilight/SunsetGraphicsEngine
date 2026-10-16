// ============================================================
//  ABI の合わない偽のバックエンド DLL (gfx_tests_reject 専用)
//  gfx_dx12.dll という名前で出力し、ABI のバージョンだけを食い違わせる。
//  本体はこの DLL を拒否し、GFX_CreateBackend は呼ばれないはず。
// ============================================================
#include "gfx/abi/GfxBackendABI.h"

extern "C" GFX_EXPORT void GFX_GetABIInfo(GfxABIInfo* out)
{
	if (!out)
		return;
	*out = gfxCurrentABIInfo();
	out->abiVersion = GFX_ABI_VERSION + 1000u;
}

extern "C" GFX_EXPORT IBackendPlugin* GFX_CreateBackend()
{
	return nullptr;
}

extern "C" GFX_EXPORT void GFX_DestroyBackend(IBackendPlugin*)
{
}
