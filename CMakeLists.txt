cmake_minimum_required(VERSION 3.24)
project(BlurLive VERSION 0.2.0 LANGUAGES CXX)

if(NOT MSVC)
  message(FATAL_ERROR "Blur Live requires MSVC (Visual Studio 2022 / Build Tools) and the Windows SDK.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_options(/permissive- /utf-8 /Zc:__cplusplus /EHsc /bigobj)
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00
                        BLURLIVE_VERSION="${PROJECT_VERSION}")

# ---------------------------------------------------------------------------
# HLSL shaders -> embedded bytecode headers (compiled at build time with fxc)
# ---------------------------------------------------------------------------
find_program(FXC_EXE fxc REQUIRED)
set(SHADER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/engine/shaders)
set(SHADER_OUT ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUT})
set(SHADER_HEADERS "")

set(FX_SHADER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/filters/shaders)

# add_shader_in(<dir> <name> <file> <profile> <entry> [extra fxc args, e.g. /DDEFINE])
function(add_shader_in dir name file profile entry)
  set(out ${SHADER_OUT}/${name}.h)
  file(GLOB includes ${dir}/*.hlsli)
  add_custom_command(
    OUTPUT ${out}
    COMMAND ${FXC_EXE} /nologo /O3 /WX /T ${profile} /E ${entry} /Vn g_${name} /Fh ${out} ${ARGN} ${dir}/${file}
    DEPENDS ${dir}/${file} ${includes}
    COMMENT "Compiling shader ${name}"
    VERBATIM)
  set(SHADER_HEADERS ${SHADER_HEADERS} ${out} PARENT_SCOPE)
endfunction()
macro(add_shader name file profile entry)
  add_shader_in(${SHADER_SRC} ${name} ${file} ${profile} ${entry} ${ARGN})
endmacro()

add_shader(luma_cs       luma_cs.hlsl       cs_5_0 main)
add_shader(downsample_cs downsample_cs.hlsl cs_5_0 main)
add_shader(me_cs         me_cs.hlsl         cs_5_0 main)
add_shader(median_cs     median_cs.hlsl     cs_5_0 main)
add_shader(stats_cs      stats_cs.hlsl      cs_5_0 main)
add_shader(synth_cs      synth_cs.hlsl      cs_5_0 main)
add_shader(blend_cs      blend_cs.hlsl      cs_5_0 main)
add_shader(present_vs    present.hlsl       vs_5_0 vs_main)
add_shader(present_ps    present.hlsl       ps_5_0 ps_main)
# Filter suite
add_shader_in(${FX_SHADER_SRC} fx_uber_cs          fx_uber_cs.hlsl    cs_5_0 main)
add_shader_in(${FX_SHADER_SRC} fx_denoise_cs       fx_denoise_cs.hlsl cs_5_0 main)
add_shader_in(${FX_SHADER_SRC} fx_pyramid_cs       fx_pyramid_cs.hlsl cs_5_0 main)
add_shader_in(${FX_SHADER_SRC} fx_pyramid_color_cs fx_pyramid_cs.hlsl cs_5_0 main /DFROM_COLOR)
add_shader_in(${FX_SHADER_SRC} fx_bloom_cs         fx_bloom_cs.hlsl   cs_5_0 main)
add_custom_target(blurlive_shaders DEPENDS ${SHADER_HEADERS})

# ---------------------------------------------------------------------------
# Core library: capture, engine, presentation, pipeline (no UI)
# ---------------------------------------------------------------------------
add_library(blurlive_core STATIC
  src/core/log.cpp
  src/core/util.cpp
  src/core/settings.cpp
  src/core/weighting.cpp
  src/core/segment_planner.cpp
  src/capture/window_list.cpp
  src/capture/wgc_capture.cpp
  src/engine/blur_engine.cpp
  src/filters/filter_registry.cpp
  src/filters/color_math.cpp
  src/filters/lut.cpp
  src/filters/filter_chain.cpp
  src/filters/presets.cpp
  src/present/output_window.cpp
  src/pipeline/pipeline.cpp
  src/platform/gpu_monitor.cpp
)
add_dependencies(blurlive_core blurlive_shaders)
target_include_directories(blurlive_core PUBLIC src PRIVATE ${SHADER_OUT})
target_link_libraries(blurlive_core PUBLIC
  d3d11 dxgi dcomp dwmapi windowsapp pdh shell32 ole32 user32 gdi32 shcore)

# ---------------------------------------------------------------------------
# Dear ImGui
# ---------------------------------------------------------------------------
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
add_library(imgui STATIC
  ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/backends/imgui_impl_win32.cpp ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_compile_options(imgui PRIVATE /W0)  # vendored code: no warnings

# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------
add_executable(BlurLive WIN32
  src/app/main.cpp
  src/ui/app.cpp
  src/ui/app_filters.cpp
  src/ui/theme.cpp
  src/ui/widgets.cpp
  src/ui/icon_cache.cpp
  resources/blurlive.rc
  resources/blurlive.manifest
)
target_link_libraries(BlurLive PRIVATE blurlive_core imgui comdlg32)

# ---------------------------------------------------------------------------
# Tests (console executable, no framework dependency)
# ---------------------------------------------------------------------------
add_executable(BlurLiveTests tests/tests.cpp tests/tests.rc resources/blurlive.manifest)
target_link_libraries(BlurLiveTests PRIVATE blurlive_core windowscodecs)

# Strict warnings for our own code only (third-party ImGui is compiled with /W0).
foreach(t blurlive_core BlurLive BlurLiveTests)
  target_compile_options(${t} PRIVATE /W4)
endforeach()

enable_testing()
add_test(NAME BlurLiveTests COMMAND BlurLiveTests)
