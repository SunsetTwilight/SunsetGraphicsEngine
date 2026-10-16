# ============================================================
#  GfxExample  (ディファードレンダリングのサンプル)
#
#  GraphicsEngine.lib をスタティックリンクし、
#  gfx_dx12.dll / gfx_vulkan.dll を実行時にロードする。
# ============================================================
add_executable(GfxExample
    main.cpp
)

target_link_libraries(GfxExample PRIVATE
    GraphicsEngine
    gfx_schema
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

# ---- ビルド順序: バックエンド DLL を先にビルドする ----
# (exe はバックエンドにリンクしない。実行時に同じ bin/ からロードする)
if(TARGET gfx_dx12)
    add_dependencies(GfxExample gfx_dx12)
endif()
if(TARGET gfx_vulkan)
    add_dependencies(GfxExample gfx_vulkan)
endif()

# DXC が見つかっていれば dxcompiler.dll を exe フォルダにコピーする
gfx_copy_dxc_dll(GfxExample)

install(TARGETS GfxExample RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

message(STATUS "[GraphicsEngine] ターゲット: GfxExample")
