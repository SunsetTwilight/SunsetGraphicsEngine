# ============================================================
#  SunsetGraphicsEngine::abi (ヘッダーのみ)
#  EXE / 本体 DLL / バックエンド DLL の境界になる C ABI。他のモジュールに依存しない。
#  公開ヘッダー: #include "gfx/abi/GfxBackendABI.h"
# ============================================================
add_library(gfx_abi INTERFACE)
add_library(gfx::abi ALIAS gfx_abi)
add_library(SunsetGraphicsEngine::abi ALIAS gfx_abi)
set_target_properties(gfx_abi PROPERTIES EXPORT_NAME abi)

target_include_directories(gfx_abi INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
# ABI のヘッダーは C++20 の機能を使う
target_compile_features(gfx_abi INTERFACE cxx_std_20)

if(GFX_INSTALL)
    install(TARGETS gfx_abi EXPORT SunsetGraphicsEngineTargets)
    install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}" FILES_MATCHING PATTERN "*.h")
endif()
