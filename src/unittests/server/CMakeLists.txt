# SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
# SPDX-License-Identifier: MIT

if(WIN32)
  set(extra_libs version app mt net)
endif()

create_test(
  NAME ServerConfigTests
  DEPENDS server
  LIBS base arch ${extra_libs}
  SOURCE ServerConfigTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/server"
)

create_test(
  NAME ServerTests
  DEPENDS server
  LIBS base arch ${extra_libs}
  SOURCE ServerTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/server"
)

create_test(
  NAME ClientProxyTests
  DEPENDS server
  LIBS base arch io mt net platform app ${extra_libs}
  SOURCE ClientProxyTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/server"
)

create_test(
  NAME ServerSafetyTests
  DEPENDS server
  LIBS base arch io mt net platform app client ${extra_libs}
  SOURCE ServerSafetyTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/server"
)
