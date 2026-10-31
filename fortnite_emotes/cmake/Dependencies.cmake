# Third-party code, fetched at configure time so the repository stays source-only.
include(FetchContent)
set(FETCHCONTENT_QUIET FALSE)

# ------------------------------------------------------------------ Dear ImGui (the emote wheel)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.5
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(imgui)

add_library(emotes_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(emotes_imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(emotes_imgui PUBLIC d3d11 dxgi)
target_compile_definitions(emotes_imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS IMGUI_DEFINE_MATH_OPERATORS)

if(MSVC)
    target_compile_options(emotes_imgui PRIVATE /utf-8 /W0)
    set_property(TARGET emotes_imgui PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ------------------------------------------------------------------ MinHook
FetchContent_Declare(minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(minhook)

add_library(emotes_minhook STATIC
    ${minhook_SOURCE_DIR}/src/buffer.c
    ${minhook_SOURCE_DIR}/src/hook.c
    ${minhook_SOURCE_DIR}/src/trampoline.c
    ${minhook_SOURCE_DIR}/src/hde/hde32.c
    ${minhook_SOURCE_DIR}/src/hde/hde64.c)
target_include_directories(emotes_minhook PUBLIC ${minhook_SOURCE_DIR}/include)

if(MSVC)
    target_compile_options(emotes_minhook PRIVATE /utf-8 /W0)
    set_property(TARGET emotes_minhook PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
