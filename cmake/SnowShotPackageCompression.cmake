include_guard(GLOBAL)

function(snow_shot_apply_dmg_compression)
    # Both editions support macOS 15+, which can mount LZMA-compressed UDIF
    # images. Keep the bundle unchanged while compressing its distribution image.
    set(CPACK_DMG_FORMAT ULMO PARENT_SCOPE)
endfunction()

function(snow_shot_apply_nsis_compression)
    # CPack passes these arguments directly to SetCompressor. A separate
    # CPACK_NSIS_COMPRESSOR_SOLID variable is not supported by the generator.
    set(CPACK_NSIS_COMPRESSOR "/SOLID lzma" PARENT_SCOPE)
    # A larger dictionary gives little additional saving on these packages.
    string(PREPEND CPACK_NSIS_DEFINES "SetCompressorDictSize 32\n")
    set(CPACK_NSIS_DEFINES "${CPACK_NSIS_DEFINES}" PARENT_SCOPE)
endfunction()
