# Included from ggml/src/ggml-cpu/CMakeLists.txt (inside ggml_add_cpu_backend_variant_impl)
# when llama.cpp is configured with -DGGML_SOC_GEMV=ON. Adds the SoC GEMV offload
# sources to the CPU backend target and defines GGML_SOC_GEMV for the hook in ggml-cpu.c.

set(SOC_SW_DIR "${GGML_SOC_DIR}/sw")
set(SOC_GEMV_SOURCES
    "${SOC_SW_DIR}/repack/soc_repack.c"
    "${SOC_SW_DIR}/gemv/soc_gemv.c"
    "${SOC_SW_DIR}/gemv/soc_gemv_backend_emul.c")

option(GGML_SOC_GEMV_FPGA "Build optional Linux UIO/u-dma-buf GEMV backend" OFF)
if (GGML_SOC_GEMV_FPGA)
    if (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "The FPGA adapter requires a Linux target")
    endif()
    list(APPEND SOC_GEMV_SOURCES
        "${SOC_SW_DIR}/hal2/soc_gemv_hal.c"
        "${SOC_SW_DIR}/hal2/soc_gemv_arena.c"
        "${SOC_SW_DIR}/hal2/soc_gemv_backend_fpga.c")
    target_include_directories(${GGML_CPU_NAME} PRIVATE "${SOC_SW_DIR}/hal2")
    target_compile_definitions(${GGML_CPU_NAME} PRIVATE SOC_GEMV_WITH_FPGA)
endif()

target_sources(${GGML_CPU_NAME} PRIVATE ${SOC_GEMV_SOURCES})
target_include_directories(${GGML_CPU_NAME} PRIVATE "${SOC_SW_DIR}/repack" "${SOC_SW_DIR}/gemv")
target_compile_definitions(${GGML_CPU_NAME} PRIVATE GGML_SOC_GEMV)

# Reproducible reference arithmetic: no fused multiply-add in our sources.
set_source_files_properties(${SOC_GEMV_SOURCES} PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")

find_package(Threads REQUIRED)
target_link_libraries(${GGML_CPU_NAME} PRIVATE Threads::Threads)

message(STATUS "SoC GEMV offload enabled for ${GGML_CPU_NAME} (sources in ${GGML_SOC_DIR})")
