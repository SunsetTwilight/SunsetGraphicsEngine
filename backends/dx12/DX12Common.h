#pragma once
// ============================================================
//  backends/dx12/DX12Common.h
//  DX12 バックエンド共通の include と前方宣言
// ============================================================

#include "gfx/abi/GfxBackendABI.h"
#include <cassert>
#include <d3d12.h>
#include <string>

// DXC (DirectX Shader Compiler) - GFX_DXC_DIR が設定されている場合のみ有効
#ifdef GFX_HAS_DXC
#include <dxcapi.h>
#endif
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <deque>
#include <algorithm>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;

// ============================================================
//  前方宣言
// ============================================================
class DX12DeviceImpl;
class DX12CommandBufferImpl;
class DX12RenderTargetImpl;
class DX12DepthStencilImpl;
class DX12SwapChainImpl;
class DX12PipelineImpl;
