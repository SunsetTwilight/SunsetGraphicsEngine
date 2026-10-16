# ============================================================
#  GraphicsEngine (スタティックライブラリ)
#
#  役割:
#    ・PluginLoader でバックエンド DLL を実行時ロード
#    ・GraphicsEngine / RenderWindow API をアプリに提供
#    ・DX12 / Vulkan のヘッダを一切インクルードしない
#  公開ヘッダー: #include "gfx/core/GraphicsEngine.h"
# ============================================================
add_library(GraphicsEngine STATIC
    src/GraphicsEngine.cpp
)
add_library(gfx::core ALIAS GraphicsEngine)

target_include_directories(GraphicsEngine PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)

target_link_libraries(GraphicsEngine
    PUBLIC  gfx_abi
    PRIVATE gfx_options
)

# Linux / macOS: dlopen は libdl にある
if(UNIX)
    target_link_libraries(GraphicsEngine PUBLIC dl)
endif()

install(TARGETS GraphicsEngine ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}" FILES_MATCHING PATTERN "*.h")
