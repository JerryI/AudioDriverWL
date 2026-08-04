#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MA_NO_GENERATION
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "microphone_helper_protocol.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct HelperState {
    ma_uint32 channels;
    ma_atomic_uint64 frames_sent;
    volatile sig_atomic_t output_failed;
} HelperState;

static int write_all(int descriptor, const void* data, size_t size)
{
    const unsigned char* cursor = (const unsigned char*)data;
    while (size > 0) {
        ssize_t written = write(descriptor, cursor, size);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            return -1;
        }
        cursor += (size_t)written;
        size -= (size_t)written;
    }
    return 0;
}

static void capture_callback(
    ma_device* device,
    void* output,
    const void* input,
    ma_uint32 frame_count)
{
    HelperState* state = (HelperState*)device->pUserData;
    size_t byte_count;

    (void)output;
    if (state == NULL || input == NULL || state->output_failed) {
        return;
    }
    byte_count = (size_t)frame_count * state->channels * sizeof(float);
    if (write_all(STDOUT_FILENO, input, byte_count) != 0) {
        state->output_failed = 1;
    } else {
        ma_atomic_uint64_fetch_add(&state->frames_sent, frame_count);
    }
}

static void write_response(unsigned char command, HelperState* state)
{
    MicrophoneHelperResponse response;
    response.magic = MICROPHONE_HELPER_MAGIC;
    response.command = command;
    response.frames_sent = ma_atomic_uint64_get(&state->frames_sent);
    (void)write_all(MICROPHONE_HELPER_STATUS_FD, &response, sizeof(response));
}

static ma_uint32 parse_uint32(const char* text)
{
    char* end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (text == end || *end != '\0' || value > UINT32_MAX) {
        return UINT32_MAX;
    }
    return (ma_uint32)value;
}

int main(int argc, char** argv)
{
    ma_device_config config;
    ma_device device;
    ma_result result;
    HelperState state;
    MicrophoneHelperHeader header;
    ma_uint32 sample_rate;
    ma_uint32 channels;
    ma_uint32 period_frames;
    ma_uint32 profile;
    unsigned char command;

    if (argc != 5) {
        return 2;
    }
    sample_rate = parse_uint32(argv[1]);
    channels = parse_uint32(argv[2]);
    period_frames = parse_uint32(argv[3]);
    profile = parse_uint32(argv[4]);
    if (sample_rate == UINT32_MAX || channels == UINT32_MAX ||
        period_frames == UINT32_MAX || profile > 1) {
        return 3;
    }

    signal(SIGPIPE, SIG_IGN);
    memset(&state, 0, sizeof(state));
    ma_atomic_uint64_set(&state.frames_sent, 0);
    config = ma_device_config_init(ma_device_type_capture);
    config.capture.format = ma_format_f32;
    config.capture.channels = channels;
    config.sampleRate = sample_rate;
    config.periodSizeInFrames = period_frames;
    config.performanceProfile = profile == 0
        ? ma_performance_profile_low_latency
        : ma_performance_profile_conservative;
    config.noFixedSizedCallback = MA_TRUE;
    config.dataCallback = capture_callback;
    config.pUserData = &state;

    result = ma_device_init(NULL, &config, &device);
    if (result != MA_SUCCESS) {
        return 4;
    }
    state.channels = device.capture.channels;

    memset(&header, 0, sizeof(header));
    header.magic = MICROPHONE_HELPER_MAGIC;
    header.sample_rate = device.sampleRate;
    header.channels = device.capture.channels;
    header.period_frames = device.capture.internalPeriodSizeInFrames;
    header.internal_sample_rate = device.capture.internalSampleRate;
    header.internal_channels = device.capture.internalChannels;
    header.internal_format = (uint32_t)device.capture.internalFormat;
    ma_strncpy_s(header.device_name, sizeof(header.device_name), device.capture.name, (size_t)-1);
    if (write_all(MICROPHONE_HELPER_STATUS_FD, &header, sizeof(header)) != 0) {
        ma_device_uninit(&device);
        return 5;
    }

    result = ma_device_start(&device);
    if (result != MA_SUCCESS) {
        ma_device_uninit(&device);
        return 6;
    }

    while (!state.output_failed) {
        ssize_t count = read(STDIN_FILENO, &command, 1);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            break;
        }
        if (command == 'Q') {
            write_response(command, &state);
            break;
        } else if (command == 'S') {
            (void)ma_device_stop(&device);
        } else if (command == 'R') {
            (void)ma_device_start(&device);
        }
        write_response(command, &state);
    }

    ma_device_uninit(&device);
    return 0;
}
