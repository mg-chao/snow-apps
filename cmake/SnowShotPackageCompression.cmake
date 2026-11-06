include_guard(GLOBAL)

function(snow_shot_apply_dmg_compression)
    # Both editions support macOS 15+, which can mount LZMA-compressed UDIF
    # images. Keep the bundle unchanged while compressing its distribution image.
    set(CPACK_DMG_FORMAT ULMO PARENT_SCOPE)
endfunction()

function(snow_shot_apply_nsis_compression)
    cmake_parse_arguments(PARSE_ARGV 0 ARG "PAYLOAD" "" "")
    if(ARG_PAYLOAD)
        # The executable-aware 7z payload is already compressed. NSIS ignores
        # SetCompress off in solid mode, so compress only the bootstrap here.
        set(CPACK_NSIS_COMPRESSOR "lzma" PARENT_SCOPE)
        set(CPACK_SNOW_SHOT_USE_7Z_PAYLOAD ON PARENT_SCOPE)
        string(APPEND CPACK_NSIS_DEFINES "\n!define SNOW_SHOT_REQUIRE_7Z_PAYLOAD\n")
    else()
        # Retain the stock CPack path for comparison and standalone fixtures.
        set(CPACK_NSIS_COMPRESSOR "/SOLID lzma" PARENT_SCOPE)
        set(CPACK_SNOW_SHOT_USE_7Z_PAYLOAD OFF PARENT_SCOPE)
    endif()
    # A larger dictionary gives little additional saving on these packages.
    string(PREPEND CPACK_NSIS_DEFINES "SetCompressorDictSize 32\n")
    set(CPACK_NSIS_DEFINES "${CPACK_NSIS_DEFINES}" PARENT_SCOPE)
endfunction()
