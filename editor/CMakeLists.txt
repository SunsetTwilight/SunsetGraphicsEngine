# ============================================================
#  GfxEditor (Phase 4)
#
#  依存: core / schema と外部ライブラリ (ImGui / imnodes / JSON)。
#  バックエンド (gfx_dx12 など) は直接参照しない。実行時に GraphicsEngine 経由でロードする。
#
#  現在は外部ライブラリの取得とリンクを確認するための最小限の実行ファイル。
#  エディター本体は Phase 4 で実装する (docs/design/editor-architecture.md)。
# ============================================================
include(GfxDependencies)
include(GNUInstallDirs)

add_executable(GfxEditor
    src/main.cpp
)

target_link_libraries(GfxEditor PRIVATE
    gfx_core
    gfx_schema
    gfx_imgui
    gfx_imnodes
    nlohmann_json::nlohmann_json
    gfx_options
)

if(TARGET gfx_dx12)
    add_dependencies(GfxEditor gfx_dx12)
endif()

set_target_properties(GfxEditor PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

install(TARGETS GfxEditor RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

message(STATUS "[SunsetGraphicsEngine] ターゲット: GfxEditor (スタブ)")
