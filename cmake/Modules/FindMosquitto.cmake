# FindMosquitto.cmake
# Find the Mosquitto includes and library
#
# This module defines:
#  MOSQUITTO_INCLUDE_DIR - where to find mosquitto.h
#  MOSQUITTO_LIBRARY - path to the mosquitto library
#  MOSQUITTO_FOUND - true if mosquitto was found

find_path(MOSQUITTO_INCLUDE_DIR
  NAMES mosquitto.h
  PATHS 
    /usr/include
    /usr/local/include
)

find_library(MOSQUITTO_LIBRARY
  NAMES mosquitto
  PATHS
    /usr/lib
    /usr/lib64
    /usr/local/lib
    /usr/local/lib64
    /usr/lib/x86_64-linux-gnu
    /usr/lib/aarch64-linux-gnu
    /usr/lib/arm-linux-gnueabihf
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Mosquitto DEFAULT_MSG
  MOSQUITTO_LIBRARY MOSQUITTO_INCLUDE_DIR)

mark_as_advanced(
  MOSQUITTO_INCLUDE_DIR
  MOSQUITTO_LIBRARY
)

if(MOSQUITTO_FOUND)
  set(LIBMOSQUITTO_FOUND TRUE)
  set(LIBMOSQUITTO_INCLUDE_DIRS ${MOSQUITTO_INCLUDE_DIR})
  set(LIBMOSQUITTO_LIBRARIES ${MOSQUITTO_LIBRARY})
endif()
