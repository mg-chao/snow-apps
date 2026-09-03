# Stages the verified Snow Shot OCR asset payload (standalone OCR runtime plus
# PP-OCRv6 models) into a development build output directory.
#
# Packaged installers ship either this exact payload (offline variant) or just
# the trusted manifest (online variant). Development builds ship nothing, so
# the managed asset pipeline in ScreenshotOcrAssets cannot find the trusted
# manifest next to the executable and rejects every request with "Text
# recognition components could not be prepared". Running this script as a
# POST_BUILD step gives development builds the same offline payload the
# offline installer ships.
#
# The checked-in manifest is the trust anchor: every staged file is verified
# against its pinned SHA-256, and downloads (cached under the repository's
# git-ignored artifacts/ directory) are verified against the same hashes.
#
# Required -D arguments:
#   SNOW_OCR_ASSET_MANIFEST  path to the checked-in asset manifest
#   SNOW_OCR_ARTIFACTS_DIR   repository artifacts cache directory
#   SNOW_OCR_DESTINATION     target's assets/ocr directory

cmake_minimum_required(VERSION 3.30)

foreach(_required IN ITEMS
    SNOW_OCR_ASSET_MANIFEST
    SNOW_OCR_ARTIFACTS_DIR
    SNOW_OCR_DESTINATION
)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "${_required} is required to stage Snow Shot OCR assets.")
    endif()
endforeach()

if(NOT EXISTS "${SNOW_OCR_ASSET_MANIFEST}")
    message(FATAL_ERROR
        "The checked-in OCR asset manifest is missing: ${SNOW_OCR_ASSET_MANIFEST}")
endif()

file(READ "${SNOW_OCR_ASSET_MANIFEST}" _manifest)

# _field(<out> <json-member-path...>): string(JSON) shortcut with hard failure;
# a malformed checked-in manifest is a repository error, not a network one.
function(_snow_ocr_manifest_field _out)
    string(JSON _value GET "${_manifest}" ${ARGN})
    if(_value STREQUAL "" OR _value MATCHES "-NOTFOUND$")
        message(FATAL_ERROR
            "The checked-in OCR asset manifest is missing '${ARGN}': "
            "${SNOW_OCR_ASSET_MANIFEST}")
    endif()
    set(${_out} "${_value}" PARENT_SCOPE)
endfunction()

_snow_ocr_manifest_field(_runtime_version runtime version)
_snow_ocr_manifest_field(_runtime_platform runtime platform)
_snow_ocr_manifest_field(_archive_name runtime archive name)
_snow_ocr_manifest_field(_archive_size runtime archive size)
_snow_ocr_manifest_field(_archive_sha256 runtime archive sha256)
_snow_ocr_manifest_field(_archive_url runtime archive url)
_snow_ocr_manifest_field(_model_id model id)

string(JSON _runtime_file_count LENGTH "${_manifest}" runtime files)
string(JSON _model_file_count LENGTH "${_manifest}" model files)
if(NOT _runtime_file_count EQUAL 3 OR NOT _model_file_count EQUAL 3)
    message(FATAL_ERROR
        "The checked-in OCR asset manifest must describe 3 runtime files and 3 "
        "model files: ${SNOW_OCR_ASSET_MANIFEST}")
endif()

# _collect_files(<out-prefix> <json-path...>): reads name/size/sha256/url
# quadruples from the manifest. URLs are optional (runtime files have none).
function(_snow_ocr_manifest_files _out_prefix)
    string(JSON _count LENGTH "${_manifest}" ${ARGN})
    math(EXPR _last "${_count} - 1")
    foreach(_index RANGE 0 ${_last})
        _snow_ocr_manifest_field(_name ${ARGN} ${_index} name)
        _snow_ocr_manifest_field(_size ${ARGN} ${_index} size)
        _snow_ocr_manifest_field(_sha256 ${ARGN} ${_index} sha256)
        string(JSON _url ERROR_VARIABLE _url_error GET "${_manifest}" ${ARGN} ${_index} url)
        if(_url MATCHES "-NOTFOUND$")
            set(_url "")
        endif()
        list(APPEND ${_out_prefix}_NAMES "${_name}")
        list(APPEND ${_out_prefix}_SIZES "${_size}")
        list(APPEND ${_out_prefix}_HASHES "${_sha256}")
        list(APPEND ${_out_prefix}_URLS "${_url}")
    endforeach()
    set(${_out_prefix}_NAMES "${${_out_prefix}_NAMES}" PARENT_SCOPE)
    set(${_out_prefix}_SIZES "${${_out_prefix}_SIZES}" PARENT_SCOPE)
    set(${_out_prefix}_HASHES "${${_out_prefix}_HASHES}" PARENT_SCOPE)
    set(${_out_prefix}_URLS "${${_out_prefix}_URLS}" PARENT_SCOPE)
    set(${_out_prefix}_COUNT "${_count}" PARENT_SCOPE)
endfunction()

_snow_ocr_manifest_files(_runtime_file runtime files)
_snow_ocr_manifest_files(_model_file model files)

# _snow_ocr_file_valid(<path> <size> <sha256> <out>): size pre-check, then hash.
function(_snow_ocr_file_valid _path _size _sha256 _out)
    set(_valid FALSE)
    if(EXISTS "${_path}")
        file(SIZE "${_path}" _actual_size)
        if(_actual_size EQUAL _size)
            file(SHA256 "${_path}" _actual_sha256)
            if(_actual_sha256 STREQUAL _sha256)
                set(_valid TRUE)
            endif()
        endif()
    endif()
    set(${_out} ${_valid} PARENT_SCOPE)
endfunction()

# _snow_ocr_fetch(<url> <size> <sha256> <dest> <out>): downloads into the
# artifacts cache when the cached copy is missing or invalid.
function(_snow_ocr_fetch _url _size _sha256 _dest _out)
    _snow_ocr_file_valid("${_dest}" "${_size}" "${_sha256}" _cached_valid)
    if(_cached_valid)
        set(${_out} TRUE PARENT_SCOPE)
        return()
    endif()
    get_filename_component(_dest_directory "${_dest}" DIRECTORY)
    file(MAKE_DIRECTORY "${_dest_directory}")
    message(STATUS "Snow Shot OCR assets: downloading ${_url}")
    file(DOWNLOAD "${_url}" "${_dest}.partial"
        STATUS _status
        TIMEOUT 600
        INACTIVITY_TIMEOUT 120
        TLS_VERIFY ON
        HTTPHEADER "Referer: https://www.modelscope.cn/"
    )
    list(GET _status 0 _status_code)
    list(GET _status 1 _status_text)
    if(NOT _status_code EQUAL 0)
        file(REMOVE "${_dest}.partial")
        message(WARNING
            "Snow Shot OCR assets: download of ${_url} failed (${_status_text}). "
            "Text recognition will be unavailable until a build with network "
            "access stages it.")
        set(${_out} FALSE PARENT_SCOPE)
        return()
    endif()
    _snow_ocr_file_valid("${_dest}.partial" "${_size}" "${_sha256}" _download_valid)
    if(NOT _download_valid)
        file(REMOVE "${_dest}.partial")
        message(WARNING
            "Snow Shot OCR assets: ${_url} failed SHA-256 verification.")
        set(${_out} FALSE PARENT_SCOPE)
        return()
    endif()
    file(RENAME "${_dest}.partial" "${_dest}")
    set(${_out} TRUE PARENT_SCOPE)
endfunction()

set(_runtime_directory
    "${SNOW_OCR_DESTINATION}/runtimes/${_runtime_version}/${_runtime_platform}")
set(_model_directory "${SNOW_OCR_DESTINATION}/models/${_model_id}")

# The runtime payload travels as a single hash-pinned ZIP; the models are
# per-file downloads shared with the release packaging cache layout.
set(_model_cache_directory "${SNOW_OCR_ARTIFACTS_DIR}/ocr-models-${_model_id}")
set(_models_ready TRUE)
foreach(_index RANGE 0 2)
    list(GET _model_file_NAMES ${_index} _name)
    list(GET _model_file_SIZES ${_index} _size)
    list(GET _model_file_HASHES ${_index} _sha256)
    list(GET _model_file_URLS ${_index} _url)
    _snow_ocr_fetch("${_url}" "${_size}" "${_sha256}"
        "${_model_cache_directory}/${_name}" _fetched)
    if(NOT _fetched)
        set(_models_ready FALSE)
    endif()
endforeach()

_snow_ocr_file_valid("${SNOW_OCR_ARTIFACTS_DIR}/${_archive_name}"
    "${_archive_size}" "${_archive_sha256}" _archive_cached)
if(NOT _archive_cached)
    _snow_ocr_fetch("${_archive_url}" "${_archive_size}" "${_archive_sha256}"
        "${SNOW_OCR_ARTIFACTS_DIR}/${_archive_name}" _archive_cached)
endif()

# Restage the runtime only when a staged file fails verification.
set(_runtime_staged TRUE)
foreach(_index RANGE 0 2)
    list(GET _runtime_file_NAMES ${_index} _name)
    list(GET _runtime_file_SIZES ${_index} _size)
    list(GET _runtime_file_HASHES ${_index} _sha256)
    _snow_ocr_file_valid("${_runtime_directory}/${_name}" "${_size}" "${_sha256}"
        _file_valid)
    if(NOT _file_valid)
        set(_runtime_staged FALSE)
    endif()
endforeach()

if(NOT _runtime_staged AND _archive_cached)
    message(STATUS
        "Snow Shot OCR assets: extracting ${_archive_name} into the output directory")
    file(MAKE_DIRECTORY "${_runtime_directory}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E tar xzf
            "${SNOW_OCR_ARTIFACTS_DIR}/${_archive_name}"
        WORKING_DIRECTORY "${_runtime_directory}"
        RESULT_VARIABLE _extract_result
    )
    if(NOT _extract_result EQUAL 0)
        message(WARNING
            "Snow Shot OCR assets: could not extract ${_archive_name}.")
    endif()
endif()

if(_models_ready)
    foreach(_index RANGE 0 2)
        list(GET _model_file_NAMES ${_index} _name)
        list(GET _model_file_SIZES ${_index} _size)
        list(GET _model_file_HASHES ${_index} _sha256)
        _snow_ocr_file_valid("${_model_directory}/${_name}" "${_size}" "${_sha256}"
            _file_valid)
        if(NOT _file_valid)
            file(MAKE_DIRECTORY "${_model_directory}")
            file(COPY_FILE "${_model_cache_directory}/${_name}"
                "${_model_directory}/${_name}" ONLY_IF_DIFFERENT)
        endif()
    endforeach()
endif()

# Completion markers are written only after a fully verified payload so the
# client never treats a partially staged component as complete.
set(_runtime_ok TRUE)
foreach(_index RANGE 0 2)
    list(GET _runtime_file_NAMES ${_index} _name)
    list(GET _runtime_file_SIZES ${_index} _size)
    list(GET _runtime_file_HASHES ${_index} _sha256)
    _snow_ocr_file_valid("${_runtime_directory}/${_name}" "${_size}" "${_sha256}"
        _file_valid)
    if(NOT _file_valid)
        set(_runtime_ok FALSE)
    endif()
endforeach()
set(_models_ok TRUE)
foreach(_index RANGE 0 2)
    list(GET _model_file_NAMES ${_index} _name)
    list(GET _model_file_SIZES ${_index} _size)
    list(GET _model_file_HASHES ${_index} _sha256)
    _snow_ocr_file_valid("${_model_directory}/${_name}" "${_size}" "${_sha256}"
        _file_valid)
    if(NOT _file_valid)
        set(_models_ok FALSE)
    endif()
endforeach()

if(_runtime_ok)
    file(WRITE "${_runtime_directory}/.complete.json"
        "{\"schema\":1,\"component\":\"${_runtime_version}\"}")
endif()
if(_models_ok)
    file(WRITE "${_model_directory}/.complete.json"
        "{\"schema\":1,\"component\":\"${_model_id}\"}")
endif()

file(MAKE_DIRECTORY "${SNOW_OCR_DESTINATION}")
file(COPY_FILE "${SNOW_OCR_ASSET_MANIFEST}"
    "${SNOW_OCR_DESTINATION}/asset-manifest.json" ONLY_IF_DIFFERENT)

if(NOT _runtime_ok OR NOT _models_ok)
    message(WARNING
        "Snow Shot OCR assets are incomplete in ${SNOW_OCR_DESTINATION}; text "
        "recognition will report its components as unprepared. Rebuild with "
        "network access to stage them.")
else()
    message(STATUS
        "Snow Shot OCR assets are staged in ${SNOW_OCR_DESTINATION}")
endif()
