# ============================================================
#  GfxGraphSample  (レンダーグラフで描くディファードのサンプル)
#
#  描画の流れは assets/ の JSON (グラフ / パイプライン) と HLSL に書いてある。
#  ビルドツリーの exe はソースの assets/ を直接読む (編集がそのまま反映される)。
#  install した exe は bin/rendergraph_assets/ を使う。
# ============================================================
add_executable(GfxGraphSample main.cpp)

file(GLOB_RECURSE GFX_GRAPH_SAMPLE_ASSETS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/*.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/*.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/*.hlsli")
target_sources(GfxGraphSample PRIVATE ${GFX_GRAPH_SAMPLE_ASSETS})
set_source_files_properties(${GFX_GRAPH_SAMPLE_ASSETS} PROPERTIES HEADER_FILE_ONLY TRUE)

target_compile_definitions(GfxGraphSample PRIVATE
    "GFX_GRAPH_SAMPLE_ASSET_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/assets\"")

target_link_libraries(GfxGraphSample PRIVATE
    SunsetGraphicsEngine::core
    SunsetGraphicsEngine::rendergraph
    gfx_options
)

set_target_properties(GfxGraphSample PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
sunset_gfx_deploy_runtime(GfxGraphSample)

install(TARGETS GfxGraphSample RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY assets/ DESTINATION "${CMAKE_INSTALL_BINDIR}/rendergraph_assets")

message(STATUS "[SunsetGraphicsEngine] ターゲット: GfxGraphSample")
