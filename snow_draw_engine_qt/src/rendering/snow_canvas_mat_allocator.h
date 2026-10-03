// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <snow/memory/pixel_buffer.h>
#include <opencv2/core.hpp>

#include <algorithm>
#include <limits>
#include <memory>

namespace snow_canvas_smart_erase {

// Set this allocator on job-owned Mat headers, including empty output headers.
// Changing OpenCV's process-wide allocator would also change unrelated workers.
class PixelMatAllocator final : public cv::MatAllocator {
  public:
    cv::UMatData* allocate(int dimensions, const int* sizes, int type, void* data,
                           std::size_t* steps, cv::AccessFlag flags,
                           cv::UMatUsageFlags usage) const override {
        if (data != nullptr) {
            return cv::Mat::getStdAllocator()->allocate(dimensions, sizes, type, data, steps, flags,
                                                        usage);
        }
        std::size_t bytes = CV_ELEM_SIZE(type);
        for (int i = dimensions; i-- > 0;) {
            if (sizes[i] < 0 || (sizes[i] > 0 && bytes > std::numeric_limits<std::size_t>::max() /
                                                             static_cast<std::size_t>(sizes[i]))) {
                CV_Error(cv::Error::StsNoMem, "image matrix allocation overflow");
            }
            if (steps != nullptr) {
                steps[i] = bytes;
            }
            bytes *= static_cast<std::size_t>(sizes[i]);
        }
        if (bytes < snow::memory::kMappedPixelBufferMinimum) {
            return cv::Mat::getStdAllocator()->allocate(dimensions, sizes, type, nullptr, steps,
                                                        flags, usage);
        }
        auto pixels = snow::memory::allocatePixelBuffer(bytes);
        if (!pixels) {
            CV_Error(cv::Error::StsNoMem, "image matrix allocation failed");
        }
        auto owner = std::make_unique<cv::UMatData>(this);
        owner->data = owner->origdata = pixels.release();
        owner->size = bytes;
        return owner.release();
    }

    bool allocate(cv::UMatData* data, cv::AccessFlag flags,
                  cv::UMatUsageFlags usage) const override {
        return cv::Mat::getStdAllocator()->allocate(data, flags, usage);
    }

    void deallocate(cv::UMatData* data) const override {
        if (data != nullptr) {
            snow::memory::releasePixelBuffer(data->origdata, data->size);
            delete data;
        }
    }
};

inline PixelMatAllocator& pixelMatAllocator() {
    static PixelMatAllocator allocator;
    return allocator;
}

template <typename T> cv::Mat_<T> pixelMatrix() {
    cv::Mat_<T> matrix;
    matrix.allocator = &pixelMatAllocator();
    return matrix;
}

template <typename T> cv::Mat_<T> pixelMatrix(cv::Size size) {
    auto matrix = pixelMatrix<T>();
    matrix.create(size);
    return matrix;
}

template <typename T> cv::Mat_<T> pixelMatrix(cv::Size size, const T& value) {
    auto matrix = pixelMatrix<T>(size);
    std::fill_n(matrix.template ptr<T>(), matrix.total(), value);
    return matrix;
}

template <typename T> cv::Mat_<T> pixelMatrixCopy(const cv::Mat_<T>& source) {
    auto matrix = pixelMatrix<T>();
    source.copyTo(matrix);
    return matrix;
}

inline cv::Mat1b pixelMatrixNot(const cv::Mat1b& source) {
    auto output = pixelMatrix<unsigned char>();
    cv::bitwise_not(source, output);
    return output;
}

} // namespace snow_canvas_smart_erase
