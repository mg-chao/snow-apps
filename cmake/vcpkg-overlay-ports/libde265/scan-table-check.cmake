add_executable(snow_libde265_scan_table_check
    "${CMAKE_CURRENT_LIST_DIR}/scan_table_check.cpp"
    "${PROJECT_SOURCE_DIR}/libde265/scan.cc"
)
target_include_directories(snow_libde265_scan_table_check PRIVATE "${PROJECT_SOURCE_DIR}/libde265")
target_compile_features(snow_libde265_scan_table_check PRIVATE cxx_std_11)
