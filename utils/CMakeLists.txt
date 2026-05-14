# CMakeLists.txt
#
# Build script for building with cmake (http://www.cmake.org)

set( PDW_INCLUDES
  ${PDW_INCLUDES}
  ${CMAKE_CURRENT_SOURCE_DIR}/OSTYPE.H
  ${CMAKE_CURRENT_SOURCE_DIR}/binary.h
  #${CMAKE_CURRENT_SOURCE_DIR}/debug.h
  ${CMAKE_CURRENT_SOURCE_DIR}/debuglog.h
  #${CMAKE_CURRENT_SOURCE_DIR}/globals.h
  #${CMAKE_CURRENT_SOURCE_DIR}/ipcnfg.h
  # mqtt.h omitted — needs external Paho MQTT C SDK (built via VS solution path).
  ${CMAKE_CURRENT_SOURCE_DIR}/rs232.h
  ${CMAKE_CURRENT_SOURCE_DIR}/smtp.h
  ${CMAKE_CURRENT_SOURCE_DIR}/smtp_int.h
  ${CMAKE_CURRENT_SOURCE_DIR}/webhook.h

  PARENT_SCOPE
)

set( PDW_SRCS
  ${PDW_SRCS}
  ${CMAKE_CURRENT_SOURCE_DIR}/Debug.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/debuglog.cpp
  # mqtt.cpp omitted — needs external Paho MQTT C SDK; build via VS solution instead.
  #${CMAKE_CURRENT_SOURCE_DIR}/OSTYPE.C
  ${CMAKE_CURRENT_SOURCE_DIR}/Ostype.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/binary.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/ipcnfg.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/rs232.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/smtp.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/webhook.cpp

  PARENT_SCOPE
)

