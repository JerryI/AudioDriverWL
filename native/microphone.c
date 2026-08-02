#include "WolframLibrary.h"
#include "WolframNumericArrayLibrary.h"

#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MA_NO_GENERATION
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
#define MIC_THREAD_LOCAL __declspec(thread)
#else
#define MIC_THREAD_LOCAL __thread
#endif

#define MIC_MAX_DEVICES 64
#define MIC_MAX_CHANNELS 32
#define MIC_MIN_SAMPLE_RATE 8000
#define MIC_MAX_SAMPLE_RATE 384000
#define MIC_MIN_BUFFER_MS 10
#define MIC_MAX_BUFFER_MS 60000
#define MIC_MAX_PERIOD_FRAMES 262144

typedef struct MicrophoneDevice {
    mint id;
    ma_device device;
    ma_pcm_rb ring;
    ma_bool32 device_initialized;
    ma_bool32 ring_initialized;
    ma_uint32 requested_sample_rate;
    ma_uint32 requested_channels;
    ma_uint32 buffer_ms;
    ma_uint32 requested_period_frames;
    ma_performance_profile performance_profile;
    ma_uint32 sample_rate;
    ma_uint32 channels;
    ma_uint32 buffer_frames;
    ma_atomic_uint64 dropped_frames;
    ma_result last_error;
} MicrophoneDevice;

static MicrophoneDevice* g_devices[MIC_MAX_DEVICES];
static ma_mutex g_registry_lock;
static ma_bool32 g_registry_lock_initialized = MA_FALSE;
static mint g_next_id = 1;
static ma_result g_last_open_error = MA_SUCCESS;
static MIC_THREAD_LOCAL char g_string_result[512];

static MicrophoneDevice* find_device_unlocked(mint id)
{
    int i;
    for (i = 0; i < MIC_MAX_DEVICES; ++i) {
        if (g_devices[i] != NULL && g_devices[i]->id == id) {
            return g_devices[i];
        }
    }
    return NULL;
}

static int find_free_slot_unlocked(void)
{
    int i;
    for (i = 0; i < MIC_MAX_DEVICES; ++i) {
        if (g_devices[i] == NULL) {
            return i;
        }
    }
    return -1;
}

static void capture_callback(ma_device* device, void* output, const void* input, ma_uint32 frame_count)
{
    MicrophoneDevice* microphone = (MicrophoneDevice*)device->pUserData;
    ma_uint32 written = 0;

    (void)output;
    if (microphone == NULL || input == NULL || !microphone->ring_initialized) {
        return;
    }

    while (written < frame_count) {
        void* destination = NULL;
        ma_uint32 frames = frame_count - written;
        ma_result result = ma_pcm_rb_acquire_write(&microphone->ring, &frames, &destination);

        if (result != MA_SUCCESS || frames == 0) {
            break;
        }

        memcpy(destination,
               ((const float*)input) + ((size_t)written * microphone->channels),
               (size_t)frames * microphone->channels * sizeof(float));

        if (ma_pcm_rb_commit_write(&microphone->ring, frames) != MA_SUCCESS) {
            break;
        }
        written += frames;
    }

    if (written < frame_count) {
        ma_atomic_uint64_fetch_add(&microphone->dropped_frames, (ma_uint64)(frame_count - written));
    }
}

static void shutdown_device(MicrophoneDevice* microphone)
{
    if (microphone->device_initialized) {
        ma_device_uninit(&microphone->device);
        microphone->device_initialized = MA_FALSE;
    }
    if (microphone->ring_initialized) {
        ma_pcm_rb_uninit(&microphone->ring);
        microphone->ring_initialized = MA_FALSE;
    }
}

static ma_result validate_configuration(ma_uint32 sample_rate,
                                        ma_uint32 channels,
                                        ma_uint32 buffer_ms,
                                        ma_uint32 period_frames,
                                        mint profile)
{
    if (sample_rate != 0 && (sample_rate < MIC_MIN_SAMPLE_RATE || sample_rate > MIC_MAX_SAMPLE_RATE)) {
        return MA_INVALID_ARGS;
    }
    if (channels > MIC_MAX_CHANNELS) {
        return MA_INVALID_ARGS;
    }
    if (buffer_ms < MIC_MIN_BUFFER_MS || buffer_ms > MIC_MAX_BUFFER_MS) {
        return MA_INVALID_ARGS;
    }
    if (period_frames > MIC_MAX_PERIOD_FRAMES || (profile != 0 && profile != 1)) {
        return MA_INVALID_ARGS;
    }
    return MA_SUCCESS;
}

static ma_result start_device(MicrophoneDevice* microphone,
                              ma_uint32 sample_rate,
                              ma_uint32 channels,
                              ma_uint32 buffer_ms,
                              ma_uint32 period_frames,
                              mint profile)
{
    ma_device_config config;
    ma_result result;
    uint64_t buffer_frames_64;

    result = validate_configuration(sample_rate, channels, buffer_ms, period_frames, profile);
    if (result != MA_SUCCESS) {
        return result;
    }

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
    config.pUserData = microphone;

    result = ma_device_init(NULL, &config, &microphone->device);
    if (result != MA_SUCCESS) {
        return result;
    }
    microphone->device_initialized = MA_TRUE;
    microphone->sample_rate = microphone->device.sampleRate;
    microphone->channels = microphone->device.capture.channels;

    buffer_frames_64 = ((uint64_t)microphone->sample_rate * buffer_ms + 999U) / 1000U;
    if (buffer_frames_64 == 0 || buffer_frames_64 > UINT32_MAX) {
        shutdown_device(microphone);
        return MA_INVALID_ARGS;
    }

    result = ma_pcm_rb_init(ma_format_f32,
                            microphone->channels,
                            (ma_uint32)buffer_frames_64,
                            NULL,
                            NULL,
                            &microphone->ring);
    if (result != MA_SUCCESS) {
        shutdown_device(microphone);
        return result;
    }
    microphone->ring_initialized = MA_TRUE;
    microphone->buffer_frames = ma_pcm_rb_get_subbuffer_size(&microphone->ring);
    ma_atomic_uint64_set(&microphone->dropped_frames, 0);

    result = ma_device_start(&microphone->device);
    if (result != MA_SUCCESS) {
        shutdown_device(microphone);
        return result;
    }

    microphone->requested_sample_rate = sample_rate;
    microphone->requested_channels = channels;
    microphone->buffer_ms = buffer_ms;
    microphone->requested_period_frames = period_frames;
    microphone->performance_profile = config.performanceProfile;
    microphone->last_error = MA_SUCCESS;
    return MA_SUCCESS;
}

static ma_result reconfigure_device(MicrophoneDevice* microphone,
                                    ma_uint32 sample_rate,
                                    ma_uint32 channels,
                                    ma_uint32 buffer_ms,
                                    ma_uint32 period_frames,
                                    mint profile)
{
    ma_uint32 old_sample_rate = microphone->requested_sample_rate;
    ma_uint32 old_channels = microphone->requested_channels;
    ma_uint32 old_buffer_ms = microphone->buffer_ms;
    ma_uint32 old_period_frames = microphone->requested_period_frames;
    mint old_profile = microphone->performance_profile == ma_performance_profile_conservative ? 1 : 0;
    ma_result result;

    shutdown_device(microphone);
    result = start_device(microphone, sample_rate, channels, buffer_ms, period_frames, profile);
    if (result != MA_SUCCESS) {
        ma_result original_result = result;
        (void)start_device(microphone,
                           old_sample_rate,
                           old_channels,
                           old_buffer_ms,
                           old_period_frames,
                           old_profile);
        microphone->last_error = original_result;
        return original_result;
    }
    return MA_SUCCESS;
}

static mint clamp_uint64_to_mint(ma_uint64 value)
{
    ma_uint64 maximum = (sizeof(mint) >= sizeof(ma_uint64))
        ? (ma_uint64)INT64_MAX
        : (ma_uint64)INT32_MAX;
    return (mint)(value > maximum ? maximum : value);
}

DLLEXPORT mint WolframLibrary_getVersion(void)
{
    return WolframLibraryVersion;
}

DLLEXPORT int WolframLibrary_initialize(WolframLibraryData library_data)
{
    (void)library_data;
    memset(g_devices, 0, sizeof(g_devices));
    if (ma_mutex_init(&g_registry_lock) != MA_SUCCESS) {
        return LIBRARY_FUNCTION_ERROR;
    }
    g_registry_lock_initialized = MA_TRUE;
    g_next_id = 1;
    g_last_open_error = MA_SUCCESS;
    return LIBRARY_NO_ERROR;
}

DLLEXPORT void WolframLibrary_uninitialize(WolframLibraryData library_data)
{
    int i;
    (void)library_data;
    if (!g_registry_lock_initialized) {
        return;
    }

    ma_mutex_lock(&g_registry_lock);
    for (i = 0; i < MIC_MAX_DEVICES; ++i) {
        if (g_devices[i] != NULL) {
            shutdown_device(g_devices[i]);
            free(g_devices[i]);
            g_devices[i] = NULL;
        }
    }
    ma_mutex_unlock(&g_registry_lock);
    ma_mutex_uninit(&g_registry_lock);
    g_registry_lock_initialized = MA_FALSE;
}

DLLEXPORT int microphoneOpen(WolframLibraryData library_data, mint argument_count, MArgument* arguments, MArgument result)
{
    MicrophoneDevice* microphone;
    ma_result native_result;
    mint sample_rate;
    mint channels;
    mint buffer_ms;
    mint period_frames;
    mint profile;
    int slot;

    (void)library_data;
    if (argument_count != 5) {
        return LIBRARY_FUNCTION_ERROR;
    }

    sample_rate = MArgument_getInteger(arguments[0]);
    channels = MArgument_getInteger(arguments[1]);
    buffer_ms = MArgument_getInteger(arguments[2]);
    period_frames = MArgument_getInteger(arguments[3]);
    profile = MArgument_getInteger(arguments[4]);
    if (sample_rate < 0 || channels < 0 || buffer_ms < 0 || period_frames < 0 ||
        sample_rate > UINT32_MAX || channels > UINT32_MAX || buffer_ms > UINT32_MAX || period_frames > UINT32_MAX) {
        g_last_open_error = MA_INVALID_ARGS;
        MArgument_setInteger(result, 0);
        return LIBRARY_NO_ERROR;
    }

    microphone = (MicrophoneDevice*)calloc(1, sizeof(*microphone));
    if (microphone == NULL) {
        g_last_open_error = MA_OUT_OF_MEMORY;
        MArgument_setInteger(result, 0);
        return LIBRARY_NO_ERROR;
    }

    ma_mutex_lock(&g_registry_lock);
    slot = find_free_slot_unlocked();
    if (slot < 0) {
        ma_mutex_unlock(&g_registry_lock);
        free(microphone);
        g_last_open_error = MA_TOO_MANY_OPEN_FILES;
        MArgument_setInteger(result, 0);
        return LIBRARY_NO_ERROR;
    }

    microphone->id = g_next_id++;
    if (g_next_id <= 0) {
        g_next_id = 1;
    }
    native_result = start_device(microphone,
                                 (ma_uint32)sample_rate,
                                 (ma_uint32)channels,
                                 (ma_uint32)buffer_ms,
                                 (ma_uint32)period_frames,
                                 profile);
    if (native_result != MA_SUCCESS) {
        ma_mutex_unlock(&g_registry_lock);
        free(microphone);
        g_last_open_error = native_result;
        MArgument_setInteger(result, 0);
        return LIBRARY_NO_ERROR;
    }

    g_devices[slot] = microphone;
    g_last_open_error = MA_SUCCESS;
    MArgument_setInteger(result, microphone->id);
    ma_mutex_unlock(&g_registry_lock);
    return LIBRARY_NO_ERROR;
}

DLLEXPORT int microphoneClose(WolframLibraryData library_data, mint argument_count, MArgument* arguments, MArgument result)
{
    mint id;
    int i;
    ma_result native_result = MA_INVALID_ARGS;

    (void)library_data;
    if (argument_count != 1) {
        return LIBRARY_FUNCTION_ERROR;
    }
    id = MArgument_getInteger(arguments[0]);

    ma_mutex_lock(&g_registry_lock);
    for (i = 0; i < MIC_MAX_DEVICES; ++i) {
        if (g_devices[i] != NULL && g_devices[i]->id == id) {
            MicrophoneDevice* microphone = g_devices[i];
            g_devices[i] = NULL;
            shutdown_device(microphone);
            free(microphone);
            native_result = MA_SUCCESS;
            break;
        }
    }
    ma_mutex_unlock(&g_registry_lock);
    MArgument_setInteger(result, (mint)native_result);
    return LIBRARY_NO_ERROR;
}

DLLEXPORT int microphoneConfigure(WolframLibraryData library_data, mint argument_count, MArgument* arguments, MArgument result)
{
    mint id;
    mint sample_rate;
    mint channels;
    mint buffer_ms;
    mint period_frames;
    mint profile;
    MicrophoneDevice* microphone;
    ma_result native_result = MA_INVALID_ARGS;

    (void)library_data;
    if (argument_count != 6) {
        return LIBRARY_FUNCTION_ERROR;
    }
    id = MArgument_getInteger(arguments[0]);
    sample_rate = MArgument_getInteger(arguments[1]);
    channels = MArgument_getInteger(arguments[2]);
    buffer_ms = MArgument_getInteger(arguments[3]);
    period_frames = MArgument_getInteger(arguments[4]);
    profile = MArgument_getInteger(arguments[5]);

    if (sample_rate < 0 || channels < 0 || buffer_ms < 0 || period_frames < 0 ||
        sample_rate > UINT32_MAX || channels > UINT32_MAX || buffer_ms > UINT32_MAX || period_frames > UINT32_MAX) {
        MArgument_setInteger(result, (mint)MA_INVALID_ARGS);
        return LIBRARY_NO_ERROR;
    }

    ma_mutex_lock(&g_registry_lock);
    microphone = find_device_unlocked(id);
    if (microphone != NULL) {
        native_result = reconfigure_device(microphone,
                                           (ma_uint32)sample_rate,
                                           (ma_uint32)channels,
                                           (ma_uint32)buffer_ms,
                                           (ma_uint32)period_frames,
                                           profile);
    }
    ma_mutex_unlock(&g_registry_lock);
    MArgument_setInteger(result, (mint)native_result);
    return LIBRARY_NO_ERROR;
}

DLLEXPORT int microphoneRead(WolframLibraryData library_data, mint argument_count, MArgument* arguments, MArgument result)
{
    WolframNumericArrayLibrary_Functions numeric_arrays = library_data->numericarrayLibraryFunctions;
    MicrophoneDevice* microphone;
    MNumericArray array = NULL;
    mint dimensions[2];
    mint id;
    mint requested_frames;
    ma_uint32 available;
    ma_uint32 frames_to_read;
    ma_uint32 frames_read = 0;
    float* output;
    int error;

    if (argument_count != 2 || numeric_arrays == NULL) {
        return LIBRARY_FUNCTION_ERROR;
    }
    id = MArgument_getInteger(arguments[0]);
    requested_frames = MArgument_getInteger(arguments[1]);

    ma_mutex_lock(&g_registry_lock);
    microphone = find_device_unlocked(id);
    if (microphone == NULL || !microphone->ring_initialized) {
        ma_mutex_unlock(&g_registry_lock);
        return LIBRARY_FUNCTION_ERROR;
    }

    available = ma_pcm_rb_available_read(&microphone->ring);
    frames_to_read = requested_frames < 0 || (ma_uint64)requested_frames > available
        ? available
        : (ma_uint32)requested_frames;
    dimensions[0] = (mint)frames_to_read;
    dimensions[1] = (mint)microphone->channels;
    error = numeric_arrays->MNumericArray_new(MNumericArray_Type_Real32, 2, dimensions, &array);
    if (error != LIBRARY_NO_ERROR) {
        ma_mutex_unlock(&g_registry_lock);
        return error;
    }

    output = (float*)numeric_arrays->MNumericArray_getData(array);
    while (frames_read < frames_to_read) {
        void* source = NULL;
        ma_uint32 frames = frames_to_read - frames_read;
        ma_result native_result = ma_pcm_rb_acquire_read(&microphone->ring, &frames, &source);
        if (native_result != MA_SUCCESS || frames == 0) {
            numeric_arrays->MNumericArray_free(array);
            ma_mutex_unlock(&g_registry_lock);
            return LIBRARY_FUNCTION_ERROR;
        }
        memcpy(output + ((size_t)frames_read * microphone->channels),
               source,
               (size_t)frames * microphone->channels * sizeof(float));
        if (ma_pcm_rb_commit_read(&microphone->ring, frames) != MA_SUCCESS) {
            numeric_arrays->MNumericArray_free(array);
            ma_mutex_unlock(&g_registry_lock);
            return LIBRARY_FUNCTION_ERROR;
        }
        frames_read += frames;
    }

    MArgument_setMNumericArray(result, array);
    ma_mutex_unlock(&g_registry_lock);
    return LIBRARY_NO_ERROR;
}

DLLEXPORT int microphoneGetInteger(WolframLibraryData library_data, mint argument_count, MArgument* arguments, MArgument result)
{
    MicrophoneDevice* microphone;
    mint id;
    mint property;
    mint value = -1;

    (void)library_data;
    if (argument_count != 2) {
        return LIBRARY_FUNCTION_ERROR;
    }
    id = MArgument_getInteger(arguments[0]);
    property = MArgument_getInteger(arguments[1]);

    ma_mutex_lock(&g_registry_lock);
    microphone = find_device_unlocked(id);
    if (microphone != NULL) {
        switch (property) {
            case 0: value = (mint)microphone->sample_rate; break;
            case 1: value = (mint)microphone->channels; break;
            case 2: value = (mint)microphone->buffer_frames; break;
            case 3: value = (mint)ma_pcm_rb_available_read(&microphone->ring); break;
            case 4: value = clamp_uint64_to_mint(ma_atomic_uint64_get(&microphone->dropped_frames)); break;
            case 5: value = (mint)microphone->device.capture.internalPeriodSizeInFrames; break;
            case 6: value = ma_device_is_started(&microphone->device) ? 1 : 0; break;
            case 7: value = microphone->performance_profile == ma_performance_profile_conservative ? 1 : 0; break;
            case 8: value = (mint)microphone->device.capture.internalSampleRate; break;
            case 9: value = (mint)microphone->device.capture.internalChannels; break;
            default: value = -1; break;
        }
    }
    ma_mutex_unlock(&g_registry_lock);
    MArgument_setInteger(result, value);
    return LIBRARY_NO_ERROR;
}

DLLEXPORT int microphoneGetString(WolframLibraryData library_data, mint argument_count, MArgument* arguments, MArgument result)
{
    MicrophoneDevice* microphone;
    mint id;
    mint property;
    const char* source = "Unknown";

    (void)library_data;
    if (argument_count != 2) {
        return LIBRARY_FUNCTION_ERROR;
    }
    id = MArgument_getInteger(arguments[0]);
    property = MArgument_getInteger(arguments[1]);
    g_string_result[0] = '\0';

    ma_mutex_lock(&g_registry_lock);
    microphone = find_device_unlocked(id);
    if (microphone != NULL) {
        switch (property) {
            case 0:
                if (ma_device_get_name(&microphone->device,
                                       ma_device_type_capture,
                                       g_string_result,
                                       sizeof(g_string_result),
                                       NULL) != MA_SUCCESS) {
                    source = "Default input device";
                } else {
                    source = NULL;
                }
                break;
            case 1:
                source = ma_get_backend_name(microphone->device.pContext->backend);
                break;
            case 2:
                source = ma_device_is_started(&microphone->device) ? "Running" : "Stopped";
                break;
            case 3:
                source = ma_result_description(microphone->last_error);
                break;
            default:
                source = "Unknown property";
                break;
        }
    } else {
        source = "Invalid microphone handle";
    }
    if (source != NULL) {
        ma_strncpy_s(g_string_result, sizeof(g_string_result), source, (size_t)-1);
    }
    ma_mutex_unlock(&g_registry_lock);

    MArgument_setUTF8String(result, g_string_result);
    return LIBRARY_NO_ERROR;
}

DLLEXPORT int microphoneDescribeResult(WolframLibraryData library_data, mint argument_count, MArgument* arguments, MArgument result)
{
    ma_result native_result;
    const char* description;

    (void)library_data;
    if (argument_count != 1) {
        return LIBRARY_FUNCTION_ERROR;
    }
    native_result = (ma_result)MArgument_getInteger(arguments[0]);
    if (native_result == MA_SUCCESS && g_last_open_error != MA_SUCCESS) {
        native_result = g_last_open_error;
    }
    description = ma_result_description(native_result);
    ma_strncpy_s(g_string_result, sizeof(g_string_result), description, (size_t)-1);
    MArgument_setUTF8String(result, g_string_result);
    return LIBRARY_NO_ERROR;
}

DLLEXPORT int microphoneControl(WolframLibraryData library_data, mint argument_count, MArgument* arguments, MArgument result)
{
    MicrophoneDevice* microphone;
    mint id;
    mint command;
    ma_result native_result = MA_INVALID_ARGS;

    (void)library_data;
    if (argument_count != 2) {
        return LIBRARY_FUNCTION_ERROR;
    }
    id = MArgument_getInteger(arguments[0]);
    command = MArgument_getInteger(arguments[1]);

    ma_mutex_lock(&g_registry_lock);
    microphone = find_device_unlocked(id);
    if (microphone != NULL) {
        if (command == 0) {
            native_result = ma_device_start(&microphone->device);
        } else if (command == 1) {
            native_result = ma_device_stop(&microphone->device);
        } else if (command == 2) {
            ma_uint32 remaining = ma_pcm_rb_available_read(&microphone->ring);
            native_result = MA_SUCCESS;
            while (remaining > 0) {
                void* ignored = NULL;
                ma_uint32 frames = remaining;
                native_result = ma_pcm_rb_acquire_read(&microphone->ring, &frames, &ignored);
                if (native_result != MA_SUCCESS || frames == 0) {
                    break;
                }
                native_result = ma_pcm_rb_commit_read(&microphone->ring, frames);
                if (native_result != MA_SUCCESS) {
                    break;
                }
                remaining -= frames;
            }
        }
        microphone->last_error = native_result;
    }
    ma_mutex_unlock(&g_registry_lock);
    MArgument_setInteger(result, (mint)native_result);
    return LIBRARY_NO_ERROR;
}

