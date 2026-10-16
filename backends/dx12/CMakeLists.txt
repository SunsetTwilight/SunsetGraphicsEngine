# ============================================================
#  gfx_dx12.dll  (Windows 専用)
#
#  エクスポート:
#    GFX_GetABIInfo()      → ABI バージョン / 構造体サイズ
#    GFX_CreateBackend()   → IBackendPlugin*
#    GFX_DestroyBackend()  → void
#
#  依存 (Windows SDK に同梱):
#    d3d12.lib / dxgi.lib / dxguid.lib / d3dcompiler.lib
# ============================================================
add_library(gfx_dx12 SHARED
    # 宣言 (DX12Device.h がすべてを include する)
    DX12Common.h          # 共通の include と前方宣言
    DX12Utils.h           # フォーマット変換と小さなヘルパー
    DX12Descriptors.h     # ディスクリプタヒープと一時リング
    DX12CommandBuffer.h
    DX12Resources.h       # RenderTarget / DepthStencil とリソースの補助構造体
    DX12SwapChain.h
    DX12Pipeline.h
    DX12Device.h
    # 実装
    DX12Export.cpp        # vtable と DLL のエクスポート関数
    DX12Device.cpp        # 初期化 / submit / 遅延解放 / ディスクリプタ / バッファ / Compute / クエリ
    DX12Pipeline.cpp      # グラフィックスパイプラインとバインディングレイアウト
    DX12Raytracing.cpp    # BLAS / TLAS / RT パイプライン
    DX12CommandBuffer.cpp # コマンドの記録と状態追跡
    DX12Resources.cpp     # テクスチャ / ビュー / サンプラー、RT / DS
    DX12SwapChain.cpp
)

target_link_libraries(gfx_dx12 PRIVATE
    gfx_abi
    gfx_options
    d3d12
    dxgi
    dxguid
    d3dcompiler
)

# ---- DXC (オプション) ----
# 見つかれば HLSL → DXIL (DXR の lib_6_x など) のコンパイルを有効にする
if(GFX_DXC_FOUND)
    if(GFX_DXC_INCLUDE_DIR)
        target_include_directories(gfx_dx12 PRIVATE "${GFX_DXC_INCLUDE_DIR}")
    endif()
    target_link_libraries(gfx_dx12 PRIVATE "${GFX_DXC_LIBRARY}")
    target_compile_definitions(gfx_dx12 PRIVATE GFX_HAS_DXC=1)
endif()

# ---- シンボル可視性 ----
# WINDOWS_EXPORT_ALL_SYMBOLS=OFF にすることで
# __declspec(dllexport) を付けた関数だけをエクスポートする
set_target_properties(gfx_dx12 PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS OFF
    OUTPUT_NAME "gfx_dx12"
    PREFIX      ""          # lib プレフィックスを付けない
)

target_compile_definitions(gfx_dx12 PRIVATE
    GFX_BUILDING_DLL
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX               # <windows.h> の min/max マクロを抑制
)

install(TARGETS gfx_dx12 RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

message(STATUS "[SunsetGraphicsEngine] ターゲット: gfx_dx12.dll")
