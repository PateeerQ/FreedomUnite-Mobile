# The launcher: a Dear ImGui window over SDL3 + OpenGL 3 that checks the
# install, sets the game's options, binds keys, shows RetroAchievements
# progress, checks GitHub for a newer release, starts mhfu_host and shows its
# log. It lands next to mhfu_host.exe.

enable_language(CXX)
include(FetchContent)

FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.8
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(imgui)

FetchContent_Declare(json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
FetchContent_MakeAvailable(json)

add_executable(mhfu_launcher WIN32
    main.cpp config.cpp game.cpp http.cpp keys.cpp online.cpp image.cpp
    ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp)
target_include_directories(mhfu_launcher PRIVATE
    ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends ${imgui_SOURCE_DIR}/misc/cpp)
target_link_libraries(mhfu_launcher PRIVATE SDL3::SDL3-static nlohmann_json::nlohmann_json)
set_target_properties(mhfu_launcher PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>")
if(WIN32)
    # The crest: the exe's icon, and mhfu.png beside it for the About page.
    target_sources(mhfu_launcher PRIVATE ${CMAKE_SOURCE_DIR}/assets/icon/mhfu.rc)
    add_custom_command(TARGET mhfu_launcher POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/assets/icon/mhfu.png $<TARGET_FILE_DIR:mhfu_launcher>/mhfu.png)
    target_link_libraries(mhfu_launcher PRIVATE opengl32 winhttp windowscodecs ole32 shell32)
    target_compile_definitions(mhfu_launcher PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
endif()
if(MSVC)
    target_compile_options(mhfu_launcher PRIVATE /utf-8)
endif()
