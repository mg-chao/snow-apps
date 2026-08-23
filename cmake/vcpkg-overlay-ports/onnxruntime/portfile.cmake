if(VCPKG_TARGET_IS_OSX OR VCPKG_TARGET_IS_IOS)
    if("framework" IN_LIST FEATURES)
        vcpkg_check_linkage(ONLY_DYNAMIC_LIBRARY)
    endif()
endif()

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO microsoft/onnxruntime
    REF "v${VERSION}"
    SHA512 31ee13a8b89f2bfd0c58086258c15b11218a675e577d287d94acc20723b0ca0110458bfaf268d3e9140fe86e9a0fe3f89abbc2d6d347f8ea65624fc0716f14c1
    PATCHES
        fix-static-delay-load.patch
        generate-reduced-ops-during-configure.patch
)

find_program(PROTOC NAMES protoc PATHS "${CURRENT_HOST_INSTALLED_DIR}/tools/protobuf" REQUIRED NO_DEFAULT_PATH NO_CMAKE_PATH)
find_program(FLATC NAMES flatc PATHS "${CURRENT_HOST_INSTALLED_DIR}/tools/flatbuffers" REQUIRED NO_DEFAULT_PATH NO_CMAKE_PATH)

# Prefer the standard-library venv module when vcpkg discovers a normal Python
# installation. Fall back to vcpkg's virtualenv bootstrap for embedded Python.
vcpkg_find_acquire_program(PYTHON3)
execute_process(
    COMMAND "${PYTHON3}" -I -c "import venv"
    RESULT_VARIABLE _snow_python_venv_result
    OUTPUT_QUIET
    ERROR_QUIET
)
if(_snow_python_venv_result EQUAL 0)
    set(_snow_onnxruntime_venv "${CURRENT_BUILDTREES_DIR}/${TARGET_TRIPLET}-venv")
    file(REMOVE_RECURSE "${_snow_onnxruntime_venv}")
    vcpkg_execute_required_process(
        COMMAND "${PYTHON3}" -I -m venv "${_snow_onnxruntime_venv}"
        WORKING_DIRECTORY "${CURRENT_BUILDTREES_DIR}"
        LOGNAME "venv-setup-${TARGET_TRIPLET}"
    )
    if(CMAKE_HOST_WIN32)
        set(_snow_onnxruntime_python_dir "${_snow_onnxruntime_venv}/Scripts")
    else()
        set(_snow_onnxruntime_python_dir "${_snow_onnxruntime_venv}/bin")
    endif()
    set(PYTHON3 "${_snow_onnxruntime_python_dir}/python${VCPKG_HOST_EXECUTABLE_SUFFIX}")
    if(NOT EXISTS "${PYTHON3}")
        message(FATAL_ERROR "Python venv creation did not produce the expected interpreter: ${PYTHON3}")
    endif()
    vcpkg_execute_required_process(
        COMMAND "${PYTHON3}" -I -m pip install --disable-pip-version-check --no-warn-script-location flatbuffers
        WORKING_DIRECTORY "${CURRENT_BUILDTREES_DIR}"
        LOGNAME "pip-install-flatbuffers-${TARGET_TRIPLET}"
    )
    set(ENV{VIRTUAL_ENV} "${_snow_onnxruntime_venv}")
    vcpkg_add_to_path(PREPEND "${_snow_onnxruntime_python_dir}")
else()
    x_vcpkg_get_python_packages(
        PYTHON_VERSION "3"
        PYTHON_EXECUTABLE "${PYTHON3}"
        PACKAGES flatbuffers
        OUT_PYTHON_VAR PYTHON3
    )
endif()

set(SNOW_SHOT_REQUIRED_OPERATORS "${CMAKE_CURRENT_LIST_DIR}/required_operators.config")
if(NOT EXISTS "${SNOW_SHOT_REQUIRED_OPERATORS}")
    message(FATAL_ERROR
        "Snow Shot ONNX Runtime operator configuration is missing: ${SNOW_SHOT_REQUIRED_OPERATORS}")
endif()

vcpkg_execute_required_process(
    COMMAND "${PYTHON3}" onnxruntime/core/flatbuffers/schema/compile_schema.py --flatc "${FLATC}"
    LOGNAME compile_schema_core
    WORKING_DIRECTORY "${SOURCE_PATH}"
)
vcpkg_execute_required_process(
    COMMAND "${PYTHON3}" onnxruntime/lora/adapter_format/compile_schema.py --flatc "${FLATC}"
    LOGNAME compile_schema_lora
    WORKING_DIRECTORY "${SOURCE_PATH}"
)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        python    onnxruntime_ENABLE_PYTHON
        training  onnxruntime_ENABLE_TRAINING
        training  onnxruntime_ENABLE_TRAINING_APIS
        cuda      onnxruntime_USE_CUDA
        cuda      onnxruntime_USE_CUDA_NHWC_OPS
        openvino  onnxruntime_USE_OPENVINO
        tensorrt  onnxruntime_USE_TENSORRT
        tensorrt  onnxruntime_USE_TENSORRT_BUILTIN_PARSER
        directml  onnxruntime_USE_DML
        winml     onnxruntime_USE_WINML
        coreml    onnxruntime_USE_COREML
        mimalloc  onnxruntime_USE_MIMALLOC
        valgrind  onnxruntime_USE_VALGRIND
        xnnpack   onnxruntime_USE_XNNPACK
        nnapi     onnxruntime_USE_NNAPI_BUILTIN
        azure     onnxruntime_USE_AZURE
        test      onnxruntime_BUILD_UNIT_TESTS
        test      onnxruntime_BUILD_BENCHMARKS
        test      onnxruntime_RUN_ONNX_TESTS
        framework onnxruntime_BUILD_APPLE_FRAMEWORK
        framework onnxruntime_BUILD_OBJC
        nccl      onnxruntime_USE_NCCL
    INVERTED_FEATURES
        cuda      onnxruntime_USE_MEMORY_EFFICIENT_ATTENTION
)

if("cuda" IN_LIST FEATURES)
    vcpkg_find_cuda(OUT_CUDA_TOOLKIT_ROOT cuda_toolkit_root)
    list(APPEND FEATURE_OPTIONS
        "-DCMAKE_CUDA_COMPILER=${NVCC}"
        "-DCUDAToolkit_ROOT=${cuda_toolkit_root}"
        "-DCMAKE_CUDA_FLAGS=-Xcudafe --diag_suppress=2803 -Wno-deprecated-gpu-targets"
    )
endif()

if("tensorrt" IN_LIST FEATURES)
    if(DEFINED ENV{TENSORRT_HOME})
        set(TENSORRT_HOME "$ENV{TENSORRT_HOME}")
    endif()
    if(DEFINED TENSORRT_HOME)
        list(APPEND FEATURE_OPTIONS "-Donnxruntime_TENSORRT_HOME:PATH=${TENSORRT_HOME}")
    else()
        message(WARNING "Define TENSORRT_HOME for onnxruntime_TENSORRT_HOME")
    endif()
endif()

string(COMPARE EQUAL "${VCPKG_LIBRARY_LINKAGE}" "dynamic" BUILD_SHARED)
if(VCPKG_LIBRARY_LINKAGE STREQUAL "static" AND "directml" IN_LIST FEATURES)
    set(VCPKG_POLICY_DLLS_IN_STATIC_LIBRARY enabled)
endif()

set(SNOW_ORT_PLATFORM_OPTIONS)
set(SNOW_ORT_RELEASE_OPTIONS)
set(SNOW_ORT_DEBUG_OPTIONS)
if(VCPKG_TARGET_IS_WINDOWS)
    if(NOT VCPKG_TARGET_IS_MINGW AND
       VCPKG_BUILD_TYPE STREQUAL "release" AND
       VCPKG_LIBRARY_LINKAGE STREQUAL "static")
        # MSVC's final LTCG intermediate has a hard 4 GiB image limit. The
        # reduced ONNX Runtime + DirectML archives alone exceed that limit, so
        # keep /O2, /Gw, and /Gy but emit native objects for this dependency.
        string(APPEND VCPKG_C_FLAGS_RELEASE " /GL-")
        string(APPEND VCPKG_CXX_FLAGS_RELEASE " /GL-")
    endif()

    if(VCPKG_CRT_LINKAGE STREQUAL "static")
        set(SNOW_ORT_MSVC_RUNTIME_RELEASE "MultiThreaded")
        set(SNOW_ORT_MSVC_RUNTIME_DEBUG "MultiThreadedDebug")
    else()
        set(SNOW_ORT_MSVC_RUNTIME_RELEASE "MultiThreadedDLL")
        set(SNOW_ORT_MSVC_RUNTIME_DEBUG "MultiThreadedDebugDLL")
    endif()
    list(APPEND SNOW_ORT_RELEASE_OPTIONS
        "-DCMAKE_MSVC_RUNTIME_LIBRARY=${SNOW_ORT_MSVC_RUNTIME_RELEASE}")
    list(APPEND SNOW_ORT_DEBUG_OPTIONS
        "-DCMAKE_MSVC_RUNTIME_LIBRARY=${SNOW_ORT_MSVC_RUNTIME_DEBUG}")

    if(VCPKG_TARGET_ARCHITECTURE STREQUAL "x64")
        # MLAS includes macamd64.inc from the Windows SDK. MASM does not inherit
        # CMAKE_C/CXX_FLAGS, so provide its SDK include directory explicitly.
        set(_snow_sdk_shared "$ENV{WindowsSdkDir}/Include/$ENV{WindowsSDKVersion}/shared")
        if(NOT IS_DIRECTORY "${_snow_sdk_shared}")
            file(GLOB _snow_sdk_shared_candidates
                "$ENV{SystemDrive}/Program Files (x86)/Windows Kits/10/Include/*/shared")
            list(SORT _snow_sdk_shared_candidates COMPARE NATURAL ORDER DESCENDING)
            list(LENGTH _snow_sdk_shared_candidates _snow_sdk_shared_count)
            if(_snow_sdk_shared_count GREATER 0)
                list(GET _snow_sdk_shared_candidates 0 _snow_sdk_shared)
            endif()
        endif()
        if(IS_DIRECTORY "${_snow_sdk_shared}")
            list(APPEND SNOW_ORT_PLATFORM_OPTIONS
                "-DCMAKE_ASM_MASM_FLAGS=/I\"${_snow_sdk_shared}\"")
        else()
            message(FATAL_ERROR
                "Windows SDK shared include directory was not found for the ONNX Runtime MASM build.")
        endif()
    endif()
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}/cmake"
    OPTIONS
        ${FEATURE_OPTIONS}
        ${SNOW_ORT_PLATFORM_OPTIONS}
        "-DPython_EXECUTABLE:FILEPATH=${PYTHON3}"
        "-DProtobuf_PROTOC_EXECUTABLE:FILEPATH=${PROTOC}"
        "-DONNX_CUSTOM_PROTOC_EXECUTABLE:FILEPATH=${PROTOC}"
        -DBUILD_PKGCONFIG_FILES=ON
        -Donnxruntime_BUILD_SHARED_LIB=${BUILD_SHARED}
        -Donnxruntime_CROSS_COMPILING=${VCPKG_CROSSCOMPILING}
        -Donnxruntime_USE_EXTENSIONS=OFF
        -Donnxruntime_USE_NNAPI_BUILTIN=${VCPKG_TARGET_IS_ANDROID}
        -Donnxruntime_USE_VCPKG=ON
        -Donnxruntime_USE_CUSTOM_DIRECTML=OFF
        -Donnxruntime_ENABLE_DELAY_LOADING_WIN_DLLS=ON
        -Donnxruntime_ENABLE_CPUINFO=ON
        -Donnxruntime_ENABLE_MICROSOFT_INTERNAL=OFF
        -Donnxruntime_ENABLE_BITCODE=OFF
        -Donnxruntime_ENABLE_PYTHON=OFF
        -Donnxruntime_ENABLE_EXTERNAL_CUSTOM_OP_SCHEMAS=OFF
        -Donnxruntime_ENABLE_MEMORY_PROFILE=OFF
        -Donnxruntime_ENABLE_LAZY_TENSOR=OFF
        -Donnxruntime_MINIMAL_BUILD=OFF
        -Donnxruntime_REDUCED_OPS_BUILD=ON
        "-Donnxruntime_REDUCED_OPS_CONFIG=${SNOW_SHOT_REQUIRED_OPERATORS}"
        -Donnxruntime_DISABLE_RTTI=OFF
        -Donnxruntime_DISABLE_ABSEIL=OFF
        --compile-no-warning-as-error
    OPTIONS_RELEASE
        ${SNOW_ORT_RELEASE_OPTIONS}
    OPTIONS_DEBUG
        ${SNOW_ORT_DEBUG_OPTIONS}
        -Donnxruntime_ENABLE_MEMLEAK_CHECKER=OFF
        -Donnxruntime_DEBUG_NODE_INPUTS_OUTPUTS=1
    MAYBE_UNUSED_VARIABLES
        Python_EXECUTABLE
        onnxruntime_TENSORRT_PLACEHOLDER_BUILDER
        onnxruntime_NVCC_THREADS
        CMAKE_CUDA_FLAGS
)
if("cuda" IN_LIST FEATURES)
    vcpkg_cmake_build(TARGET onnxruntime_providers_cuda LOGFILE_BASE build-cuda)
endif()
if("tensorrt" IN_LIST FEATURES)
    vcpkg_cmake_build(TARGET onnxruntime_providers_tensorrt LOGFILE_BASE build-tensorrt)
endif()
if(VCPKG_BUILD_TYPE STREQUAL "release" AND VCPKG_LIBRARY_LINKAGE STREQUAL "static")
    # LTCG objects exhaust memory under vcpkg's default parallel build, which
    # otherwise forces the helper to discard progress and retry serially.
    vcpkg_cmake_install(DISABLE_PARALLEL)
else()
    vcpkg_cmake_install()
endif()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/onnxruntime)
vcpkg_fixup_pkgconfig()

function(reolocate_ort_providers)
    if(VCPKG_TARGET_IS_WINDOWS AND VCPKG_LIBRARY_LINKAGE STREQUAL "dynamic")
        file(GLOB PROVIDER_BINS_DEBUG "${CURRENT_PACKAGES_DIR}/debug/lib/onnxruntime_providers_*.dll")
        file(COPY ${PROVIDER_BINS_DEBUG} DESTINATION "${CURRENT_PACKAGES_DIR}/debug/bin")
        file(GLOB PROVIDER_BINS_RELEASE "${CURRENT_PACKAGES_DIR}/lib/onnxruntime_providers_*.dll")
        file(COPY ${PROVIDER_BINS_RELEASE} DESTINATION "${CURRENT_PACKAGES_DIR}/bin")
        file(REMOVE ${PROVIDER_BINS_DEBUG} ${PROVIDER_BINS_RELEASE})
    endif()
endfunction()

reolocate_ort_providers()
vcpkg_copy_pdbs()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
if(VCPKG_LIBRARY_LINKAGE STREQUAL "static")
    file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/bin" "${CURRENT_PACKAGES_DIR}/bin")
endif()

if("directml" IN_LIST FEATURES)
    set(DIRECTML_PACKAGE_DIR "${CURRENT_BUILDTREES_DIR}/packages/Microsoft.AI.DirectML.1.15.4")
    set(DIRECTML_RUNTIME "${DIRECTML_PACKAGE_DIR}/bin/x64-win/DirectML.dll")
    set(DIRECTML_IMPORT_LIBRARY "${DIRECTML_PACKAGE_DIR}/bin/x64-win/DirectML.lib")
    foreach(DIRECTML_FILE IN ITEMS "${DIRECTML_RUNTIME}" "${DIRECTML_IMPORT_LIBRARY}")
        if(NOT EXISTS "${DIRECTML_FILE}")
            message(FATAL_ERROR "DirectML package file was not restored: ${DIRECTML_FILE}")
        endif()
    endforeach()

    file(INSTALL "${DIRECTML_IMPORT_LIBRARY}" DESTINATION "${CURRENT_PACKAGES_DIR}/lib")
    file(TO_CMAKE_PATH "${DIRECTML_IMPORT_LIBRARY}" DIRECTML_IMPORT_LIBRARY_CMAKE)
    file(GLOB ONNXRUNTIME_TARGETS_FILES
        "${CURRENT_PACKAGES_DIR}/share/onnxruntime/onnxruntimeTargets*.cmake")
    set(DIRECTML_TARGET_REFERENCE_FOUND FALSE)
    foreach(ONNXRUNTIME_TARGETS_FILE IN LISTS ONNXRUNTIME_TARGETS_FILES)
        file(READ "${ONNXRUNTIME_TARGETS_FILE}" ONNXRUNTIME_TARGETS_CONTENT)
        string(REPLACE
            "${DIRECTML_IMPORT_LIBRARY_CMAKE}"
            "\${_IMPORT_PREFIX}/lib/DirectML.lib"
            RELOCATABLE_ONNXRUNTIME_TARGETS_CONTENT
            "${ONNXRUNTIME_TARGETS_CONTENT}"
        )
        if(NOT RELOCATABLE_ONNXRUNTIME_TARGETS_CONTENT STREQUAL ONNXRUNTIME_TARGETS_CONTENT)
            set(DIRECTML_TARGET_REFERENCE_FOUND TRUE)
            file(WRITE "${ONNXRUNTIME_TARGETS_FILE}"
                "${RELOCATABLE_ONNXRUNTIME_TARGETS_CONTENT}")
        endif()
    endforeach()
    if(NOT DIRECTML_TARGET_REFERENCE_FOUND)
        message(STATUS
            "ONNX Runtime targets do not reference an absolute DirectML import path; "
            "using the staged lib/DirectML.lib directly")
    endif()

    file(INSTALL "${DIRECTML_RUNTIME}" DESTINATION "${CURRENT_PACKAGES_DIR}/bin")
    if(NOT DEFINED VCPKG_BUILD_TYPE OR VCPKG_BUILD_TYPE STREQUAL "debug")
        file(INSTALL "${DIRECTML_RUNTIME}" DESTINATION "${CURRENT_PACKAGES_DIR}/debug/bin")
    endif()
endif()

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
