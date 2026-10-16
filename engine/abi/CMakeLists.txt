# ============================================================
#  gfx_abi (ヘッダーのみ)
#  EXE とバックエンド DLL の境界になる C ABI。他のモジュールに依存しない。
#  公開ヘッダー: #include "gfx/abi/GfxBackendABI.h"
# ============================================================
add_library(gfx_abi INTERFACE)
add_library(gfx::abi ALIAS gfx_abi)

target_include_directories(gfx_abi INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)

install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}" FILES_MATCHING PATTERN "*.h")
