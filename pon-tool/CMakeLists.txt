# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 John Crispin <john@phrozen.org>

cmake_minimum_required(VERSION 3.13)
project(pon-tool C)

option(PT_HOST "host build: the tool and its tests" OFF)
set(LIBNL_INCLUDE "" CACHE PATH "libnl-tiny include directory")
set(PON_UAPI_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../target/linux/airoha/files/include/uapi
    CACHE PATH "directory holding linux/pon.h")
set(LIBPON_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/../../../../libs/libpon/src/include
    CACHE PATH "directory holding libpon/pon.h")
set(LIBPON_TEST_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/../../../../libs/libpon/src/test
    CACHE PATH "directory holding the test.h, fake.h and netns.sh of libpon")
set(LIBOMCI_APPLY_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/../../../../libs/libomci-apply/src/include
    CACHE PATH "directory holding libomci-apply/apply.h")

add_definitions(-D_GNU_SOURCE)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wno-unused-parameter -Wformat-signedness")

include_directories(${PON_UAPI_INCLUDE} ${LIBPON_INCLUDE} ${LIBOMCI_APPLY_INCLUDE})
if(LIBNL_INCLUDE)
	include_directories(${LIBNL_INCLUDE})
endif()

find_library(PON_LIBRARY NAMES pon)
find_library(OMCI_APPLY_LIBRARY NAMES omci-apply)
find_library(NL_LIBRARY NAMES nl-tiny)
if(NOT PON_LIBRARY OR NOT OMCI_APPLY_LIBRARY OR NOT NL_LIBRARY)
	message(FATAL_ERROR "libpon, libomci-apply and libnl-tiny are required")
endif()

add_executable(pon pon.c)
target_link_libraries(pon ${OMCI_APPLY_LIBRARY} ${PON_LIBRARY})

if(PT_HOST)
	enable_testing()
	add_executable(test-pon test/test_pon.c)
	target_include_directories(test-pon PRIVATE ${LIBPON_TEST_INCLUDE})
	target_link_libraries(test-pon ${OMCI_APPLY_LIBRARY} ${PON_LIBRARY}
			      ${NL_LIBRARY})
	add_test(NAME pon COMMAND sh ${LIBPON_TEST_INCLUDE}/netns.sh
		 $<TARGET_FILE:test-pon>)
	set_tests_properties(pon PROPERTIES SKIP_RETURN_CODE 77)
endif()
