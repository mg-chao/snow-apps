# Keep the build target/executable and development bundle names stable. Only the
# distributable bundle gets the product name; renaming does not alter its seal.
if(NOT CPACK_SNOW_SHOT_BUNDLE_NAME)
    set(CPACK_SNOW_SHOT_BUNDLE_NAME snow_shot)
    set(CPACK_SNOW_SHOT_PRODUCT_NAME "Snow Shot")
endif()
set(_snow_staged_app "${CPACK_TEMPORARY_DIRECTORY}/${CPACK_SNOW_SHOT_BUNDLE_NAME}.app")
if(NOT EXISTS "${_snow_staged_app}/Contents/Info.plist")
    message(FATAL_ERROR "The staged Snow Shot application is missing: ${_snow_staged_app}")
endif()
file(RENAME "${_snow_staged_app}" "${CPACK_TEMPORARY_DIRECTORY}/${CPACK_SNOW_SHOT_PRODUCT_NAME}.app")
