include(GNUInstallDirs)

add_subdirectory(deferred)
if(WIN32)
    add_subdirectory(rendergraph)   # レンダーグラフ (M3b)。サンプルのウィンドウは Win32 だけ
endif()
