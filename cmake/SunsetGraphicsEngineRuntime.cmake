# ============================================================
#  sunset_gfx_deploy_runtime(<target>)
#
#  アプリの実行ファイルの横に、エンジンの実行時に必要なファイルをコピーする。
#    ・SunsetGraphicsEngine.dll (本体)
#    ・gfx_dx12.dll / gfx_vulkan (ビルド / インストールされているバックエンド)
#    ・dxcompiler.dll など SUNSET_GFX_RUNTIME_EXTRA_FILES に登録されたファイル
#
#  find_package(SunsetGraphicsEngine) でも FetchContent / add_subdirectory でも同じように使える。
#  どちらの場合もターゲット名は SunsetGraphicsEngine::core / ::dx12 / ::vulkan。
#
#  使い方:
#    add_executable(MyApp main.cpp)
#    target_link_libraries(MyApp PRIVATE SunsetGraphicsEngine::core)
#    sunset_gfx_deploy_runtime(MyApp)
# ============================================================
include_guard(GLOBAL)

function(sunset_gfx_deploy_runtime target)
    set(_files "")
    foreach(_name core dx12 vulkan)
        set(_tgt "SunsetGraphicsEngine::${_name}")
        if(NOT TARGET ${_tgt})
            continue()
        endif()
        list(APPEND _files "$<TARGET_FILE:${_tgt}>")

        # 同じビルドツリーのターゲット (FetchContent / add_subdirectory) なら先にビルドさせる。
        # core はリンクで依存するが、バックエンドはリンクしないので明示的に依存を張る
        get_target_property(_imported ${_tgt} IMPORTED)
        if(NOT _imported)
            get_target_property(_real ${_tgt} ALIASED_TARGET)
            if(_real)
                add_dependencies(${target} ${_real})
            endif()
        endif()
    endforeach()

    get_property(_extra GLOBAL PROPERTY SUNSET_GFX_RUNTIME_EXTRA_FILES)
    list(APPEND _files ${_extra})

    if(_files)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_files} "$<TARGET_FILE_DIR:${target}>"
            COMMENT "Deploying SunsetGraphicsEngine runtime files for ${target}"
            VERBATIM
        )
    endif()
endfunction()
