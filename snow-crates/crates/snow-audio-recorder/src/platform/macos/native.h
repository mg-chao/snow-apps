// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char id[1024];
    char name[1024];
    uint8_t is_default;
} SnowAudioDevice;

typedef struct {
    uint32_t frames;
    uint64_t end_host_ns;
    uint64_t dropped_frames;
} SnowAudioPacket;

int32_t snow_audio_devices(SnowAudioDevice* devices, size_t capacity, char* error,
                           size_t error_capacity);
void* snow_audio_start(uint8_t microphone, const char* device, uint32_t rate, uint16_t channels,
                       size_t queue_depth, int32_t* status, char* error, size_t error_capacity);
int32_t snow_audio_poll(void* handle, int16_t* samples, size_t sample_capacity,
                        SnowAudioPacket* packet, uint32_t timeout_ms, char* error,
                        size_t error_capacity);
void snow_audio_stop(void* handle);
uint64_t snow_audio_host_time_ns(void);
