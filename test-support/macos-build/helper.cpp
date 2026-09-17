#include <dlfcn.h>
#include <filesystem>

int main(int, char** argv) {
    const auto library = std::filesystem::absolute(argv[0]).parent_path() / "libonnxruntime.dylib";
    void* handle = dlopen(library.c_str(), RTLD_NOW);
    if (handle == nullptr) {
        return 1;
    }
    auto probe = reinterpret_cast<int (*)()>(dlsym(handle, "snow_test_onnx"));
    const int result = probe == nullptr ? 2 : probe();
    dlclose(handle);
    return result;
}
