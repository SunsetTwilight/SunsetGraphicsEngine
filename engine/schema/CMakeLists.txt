# ============================================================
#  gfx_schema (ヘッダーのみ)
#  ABI v2 記述子のフィールドのメタデータ表 (エディターのインスペクターと JSON 入出力の土台)。
#  公開ヘッダー: #include "gfx/schema/GfxSchema.h"
# ============================================================
add_library(gfx_schema INTERFACE)
add_library(gfx::schema ALIAS gfx_schema)

target_include_directories(gfx_schema INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_link_libraries(gfx_schema INTERFACE gfx_abi)

install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}" FILES_MATCHING PATTERN "*.h")
