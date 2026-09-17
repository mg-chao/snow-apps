# CPack runs this before computing checksums and copying the final packages.
foreach(_package IN LISTS CPACK_PACKAGE_FILES)
    if(_package MATCHES "\\.dmg$")
        execute_process(COMMAND /usr/bin/codesign --force --sign - "${_package}"
            COMMAND_ERROR_IS_FATAL ANY)
        execute_process(COMMAND /usr/bin/codesign --verify --strict "${_package}"
            COMMAND_ERROR_IS_FATAL ANY)
        execute_process(COMMAND /usr/bin/hdiutil verify "${_package}"
            COMMAND_ERROR_IS_FATAL ANY)
    endif()
endforeach()
