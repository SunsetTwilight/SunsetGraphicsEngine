# ============================================================
#  gfx_core → SunsetGraphicsEngine.dll (エンジン本体)
#
#  役割:
#    ・バックエンド DLL (gfx_dx12 など) を実行時ロードし、ABI を照合する
#    ・デバイス / ウィンドウ / switchAPI の管理
#  DLL の境界は C ABI (gfx/core/CoreAPI.h)。C++ API (gfx/core/GraphicsEngine.h) は
#  ヘッダーのみのラッパーで、アプリ側でコンパイルされる。
#  DX12 / Vulkan のヘッダは一切インクルードしない。
# ============================================================
add_library(gfx_core SHARED
    include/gfx/core/CoreAPI.h          # 公開: C ABI
    include/gfx/core/GraphicsEngine.h   # 公開: ヘッダーのみの C++ ラッパー
    src/Engine.cpp
    src/PluginLoader.h                  # 非公開: LoadLibrary / dlopen ラッパー
)
add_library(gfx::core ALIAS gfx_core)

set_target_properties(gfx_core PROPERTIES
    OUTPUT_NAME "SunsetGraphicsEngine"
    PREFIX      ""          # Linux / macOS でも lib プレフィックスを付けない
)

target_include_directories(gfx_core PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)

target_link_libraries(gfx_core
    PUBLIC  gfx_abi
    PRIVATE gfx_options
)

target_compile_definitions(gfx_core PRIVATE GFX_CORE_BUILDING_DLL)

if(UNIX)
    # dlopen は libdl にある。C ABI 以外のシンボルは公開しない
    target_link_libraries(gfx_core PRIVATE dl)
    target_compile_options(gfx_core PRIVATE -fvisibility=hidden)
endif()
