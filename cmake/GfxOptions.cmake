# ============================================================
#  gfx_options
#  全ターゲット共通のコンパイルオプション。各ターゲットに PRIVATE でリンクする。
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
