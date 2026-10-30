# SPDX-FileCopyrightText: (C) 2024 Chris Rizzitello <sithlord48@gmail.com>
# SPDX-License-Identifier: MIT

# HACK This is set when the files is included so its the real path
# calling CMAKE_CURRENT_LIST_DIR after include would return the wrong scope var
set(MY_DIR ${CMAKE_CURRENT_LIST_DIR})
set(OSX_BUNDLE ${BUILD_OSX_BUNDLE})

set(OS_STRING "macos-${BUILD_ARCHITECTURE}")

if (OSX_BUNDLE)
  # macdeployqt always copies SVG icon plugins when they are installed. With
  # split Qt prefixes it cannot resolve their @rpath dependency after copying
  # them unless QtSvg was deployed with the application's initial frameworks.
  # Discover the optional installed module; do not require or install a new one.
  find_package(Qt6Svg ${Qt6_VERSION} EXACT QUIET CONFIG GLOBAL)
  if(TARGET Qt6::Svg)
    target_link_libraries(${CMAKE_PROJECT_PROPER_NAME} Qt6::Svg)
  endif()
  # Split Qt installations keep plugins and their frameworks in different
  # formulae. Include the configured prefix's shared library directory.
  set(_qt_library_paths "")
  foreach(_prefix IN LISTS CMAKE_PREFIX_PATH)
    if(IS_DIRECTORY "${_prefix}/lib")
      string(APPEND _qt_library_paths " \"-libpath=${_prefix}/lib\"")
    endif()
  endforeach()
  set(_codesign_identity "-")
  set(_hardened_arg "")
  set(_timestamp_arg "")
  if (APPLE_CODESIGN_DEV)
    set(_codesign_identity "${APPLE_CODESIGN_DEV}")
    set(_timestamp_arg "-timestamp")
    set(_hardened_arg "-hardened-runtime")
  endif()
  install(CODE "execute_process(COMMAND
    \"${DEPLOYQT}\"
    \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_PROJECT_PROPER_NAME}.app\"
    \"-executable=\${CMAKE_INSTALL_PREFIX}/${CMAKE_PROJECT_PROPER_NAME}.app/Contents/MacOS/deskflow-core\"
    ${_hardened_arg} ${_timestamp_arg} \"-codesign=${_codesign_identity}\" ${_qt_library_paths}
    RESULT_VARIABLE _deploy_result
  )
  if(NOT _deploy_result EQUAL 0)
    message(FATAL_ERROR \"macdeployqt failed: \${_deploy_result}\")
  endif()
  execute_process(COMMAND /usr/bin/python3
    \"${CMAKE_SOURCE_DIR}/deploy/lanbridge/finalize-macos-bundle.py\"
    \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_PROJECT_PROPER_NAME}.app\"
    \"--identity=${_codesign_identity}\"
    RESULT_VARIABLE _finalize_result
  )
  if(NOT _finalize_result EQUAL 0)
    message(FATAL_ERROR \"Bundle finalization failed: \${_finalize_result}\")
  endif()
  execute_process(COMMAND /usr/bin/python3
    \"${CMAKE_SOURCE_DIR}/deploy/lanbridge/verify-macos-bundle.py\"
    \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_PROJECT_PROPER_NAME}.app\"
    --report \"${CMAKE_SOURCE_DIR}/build/verification/bundle-audit.json\"
    RESULT_VARIABLE _audit_result
  )
  if(NOT _audit_result EQUAL 0)
    message(FATAL_ERROR \"Bundle portability or loader audit failed: \${_audit_result}\")
  endif()")
  set(CPACK_PACKAGE_ICON "${MY_DIR}/dmg-volume.icns")
  set(CPACK_DMG_BACKGROUND_IMAGE "${MY_DIR}/dmg-background.tiff")
  set(CPACK_DMG_DS_STORE_SETUP_SCRIPT "${MY_DIR}/generate_ds_store.applescript")
  set(CPACK_DMG_VOLUME_NAME "${CMAKE_PROJECT_PROPER_NAME}")
  set(CPACK_DMG_SLA_USE_RESOURCE_FILE_LICENSE ON)
  set(CPACK_GENERATOR "DragNDrop")
endif()
