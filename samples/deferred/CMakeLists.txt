# ============================================================
#  GfxExample  (ディファードレンダリングのサンプル)
#
#  SunsetGraphicsEngine.dll (gfx_core) にリンクし、
#  gfx_dx12.dll / gfx_vulkan.dll を実行時にロードする。
# ============================================================
add_executable(GfxExample
    main.cpp
    SampleUtils.h       # エラーチェックとスキーマによるダンプ
    ShaderFiles.h       # shaders/*.hlsl の読み込み
    ShaderFiles.cpp
    Win32Window.h       # Win32 ウィンドウとメッセージループ
    Win32Window.cpp
)

# ---- シェーダー ----
# samples/deferred/shaders/*.hlsl を bin/shaders/ にコピーする。
# HLSL だけを編集した場合も反映されるよう、ビルドのたびにコピーする (exe の再リンクは不要)
file(GLOB GFX_EXAMPLE_SHADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.hlsl")
target_sources(GfxExample PRIVATE ${GFX_EXAMPLE_SHADERS})
set_source_files_properties(${GFX_EXAMPLE_SHADERS} PROPERTIES HEADER_FILE_ONLY TRUE)   # VS でコンパイル対象にしない
add_custom_target(GfxExample_shaders ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders" "${CMAKE_BINARY_DIR}/bin/shaders"
    COMMENT "Copying sample shaders to bin/shaders"
    VERBATIM
)
add_dependencies(GfxExample GfxExample_shaders)
install(DIRECTORY shaders DESTINATION "${CMAKE_INSTALL_BINDIR}")

# 外部のアプリと同じく、名前空間付きのターゲットだけを使う
target_link_libraries(GfxExample PRIVATE
    SunsetGraphicsEngine::core
    SunsetGraphicsEngine::schema
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

# ---- 本体 DLL / バックエンド DLL / dxcompiler.dll を exe の横に配置する ----
# (単独ビルドでは同じ bin/ に出力済みなので実質何もしないが、外部のアプリと同じ手順を使う)
sunset_gfx_deploy_runtime(GfxExample)

install(TARGETS GfxExample RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

message(STATUS "[SunsetGraphicsEngine] ターゲット: GfxExample")
