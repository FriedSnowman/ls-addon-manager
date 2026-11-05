cmake_minimum_required(VERSION 3.20)
project(NeuralRendering LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)

# Vendor source hashes are checked before compiling any derived binary.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/third_party/manifest.json" vendor_manifest)
foreach(source third_party/nvof/nvOpticalFlowD3D12.h third_party/nvof/nvOpticalFlowCommon.h third_party/optiscaler/dlssnr.hlsl)
    string(JSON expected GET "${vendor_manifest}" files "${source}")
    file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/${source}" actual)
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR "Pinned vendor source changed: ${source}. Review and update provenance explicitly.")
    endif()
endforeach()

file(GLOB sdk_bins "C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64")
list(SORT sdk_bins COMPARE NATURAL ORDER DESCENDING)
find_program(FXC fxc HINTS ${sdk_bins} REQUIRED)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
function(neural_shader name source)
    set(output "${CMAKE_CURRENT_BINARY_DIR}/generated/${name}.h")
    add_custom_command(OUTPUT "${output}"
        COMMAND "${FXC}" /nologo /T cs_5_0 /E CSMain /O3 /Ges /Fh "${output}" /Vn "g_${name}" "${CMAKE_CURRENT_SOURCE_DIR}/${source}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${source}" VERBATIM)
    set(shader_headers ${shader_headers} "${output}" PARENT_SCOPE)
endfunction()
neural_shader(flow_pack shaders/flow_pack.hlsl)
neural_shader(flow_convert shaders/flow_convert.hlsl)
neural_shader(optiscaler third_party/optiscaler/dlssnr.hlsl)

add_library(neural_backend STATIC src/gpu.cpp src/optical_flow.cpp ${shader_headers})
target_include_directories(neural_backend PUBLIC src third_party/nvof "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(neural_backend PUBLIC d3d12 dxgi d3dcompiler)
target_compile_options(neural_backend PRIVATE /W4)

add_executable(neural_flowtest tests/flow_test.cpp)
target_link_libraries(neural_flowtest PRIVATE neural_backend)
