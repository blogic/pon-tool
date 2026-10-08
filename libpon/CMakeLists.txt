# SPDX-License-Identifier: LGPL-2.1-only
# Copyright (C) 2026 John Crispin <john@phrozen.org>

cmake_minimum_required(VERSION 3.13)
project(libpon C)

option(LP_HOST "host build: the library and its tests" OFF)
set(LIBNL_INCLUDE "" CACHE PATH "libnl-tiny include directory")

add_definitions(-D_GNU_SOURCE)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers")

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
if(LIBNL_INCLUDE)
	include_directories(${LIBNL_INCLUDE})
endif()
if(LP_HOST)
	set(PON_UAPI_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/../../../../target/linux/airoha/files/include/uapi
	    CACHE PATH "directory holding linux/pon.h")
	include_directories(${PON_UAPI_INCLUDE})
endif()

find_library(NL_LIBRARY NAMES nl-tiny)
if(NOT NL_LIBRARY)
	message(FATAL_ERROR "libnl-tiny is required")
endif()

set(LIBPON_SOURCES nl.c genl.c objects.c omci.c link.c names.c)

add_library(pon SHARED ${LIBPON_SOURCES})
target_link_libraries(pon ${NL_LIBRARY})
set_target_properties(pon PROPERTIES VERSION 0.1 SOVERSION 0
	LINK_FLAGS "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/libpon.map")

install(TARGETS pon LIBRARY DESTINATION lib)
install(FILES include/libpon/pon.h DESTINATION include/libpon)

if(LP_HOST)
	enable_testing()
	add_library(pon-test STATIC ${LIBPON_SOURCES})
	target_link_libraries(pon-test ${NL_LIBRARY})
	add_executable(test-names test/test_names.c)
	target_link_libraries(test-names pon-test)
	add_test(NAME names COMMAND test-names)
	add_executable(test-decode test/test_decode.c)
	target_include_directories(test-decode PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
	target_link_libraries(test-decode pon-test)
	add_test(NAME decode COMMAND test-decode)
	add_executable(test-calls test/test_calls.c)
	target_link_libraries(test-calls pon-test)
	target_link_options(test-calls PRIVATE -Wl,--wrap=nlmsg_alloc
			    -Wl,--wrap=nlmsg_alloc_simple)
	add_test(NAME calls COMMAND test-calls)
	add_executable(test-runner test/test_runner.c)
	target_link_libraries(test-runner pon-test)
	target_link_options(test-runner PRIVATE -Wl,--wrap=nlmsg_free)
	add_test(NAME runner COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/test/netns.sh
		 $<TARGET_FILE:test-runner>)
	set_tests_properties(runner PROPERTIES SKIP_RETURN_CODE 77)
endif()
