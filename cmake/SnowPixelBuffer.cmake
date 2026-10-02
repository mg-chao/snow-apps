include_guard(GLOBAL)

# Private implementation dependency: installed library headers do not expose
# this allocator, and installed static libraries need no extra runtime library.
add_library(snow_pixel_buffer INTERFACE)
target_include_directories(snow_pixel_buffer INTERFACE
    "${CMAKE_CURRENT_LIST_DIR}/../snow_memory/include")
target_compile_features(snow_pixel_buffer INTERFACE cxx_std_17)
