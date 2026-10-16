cmake_minimum_required(VERSION 3.22)
project(SunsetGraphicsEngine VERSION 1.0.0 LANGUAGES CXX)

# ============================================================
#  ディレクトリ構成と依存の向き
#
#    engine/abi          gfx_abi        DLL 境界の C ABI (何にも依存しない)
#    engine/schema       gfx_schema     記述子のメタデータ表        → abi
#    engine/core         GraphicsEngine プラグインのロード / ウィンドウ → abi
#    engine/rendergraph  (Phase 3)                                  → core, schema
#    backends/dx12       gfx_dx12.dll                               → abi のみ
#    backends/vulkan     gfx_vulkan                                 → abi のみ
#    samples/*           サンプルアプリ                              → core, schema
#    editor              GfxEditor (Phase 4)                        → core, schema (backends は参照しない)
#
#  backends は core を知らない。editor は backends を直接参照しない。
# ============================================================

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

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
option(GFX_BUILD_VULKAN      "Vulkan バックエンド DLL をビルドする (現在は ABI と不一致でビルド不可)" OFF)
option(GFX_BUILD_EXAMPLE     "サンプルアプリをビルドする"                               ON)
option(GFX_BUILD_EDITOR      "エディター (Phase 4) をビルドする。外部ライブラリを FetchContent で取得する" OFF)
option(GFX_ENABLE_VALIDATION "GPU 検証レイヤー / デバッグレイヤーを有効にする"          OFF)

# DXC のルートディレクトリ (省略時は自動検出。見つからなければ DXR のシェーダーコンパイルがスタブになる)
set(GFX_DXC_DIR "" CACHE PATH
    "DirectX Shader Compiler のルートディレクトリ (inc/ と lib/x64/ を含む)")

include(GfxOptions)   # gfx_options (警告・定義の共通設定)
include(GfxDxc)       # DXC の検出 (GFX_DXC_FOUND / GFX_DXC_INCLUDE_DIR / GFX_DXC_LIBRARY / GFX_DXC_DLL)

# ============================================================
#  サブディレクトリ
# ============================================================
add_subdirectory(engine)
add_subdirectory(backends)

if(GFX_BUILD_EXAMPLE)
    add_subdirectory(samples)
endif()

if(GFX_BUILD_EDITOR)
    add_subdirectory(editor)
endif()

# ============================================================
#  ビルド設定サマリー
# ============================================================
message(STATUS "")
message(STATUS "=========================================")
message(STATUS "  SunsetGraphicsEngine ${PROJECT_VERSION}")
message(STATUS "  コンパイラ    : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  ビルドタイプ  : ${CMAKE_BUILD_TYPE}")
message(STATUS "  出力先        : ${CMAKE_BINARY_DIR}/bin")
message(STATUS "  gfx_dx12      : ${GFX_BUILD_DX12}")
message(STATUS "  gfx_vulkan    : ${GFX_BUILD_VULKAN}")
message(STATUS "  バリデーション: ${GFX_ENABLE_VALIDATION}")
message(STATUS "  サンプル      : ${GFX_BUILD_EXAMPLE}")
message(STATUS "  エディター    : ${GFX_BUILD_EDITOR}")
if(GFX_DXC_FOUND)
    message(STATUS "  DXC           : ${GFX_DXC_LIBRARY}")
endif()
message(STATUS "=========================================")
message(STATUS "")
