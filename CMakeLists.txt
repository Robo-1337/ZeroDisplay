cmake_minimum_required(VERSION 3.24)
project(ZeroDisplay VERSION 1.1.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT so the exe has no runtime dependencies beyond Windows itself.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

find_program(FXC fxc REQUIRED)

# ---- Shaders: compiled at build time, embedded as byte arrays ----
set(SHADER_DIR ${CMAKE_SOURCE_DIR}/src/render/shaders)
set(SHADER_OUT ${CMAKE_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUT})
set(SHADER_HEADERS "")

function(add_shader name profile)
  set(out ${SHADER_OUT}/${name}.h)
  add_custom_command(
    OUTPUT ${out}
    COMMAND ${FXC} /nologo /O3 /T ${profile} /E main /Vn g_${name} /Fh ${out} ${SHADER_DIR}/${name}.hlsl
    DEPENDS ${SHADER_DIR}/${name}.hlsl ${SHADER_DIR}/common.hlsli
    COMMENT "fxc ${name}.hlsl"
    VERBATIM)
  set(SHADER_HEADERS ${SHADER_HEADERS} ${out} PARENT_SCOPE)
endfunction()

add_shader(vs_fullscreen vs_5_0)
add_shader(ps_yuy2 ps_5_0)
add_shader(ps_nv12 ps_5_0)
add_shader(ps_bgra ps_5_0)
add_shader(ps_present ps_5_0)
add_shader(ps_item ps_5_0)
add_shader(ps_transition ps_5_0)
add_shader(ps_filter ps_5_0)
add_shader(ps_bicubic ps_5_0)
add_shader(ps_rgb2y ps_5_0)
add_shader(ps_rgb2uv ps_5_0)
add_shader(ps_rgb2y10 ps_5_0)
add_shader(ps_rgb2uv10 ps_5_0)
add_shader(ps_upscale ps_5_0)
add_shader(ps_sharpen ps_5_0)
add_shader(ps_layer ps_5_0)
add_shader(ps_tohdr ps_5_0)
add_shader(ps_hdrpic ps_5_0)
add_shader(ps_layerhdr ps_5_0)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.cpp ${CMAKE_SOURCE_DIR}/src/*.h)

add_executable(ZeroDisplay WIN32 ${SOURCES} ${SHADER_HEADERS} ${CMAKE_SOURCE_DIR}/res/app.rc)
# The resource script embeds these; rebuild it when they change.
set_source_files_properties(${CMAKE_SOURCE_DIR}/res/app.rc PROPERTIES OBJECT_DEPENDS
  "${CMAKE_SOURCE_DIR}/res/app.manifest;${CMAKE_SOURCE_DIR}/res/app.ico")
target_include_directories(ZeroDisplay PRIVATE ${CMAKE_SOURCE_DIR}/src ${SHADER_OUT})
target_compile_definitions(ZeroDisplay PRIVATE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A00000C
  CC_VERSION=L"${PROJECT_VERSION}"
  # For res/app.rc (the exe's version info shown in Explorer): the same version, never typed twice.
  CC_VERSION_NUM=${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0
  CC_VERSION_STR="${PROJECT_VERSION}")
target_compile_options(ZeroDisplay PRIVATE /W4 /permissive- /utf-8 /EHsc /Zi /wd4100 /wd4324)
target_link_options(ZeroDisplay PRIVATE /MANIFEST:NO /DEBUG /OPT:REF /OPT:ICF
  /DELAYLOAD:mfplat.dll /DELAYLOAD:mf.dll /DELAYLOAD:mfreadwrite.dll)
# /DEPENDENTLOADFLAG:0x800 (LOAD_LIBRARY_SEARCH_SYSTEM32): Windows loads our DLLs from System32 only, never a
# same-named DLL planted next to the exe (e.g. in a Downloads folder). Not for AddressSanitizer test builds: their
# runtime DLL sits next to the exe.
if(NOT CMAKE_CXX_FLAGS MATCHES "fsanitize")
  target_link_options(ZeroDisplay PRIVATE /DEPENDENTLOADFLAG:0x800)
endif()
target_link_libraries(ZeroDisplay PRIVATE
  d3d11 dxgi d2d1 dwrite
  mfplat mf mfreadwrite mfuuid strmiids
  ole32 oleaut32 uuid windowscodecs
  avrt winmm cfgmgr32 shell32 dwmapi dcomp
  ws2_32 iphlpapi wmcodecdspuuid bcrypt crypt32 delayimp windowsapp CoreMessaging)
