cmake_minimum_required(VERSION 3.10)
set(CMAKE_CXX_STANDARD 17)

# Apply the native ns-3.40 integration fixes, or verify an already patched checkout.
find_program(SIONNA_PATCH_EXECUTABLE patch REQUIRED)
foreach(patch_name ns-3.40-he-calibration-context.patch ns-3.40-mimo-stream-limits.patch)
    set(core_patch "${CMAKE_CURRENT_SOURCE_DIR}/patches/${patch_name}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${core_patch}")
    execute_process(COMMAND ${SIONNA_PATCH_EXECUTABLE} --batch --dry-run --forward -p1 -i "${core_patch}"
                    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR} RESULT_VARIABLE patch_needed
                    OUTPUT_QUIET ERROR_QUIET)
    if(patch_needed EQUAL 0)
        execute_process(COMMAND ${SIONNA_PATCH_EXECUTABLE} --batch --forward -p1 -i "${core_patch}"
                        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR} RESULT_VARIABLE patch_result)
    else()
        execute_process(COMMAND ${SIONNA_PATCH_EXECUTABLE} --batch --dry-run --reverse -p1 -i "${core_patch}"
                        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR} RESULT_VARIABLE patch_result
                        OUTPUT_QUIET ERROR_QUIET)
    endif()
    if(NOT patch_result EQUAL 0)
        message(FATAL_ERROR "Cannot apply or verify ${patch_name}")
    endif()
endforeach()

# Check for stdint.h
check_include_file_cxx(stdint.h HAVE_STDINT_H)
if(HAVE_STDINT_H)
    add_definitions(-DHAVE_STDINT_H)
endif()

include(FindPkgConfig)
if(NOT PKG_CONFIG_FOUND)
    message(STATUS "pkgconf not found")
    return()
endif()

# Find ZeroMQ
find_package(PkgConfig REQUIRED)
pkg_check_modules(ZeroMQ REQUIRED libzmq)
include_directories(${ZeroMQ_INCLUDE_DIRS})


## use the hint from above to find where 'zmq.hpp' is located
find_path(ZeroMQ_INCLUDE_DIR
        NAMES zmq.h
        PATHS ${PC_ZeroMQ_INCLUDE_DIRS}
)

## use the hint from above to find the location of libzmq
find_library(ZeroMQ_LIBRARY
        NAMES zmq
        PATHS ${PC_ZeroMQ_LIBRARY_DIRS}
)

#target_include_directories(sionna PRIVATE ${ZMQ_INCLUDE_DIRS})
#target_link_libraries(sionna PRIVATE ${ZMQ_LIBRARIES})

# protobuf lib
find_package(Protobuf REQUIRED)
if(NOT Protobuf_FOUND)
    message(STATUS "protobuf not found")
    return()
endif()

set(proto_source_files model/message.proto)

# Module sources and headers
set(SOURCE_FILES
        model/he-data-tone-mask.cc
        model/he-ber-error-rate-model.cc
        model/he-effective-sinr-mapping.cc
        model/he-calibrated-error-rate-model.cc
        model/dl-mu-mimo-runtime.cc
        model/dl-mu-mimo-link-model.cc
        model/he-dl-mu-mimo-link-abstraction.cc
        model/he-ul-mu-mimo-phy-adapter.cc
        model/he-ul-mu-mimo-link-abstraction.cc
        model/he-su-mimo-link-abstraction.cc
        model/mimo-channel-matrix.cc
        model/mimo-csi-feedback.cc
        model/mimo-precoding-matrix.cc
        model/su-mimo-link-model.cc
        model/ul-mu-mimo-link-model.cc
        model/wifi-link-budget.cc
        model/sionna-mobility-model.cc
        model/sionna-propagation-cache.cc
        model/sionna-propagation-delay-model.cc
        model/sionna-propagation-loss-model.cc
        model/sionna-spectrum-propagation-loss-model.cc
        model/sionna-phased-array-spectrum-propagation-loss-model.cc
        model/cfr-tag.cc
        helper/sionna-mimo-helper.cc
        helper/sionna-helper.cc
        ${proto_source_files}
)

set(HEADER_FILES
        model/he-data-tone-mask.h
        model/he-ber-error-rate-model.h
        model/he-effective-sinr-mapping.h
        model/he-calibrated-error-rate-model.h
        model/dl-mu-mimo-runtime.h
        model/dl-mu-mimo-link-model.h
        model/he-dl-mu-mimo-link-abstraction.h
        model/he-ul-mu-mimo-phy-adapter.h
        model/he-ul-mu-mimo-link-abstraction.h
        model/he-su-mimo-link-abstraction.h
        model/mimo-channel-matrix.h
        model/mimo-csi-feedback.h
        model/mimo-precoding-matrix.h
        model/su-mimo-link-model.h
        model/ul-mu-mimo-link-model.h
        model/wifi-link-budget.h
        model/sionna-mobility-model.h
        model/sionna-propagation-cache.h
        model/sionna-propagation-delay-model.h
        model/sionna-propagation-loss-model.h
        model/sionna-spectrum-propagation-loss-model.h
        model/sionna-phased-array-spectrum-propagation-loss-model.h
        model/cfr-tag.h
        helper/sionna-mimo-helper.h
        helper/sionna-helper.h
        helper/sionna-utils.h
        #        ${PROTO_HDR}        # protobuf-generated header
)

# Examples (optional)
set(examples_as_tests_sources)
if(${ENABLE_EXAMPLES})
    set(examples_as_tests_sources
            test/sionna-examples-test-suite.cc
    )
endif()

# Build the ns-3 module
build_lib(
        LIBNAME sionna
        SOURCE_FILES ${SOURCE_FILES}
        HEADER_FILES ${HEADER_FILES}
        LIBRARIES_TO_LINK
        ${libcore}
        ${libnetwork}
        ${libpropagation}
        ${libspectrum}
        ${libwifi}
        ${ZeroMQ_LIBRARIES}
        ${Protobuf_LIBRARIES}
        TEST_SOURCES
        test/he-effective-sinr-mapping-test-suite.cc
        test/sionna-test-suite.cc
        test/mimo-csi-feedback-test-suite.cc
        test/dl-mu-mimo-runtime-test-suite.cc
        test/dl-mu-mimo-ru-test-suite.cc
        test/he-dl-mu-spectrum-ru-test-suite.cc
        test/ul-mu-mimo-link-test-suite.cc
        test/he-ul-mu-mimo-phy-adapter-test-suite.cc
        ${examples_as_tests_sources}
)

# need protobuf_generate func to generate messages
check_function_exists(protobuf_generate protobuf_generate_exists)
if(${protobuf_generate_exists})
    message(STATUS "protobuf_generate command found")
else()
    message(STATUS "protobuf_generate command not found -> use a local copy from ${CMAKE_CURRENT_SOURCE_DIR}/protobuf-generate.cmake")
    include(${CMAKE_CURRENT_SOURCE_DIR}/protobuf-generate.cmake)
endif()

# generate proto for cpp
protobuf_generate(
        TARGET ${libsionna-obj}
        IMPORT_DIRS model/
        LANGUAGE cpp
        PROTOC_OUT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/model
)

# generate proto for Python
protobuf_generate(
        TARGET ${libsionna-obj}
        IMPORT_DIRS model/
        LANGUAGE python
        PROTOC_OUT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/model/ns3sionna/common
)
