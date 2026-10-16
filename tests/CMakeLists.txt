# ============================================================
#  自動テスト (doctest + CTest)
#
#    gfx_tests_unit    GPU 不要 (ABI 情報、フラグ判定、スキーマの表)
#    gfx_tests_render  ヘッドレスのオフスクリーン描画。デバッグレイヤーのメッセージ 0 件を確認する
#                      環境変数 GFX_TEST_ADAPTER=software で WARP を使う (GPU のない CI 用)
#    gfx_tests_reject  ABI の合わないバックエンド DLL が拒否されることを確認する
#
#  実行: ctest --test-dir build/<preset> --output-on-failure
# ============================================================
include(FetchContent)

# doctest はヘッダー 1 つだけを使う (同梱の CMake はテストやサンプルもビルドするため使わない)
set(GFX_DOCTEST_VERSION "2.5.2")
FetchContent_Declare(doctest
    URL      https://github.com/doctest/doctest/archive/refs/tags/v${GFX_DOCTEST_VERSION}.tar.gz
    URL_HASH SHA256=9189960c2bbbc4f3382ce0773b2bb5f13e3afd8fed47f55f193e11e85a4f9854
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR  _gfx_no_cmake_
)
FetchContent_MakeAvailable(doctest)

add_library(gfx_doctest INTERFACE)
target_include_directories(gfx_doctest SYSTEM INTERFACE "${doctest_SOURCE_DIR}")   # #include <doctest/doctest.h>

# ---- GPU 不要のテスト ----
add_executable(gfx_tests_unit
    doctest_main.cpp
    test_abi.cpp
)
target_link_libraries(gfx_tests_unit PRIVATE
    SunsetGraphicsEngine::abi SunsetGraphicsEngine::schema gfx_doctest gfx_options)
add_test(NAME unit COMMAND gfx_tests_unit)

# ---- 描画テスト (GPU または WARP) ----
add_executable(gfx_tests_render
    doctest_main.cpp
    test_render.cpp
)
target_link_libraries(gfx_tests_render PRIVATE
    SunsetGraphicsEngine::core gfx_doctest gfx_options)
sunset_gfx_deploy_runtime(gfx_tests_render)
add_test(NAME render COMMAND gfx_tests_render)
set_tests_properties(render PROPERTIES LABELS "gpu")

# ---- ABI 不一致の拒否 ----
# 実行ファイルと「ABI の合わない偽の gfx_dx12.dll」を専用のディレクトリに置き、本体 DLL だけをコピーする
set(_reject_dir "${CMAKE_CURRENT_BINARY_DIR}/reject")

add_library(gfx_tests_fake_backend SHARED reject/FakeBackend.cpp)
target_link_libraries(gfx_tests_fake_backend PRIVATE SunsetGraphicsEngine::abi gfx_options)
set_target_properties(gfx_tests_fake_backend PROPERTIES OUTPUT_NAME "gfx_dx12" PREFIX "")

add_executable(gfx_tests_reject
    doctest_main.cpp
    reject/test_reject.cpp
)
target_link_libraries(gfx_tests_reject PRIVATE SunsetGraphicsEngine::core gfx_doctest gfx_options)
add_dependencies(gfx_tests_reject gfx_tests_fake_backend)

foreach(_t gfx_tests_fake_backend gfx_tests_reject)
    set_target_properties(${_t} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${_reject_dir}"
        LIBRARY_OUTPUT_DIRECTORY "${_reject_dir}"
        ARCHIVE_OUTPUT_DIRECTORY "${_reject_dir}"   # インポートライブラリが本物の gfx_dx12.lib と衝突しないように
    )
    foreach(_cfg Debug Release RelWithDebInfo MinSizeRel)
        string(TOUPPER ${_cfg} _CFG)
        set_target_properties(${_t} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY_${_CFG} "${_reject_dir}"
            LIBRARY_OUTPUT_DIRECTORY_${_CFG} "${_reject_dir}"
            ARCHIVE_OUTPUT_DIRECTORY_${_CFG} "${_reject_dir}"
        )
    endforeach()
endforeach()

add_custom_command(TARGET gfx_tests_reject POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:SunsetGraphicsEngine::core>" "$<TARGET_FILE_DIR:gfx_tests_reject>"
    VERBATIM
)
add_test(NAME reject COMMAND gfx_tests_reject)
