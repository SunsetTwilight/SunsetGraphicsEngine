# ============================================================
#  DXC (DirectX Shader Compiler) の取得
#
#  DX12 バックエンドはすべての HLSL を DXC でコンパイルする (DXIL、既定は SM 6.0)。
#  dxcompiler.dll は実行時に LoadLibrary するので、リンクするライブラリはない。
#
#  取得元:
#    ・GFX_DXC_DIR を指定した場合はそこを使う (inc/dxcapi.h と bin/<arch>/dxcompiler.dll を含むフォルダ)
#    ・指定しなければ、公式リリース (microsoft/DirectXShaderCompiler) のアーカイブを
#      FetchContent で取得する。バージョンと SHA256 はここで固定する。
#
#  出力変数:
#    GFX_DXC_FOUND          見つかったら TRUE
#    GFX_DXC_INCLUDE_DIR    dxcapi.h / d3d12shader.h のあるディレクトリ
#    GFX_DXC_RUNTIME_FILES  実行時に exe の横に置くファイル (dxcompiler.dll、dxil.dll)
#
#  ライセンス: DXC は University of Illinois/NCSA + MIT (アーカイブの LICENSE-*.txt)。再配布可。
# ============================================================
set(GFX_DXC_FOUND         FALSE)
set(GFX_DXC_INCLUDE_DIR   "")
set(GFX_DXC_RUNTIME_FILES "")

if(NOT WIN32)
    return()
endif()

set(GFX_DXC_RELEASE "v1.8.2505")
set(GFX_DXC_URL     "https://github.com/microsoft/DirectXShaderCompiler/releases/download/${GFX_DXC_RELEASE}/dxc_2025_05_24.zip")
set(GFX_DXC_SHA256  "81380f3eca156d902d6404fd6df9f4b0886f576ff3e18b2cc10d3075ffc9d119")

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_gfx_dxc_arch x64)
else()
    set(_gfx_dxc_arch x86)
endif()

if(GFX_DXC_DIR)
    set(_gfx_dxc_root "${GFX_DXC_DIR}")
    set(_gfx_dxc_from "GFX_DXC_DIR")
else()
    include(FetchContent)
    # アーカイブには CMakeLists.txt がないので、ソースの取得だけを行う
    FetchContent_Declare(gfx_dxc
        URL      "${GFX_DXC_URL}"
        URL_HASH SHA256=${GFX_DXC_SHA256}
        # zip のディレクトリの時刻を復元できずに展開が失敗することがあるので、展開した時刻を使う
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE
        SOURCE_SUBDIR _gfx_no_cmake_
    )
    FetchContent_MakeAvailable(gfx_dxc)
    set(_gfx_dxc_root "${gfx_dxc_SOURCE_DIR}")
    set(_gfx_dxc_from "${GFX_DXC_RELEASE} (FetchContent)")
endif()

set(_inc "${_gfx_dxc_root}/inc")
set(_bin "${_gfx_dxc_root}/bin/${_gfx_dxc_arch}")
if(EXISTS "${_inc}/dxcapi.h" AND EXISTS "${_inc}/d3d12shader.h" AND EXISTS "${_bin}/dxcompiler.dll")
    set(GFX_DXC_FOUND         TRUE)
    set(GFX_DXC_INCLUDE_DIR   "${_inc}")
    set(GFX_DXC_RUNTIME_FILES "${_bin}/dxcompiler.dll")
    # dxil.dll は DXIL の署名に使う (ないと PSO の作成に失敗する)
    if(EXISTS "${_bin}/dxil.dll")
        list(APPEND GFX_DXC_RUNTIME_FILES "${_bin}/dxil.dll")
    else()
        message(WARNING "[GfxDxc] ${_bin}/dxil.dll がありません。DXIL が署名されず、PSO の作成に失敗します")
    endif()
    message(STATUS "[GfxDxc] DXC: ${_gfx_dxc_root} (${_gfx_dxc_from})")
else()
    message(FATAL_ERROR "[GfxDxc] DXC が見つかりません (${_gfx_dxc_root})。"
        "GFX_DXC_DIR には inc/dxcapi.h と bin/${_gfx_dxc_arch}/dxcompiler.dll を含むフォルダを指定してください")
endif()
