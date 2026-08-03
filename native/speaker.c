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
#define SPK_THREAD_LOCAL __declspec(thread)
#else
#define SPK_THREAD_LOCAL __thread
#endif

#define SPK_MAX_DEVICES 64
#define SPK_MAX_CHANNELS 32
#define SPK_MIN_SAMPLE_RATE 8000
#define SPK_MAX_SAMPLE_RATE 384000
#define SPK_MIN_BUFFER_MS 10
#define SPK_MAX_BUFFER_MS 60000
#define SPK_MAX_PERIOD_FRAMES 262144

typedef struct SpeakerDevice {
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
    ma_atomic_uint64 underrun_frames;
    ma_result last_error;
} SpeakerDevice;

static SpeakerDevice* g_devices[SPK_MAX_DEVICES];
static ma_mutex g_registry_lock;
static ma_bool32 g_registry_lock_initialized = MA_FALSE;
static mint g_next_id = 1;
static ma_result g_last_open_error = MA_SUCCESS;
static SPK_THREAD_LOCAL char g_string_result[512];

static SpeakerDevice* find_device_unlocked(mint id)
{
    int i;
    for (i = 0; i < SPK_MAX_DEVICES; ++i) {
        if (g_devices[i] != NULL && g_devices[i]->id == id) {
            return g_devices[i];
        }
    }
    return NULL;
}

static int find_free_slot_unlocked(void)
{
    int i;
    for (i = 0; i < SPK_MAX_DEVICES; ++i) {
        if (g_devices[i] == NULL) {
            return i;
        }
    }
    return -1;
}

static void playback_callback(ma_device* device, void* output, const void* input, ma_uint32 frame_count)
{
    SpeakerDevice* speaker = (SpeakerDevice*)device->pUserData;
    ma_uint32 frames_read = 0;
    float* destination = (float*)output;

    (void)input;
    if (speaker == NULL || destination == NULL || !speaker->ring_initialized) {
        return;
    }

    memset(destination, 0, (size_t)frame_count * speaker->channels * sizeof(float));
    while (frames_read < frame_count) {
        void* source = NULL;
        ma_uint32 frames = frame_count - frames_read;
        ma_result result = ma_pcm_rb_acquire_read(&speaker->ring, &frames, &source);

        if (result != MA_SUCCESS || frames == 0) {
            break;
        }
        memcpy(destination + ((size_t)frames_read * speaker->channels),
               source,
               (size_t)frames * speaker->channels * sizeof(float));
        if (ma_pcm_rb_commit_read(&speaker->ring, frames) != MA_SUCCESS) {
            break;
        }
        frames_read += frames;
    }

    /* An entirely empty queue is normal between writes. Count only a buffer
       that ran dry partway through a hardware period. */
    if (frames_read > 0 && frames_read < frame_count) {
        ma_atomic_uint64_fetch_add(&speaker->underrun_frames,
                                   (ma_uint64)(frame_count - frames_read));
    }
}

static void shutdown_device(SpeakerDevice* speaker)
{
    if (speaker->device_initialized) {
        ma_device_uninit(&speaker->device);
        speaker->device_initialized = MA_FALSE;
    }
    if (speaker->ring_initialized) {
        ma_pcm_rb_uninit(&speaker->ring);
        speaker->ring_initialized = MA_FALSE;
    }
}

static ma_result validate_configuration(ma_uint32 sample_rate,
                                        ma_uint32 channels,
                                        ma_uint32 buffer_ms,
                                        ma_uint32 period_frames,
                                        mint profile)
{
    if (sample_rate != 0 && (sample_rate < SPK_MIN_SAMPLE_RATE || sample_rate > SPK_MAX_SAMPLE_RATE)) {
        return MA_INVALID_ARGS;
    }
    if (channels > SPK_MAX_CHANNELS) {
        return MA_INVALID_ARGS;
    }
    if (buffer_ms < SPK_MIN_BUFFER_MS || buffer_ms > SPK_MAX_BUFFER_MS) {
        return MA_INVALID_ARGS;
    }
    if (period_frames > SPK_MAX_PERIOD_FRAMES || (profile != 0 && profile != 1)) {
        return MA_INVALID_ARGS;
    }
    return MA_SUCCESS;
}

static ma_result start_device(SpeakerDevice* speaker,
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

    config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = channels;
    config.sampleRate = sample_rate;
    config.periodSizeInFrames = period_frames;
    config.performanceProfile = profile == 0
        ? ma_performance_profile_low_latency
        : ma_performance_profile_conservative;
    config.noFixedSizedCallback = MA_TRUE;
    config.dataCallback = playback_callback;
    config.pUserData = speaker;

    result = ma_device_init(NULL, &config, &speaker->device);
    if (result != MA_SUCCESS) {
        return result;
    }
    speaker->device_initialized = MA_TRUE;
    speaker->sample_rate = speaker->device.sampleRate;
    speaker->channels = speaker->device.playback.channels;

    buffer_frames_64 = ((uint64_t)speaker->sample_rate * buffer_ms + 999U) / 1000U;
    if (buffer_frames_64 == 0 || buffer_frames_64 > UINT32_MAX) {
        shutdown_device(speaker);
        return MA_INVALID_ARGS;
    }

    result = ma_pcm_rb_init(ma_format_f32,
                            speaker->channels,
                            (ma_uint32)buffer_frames_64,
                            NULL,
                            NULL,
                            &speaker->ring);
    if (result != MA_SUCCESS) {
        shutdown_device(speaker);
        return result;
    }
    speaker->ring_initialized = MA_TRUE;
    speaker->buffer_frames = ma_pcm_rb_get_subbuffer_size(&speaker->ring);
    ma_atomic_uint64_set(&speaker->dropped_frames, 0);
    ma_atomic_uint64_set(&speaker->underrun_frames, 0);

    result = ma_device_start(&speaker->device);
    if (result != MA_SUCCESS) {
        shutdown_device(speaker);
        return result;
    }

    speaker->requested_sample_rate = sample_rate;
    speaker->requested_channels = channels;
    speaker->buffer_ms = buffer_ms;
    speaker->requested_period_frames = period_frames;
    speaker->performance_profile = config.performanceProfile;
    speaker->last_error = MA_SUCCESS;
    return MA_SUCCESS;
}

static ma_result reconfigure_device(SpeakerDevice* speaker,
                                    ma_uint32 sample_rate,
                                    ma_uint32 channels,
                                    ma_uint32 buffer_ms,
                                    ma_uint32 period_frames,
                                    mint profile)
{
    ma_uint32 old_sample_rate = speaker->requested_sample_rate;
    ma_uint32 old_channels = speaker->requested_channels;
    ma_uint32 old_buffer_ms = speaker->buffer_ms;
    ma_uint32 old_period_frames = speaker->requested_period_frames;
    mint old_profile = speaker->performance_profile == ma_performance_profile_conservative ? 1 : 0;
    ma_result result;

    shutdown_device(speaker);
    result = start_device(speaker, sample_rate, channels, buffer_ms, period_frames, profile);
    if (result != MA_SUCCESS) {
        ma_result original_result = result;
        (void)start_device(speaker,
                           old_sample_rate,
                           old_channels,
                           old_buffer_ms,
                           old_period_frames,
                           old_profile);
        speaker->last_error = original_result;
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
    for (i = 0; i < SPK_MAX_DEVICES; ++i) {
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

DLLEXPORT int speakerOpen(WolframLibraryData library_data, mint argument_count, MArgument* arguments, MArgument result)
{
    SpeakerDevice* speaker;
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

    speaker = (SpeakerDevice*)calloc(1, sizeof(*speaker));
    if (speaker == NULL) {
        g_last_open_error = MA_OUT_OF_MEMORY;
        MArgument_setInteger(result, 0);
        return LIBRARY_NO_ERROR;
    }

    ma_mutex_lock(&g_registry_lock);
    slot = find_free_slot_unlocked();
    if (slot < 0) {
        ma_mutex_unlock(&g_registry_lock);
        free(speaker);
        g_last_open_error = MA_TOO_MANY_OPEN_FILES;
        MArgument_setInteger(result, 0);
        return LIBRARY_NO_ERROR;
    }

    speaker->id = g_next_id++;
    if (g_next_id <= 0) {
        g_next_id = 1;
    }
    native_result = start_device(speaker,
                                 (ma_uint32)sample_rate,
                                 (ma_uint32)channels,
                                 (ma_uint32)buffer_ms,
                                 (ma_uint32)period_frames,
                                 profile);
    if (native_result != MA_SUCCESS) {
        ma_mutex_unlock(&g_registry_lock);
        free(speaker);
        g_last_open_error = native_result;
        MArgument_setInteger(result, 0);
        return LIBRARY_NO_ERROR;
    }

    g_devices[slot] = speaker;
    g_last_open_error = MA_SUCCESS;
    MArgument_setInteger(result, speaker->id);
    ma_mutex_unlock(&g_registry_lock);
    return LIBRARY_NO_ERROR;
}

DLLEXPORT int speakerClose(WolframLibraryData library_data, mint argument_count, MArgument* arguments, MArgument result)
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
    for (i = 0; i < SPK_MAX_DEVICES; ++i) {
        if (g_devices[i] != NULL && g_devices[i]->id == id) {
            SpeakerDevice* speaker = g_devices[i];
            g_devices[i] = NULL;
            shutdown_device(speaker);
            free(speaker);
            native_result = MA_SUCCESS;
            break;
        }
    }
    ma_mutex_unlock(&g_registry_lock);
    MArgument_setInteger(result, (mint)native_result);
    return LIBRARY_NO_ERROR;
}

DLLEXPORT int speakerConfigure(WolframLibraryData library_data, mint argument_count, MArgument* arguments, MArgument result)
{
    mint id;
    mint sample_rate;
    mint channels;
    mint buffer_ms;
    mint period_frames;
    mint profile;
    SpeakerDevice* speaker;
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
    speaker = find_device_unlocked(id);
    if (speaker != NULL) {
        native_result = reconfigure_device(speaker,
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

DLLEXPORT int speakerWrite(WolframLibraryData library_data, mint argument_count, MArgument* arguments, MArgument result)
{
    WolframNumericArrayLibrary_Functions numeric_arrays = library_data->numericarrayLibraryFunctions;
    MNumericArray array;
    SpeakerDevice* speaker;
    mint const* dimensions;
    mint id;
    mint frame_count;
    ma_uint32 frames_to_write;
    ma_uint32 frames_written = 0;
    float* input;

    if (argument_count != 2 || numeric_arrays == NULL) {
        return LIBRARY_FUNCTION_ERROR;
    }
    id = MArgument_getInteger(arguments[0]);
    array = MArgument_getMNumericArray(arguments[1]);
    if (array == NULL ||
        numeric_arrays->MNumericArray_getType(array) != MNumericArray_Type_Real32 ||
        numeric_arrays->MNumericArray_getRank(array) != 2) {
        MArgument_setInteger(result, (mint)-1);
        return LIBRARY_NO_ERROR;
    }
    dimensions = numeric_arrays->MNumericArray_getDimensions(array);
    frame_count = dimensions[0];
    if (frame_count < 0 || (ma_uint64)frame_count > UINT32_MAX) {
        MArgument_setInteger(result, (mint)-1);
        return LIBRARY_NO_ERROR;
    }

    ma_mutex_lock(&g_registry_lock);
    speaker = find_device_unlocked(id);
    if (speaker == NULL || !speaker->ring_initialized || dimensions[1] != (mint)speaker->channels) {
        ma_mutex_unlock(&g_registry_lock);
        MArgument_setInteger(result, (mint)-1);
        return LIBRARY_NO_ERROR;
    }

    frames_to_write = (ma_uint32)frame_count;
    if (frames_to_write > ma_pcm_rb_available_write(&speaker->ring)) {
        frames_to_write = ma_pcm_rb_available_write(&speaker->ring);
    }
    input = (float*)numeric_arrays->MNumericArray_getData(array);
    while (frames_written < frames_to_write) {
        void* destination = NULL;
        ma_uint32 frames = frames_to_write - frames_written;
        ma_result native_result = ma_pcm_rb_acquire_write(&speaker->ring, &frames, &destination);
        if (native_result != MA_SUCCESS || frames == 0) {
            break;
        }
        memcpy(destination,
               input + ((size_t)frames_written * speaker->channels),
               (size_t)frames * speaker->channels * sizeof(float));
        if (ma_pcm_rb_commit_write(&speaker->ring, frames) != MA_SUCCESS) {
            break;
        }
        frames_written += frames;
    }
    if (frames_written < (ma_uint32)frame_count) {
        ma_atomic_uint64_fetch_add(&speaker->dropped_frames,
                                   (ma_uint64)((ma_uint32)frame_count - frames_written));
    }
    ma_mutex_unlock(&g_registry_lock);

    MArgument_setInteger(result, (mint)frames_written);
    return LIBRARY_NO_ERROR;
}

DLLEXPORT int speakerGetInteger(WolframLibraryData library_data, mint argument_count, MArgument* arguments, MArgument result)
{
    SpeakerDevice* speaker;
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
    speaker = find_device_unlocked(id);
    if (speaker != NULL) {
        switch (property) {
            case 0: value = (mint)speaker->sample_rate; break;
            case 1: value = (mint)speaker->channels; break;
            case 2: value = (mint)speaker->buffer_frames; break;
            case 3: value = (mint)ma_pcm_rb_available_read(&speaker->ring); break;
            case 4: value = (mint)ma_pcm_rb_available_write(&speaker->ring); break;
            case 5: value = clamp_uint64_to_mint(ma_atomic_uint64_get(&speaker->dropped_frames)); break;
            case 6: value = clamp_uint64_to_mint(ma_atomic_uint64_get(&speaker->underrun_frames)); break;
            case 7: value = (mint)speaker->device.playback.internalPeriodSizeInFrames; break;
            case 8: value = ma_device_is_started(&speaker->device) ? 1 : 0; break;
            case 9: value = speaker->performance_profile == ma_performance_profile_conservative ? 1 : 0; break;
            case 10: value = (mint)speaker->device.playback.internalSampleRate; break;
            case 11: value = (mint)speaker->device.playback.internalChannels; break;
            default: value = -1; break;
        }
    }
    ma_mutex_unlock(&g_registry_lock);
    MArgument_setInteger(result, value);
    return LIBRARY_NO_ERROR;
}

DLLEXPORT int speakerGetString(WolframLibraryData library_data, mint argument_count, MArgument* arguments, MArgument result)
{
    SpeakerDevice* speaker;
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
    speaker = find_device_unlocked(id);
    if (speaker != NULL) {
        switch (property) {
            case 0:
                if (ma_device_get_name(&speaker->device,
                                       ma_device_type_playback,
                                       g_string_result,
                                       sizeof(g_string_result),
                                       NULL) != MA_SUCCESS) {
                    source = "Default output device";
                } else {
                    source = NULL;
                }
                break;
            case 1:
                source = ma_get_backend_name(speaker->device.pContext->backend);
                break;
            case 2:
                source = ma_device_is_started(&speaker->device) ? "Running" : "Stopped";
                break;
            case 3:
                source = ma_result_description(speaker->last_error);
                break;
            default:
                source = "Unknown property";
                break;
        }
    } else {
        source = "Invalid speaker handle";
    }
    if (source != NULL) {
        ma_strncpy_s(g_string_result, sizeof(g_string_result), source, (size_t)-1);
    }
    ma_mutex_unlock(&g_registry_lock);

    MArgument_setUTF8String(result, g_string_result);
    return LIBRARY_NO_ERROR;
}

DLLEXPORT int speakerDescribeResult(WolframLibraryData library_data, mint argument_count, MArgument* arguments, MArgument result)
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

DLLEXPORT int speakerControl(WolframLibraryData library_data, mint argument_count, MArgument* arguments, MArgument result)
{
    SpeakerDevice* speaker;
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
    speaker = find_device_unlocked(id);
    if (speaker != NULL) {
        if (command == 0) {
            native_result = ma_device_start(&speaker->device);
        } else if (command == 1) {
            native_result = ma_device_stop(&speaker->device);
        } else if (command == 2) {
            ma_uint32 remaining = ma_pcm_rb_available_read(&speaker->ring);
            native_result = MA_SUCCESS;
            while (remaining > 0) {
                void* ignored = NULL;
                ma_uint32 frames = remaining;
                native_result = ma_pcm_rb_acquire_read(&speaker->ring, &frames, &ignored);
                if (native_result != MA_SUCCESS || frames == 0) {
                    break;
                }
                native_result = ma_pcm_rb_commit_read(&speaker->ring, frames);
                if (native_result != MA_SUCCESS) {
                    break;
                }
                remaining -= frames;
            }
        } else if (command == 3) {
            ma_atomic_uint64_set(&speaker->dropped_frames, 0);
            ma_atomic_uint64_set(&speaker->underrun_frames, 0);
            native_result = MA_SUCCESS;
        }
        speaker->last_error = native_result;
    }
    ma_mutex_unlock(&g_registry_lock);
    MArgument_setInteger(result, (mint)native_result);
    return LIBRARY_NO_ERROR;
}
