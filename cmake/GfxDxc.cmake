# ============================================================
#  DXC (DirectX Shader Compiler) の検出
#
#  出力変数:
#    GFX_DXC_FOUND        見つかったら TRUE
#    GFX_DXC_INCLUDE_DIR  dxcapi.h のあるディレクトリ (空なら Windows SDK のものを使う)
#    GFX_DXC_LIBRARY      dxcompiler.lib
#    GFX_DXC_DLL          dxcompiler.dll (実行時に exe と同じ場所へコピーする。見つからなければ空)
#
#  GFX_DXC_DIR が指定されていればそこを使い、なければ VULKAN_SDK から自動検索する。
# ============================================================
set(GFX_DXC_FOUND       FALSE)
set(GFX_DXC_INCLUDE_DIR "")
set(GFX_DXC_LIBRARY     "")
set(GFX_DXC_DLL         "")

if(NOT WIN32)
    return()
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_gfx_dxc_arch x64)
else()
    set(_gfx_dxc_arch x86)
endif()

if(GFX_DXC_DIR)
    set(_inc "${GFX_DXC_DIR}/inc")
    set(_lib "${GFX_DXC_DIR}/lib/${_gfx_dxc_arch}/dxcompiler.lib")
    set(_dll "${GFX_DXC_DIR}/bin/${_gfx_dxc_arch}/dxcompiler.dll")

    if(EXISTS "${_inc}/dxcapi.h" AND EXISTS "${_lib}")
        set(GFX_DXC_FOUND       TRUE)
        set(GFX_DXC_INCLUDE_DIR "${_inc}")
        set(GFX_DXC_LIBRARY     "${_lib}")
        if(EXISTS "${_dll}")
            set(GFX_DXC_DLL "${_dll}")
        endif()
    else()
        message(WARNING "[GfxDxc] GFX_DXC_DIR=${GFX_DXC_DIR} に DXC が見つかりません")
    endif()
else()
    # VULKAN_SDK / VK_SDK_PATH に同梱されている DXC を自動検索
    find_library(GFX_DXC_LIB_AUTO dxcompiler
        PATHS "$ENV{VULKAN_SDK}/Lib" "$ENV{VK_SDK_PATH}/Lib"
        NO_DEFAULT_PATH
    )
    if(GFX_DXC_LIB_AUTO)
        set(GFX_DXC_FOUND   TRUE)
        set(GFX_DXC_LIBRARY "${GFX_DXC_LIB_AUTO}")
        foreach(_root "$ENV{VULKAN_SDK}" "$ENV{VK_SDK_PATH}")
            if(_root AND EXISTS "${_root}/Bin/dxcompiler.dll")
                set(GFX_DXC_DLL "${_root}/Bin/dxcompiler.dll")
                break()
            endif()
        endforeach()
    endif()
endif()

if(GFX_DXC_FOUND)
    message(STATUS "[GfxDxc] DXC: ${GFX_DXC_LIBRARY}")
else()
    message(STATUS "[GfxDxc] DXC 未検出 — DXR のシェーダーコンパイルはスタブになります")
    message(STATUS "         有効にするには -DGFX_DXC_DIR=<path> を指定してください")
endif()

# 実行ファイルの横に dxcompiler.dll をコピーする (DXC が見つかった場合のみ)
function(gfx_copy_dxc_dll target)
    if(GFX_DXC_DLL)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${GFX_DXC_DLL}" "$<TARGET_FILE_DIR:${target}>/dxcompiler.dll"
            COMMENT "Copying dxcompiler.dll to output directory"
        )
    endif()
endfunction()
