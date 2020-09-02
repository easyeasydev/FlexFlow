if(NOT ${FLOW_USE_EXTERNAL_PROTOBUF})
  set(protobuf_BUILD_TESTS OFF CACHE BOOL "Disable tests for protobuf")
  set(BUILD_SHARED_LIBS OFF)
  set(LIBRARY_POLICY STATIC)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../protobuf/cmake ${CMAKE_CURRENT_BINARY_DIR}/protobuf)
  include_directories(${CMAKE_CURRENT_LIST_DIR}/../protobuf/src)
  link_directories(${CMAKE_CURRENT_BINARY_DIR}/protobuf)
  set(protobuf_lib_name protobuf)
  if(CMAKE_BUILD_TYPE MATCHES "Debug")
    set(protobuf_lib_name protobufd)
  endif()
else()
  set(PROTOBUF_ROOT /home/wwu12/opt)
  list(APPEND CMAKE_PREFIX_PATH ${PROTOBUF_ROOT})
  set(protobuf_lib_name protobuf)
  # include_directories(${Protobuf_INCLUDE_DIRS})
  #find_package_handle_standard_args(Protobuf
  #  REQUIRED_VARS Protobuf_LIBRARIES Protobuf_INCLUDE_DIR
  #  VERSION_VAR Protobuf_VERSION)
endif()

find_package(Protobuf REQUIRED)
#include(FindProtobuf)
if ( Protobuf_FOUND )
  message( STATUS "Protobuf version : ${Protobuf_VERSION}" )
  message( STATUS "Protobuf include path : ${Protobuf_INCLUDE_DIRS}" )
  message( STATUS "Protobuf libraries : ${Protobuf_LIBRARIES}" )
  message( STATUS "Protobuf compiler libraries : ${Protobuf_PROTOC_LIBRARIES}")
  message( STATUS "Protobuf lite libraries : ${Protobuf_LITE_LIBRARIES}")
else()
  message( WARNING "Protobuf package not found -> specify search path via PROTOBUF_ROOT variable")
endif()

include_directories(${Protobuf_INCLUDE_DIRS})
