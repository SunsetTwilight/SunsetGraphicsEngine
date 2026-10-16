# ============================================================
#  gfx_vulkan.dll / gfx_vulkan.so / gfx_vulkan.dylib
#
#  ※ 現在のソースは ABI v1 時代のもので、現行の ABI と一致せずビルドできない。
#     ABI v2 で作り直すまで GFX_BUILD_VULKAN は OFF のままにすること。
#
#  依存:
#    Vulkan SDK  https://vulkan.lunarg.com/
#    shaderc_combined または glslang (Vulkan SDK に同梱)
# ============================================================
find_package(Vulkan QUIET)

if(NOT Vulkan_FOUND)
    message(WARNING
        "[gfx_vulkan] Vulkan SDK が見つかりません — スキップします\n"
        "             VULKAN_SDK 環境変数を設定するか\n"
        "             -DVulkan_INCLUDE_DIR=... -DVulkan_LIBRARY=... を指定してください"
    )
    return()
endif()

add_library(gfx_vulkan SHARED
    VulkanBackend.cpp
)

target_include_directories(gfx_vulkan PRIVATE "${Vulkan_INCLUDE_DIRS}")

target_link_libraries(gfx_vulkan PRIVATE
    gfx_abi
    gfx_options
    Vulkan::Vulkan
)

# ---- shaderc / glslang (GLSL → SPIR-V) ----
if(TARGET Vulkan::shaderc_combined)
    target_link_libraries(gfx_vulkan PRIVATE Vulkan::shaderc_combined)
    target_compile_definitions(gfx_vulkan PRIVATE GFX_HAS_SHADERC=1)
    message(STATUS "[gfx_vulkan] シェーダーコンパイラ: shaderc_combined")
elseif(TARGET Vulkan::glslang AND TARGET Vulkan::SPIRV)
    target_link_libraries(gfx_vulkan PRIVATE Vulkan::glslang Vulkan::SPIRV)
    target_compile_definitions(gfx_vulkan PRIVATE GFX_HAS_GLSLANG=1)
    message(STATUS "[gfx_vulkan] シェーダーコンパイラ: glslang + SPIRV")
elseif(DEFINED ENV{VULKAN_SDK})
    # VULKAN_SDK 環境変数から手動検索 (古い SDK 向け fallback)
    if(WIN32)
        set(_sc "$ENV{VULKAN_SDK}/Lib/shaderc_combined.lib")
    else()
        set(_sc "$ENV{VULKAN_SDK}/lib/libshaderc_combined.a")
    endif()
    if(EXISTS "${_sc}")
        target_link_libraries(gfx_vulkan PRIVATE "${_sc}")
        target_compile_definitions(gfx_vulkan PRIVATE GFX_HAS_SHADERC=1)
        message(STATUS "[gfx_vulkan] シェーダーコンパイラ: ${_sc} (fallback)")
    else()
        message(STATUS "[gfx_vulkan] shaderc/glslang 未検出 — シェーダーコンパイルはスタブ実装")
    endif()
else()
    message(STATUS "[gfx_vulkan] shaderc/glslang 未検出 — シェーダーコンパイルはスタブ実装")
endif()

# ---- シンボル可視性 ----
if(WIN32)
    set_target_properties(gfx_vulkan PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)
else()
    # Linux / macOS: デフォルト可視性を hidden にして
    # GFX_EXPORT (__attribute__((visibility("default")))) だけを公開する
    target_compile_options(gfx_vulkan PRIVATE -fvisibility=hidden)
endif()

set_target_properties(gfx_vulkan PROPERTIES
    OUTPUT_NAME "gfx_vulkan"
    PREFIX      ""
)

target_compile_definitions(gfx_vulkan PRIVATE
    GFX_BUILDING_DLL
    # プラットフォーム別サーフェス拡張を有効化
    $<$<PLATFORM_ID:Windows>:VK_USE_PLATFORM_WIN32_KHR>
    $<$<PLATFORM_ID:Windows>:WIN32_LEAN_AND_MEAN>
    $<$<PLATFORM_ID:Windows>:NOMINMAX>
    $<$<PLATFORM_ID:Darwin>:VK_USE_PLATFORM_METAL_EXT>
    $<$<PLATFORM_ID:Linux>:VK_USE_PLATFORM_XCB_KHR>
)

install(TARGETS gfx_vulkan
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"   # Windows .dll
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"   # Linux/macOS .so/.dylib
)

message(STATUS "[SunsetGraphicsEngine] ターゲット: gfx_vulkan (Vulkan ${Vulkan_VERSION})")
