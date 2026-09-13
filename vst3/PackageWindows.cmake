if(NOT EXISTS "${SOURCE_BUNDLE}")
  message(FATAL_ERROR "Windows VST3 bundle does not exist: ${SOURCE_BUNDLE}")
endif()

set(STAGE "${PACKAGE_DIR}/Ensoniq-EPS-16-Plus-Windows-x64")
set(ARCHIVE "${PACKAGE_DIR}/${ARCHIVE_NAME}")
file(REMOVE_RECURSE "${STAGE}")
file(MAKE_DIRECTORY "${STAGE}")
file(COPY "${SOURCE_BUNDLE}" DESTINATION "${STAGE}")
file(MAKE_DIRECTORY "${STAGE}/EPS_files")
file(COPY "${RESOURCE_README}" DESTINATION "${STAGE}/EPS_files")
file(COPY "${THIRD_PARTY_NOTICES}" "${JUCE_LICENSE}" DESTINATION "${STAGE}")
file(WRITE "${STAGE}/INSTALL-WINDOWS.txt"
"Ensoniq EPS-16 Plus VST3 for Windows x64\n\n"
"1. Copy 'Ensoniq EPS-16 Plus.vst3' to:\n"
"   C:\\Program Files\\Common Files\\VST3\\\n"
"2. Copy the 'EPS_files' folder beside the plug-in bundle.\n"
"3. Add your legally obtained ROM, KPC and OS files to EPS_files.\n"
"4. Rescan VST3 plug-ins in your DAW.\n")
file(REMOVE "${ARCHIVE}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E tar cf "${ARCHIVE}" --format=zip
          "Ensoniq-EPS-16-Plus-Windows-x64"
  WORKING_DIRECTORY "${PACKAGE_DIR}"
  RESULT_VARIABLE ZIP_RESULT
)
if(NOT ZIP_RESULT EQUAL 0 OR NOT EXISTS "${ARCHIVE}")
  message(FATAL_ERROR "Could not create Windows VST3 archive")
endif()
message(STATUS "Created Windows VST3 package: ${ARCHIVE}")
