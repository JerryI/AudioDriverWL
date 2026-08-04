#include "WolframLibrary.h"
#include "WolframIOLibraryFunctions.h"
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

#if defined(__APPLE__)
#include "microphone_helper_protocol.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

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
#if defined(__APPLE__)
    pid_t helper_pid;
    int helper_audio_fd;
    int helper_control_fd;
    int helper_status_fd;
    pthread_t helper_reader_thread;
    ma_bool32 helper_thread_started;
    ma_bool32 helper_running;
    ma_atomic_uint64 helper_frames_received;
    ma_uint32 helper_period_frames;
    ma_uint32 helper_internal_sample_rate;
    ma_uint32 helper_internal_channels;
    ma_uint32 helper_internal_format;
    char helper_device_name[MICROPHONE_HELPER_NAME_CAPACITY];
#endif
    ma_event async_event;
    ma_event async_stopped_event;
    ma_bool32 async_events_initialized;
    ma_atomic_uint32 async_active;
    ma_atomic_uint32 async_threshold;
    ma_atomic_uint32 async_available;
    ma_atomic_uint32 async_pending;
    mint async_task_id;
    WolframIOLibrary_Functions async_io_library;
    ma_result last_error;
} MicrophoneDevice;

typedef struct BufferReadyTaskArguments {
    MicrophoneDevice* microphone;
    WolframIOLibrary_Functions io_library;
} BufferReadyTaskArguments;

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

static void update_async_notification(MicrophoneDevice* microphone)
{
    ma_uint32 available;
    ma_uint32 threshold;

    if (microphone == NULL ||
        ma_atomic_uint32_get(&microphone->async_active) == 0 ||
        !microphone->ring_initialized) {
        return;
    }

    threshold = ma_atomic_uint32_get(&microphone->async_threshold);
    available = ma_pcm_rb_available_read(&microphone->ring);
    ma_atomic_uint32_set(&microphone->async_available, available);

    if (threshold > 0 && available >= threshold) {
        if (ma_atomic_uint32_exchange(&microphone->async_pending, 1) == 0) {
            (void)ma_event_signal(&microphone->async_event);
        }
    } else {
        ma_atomic_uint32_set(&microphone->async_pending, 0);
    }
}

static void buffer_ready_task_runner(mint async_task_id, void* user_data)
{
    BufferReadyTaskArguments* arguments = (BufferReadyTaskArguments*)user_data;
    MicrophoneDevice* microphone = arguments->microphone;
    WolframIOLibrary_Functions io_library = arguments->io_library;
    DataStore event_data;

    free(arguments);
    while (io_library->asynchronousTaskAliveQ(async_task_id)) {
        if (ma_event_wait(&microphone->async_event) != MA_SUCCESS) {
            break;
        }
        if (!io_library->asynchronousTaskAliveQ(async_task_id)) {
            break;
        }
        if (ma_atomic_uint32_get(&microphone->async_active) == 0) {
            continue;
        }

        event_data = io_library->createDataStore();
        if (event_data != NULL) {
            io_library->DataStore_addInteger(
                event_data,
                (mint)ma_atomic_uint32_get(&microphone->async_available));
            io_library->raiseAsyncEvent(async_task_id, "BufferReady", event_data);
        }
    }

    ma_atomic_uint32_set(&microphone->async_active, 0);
    (void)ma_event_signal(&microphone->async_stopped_event);
}

static void stop_async_task(MicrophoneDevice* microphone)
{
    if (microphone == NULL || microphone->async_task_id <= 0) {
        return;
    }

    ma_atomic_uint32_set(&microphone->async_active, 0);
    ma_atomic_uint32_set(&microphone->async_pending, 0);
    if (microphone->async_io_library != NULL &&
        microphone->async_io_library->asynchronousTaskAliveQ(microphone->async_task_id)) {
        (void)microphone->async_io_library->removeAsynchronousTask(microphone->async_task_id);
    }
    (void)ma_event_signal(&microphone->async_event);
    (void)ma_event_wait(&microphone->async_stopped_event);
    microphone->async_task_id = 0;
    microphone->async_io_library = NULL;
}

static ma_result initialize_async_state(MicrophoneDevice* microphone)
{
    ma_result result;

    result = ma_event_init(&microphone->async_event);
    if (result != MA_SUCCESS) {
        return result;
    }
    result = ma_event_init(&microphone->async_stopped_event);
    if (result != MA_SUCCESS) {
        ma_event_uninit(&microphone->async_event);
        return result;
    }

    microphone->async_events_initialized = MA_TRUE;
    microphone->async_task_id = 0;
    microphone->async_io_library = NULL;
    ma_atomic_uint32_set(&microphone->async_active, 0);
    ma_atomic_uint32_set(&microphone->async_threshold, 0);
    ma_atomic_uint32_set(&microphone->async_available, 0);
    ma_atomic_uint32_set(&microphone->async_pending, 0);
    return MA_SUCCESS;
}

static void uninitialize_async_state(MicrophoneDevice* microphone)
{
    if (microphone == NULL || !microphone->async_events_initialized) {
        return;
    }
    stop_async_task(microphone);
    ma_event_uninit(&microphone->async_stopped_event);
    ma_event_uninit(&microphone->async_event);
    microphone->async_events_initialized = MA_FALSE;
}

static void store_captured_frames(
    MicrophoneDevice* microphone,
    const float* input,
    ma_uint32 frame_count)
{
    ma_uint32 written = 0;

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
    update_async_notification(microphone);
}

static void capture_callback(ma_device* device, void* output, const void* input, ma_uint32 frame_count)
{
    (void)output;
    store_captured_frames(
        (MicrophoneDevice*)device->pUserData,
        (const float*)input,
        frame_count);
}

#if defined(__APPLE__)
static int read_all(int descriptor, void* data, size_t size)
{
    unsigned char* cursor = (unsigned char*)data;
    while (size > 0) {
        ssize_t count = read(descriptor, cursor, size);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return -1;
        }
        cursor += (size_t)count;
        size -= (size_t)count;
    }
    return 0;
}

static int helper_send_command(MicrophoneDevice* microphone, unsigned char command)
{
    ssize_t count;
    MicrophoneHelperResponse response;
    ma_uint32 wait_milliseconds = 0;
    do {
        count = write(microphone->helper_control_fd, &command, 1);
    } while (count < 0 && errno == EINTR);
    if (count != 1 || read_all(
            microphone->helper_status_fd,
            &response,
            sizeof(response)) != 0 ||
        response.magic != MICROPHONE_HELPER_MAGIC ||
        response.command != command) {
        return -1;
    }
    if (command == 'S') {
        while (ma_atomic_uint64_get(&microphone->helper_frames_received) < response.frames_sent) {
            if (wait_milliseconds++ >= 5000) {
                return -1;
            }
            ma_sleep(1);
        }
    }
    return 0;
}

static void* helper_reader(void* user_data)
{
    MicrophoneDevice* microphone = (MicrophoneDevice*)user_data;
    unsigned char buffer[65536 + (MIC_MAX_CHANNELS * sizeof(float))];
    size_t buffered_bytes = 0;
    size_t bytes_per_frame = (size_t)microphone->channels * sizeof(float);

    while (bytes_per_frame > 0) {
        ssize_t count = read(
            microphone->helper_audio_fd,
            buffer + buffered_bytes,
            sizeof(buffer) - buffered_bytes);
        size_t complete_bytes;
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            break;
        }
        buffered_bytes += (size_t)count;
        complete_bytes = (buffered_bytes / bytes_per_frame) * bytes_per_frame;
        if (complete_bytes > 0) {
            store_captured_frames(
                microphone,
                (const float*)buffer,
                (ma_uint32)(complete_bytes / bytes_per_frame));
            ma_atomic_uint64_fetch_add(
                &microphone->helper_frames_received,
                (ma_uint64)(complete_bytes / bytes_per_frame));
            buffered_bytes -= complete_bytes;
            if (buffered_bytes > 0) {
                memmove(buffer, buffer + complete_bytes, buffered_bytes);
            }
        }
    }
    microphone->helper_running = MA_FALSE;
    return NULL;
}

static ma_result resolve_helper_path(char* path, size_t capacity)
{
    Dl_info info;
    char* separator;
    size_t directory_length;
    const char* helper_name = "microphone_helper";

    if (dladdr((const void*)&resolve_helper_path, &info) == 0 || info.dli_fname == NULL) {
        return MA_DOES_NOT_EXIST;
    }
    separator = strrchr(info.dli_fname, '/');
    if (separator == NULL) {
        return MA_DOES_NOT_EXIST;
    }
    directory_length = (size_t)(separator - info.dli_fname + 1);
    if (directory_length + strlen(helper_name) + 1 > capacity) {
        return MA_PATH_TOO_LONG;
    }
    memcpy(path, info.dli_fname, directory_length);
    strcpy(path + directory_length, helper_name);
    return access(path, X_OK) == 0 ? MA_SUCCESS : MA_DOES_NOT_EXIST;
}

static ma_result start_helper_process(
    MicrophoneDevice* microphone,
    ma_uint32 sample_rate,
    ma_uint32 channels,
    ma_uint32 period_frames,
    mint profile)
{
    char helper_path[PATH_MAX];
    char sample_rate_text[16];
    char channels_text[16];
    char period_text[16];
    char profile_text[16];
    char* arguments[6];
    int audio_pipe[2] = {-1, -1};
    int control_pipe[2] = {-1, -1};
    int status_pipe[2] = {-1, -1};
    posix_spawn_file_actions_t actions;
    MicrophoneHelperHeader header;
    int spawn_result;

    if (resolve_helper_path(helper_path, sizeof(helper_path)) != MA_SUCCESS) {
        return MA_DOES_NOT_EXIST;
    }
    snprintf(sample_rate_text, sizeof(sample_rate_text), "%u", sample_rate);
    snprintf(channels_text, sizeof(channels_text), "%u", channels);
    snprintf(period_text, sizeof(period_text), "%u", period_frames);
    snprintf(profile_text, sizeof(profile_text), "%d", profile == 0 ? 0 : 1);
    arguments[0] = helper_path;
    arguments[1] = sample_rate_text;
    arguments[2] = channels_text;
    arguments[3] = period_text;
    arguments[4] = profile_text;
    arguments[5] = NULL;

    if (pipe(audio_pipe) != 0 || pipe(control_pipe) != 0 || pipe(status_pipe) != 0) {
        if (audio_pipe[0] >= 0) close(audio_pipe[0]);
        if (audio_pipe[1] >= 0) close(audio_pipe[1]);
        if (control_pipe[0] >= 0) close(control_pipe[0]);
        if (control_pipe[1] >= 0) close(control_pipe[1]);
        if (status_pipe[0] >= 0) close(status_pipe[0]);
        if (status_pipe[1] >= 0) close(status_pipe[1]);
        return MA_ERROR;
    }

    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, audio_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, control_pipe[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, status_pipe[1], MICROPHONE_HELPER_STATUS_FD);
    posix_spawn_file_actions_addclose(&actions, audio_pipe[0]);
    posix_spawn_file_actions_addclose(&actions, control_pipe[1]);
    posix_spawn_file_actions_addclose(&actions, status_pipe[0]);
    spawn_result = posix_spawn(
        &microphone->helper_pid,
        helper_path,
        &actions,
        NULL,
        arguments,
        environ);
    posix_spawn_file_actions_destroy(&actions);
    close(audio_pipe[1]);
    close(control_pipe[0]);
    close(status_pipe[1]);
    if (spawn_result != 0) {
        close(audio_pipe[0]);
        close(control_pipe[1]);
        close(status_pipe[0]);
        microphone->helper_pid = 0;
        return MA_ERROR;
    }

    microphone->helper_audio_fd = audio_pipe[0];
    microphone->helper_control_fd = control_pipe[1];
    microphone->helper_status_fd = status_pipe[0];
#if defined(F_SETNOSIGPIPE)
    {
        int enabled = 1;
        (void)fcntl(microphone->helper_control_fd, F_SETNOSIGPIPE, enabled);
    }
#endif
    if (read_all(microphone->helper_status_fd, &header, sizeof(header)) != 0 ||
        header.magic != MICROPHONE_HELPER_MAGIC ||
        header.sample_rate == 0 ||
        header.channels == 0 ||
        header.channels > MIC_MAX_CHANNELS) {
        (void)kill(microphone->helper_pid, SIGTERM);
        close(microphone->helper_audio_fd);
        close(microphone->helper_control_fd);
        close(microphone->helper_status_fd);
        (void)waitpid(microphone->helper_pid, NULL, 0);
        microphone->helper_pid = 0;
        return MA_ERROR;
    }

    microphone->sample_rate = header.sample_rate;
    microphone->channels = header.channels;
    microphone->helper_period_frames = header.period_frames;
    microphone->helper_internal_sample_rate = header.internal_sample_rate;
    microphone->helper_internal_channels = header.internal_channels;
    microphone->helper_internal_format = header.internal_format;
    memcpy(microphone->helper_device_name, header.device_name, sizeof(header.device_name));
    microphone->helper_device_name[sizeof(microphone->helper_device_name) - 1] = '\0';
    ma_atomic_uint64_set(&microphone->helper_frames_received, 0);
    microphone->helper_running = MA_TRUE;
    return MA_SUCCESS;
}

static void shutdown_helper_process(MicrophoneDevice* microphone)
{
    if (microphone->helper_pid > 0) {
        (void)helper_send_command(microphone, 'Q');
        close(microphone->helper_control_fd);
        microphone->helper_control_fd = -1;
        close(microphone->helper_status_fd);
        microphone->helper_status_fd = -1;
        if (microphone->helper_thread_started) {
            (void)pthread_join(microphone->helper_reader_thread, NULL);
            microphone->helper_thread_started = MA_FALSE;
        } else if (microphone->helper_audio_fd >= 0) {
            close(microphone->helper_audio_fd);
        }
        microphone->helper_audio_fd = -1;
        (void)waitpid(microphone->helper_pid, NULL, 0);
        microphone->helper_pid = 0;
    }
    microphone->helper_running = MA_FALSE;
}
#endif

static void shutdown_device(MicrophoneDevice* microphone)
{
#if defined(__APPLE__)
    shutdown_helper_process(microphone);
#else
    if (microphone->device_initialized) {
        ma_device_uninit(&microphone->device);
        microphone->device_initialized = MA_FALSE;
    }
#endif
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
#if !defined(__APPLE__)
    ma_device_config config;
#endif
    ma_result result;
    uint64_t buffer_frames_64;

    result = validate_configuration(sample_rate, channels, buffer_ms, period_frames, profile);
    if (result != MA_SUCCESS) {
        return result;
    }

#if defined(__APPLE__)
    result = start_helper_process(microphone, sample_rate, channels, period_frames, profile);
    if (result != MA_SUCCESS) {
        return result;
    }
#else
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
#endif

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

#if defined(__APPLE__)
    if (pthread_create(&microphone->helper_reader_thread, NULL, helper_reader, microphone) != 0) {
        shutdown_device(microphone);
        return MA_ERROR;
    }
    microphone->helper_thread_started = MA_TRUE;
#else
    result = ma_device_start(&microphone->device);
    if (result != MA_SUCCESS) {
        shutdown_device(microphone);
        return result;
    }
#endif

    microphone->requested_sample_rate = sample_rate;
    microphone->requested_channels = channels;
    microphone->buffer_ms = buffer_ms;
    microphone->requested_period_frames = period_frames;
    microphone->performance_profile = profile == 0
        ? ma_performance_profile_low_latency
        : ma_performance_profile_conservative;
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

    ma_atomic_uint32_set(&microphone->async_pending, 0);
    ma_atomic_uint32_set(&microphone->async_available, 0);
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
        update_async_notification(microphone);
        return original_result;
    }
    update_async_notification(microphone);
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
            uninitialize_async_state(g_devices[i]);
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

    native_result = initialize_async_state(microphone);
    if (native_result != MA_SUCCESS) {
        free(microphone);
        g_last_open_error = native_result;
        MArgument_setInteger(result, 0);
        return LIBRARY_NO_ERROR;
    }

    ma_mutex_lock(&g_registry_lock);
    slot = find_free_slot_unlocked();
    if (slot < 0) {
        ma_mutex_unlock(&g_registry_lock);
        uninitialize_async_state(microphone);
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
        uninitialize_async_state(microphone);
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
            uninitialize_async_state(microphone);
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

DLLEXPORT int microphoneStartBufferReadyTask(WolframLibraryData library_data,
                                             mint argument_count,
                                             MArgument* arguments,
                                             MArgument result)
{
    WolframIOLibrary_Functions io_library = library_data->ioLibraryFunctions;
    BufferReadyTaskArguments* task_arguments;
    MicrophoneDevice* microphone;
    mint id;
    mint threshold;
    mint async_task_id = -1;

    if (argument_count != 2 || io_library == NULL) {
        return LIBRARY_FUNCTION_ERROR;
    }
    id = MArgument_getInteger(arguments[0]);
    threshold = MArgument_getInteger(arguments[1]);

    ma_mutex_lock(&g_registry_lock);
    microphone = find_device_unlocked(id);
    if (microphone == NULL ||
        !microphone->ring_initialized ||
        !microphone->async_events_initialized ||
        threshold <= 0 ||
        (ma_uint64)threshold > microphone->buffer_frames) {
        ma_mutex_unlock(&g_registry_lock);
        MArgument_setInteger(result, -1);
        return LIBRARY_NO_ERROR;
    }

    stop_async_task(microphone);
    task_arguments = (BufferReadyTaskArguments*)malloc(sizeof(*task_arguments));
    if (task_arguments == NULL) {
        ma_mutex_unlock(&g_registry_lock);
        MArgument_setInteger(result, -1);
        return LIBRARY_NO_ERROR;
    }

    task_arguments->microphone = microphone;
    task_arguments->io_library = io_library;
    microphone->async_io_library = io_library;
    ma_atomic_uint32_set(&microphone->async_threshold, (ma_uint32)threshold);
    ma_atomic_uint32_set(&microphone->async_available,
                         ma_pcm_rb_available_read(&microphone->ring));
    ma_atomic_uint32_set(&microphone->async_pending, 0);
    ma_atomic_uint32_set(&microphone->async_active, 1);

    async_task_id = io_library->createAsynchronousTaskWithThread(
        buffer_ready_task_runner,
        task_arguments);
    if (async_task_id <= 0) {
        ma_atomic_uint32_set(&microphone->async_active, 0);
        microphone->async_io_library = NULL;
        free(task_arguments);
    } else {
        microphone->async_task_id = async_task_id;
        update_async_notification(microphone);
    }
    ma_mutex_unlock(&g_registry_lock);

    MArgument_setInteger(result, async_task_id);
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

    if (frames_read > 0) {
        ma_atomic_uint32_set(&microphone->async_pending, 0);
        update_async_notification(microphone);
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
            case 5:
#if defined(__APPLE__)
                value = (mint)microphone->helper_period_frames;
#else
                value = (mint)microphone->device.capture.internalPeriodSizeInFrames;
#endif
                break;
            case 6:
#if defined(__APPLE__)
                value = microphone->helper_running ? 1 : 0;
#else
                value = ma_device_is_started(&microphone->device) ? 1 : 0;
#endif
                break;
            case 7: value = microphone->performance_profile == ma_performance_profile_conservative ? 1 : 0; break;
            case 8:
#if defined(__APPLE__)
                value = (mint)microphone->helper_internal_sample_rate;
#else
                value = (mint)microphone->device.capture.internalSampleRate;
#endif
                break;
            case 9:
#if defined(__APPLE__)
                value = (mint)microphone->helper_internal_channels;
#else
                value = (mint)microphone->device.capture.internalChannels;
#endif
                break;
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
#if defined(__APPLE__)
                source = microphone->helper_device_name;
#else
                if (ma_device_get_name(&microphone->device,
                                       ma_device_type_capture,
                                       g_string_result,
                                       sizeof(g_string_result),
                                       NULL) != MA_SUCCESS) {
                    source = "Default input device";
                } else {
                    source = NULL;
                }
#endif
                break;
            case 1:
#if defined(__APPLE__)
                source = "Core Audio";
#else
                source = ma_get_backend_name(microphone->device.pContext->backend);
#endif
                break;
            case 2:
#if defined(__APPLE__)
                source = microphone->helper_running ? "Running" : "Stopped";
#else
                source = ma_device_is_started(&microphone->device) ? "Running" : "Stopped";
#endif
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
#if defined(__APPLE__)
            native_result = helper_send_command(microphone, 'R') == 0 ? MA_SUCCESS : MA_ERROR;
            if (native_result == MA_SUCCESS) microphone->helper_running = MA_TRUE;
#else
            native_result = ma_device_start(&microphone->device);
#endif
        } else if (command == 1) {
#if defined(__APPLE__)
            native_result = helper_send_command(microphone, 'S') == 0 ? MA_SUCCESS : MA_ERROR;
            if (native_result == MA_SUCCESS) microphone->helper_running = MA_FALSE;
#else
            native_result = ma_device_stop(&microphone->device);
#endif
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
            ma_atomic_uint32_set(&microphone->async_available, 0);
            ma_atomic_uint32_set(&microphone->async_pending, 0);
        }
        microphone->last_error = native_result;
    }
    ma_mutex_unlock(&g_registry_lock);
    MArgument_setInteger(result, (mint)native_result);
    return LIBRARY_NO_ERROR;
}
