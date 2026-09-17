#include <QCoreApplication>

extern "C" int snow_test_onnx();

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    return snow_test_onnx();
}
