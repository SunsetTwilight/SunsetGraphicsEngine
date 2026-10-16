include(GNUInstallDirs)

# バックエンドは gfx_abi にだけ依存する (GraphicsEngine / core は参照しない)
if(GFX_BUILD_DX12)
    if(NOT WIN32)
        message(WARNING "[gfx_dx12] Windows 専用のためスキップします")
    else()
        add_subdirectory(dx12)
    endif()
endif()

if(GFX_BUILD_VULKAN)
    add_subdirectory(vulkan)
endif()
