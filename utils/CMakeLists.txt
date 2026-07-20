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
  ${CMAKE_CURRENT_SOURCE_DIR}/telnet_server.h
  ${CMAKE_CURRENT_SOURCE_DIR}/mysql.h
  ${CMAKE_CURRENT_SOURCE_DIR}/sqlite_feed.h
  ${CMAKE_CURRENT_SOURCE_DIR}/logmanager.h
  ${CMAKE_CURRENT_SOURCE_DIR}/rxq.h
  ${CMAKE_CURRENT_SOURCE_DIR}/feedstatus.h
  ${CMAKE_CURRENT_SOURCE_DIR}/winrt_toast.h

  PARENT_SCOPE
)

set( PDW_SRCS
  ${PDW_SRCS}
  ${CMAKE_CURRENT_SOURCE_DIR}/Debug.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/debuglog.cpp
  # mqtt.cpp omitted — needs external Paho MQTT C SDK; build via the VS solution instead.
  # NOTE: a full CMake link therefore still fails on the unresolved MqttInit/MqttNotify/... symbols
  # (they are referenced from PDW.cpp). The pdw_vs2017 solution is the complete, verified build;
  # CMake is a partial/secondary path. Everything below is first-party and SDK-free.
  #${CMAKE_CURRENT_SOURCE_DIR}/OSTYPE.C
  ${CMAKE_CURRENT_SOURCE_DIR}/Ostype.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/binary.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/ipcnfg.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/rs232.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/smtp.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/webhook.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/telnet_server.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/mysql.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/sqlite_feed.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/sqlite/sqlite3.c
  ${CMAKE_CURRENT_SOURCE_DIR}/logmanager.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/rxq.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/feedstatus.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/winrt_toast.cpp

  PARENT_SCOPE
)

