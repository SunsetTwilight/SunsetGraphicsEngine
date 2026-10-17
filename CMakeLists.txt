cmake_minimum_required(VERSION 3.22)
project(SunsetGraphicsEngine VERSION 0.3.0 LANGUAGES CXX)

# ============================================================
#  ディレクトリ構成と依存の向き
#
#    engine/abi          SunsetGraphicsEngine::abi     DLL 境界の C ABI (何にも依存しない)
#    engine/schema       SunsetGraphicsEngine::schema  記述子のメタデータ表      → abi
#    engine/core         SunsetGraphicsEngine::core    本体 DLL (SunsetGraphicsEngine.dll) → abi
#    engine/rendergraph  (Phase 3)                                               → core, schema
#    backends/dx12       SunsetGraphicsEngine::dx12    gfx_dx12.dll              → abi のみ
#    backends/vulkan     SunsetGraphicsEngine::vulkan  gfx_vulkan                → abi のみ
#    samples/*           サンプルアプリ                                          → core, schema
#
#  backends は core を知らない。アプリは core / schema にリンクし、
#  バックエンドは本体 DLL が実行時にロードする。
#
#  このプロジェクトは単独でビルドすることも、別のプロジェクトから
#  find_package(SunsetGraphicsEngine) / FetchContent / add_subdirectory で使うこともできる。
#  アプリの exe の横に DLL を配置するには sunset_gfx_deploy_runtime(<target>) を使う。
# ============================================================

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ============================================================
#  C++ 標準 (このディレクトリ以下にだけ適用される)
# ============================================================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ============================================================
#  成果物の出力先 (単独でビルドするときのみ)
#  DLL・EXE をすべて build/<type>/bin/ に集約する。
#  これにより LoadLibrary / dlopen がパス指定なしで DLL を発見できる。
#  別のプロジェクトに取り込まれたときは、取り込み側の設定を尊重して何もしない。
# ============================================================
if(PROJECT_IS_TOP_LEVEL)
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
endif()

# ============================================================
#  ビルドオプション
#  サンプルは単独でビルドするときだけ既定で ON にする
# ============================================================
option(GFX_BUILD_DX12        "DirectX 12 バックエンド DLL をビルドする (Windows 専用)" ${WIN32})
option(GFX_BUILD_VULKAN      "Vulkan バックエンド DLL をビルドする (現在は ABI と不一致でビルド不可)" OFF)
option(GFX_BUILD_EXAMPLE     "サンプルアプリをビルドする"                               ${PROJECT_IS_TOP_LEVEL})
option(GFX_BUILD_TESTS       "自動テスト (doctest) をビルドする。configure 時に doctest を取得する" ${PROJECT_IS_TOP_LEVEL})
option(GFX_ENABLE_VALIDATION "GPU 検証レイヤー / デバッグレイヤーを有効にする"          OFF)
option(GFX_INSTALL           "install / export の規則を生成する"                         ON)

# DXC のルートディレクトリ (省略時は公式リリースを FetchContent で取得する。cmake/GfxDxc.cmake)
set(GFX_DXC_DIR "" CACHE PATH
    "DirectX Shader Compiler のルートディレクトリ (inc/ と bin/x64/ を含む)。空なら自動で取得する")

include(GNUInstallDirs)
include(GfxOptions)                        # gfx_options (警告・定義の共通設定)
include(GfxDxc)                            # DXC の取得 (GFX_DXC_FOUND / GFX_DXC_INCLUDE_DIR / GFX_DXC_RUNTIME_FILES)
include(SunsetGraphicsEngineRuntime)       # sunset_gfx_deploy_runtime()

# 実行時に必要な追加ファイル (dxcompiler.dll / dxil.dll) を配置関数に登録する
if(GFX_DXC_RUNTIME_FILES)
    set_property(GLOBAL APPEND PROPERTY SUNSET_GFX_RUNTIME_EXTRA_FILES ${GFX_DXC_RUNTIME_FILES})
endif()

# ============================================================
#  サブディレクトリ
# ============================================================
add_subdirectory(engine)
add_subdirectory(backends)

if(GFX_BUILD_EXAMPLE)
    add_subdirectory(samples)
endif()

if(GFX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ============================================================
#  パッケージ (find_package(SunsetGraphicsEngine) 用)
# ============================================================
if(GFX_INSTALL)
    include(CMakePackageConfigHelpers)

    set(_gfx_cmake_dir "${CMAKE_INSTALL_LIBDIR}/cmake/SunsetGraphicsEngine")

    install(EXPORT SunsetGraphicsEngineTargets
        FILE        SunsetGraphicsEngineTargets.cmake
        NAMESPACE   SunsetGraphicsEngine::
        DESTINATION "${_gfx_cmake_dir}"
    )
    configure_package_config_file(
        cmake/SunsetGraphicsEngineConfig.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/SunsetGraphicsEngineConfig.cmake"
        INSTALL_DESTINATION "${_gfx_cmake_dir}"
    )
    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/SunsetGraphicsEngineConfigVersion.cmake"
        VERSION       ${PROJECT_VERSION}
        # 0.x の間はマイナーバージョンで API が変わりうるため、マイナーまで一致を要求する
        COMPATIBILITY SameMinorVersion
    )
    install(FILES
            "${CMAKE_CURRENT_BINARY_DIR}/SunsetGraphicsEngineConfig.cmake"
            "${CMAKE_CURRENT_BINARY_DIR}/SunsetGraphicsEngineConfigVersion.cmake"
            cmake/SunsetGraphicsEngineRuntime.cmake
        DESTINATION "${_gfx_cmake_dir}"
    )
    # dxcompiler.dll / dxil.dll もアプリと一緒に配置できるよう同梱する
    if(GFX_DXC_RUNTIME_FILES)
        install(FILES ${GFX_DXC_RUNTIME_FILES} DESTINATION "${CMAKE_INSTALL_BINDIR}")
    endif()
endif()

# ============================================================
#  ビルド設定サマリー
# ============================================================
message(STATUS "")
message(STATUS "=========================================")
message(STATUS "  SunsetGraphicsEngine ${PROJECT_VERSION}")
message(STATUS "  コンパイラ    : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  ビルドタイプ  : ${CMAKE_BUILD_TYPE}")
message(STATUS "  単独ビルド    : ${PROJECT_IS_TOP_LEVEL}")
message(STATUS "  gfx_dx12      : ${GFX_BUILD_DX12}")
message(STATUS "  gfx_vulkan    : ${GFX_BUILD_VULKAN}")
message(STATUS "  バリデーション: ${GFX_ENABLE_VALIDATION}")
message(STATUS "  サンプル      : ${GFX_BUILD_EXAMPLE}")
message(STATUS "  テスト        : ${GFX_BUILD_TESTS}")
if(GFX_DXC_FOUND)
    message(STATUS "  DXC           : ${GFX_DXC_INCLUDE_DIR}")
endif()
message(STATUS "=========================================")
message(STATUS "")
