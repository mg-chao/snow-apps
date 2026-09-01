#include <snow/image/service.h>
#include <snow/image/version.h>

#include <iostream>

int main() {
    snow::image::Service service;
    if (service.formats().empty() || SNOW_IMAGE_VERSION_MAJOR != 1) {
        return 1;
    }
    std::cout << service.formats().size() << " formats\n";
    return 0;
}
