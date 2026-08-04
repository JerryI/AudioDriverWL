#ifndef MICROPHONE_HELPER_PROTOCOL_H
#define MICROPHONE_HELPER_PROTOCOL_H

#include <stdint.h>

#define MICROPHONE_HELPER_MAGIC UINT32_C(0x4D494348)
#define MICROPHONE_HELPER_NAME_CAPACITY 256
#define MICROPHONE_HELPER_STATUS_FD 2

typedef struct MicrophoneHelperHeader {
    uint32_t magic;
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t period_frames;
    uint32_t internal_sample_rate;
    uint32_t internal_channels;
    uint32_t internal_format;
    char device_name[MICROPHONE_HELPER_NAME_CAPACITY];
} MicrophoneHelperHeader;

typedef struct MicrophoneHelperResponse {
    uint32_t magic;
    uint32_t command;
    uint64_t frames_sent;
} MicrophoneHelperResponse;

#endif
