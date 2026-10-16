# ============================================================
#  外部ライブラリ (FetchContent)
#
#  バージョンはタグ / コミットで固定する。更新するときはここだけを書き換える。
#  初回の configure 時にダウンロードし、以降は build/<preset>/_deps/ のものを使う。
#
#    gfx_imgui          Dear ImGui (docking ブランチ)      … エディターの UI
#    gfx_imnodes        imnodes                           … レンダーグラフのノードエディター
#    nlohmann_json      nlohmann/json                     … グラフ / パイプラインアセットの JSON
# ============================================================
include(FetchContent)

set(GFX_IMGUI_TAG     "v1.92.9-docking")
set(GFX_IMNODES_TAG   "eb36902c892548ef94f88f51ad7e7c9c7058a71c")   # master (タグ v0.5 は古い ImGui 向け)
set(GFX_JSON_VERSION  "v3.12.0")

# ImGui / imnodes は同梱の CMake を使わない (ImGui は持たず、imnodes のものは find_package(imgui) 前提)。
# SOURCE_SUBDIR に存在しないディレクトリを指定して、ソースの取得だけを行い、ターゲットは下で自前で作る。
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        ${GFX_IMGUI_TAG}
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  _gfx_no_cmake_
)
FetchContent_Declare(imnodes
    GIT_REPOSITORY https://github.com/Nelarius/imnodes.git
    GIT_TAG        ${GFX_IMNODES_TAG}
    SOURCE_SUBDIR  _gfx_no_cmake_
)
FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/${GFX_JSON_VERSION}/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_MakeAvailable(imgui imnodes nlohmann_json)

# ---- Dear ImGui (コアのみ。描画はエンジンの ABI の上に実装するため、プラットフォーム / レンダラーのバックエンドは使わない) ----
add_library(gfx_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
)
target_include_directories(gfx_imgui SYSTEM PUBLIC "${imgui_SOURCE_DIR}")
target_compile_definitions(gfx_imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
add_library(gfx::imgui ALIAS gfx_imgui)

# ---- imnodes ----
add_library(gfx_imnodes STATIC ${imnodes_SOURCE_DIR}/imnodes.cpp)
target_include_directories(gfx_imnodes SYSTEM PUBLIC "${imnodes_SOURCE_DIR}")
target_link_libraries(gfx_imnodes PUBLIC gfx_imgui)
add_library(gfx::imnodes ALIAS gfx_imnodes)

message(STATUS "[GfxDependencies] imgui ${GFX_IMGUI_TAG} / imnodes ${GFX_IMNODES_TAG} / nlohmann_json ${GFX_JSON_VERSION}")
