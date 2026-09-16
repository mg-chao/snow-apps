# Downloads the WeChat QR detection/super-resolution models consumed by
# OpenCV's contrib wechat_qrcode module in Snow Shot's barcode recognition
# service. The URLs and hashes mirror the opencv4 vcpkg port's downloads
# (WeChatCV/opencv_3rdparty @ a8b69ccc) so both builds consume identical,
# pinned payloads. The vcpkg port fetches them only for its own build tree,
# so Snow Shot acquires its runtime copies here.
#
# snow_shot_fetch_qr_models(<out-var>) sets <out-var> to the directory that
# contains detect.prototxt, detect.caffemodel, sr.prototxt, and sr.caffemodel.

function(snow_shot_fetch_qr_models out_var)
    set(_destination "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../artifacts/qr-models-wechat")
    file(MAKE_DIRECTORY "${_destination}")

    set(_base_url "https://raw.githubusercontent.com/WeChatCV/opencv_3rdparty/a8b69ccc738421293254aec5ddb38bd523503252")
    set(_models
        "detect.caffemodel|58d62faf8679d3f568a26a1d9f7c2e88060426a440315ca8bce7b3b5a8efa34be670afd0abfd0dd5d89f89a042a2408ea602f937080abc6910c2e497b7f5a4b8"
        "sr.caffemodel|917c6f6b84a898b8c8c85c79359e48a779c8a600de563dac2e1c5d013401e9ac9dbcd435013a4ed7a69fc936839fb189aaa3038c127d04ceb6fd3b8fd9dd67bd"
        "detect.prototxt|2239d31a597049f358f09dbb4c0a7af0b384d9b67cfa3224f8c7e44329647cf19ee7929ac06199cca23bbbf431de0481b74ab51eace6aa20bb2e2fd19b536e49"
        "sr.prototxt|6b715ec45c3fd081e7e113e351edcef0f3d32a75f8b5a9ca2273cb5da9a1116a1b78cba45582a9acf67a7ab76dc4fcdf123f7b3a0d3de2f5c39b26ef450058b7")

    foreach(_model IN LISTS _models)
        string(REPLACE "|" ";" _parts "${_model}")
        list(GET _parts 0 _name)
        list(GET _parts 1 _sha512)
        # file(DOWNLOAD) skips the transfer when the existing file already
        # matches EXPECTED_HASH, so repeated configures stay offline-friendly.
        file(DOWNLOAD
            "${_base_url}/${_name}"
            "${_destination}/${_name}"
            EXPECTED_HASH "SHA512=${_sha512}"
            STATUS _status)
        list(GET _status 0 _status_code)
        if(NOT _status_code EQUAL 0)
            list(GET _status 1 _status_message)
            file(REMOVE "${_destination}/${_name}")
            message(FATAL_ERROR
                "Failed to download the WeChat QR model ${_name}: ${_status_message}")
        endif()
    endforeach()

    set(${out_var} "${_destination}" PARENT_SCOPE)
endfunction()
