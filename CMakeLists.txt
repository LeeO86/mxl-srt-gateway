cmake_minimum_required(VERSION 3.24 FATAL_ERROR)

project(mxl-srt-gateway
    VERSION 1.2.0
    DESCRIPTION "SRT contribution gateway for MXL"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(SRTGW_BUILD_TESTS "Build unit tests" ON)
option(SRTGW_WITH_NMOS "Link Sony nmos-cpp" OFF)
set(NMOS_CPP_DIR "" CACHE PATH "Path to sony/nmos-cpp Development/")

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

set(SRTGW_CORE_SOURCES
    src/util/sha1.cpp
    src/util/uuid.cpp
    src/util/logging.cpp
    src/util/net.cpp
    src/media/format.cpp
    src/media/adapt.cpp
    src/media/framesync.cpp
    src/media/matrix.cpp
    src/media/v210.cpp
    src/media/slate.cpp
    src/srt/access.cpp
    src/mxl/domain_files.cpp
    src/config/config.cpp
    src/config/store.cpp
    src/nmos/ids.cpp
    src/ops/metrics.cpp
)

add_library(srtgw-core STATIC ${SRTGW_CORE_SOURCES})
target_include_directories(srtgw-core PUBLIC src third_party)
target_link_libraries(srtgw-core PUBLIC Threads::Threads)
target_compile_options(srtgw-core PRIVATE -Wall -Wextra -Wno-unused-parameter)

if(SRTGW_BUILD_TESTS)
    enable_testing()
    add_executable(unit-tests
        tests/unit/test_main.cpp
        tests/unit/test_logic.cpp
    )
    target_link_libraries(unit-tests PRIVATE srtgw-core)
    target_compile_options(unit-tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
    add_test(NAME unit-tests COMMAND unit-tests)
endif()

pkg_check_modules(FFMPEG IMPORTED_TARGET libavformat libavcodec libavfilter libavutil libswresample libswscale)
pkg_check_modules(SRT IMPORTED_TARGET srt)
find_package(mxl CONFIG)

set(SRTGW_HAVE_MEDIA FALSE)
if(FFMPEG_FOUND AND SRT_FOUND)
    set(SRTGW_HAVE_MEDIA TRUE)
endif()

if(SRTGW_HAVE_MEDIA)
    add_library(srtgw-media STATIC
        src/srt/socket.cpp
        src/media/engine.cpp
        src/media/cuda_convert.cpp
        src/mxl/domain.cpp
        src/channel/runtime.cpp
        src/ops/httpserver.cpp
        src/ops/api.cpp
        src/nmos/node.cpp
    )
    target_include_directories(srtgw-media PUBLIC src third_party ${CMAKE_CURRENT_BINARY_DIR}/generated)
    target_link_libraries(srtgw-media PUBLIC srtgw-core PkgConfig::FFMPEG PkgConfig::SRT)
    target_compile_options(srtgw-media PRIVATE -Wall -Wextra -Wno-unused-parameter)

    # v210 <-> NV12/P010 kernels for the NVDEC and NVENC paths. Clang compiles them
    # to PTX without CUDA headers or libraries (like FFmpeg's --enable-cuda-llvm
    # filters); the PTX is embedded and loaded through the driver at runtime.
    find_program(SRTGW_CLANG NAMES clang++ clang)
    find_path(SRTGW_FFNVCODEC_INCLUDE ffnvcodec/dynlink_loader.h)
    if(SRTGW_CLANG AND SRTGW_FFNVCODEC_INCLUDE)
        set(V210_PTX ${CMAKE_CURRENT_BINARY_DIR}/generated/v210_cuda.ptx)
        set(V210_PTX_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/media/v210_cuda_ptx.hpp)
        add_custom_command(
            OUTPUT ${V210_PTX}
            COMMAND ${SRTGW_CLANG} -x cuda --cuda-device-only -nocudainc -nocudalib --cuda-gpu-arch=sm_60 -O2 -S
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/media/v210_cuda.cu -o ${V210_PTX}
            DEPENDS src/media/v210_cuda.cu
            COMMENT "Compiling v210 CUDA kernels to PTX"
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${V210_PTX_HEADER}
            COMMAND ${CMAKE_COMMAND} -DINPUT=${V210_PTX} -DOUTPUT=${V210_PTX_HEADER} -DVAR=v210CudaPtx -DNAMESPACE=srtgw::cudaconvert
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFile.cmake
            DEPENDS ${V210_PTX} cmake/EmbedFile.cmake
            COMMENT "Embedding v210 CUDA kernels"
        )
        target_sources(srtgw-media PRIVATE ${V210_PTX_HEADER})
        target_include_directories(srtgw-media PRIVATE ${SRTGW_FFNVCODEC_INCLUDE})
        target_compile_definitions(srtgw-media PRIVATE SRTGW_WITH_CUDA_CONVERT=1)
        target_link_libraries(srtgw-media PUBLIC ${CMAKE_DL_LIBS})
    else()
        message(STATUS "v210 CUDA kernels disabled (clang or ffnvcodec headers missing): CPU conversion only")
    endif()
    if(mxl_FOUND)
        target_link_libraries(srtgw-media PUBLIC mxl::mxl)
        target_compile_definitions(srtgw-media PUBLIC SRTGW_WITH_MXL=1)
    else()
        target_compile_definitions(srtgw-media PUBLIC SRTGW_WITH_MXL=0)
    endif()

    set(WEBUI_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/web/index.html
        ${CMAKE_CURRENT_SOURCE_DIR}/web/package.json
        ${CMAKE_CURRENT_SOURCE_DIR}/web/package-lock.json
        ${CMAKE_CURRENT_SOURCE_DIR}/web/vite.config.js
        ${CMAKE_CURRENT_SOURCE_DIR}/web/src/main.js
        ${CMAKE_CURRENT_SOURCE_DIR}/web/src/App.vue
        ${CMAKE_CURRENT_SOURCE_DIR}/web/src/style.css
    )
    set(WEBUI_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/ops/webui_generated.hpp)
    option(SRTGW_USE_PREBUILT_WEBUI "Embed web/dist/index.html instead of running npm" OFF)
    find_program(NPM_EXECUTABLE npm)
    set(SRTGW_UI_SOURCE "")
    if(SRTGW_USE_PREBUILT_WEBUI)
        set(WEBUI_HTML ${CMAKE_CURRENT_SOURCE_DIR}/web/dist/index.html)
        if(NOT EXISTS ${WEBUI_HTML})
            message(FATAL_ERROR "SRTGW_USE_PREBUILT_WEBUI=ON but ${WEBUI_HTML} is missing")
        endif()
        add_custom_command(
            OUTPUT ${WEBUI_GENERATED}
            COMMAND ${CMAKE_COMMAND} -DINPUT=${WEBUI_HTML} -DOUTPUT=${WEBUI_GENERATED} -DVAR=indexHtml
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFile.cmake
            DEPENDS ${WEBUI_HTML} cmake/EmbedFile.cmake
            COMMENT "Embedding prebuilt web UI"
        )
        set(SRTGW_UI_SOURCE ${WEBUI_GENERATED})
    elseif(NPM_EXECUTABLE AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/web/package-lock.json)
        set(WEBUI_HTML ${CMAKE_CURRENT_BINARY_DIR}/webui/index.html)
        add_custom_command(
            OUTPUT ${WEBUI_HTML}
            COMMAND ${NPM_EXECUTABLE} ci --no-fund --no-audit
            COMMAND ${NPM_EXECUTABLE} run build
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/webui
            COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/web/dist/index.html ${WEBUI_HTML}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/web
            DEPENDS ${WEBUI_SOURCES}
            COMMENT "Building Vue web UI"
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WEBUI_GENERATED}
            COMMAND ${CMAKE_COMMAND} -DINPUT=${WEBUI_HTML} -DOUTPUT=${WEBUI_GENERATED} -DVAR=indexHtml
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFile.cmake
            DEPENDS ${WEBUI_HTML} cmake/EmbedFile.cmake
            COMMENT "Embedding web UI"
        )
        set(SRTGW_UI_SOURCE ${WEBUI_GENERATED})
    endif()

    add_executable(mxl-srt-gateway src/main.cpp ${SRTGW_UI_SOURCE})
    target_include_directories(mxl-srt-gateway PRIVATE src third_party ${CMAKE_CURRENT_BINARY_DIR}/generated)
    target_link_libraries(mxl-srt-gateway PRIVATE srtgw-media)
    target_compile_options(mxl-srt-gateway PRIVATE -Wall -Wextra -Wno-unused-parameter)
    if(SRTGW_UI_SOURCE)
        target_compile_definitions(mxl-srt-gateway PRIVATE SRTGW_HAS_UI=1)
    endif()

    if(mxl_FOUND)
        add_executable(mxl-srt-flow-writer tests/tools/flow_writer.cpp)
        target_link_libraries(mxl-srt-flow-writer PRIVATE srtgw-media)
        add_executable(mxl-srt-flow-verify tests/tools/flow_verify.cpp)
        target_link_libraries(mxl-srt-flow-verify PRIVATE srtgw-media)
        install(TARGETS mxl-srt-gateway mxl-srt-flow-writer mxl-srt-flow-verify RUNTIME DESTINATION bin)
    else()
        install(TARGETS mxl-srt-gateway RUNTIME DESTINATION bin)
    endif()

    if(SRTGW_WITH_NMOS)
        if(NOT NMOS_CPP_DIR)
            message(FATAL_ERROR "SRTGW_WITH_NMOS=ON requires -DNMOS_CPP_DIR")
        endif()
        set(NMOS_CPP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(NMOS_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(NMOS_CPP_USE_SUPPLIED_JSON_SCHEMA_VALIDATOR ON CACHE BOOL "" FORCE)
        set(NMOS_CPP_USE_SUPPLIED_JWT_CPP ON CACHE BOOL "" FORCE)
        set(CMAKE_CXX_STANDARD 17)
        add_subdirectory(${NMOS_CPP_DIR} ${CMAKE_BINARY_DIR}/nmos-cpp EXCLUDE_FROM_ALL)
        set(CMAKE_CXX_STANDARD 20)
        # node.cpp lives in the static library, so the switch has to be set there.
        target_compile_definitions(srtgw-media PUBLIC SRTGW_WITH_NMOS=1)
        target_link_libraries(srtgw-media PUBLIC nmos-cpp::nmos-cpp)
        set_source_files_properties(src/nmos/node.cpp PROPERTIES
            CXX_STANDARD 17
            CXX_STANDARD_REQUIRED ON
            COMPILE_OPTIONS "-w")
    endif()
else()
    message(STATUS "FFmpeg or libsrt not found; building unit tests only")
endif()
