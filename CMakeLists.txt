cmake_minimum_required(VERSION 3.20)
project(SkyForge LANGUAGES CXX)

# SkyForge (module « Météo & Eau ») : démonstrateur DirectX 12 réglable en temps réel.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.5
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)

file(GLOB MEO_SOURCES CONFIGURE_DEPENDS src/*.cpp src/*.h)
file(GLOB MEO_SHADERS CONFIGURE_DEPENDS shaders/*.hlsl shaders/*.hlsli)

add_executable(SkyForge WIN32 ${MEO_SOURCES} ${MEO_SHADERS})
set_source_files_properties(${MEO_SHADERS} PROPERTIES HEADER_FILE_ONLY TRUE) # compilés à l'exécution
target_link_libraries(SkyForge PRIVATE imgui d3d12 dxgi d3dcompiler ole32)
if(MSVC)
    target_link_libraries(SkyForge PRIVATE xaudio2)
else()
    target_link_libraries(SkyForge PRIVATE xaudio2_8)
endif()
target_compile_definitions(SkyForge PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
if(MSVC)
    target_compile_options(SkyForge PRIVATE /W4 /permissive- /utf-8)
    set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT SkyForge)
    set_target_properties(SkyForge PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:SkyForge>")
else()
    target_link_options(SkyForge PRIVATE -static -municode)
endif()

# Les shaders sont copiés à côté de l'exécutable : on peut les modifier
# et les recharger à chaud depuis le panneau (bouton « Recharger les shaders »).
add_custom_command(TARGET SkyForge POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/shaders $<TARGET_FILE_DIR:SkyForge>/shaders
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/sons $<TARGET_FILE_DIR:SkyForge>/sons)
