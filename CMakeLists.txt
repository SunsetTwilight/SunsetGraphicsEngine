cmake_minimum_required(VERSION 3.22)
project(GraphicsEngine VERSION 1.0.0 LANGUAGES CXX)

# ============================================================
#  C++ 標準
# ============================================================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ============================================================
#  成果物の出力先
#  DLL・EXE をすべて build/<type>/bin/ に集約する。
#  これにより LoadLibrary / dlopen がパス指定なしで DLL を発見できる。
# ============================================================
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# マルチコンフィグ (Visual Studio / Xcode) でも bin/ に揃える
foreach(_cfg Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER ${_cfg} _CFG)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_CFG} "${CMAKE_BINARY_DIR}/bin")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${_CFG} "${CMAKE_BINARY_DIR}/bin")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${_CFG} "${CMAKE_BINARY_DIR}/lib")
endforeach()

# ============================================================
#  ビルドオプション
# ============================================================
option(GFX_BUILD_DX12        "DirectX 12 バックエンド DLL をビルドする (Windows 専用)" ${WIN32})
option(GFX_BUILD_VULKAN      "Vulkan バックエンド DLL をビルドする"                     OFF)
option(GFX_BUILD_EXAMPLE     "サンプルアプリをビルドする"                               ON)
option(GFX_ENABLE_VALIDATION "GPU 検証レイヤー / デバッグレイヤーを有効にする"          OFF)

# DXC のルートディレクトリ (省略時は HLSL コンパイルがスタブ実装になる)
set(GFX_DXC_DIR "" CACHE PATH
    "DirectX Shader Compiler のルートディレクトリ (inc/ と lib/x64/ を含む)")

# ============================================================
#  共通インターフェースライブラリ (コンパイルオプション)
#  各ターゲットに PRIVATE でリンクして設定を伝播させる
# ============================================================
add_library(gfx_options INTERFACE)

target_compile_options(gfx_options INTERFACE
    # MSVC
    $<$<CXX_COMPILER_ID:MSVC>:/W4>           # 警告レベル4
    $<$<CXX_COMPILER_ID:MSVC>:/WX>           # 警告をエラーに
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8>        # ソースファイルを UTF-8 として扱う
    $<$<CXX_COMPILER_ID:MSVC>:/permissive->  # 準拠モード
    $<$<CXX_COMPILER_ID:MSVC>:/Zc:__cplusplus>  # __cplusplus を正確に定義
    $<$<CXX_COMPILER_ID:MSVC>:/MP>           # 並列コンパイル
    # GCC / Clang
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wextra>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wpedantic>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wno-unused-parameter>
)

target_compile_definitions(gfx_options INTERFACE
    $<$<CONFIG:Debug>:_DEBUG>
    $<$<CONFIG:RelWithDebInfo>:_DEBUG>
    $<$<BOOL:${GFX_ENABLE_VALIDATION}>:GFX_VALIDATION=1>
)

# ============================================================
#  include ディレクトリ
# ============================================================
set(GFX_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")

# ============================================================
#  GraphicsEngine  (スタティックライブラリ)
#
#  役割:
#    ・PluginLoader で DLL を実行時ロード
#    ・GraphicsEngine / RenderWindow API をアプリに提供
#    ・DX12 / Vulkan のヘッダを一切インクルードしない
# ============================================================
add_library(GraphicsEngine STATIC
    src/GraphicsEngine.cpp
)

target_include_directories(GraphicsEngine
    PUBLIC "${GFX_INCLUDE_DIR}"
)

target_link_libraries(GraphicsEngine
    PRIVATE gfx_options
)

# Linux / macOS: dlopen は libdl にある
if(UNIX)
    target_link_libraries(GraphicsEngine PUBLIC dl)
endif()

# ============================================================
#  gfx_dx12.dll  (Windows 専用)
#
#  エクスポート:
#    GFX_CreateBackend()   → IBackendPlugin*
#    GFX_DestroyBackend()  → void
#
#  依存 (Windows SDK に同梱):
#    d3d12.lib / dxgi.lib / dxguid.lib
# ============================================================
if(GFX_BUILD_DX12)
    if(NOT WIN32)
        message(WARNING "[gfx_dx12] Windows 専用のためスキップします")
    else()
        add_library(gfx_dx12 SHARED
            plugins/dx12/DX12Backend.cpp
        )

        target_include_directories(gfx_dx12 PRIVATE "${GFX_INCLUDE_DIR}")

        target_link_libraries(gfx_dx12 PRIVATE
            gfx_options
            d3d12
            dxgi
            dxguid
            d3dcompiler
        )

        # ---- DXC (オプション) ----
        # GFX_DXC_DIR を指定すると HLSL → DXIL のコンパイルが有効になる
        if(GFX_DXC_DIR)
            set(_dxc_inc "${GFX_DXC_DIR}/inc")
            if(CMAKE_SIZEOF_VOID_P EQUAL 8)
                set(_dxc_lib "${GFX_DXC_DIR}/lib/x64/dxcompiler.lib")
            else()
                set(_dxc_lib "${GFX_DXC_DIR}/lib/x86/dxcompiler.lib")
            endif()

            if(EXISTS "${_dxc_inc}/dxcapi.h" AND EXISTS "${_dxc_lib}")
                target_include_directories(gfx_dx12 PRIVATE "${_dxc_inc}")
                target_link_libraries(gfx_dx12 PRIVATE "${_dxc_lib}")
                target_compile_definitions(gfx_dx12 PRIVATE GFX_HAS_DXC=1)
                message(STATUS "[gfx_dx12] DXC: ${_dxc_lib}")
            else()
                message(WARNING "[gfx_dx12] GFX_DXC_DIR=${GFX_DXC_DIR} に DXC が見つかりません")
            endif()
        else()
            # VULKAN_SDK / Windows Kits に同梱されている DXC を自動検索
            find_library(GFX_DXC_LIB dxcompiler
                PATHS
                    "$ENV{VULKAN_SDK}/Lib"
                    "$ENV{VK_SDK_PATH}/Lib"
                NO_DEFAULT_PATH
            )
            if(GFX_DXC_LIB)
                target_link_libraries(gfx_dx12 PRIVATE "${GFX_DXC_LIB}")
                target_compile_definitions(gfx_dx12 PRIVATE GFX_HAS_DXC=1)
                message(STATUS "[gfx_dx12] DXC (auto): ${GFX_DXC_LIB}")
            else()
                message(STATUS "[gfx_dx12] DXC 未検出 — HLSL コンパイルはスタブ実装")
                message(STATUS "           有効にするには -DGFX_DXC_DIR=<path> を指定してください")
            endif()
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

        message(STATUS "[GraphicsEngine] ターゲット: gfx_dx12.dll")
    endif()
endif()

# ============================================================
#  gfx_vulkan.dll / libgfx_vulkan.so / libgfx_vulkan.dylib
#
#  エクスポート:
#    GFX_CreateBackend()   → IBackendPlugin*
#    GFX_DestroyBackend()  → void
#
#  依存:
#    Vulkan SDK  https://vulkan.lunarg.com/
#    shaderc_combined または glslang (Vulkan SDK に同梱)
# ============================================================
if(GFX_BUILD_VULKAN)
    find_package(Vulkan QUIET)

    if(NOT Vulkan_FOUND)
        message(WARNING
            "[gfx_vulkan] Vulkan SDK が見つかりません — スキップします\n"
            "             VULKAN_SDK 環境変数を設定するか\n"
            "             -DVulkan_INCLUDE_DIR=... -DVulkan_LIBRARY=... を指定してください"
        )
    else()
        add_library(gfx_vulkan SHARED
            plugins/vulkan/VulkanBackend.cpp
        )

        target_include_directories(gfx_vulkan PRIVATE
            "${GFX_INCLUDE_DIR}"
            "${Vulkan_INCLUDE_DIRS}"
        )

        target_link_libraries(gfx_vulkan PRIVATE
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
        else()
            # VULKAN_SDK 環境変数から手動検索 (古い SDK 向け fallback)
            if(DEFINED ENV{VULKAN_SDK})
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
        endif()

        # ---- シンボル可視性 ----
        if(WIN32)
            set_target_properties(gfx_vulkan PROPERTIES
                WINDOWS_EXPORT_ALL_SYMBOLS OFF
            )
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

        message(STATUS "[GraphicsEngine] ターゲット: gfx_vulkan (Vulkan ${Vulkan_VERSION})")
    endif()
endif()

# ============================================================
#  GfxExample  (サンプル実行ファイル)
#
#  GraphicsEngine.lib をスタティックリンクし、
#  gfx_dx12.dll / gfx_vulkan.dll を実行時にロードする。
# ============================================================
if(GFX_BUILD_EXAMPLE)
    add_executable(GfxExample
        examples/main.cpp
    )

    target_include_directories(GfxExample PRIVATE "${GFX_INCLUDE_DIR}")

    target_link_libraries(GfxExample PRIVATE
        GraphicsEngine
        gfx_options
    )

    # Linux: 実行時に同ディレクトリの .so を探す ($ORIGIN)
    if(UNIX AND NOT APPLE)
        set_target_properties(GfxExample PROPERTIES
            BUILD_RPATH   "$ORIGIN"
            INSTALL_RPATH "$ORIGIN/../lib"
        )
    endif()

    # ---- Visual Studio / Xcode のデバッグ作業ディレクトリ ----
    # F5 実行時に bin/ が CWD になるため DLL を発見できる
    set_target_properties(GfxExample PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY  "${CMAKE_BINARY_DIR}/bin"
        XCODE_SCHEME_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )

    # ---- ビルド後コピー (DLL を EXE と同じ bin/ へ) ----
    # CMAKE_RUNTIME_OUTPUT_DIRECTORY に既に揃っているが、
    # add_dependencies でビルド順序を明示する
    if(GFX_BUILD_DX12 AND WIN32 AND TARGET gfx_dx12)
        add_dependencies(GfxExample gfx_dx12)
    endif()
    if(GFX_BUILD_VULKAN AND TARGET gfx_vulkan)
        add_dependencies(GfxExample gfx_vulkan)
    endif()

    # DXC が有効な場合、dxcompiler.dll を exe フォルダに自動コピーする
    if(GFX_DXC_DIR)
        set(_dxc_dll "${GFX_DXC_DIR}/bin/x64/dxcompiler.dll")
        if(EXISTS "${_dxc_dll}")
            add_custom_command(TARGET GfxExample POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_dxc_dll}"
                    "$<TARGET_FILE_DIR:GfxExample>/dxcompiler.dll"
                COMMENT "Copying dxcompiler.dll to output directory"
            )
            message(STATUS "[GfxExample] dxcompiler.dll を自動コピーします: ${_dxc_dll}")
        endif()
    endif()

    message(STATUS "[GraphicsEngine] ターゲット: GfxExample")
endif()

# ============================================================
#  インストール規則
# ============================================================
include(GNUInstallDirs)

# ヘッダとスタティックライブラリ
install(TARGETS GraphicsEngine
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
)
install(DIRECTORY "${GFX_INCLUDE_DIR}/"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/GraphicsEngine"
    FILES_MATCHING PATTERN "*.h"
)

# DLL / 共有ライブラリ
if(GFX_BUILD_DX12 AND WIN32 AND TARGET gfx_dx12)
    install(TARGETS gfx_dx12
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    )
endif()
if(GFX_BUILD_VULKAN AND TARGET gfx_vulkan)
    install(TARGETS gfx_vulkan
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"   # Windows .dll
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"   # Linux/macOS .so/.dylib
    )
endif()

# サンプル
if(GFX_BUILD_EXAMPLE AND TARGET GfxExample)
    install(TARGETS GfxExample
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    )
endif()

# ============================================================
#  ビルド設定サマリー
# ============================================================
message(STATUS "")
message(STATUS "=========================================")
message(STATUS "  GraphicsEngine ${PROJECT_VERSION}")
message(STATUS "  コンパイラ    : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  ビルドタイプ  : ${CMAKE_BUILD_TYPE}")
message(STATUS "  出力先        : ${CMAKE_BINARY_DIR}/bin")
message(STATUS "  gfx_dx12      : ${GFX_BUILD_DX12}")
message(STATUS "  gfx_vulkan    : ${GFX_BUILD_VULKAN}")
message(STATUS "  バリデーション: ${GFX_ENABLE_VALIDATION}")
message(STATUS "  サンプル      : ${GFX_BUILD_EXAMPLE}")
if(GFX_DXC_DIR)
    message(STATUS "  DXC_DIR       : ${GFX_DXC_DIR}")
endif()
message(STATUS "=========================================")
message(STATUS "")
