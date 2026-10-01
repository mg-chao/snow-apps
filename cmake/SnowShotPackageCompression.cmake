include_guard(GLOBAL)

function(snow_shot_apply_nsis_compression)
    # CPack passes these arguments directly to SetCompressor. A separate
    # CPACK_NSIS_COMPRESSOR_SOLID variable is not supported by the generator.
    set(CPACK_NSIS_COMPRESSOR "/SOLID lzma" PARENT_SCOPE)
    # A larger dictionary gives little additional saving on these packages.
    string(PREPEND CPACK_NSIS_DEFINES "SetCompressorDictSize 32\n")
    set(CPACK_NSIS_DEFINES "${CPACK_NSIS_DEFINES}" PARENT_SCOPE)
endfunction()
