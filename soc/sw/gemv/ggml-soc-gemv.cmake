# Included from ggml/src/ggml-cpu/CMakeLists.txt (inside ggml_add_cpu_backend_variant_impl)
# when llama.cpp is configured with -DGGML_SOC_GEMV=ON. Adds the SoC GEMV offload
# sources to the CPU backend target and defines GGML_SOC_GEMV for the hook in ggml-cpu.c.

set(SOC_SW_DIR "${GGML_SOC_DIR}/sw")
set(SOC_GEMV_SOURCES
    "${SOC_SW_DIR}/repack/soc_repack.c"
    "${SOC_SW_DIR}/gemv/soc_gemv.c"
    "${SOC_SW_DIR}/gemv/soc_gemv_backend_emul.c")

set(GGML_SOC_GEMM_WEIGHT_BITS 8 CACHE STRING
    "Compile-time GEMM weight format: 4 for Q4_0 or 8 for Q8_0")
set_property(CACHE GGML_SOC_GEMM_WEIGHT_BITS PROPERTY STRINGS 4 8)
if (NOT GGML_SOC_GEMM_WEIGHT_BITS MATCHES "^(4|8)$")
    message(FATAL_ERROR "GGML_SOC_GEMM_WEIGHT_BITS must be 4 or 8")
endif()

option(GGML_SOC_GEMV_FPGA "Build optional Linux UIO/u-dma-buf GEMV backend" OFF)
option(GGML_SOC_GEMV_XRT "Use PYNQ/XRT buffers instead of u-dma-buf" OFF)
if (GGML_SOC_GEMV_XRT AND NOT GGML_SOC_GEMV_FPGA)
    message(FATAL_ERROR "GGML_SOC_GEMV_XRT requires GGML_SOC_GEMV_FPGA=ON")
endif()
if (GGML_SOC_GEMV_FPGA)
    if (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "The FPGA adapter requires a Linux target")
    endif()
    list(APPEND SOC_GEMV_SOURCES
        "${SOC_SW_DIR}/hal2/soc_gemv_hal.c")
    if (GGML_SOC_GEMV_XRT)
        # Repository-local board headers are already target headers. Do not
        # reroot this path into the cross compiler's sysroot.
        if (NOT SOC_GEMV_XRT_INCLUDE_DIR AND
            EXISTS "${SOC_SW_DIR}/../third_party/xrt/include/xrt/xrt.h")
            set(SOC_GEMV_XRT_INCLUDE_DIR "${SOC_SW_DIR}/../third_party/xrt/include/xrt"
                CACHE PATH "Board-matched XRT include directory" FORCE)
        endif()
        find_path(SOC_GEMV_XRT_INCLUDE_DIR xrt.h PATH_SUFFIXES xrt)
        if (NOT SOC_GEMV_XRT_INCLUDE_DIR)
            message(FATAL_ERROR "Set SOC_GEMV_XRT_INCLUDE_DIR to the board XRT headers")
        endif()
        list(APPEND SOC_GEMV_SOURCES "${SOC_SW_DIR}/hal2/soc_gemv_backend_fpga_xrt.c")
        target_include_directories(${GGML_CPU_NAME} PRIVATE "${SOC_GEMV_XRT_INCLUDE_DIR}")
        target_link_libraries(${GGML_CPU_NAME} PRIVATE ${CMAKE_DL_LIBS})
    else()
        list(APPEND SOC_GEMV_SOURCES
            "${SOC_SW_DIR}/hal2/soc_gemv_arena.c"
            "${SOC_SW_DIR}/hal2/soc_gemv_backend_fpga.c")
    endif()
    target_include_directories(${GGML_CPU_NAME} PRIVATE "${SOC_SW_DIR}/hal2")
    target_compile_definitions(${GGML_CPU_NAME} PRIVATE SOC_GEMV_WITH_FPGA)
endif()

target_sources(${GGML_CPU_NAME} PRIVATE ${SOC_GEMV_SOURCES})
target_include_directories(${GGML_CPU_NAME} PRIVATE "${SOC_SW_DIR}/repack" "${SOC_SW_DIR}/gemv")
target_compile_definitions(${GGML_CPU_NAME} PRIVATE
    GGML_SOC_GEMV
    SOC_GEMM_WEIGHT_BITS=${GGML_SOC_GEMM_WEIGHT_BITS})

# Reproducible reference arithmetic: no fused multiply-add in our sources.
set_source_files_properties(${SOC_GEMV_SOURCES} PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")

find_package(Threads REQUIRED)
target_link_libraries(${GGML_CPU_NAME} PRIVATE Threads::Threads)

message(STATUS "SoC GEMM Q${GGML_SOC_GEMM_WEIGHT_BITS}_0 offload enabled for ${GGML_CPU_NAME} "
               "(sources in ${GGML_SOC_DIR})")
