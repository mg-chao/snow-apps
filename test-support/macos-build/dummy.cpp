extern "C" unsigned avutil_version(void);
extern "C" int snow_test_onnx();

int snow_test_onnx() {
    return avutil_version() == 0;
}
