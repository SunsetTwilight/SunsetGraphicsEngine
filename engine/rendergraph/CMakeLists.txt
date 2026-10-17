# ============================================================
#  SunsetGraphicsEngine::rendergraph (静的ライブラリ、C++)
#
#  レンダーグラフ。設計: docs/design/editor-architecture.md の「Phase 3」「M3 の詳細設計」
#    M3a: グラフのデータ (GraphDesc)、JSON との相互変換 (GraphJson)、コンパイル (Compiler)。GPU を使わない
#    M3b 以降: アセットの読み込み (AssetLibrary)、実行 (Executor)
#
#  エンジンの公開 API だけを使う (バックエンドは参照しない)。C++ の型を使うので、
#  アプリと同じコンパイラ / 構成 (Debug / Release) でビルドしたものをリンクすること。
#  公開ヘッダー: #include "gfx/rendergraph/RenderGraph.h"
# ============================================================
add_library(gfx_rendergraph STATIC
    include/gfx/rendergraph/RenderGraph.h   # まとめて読み込む
    include/gfx/rendergraph/GraphDesc.h     # グラフのデータ
    include/gfx/rendergraph/GraphJson.h     # JSON との相互変換
    include/gfx/rendergraph/Compiler.h      # コンパイル (順序 / カリング / 寿命 / バリア / bind の検証)
    src/GraphJson.cpp
    src/Compiler.cpp
)
add_library(gfx::rendergraph ALIAS gfx_rendergraph)
add_library(SunsetGraphicsEngine::rendergraph ALIAS gfx_rendergraph)
set_target_properties(gfx_rendergraph PROPERTIES EXPORT_NAME rendergraph)

target_include_directories(gfx_rendergraph PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_compile_features(gfx_rendergraph PUBLIC cxx_std_20)
target_link_libraries(gfx_rendergraph
    PUBLIC  gfx_schema
    PRIVATE $<BUILD_INTERFACE:gfx_options>   # 警告の設定はこのライブラリのビルドにだけ使う (export しない)
)

if(GFX_INSTALL)
    install(TARGETS gfx_rendergraph EXPORT SunsetGraphicsEngineTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    )
    install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}" FILES_MATCHING PATTERN "*.h")
endif()
