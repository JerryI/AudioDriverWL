# Wolfram Device Framework microphone driver

This paclet exposes the operating system's default audio input as a Wolfram
Device Framework device:

```wl
dev = DeviceOpen["Microphone"];

dev["SampleRate"]
dev["AvailableFrames"]

raw = DeviceReadBuffer[dev];       (* Real32 NumericArray: frames x channels *)
audio = DeviceRead[dev];           (* Audio, or Missing if the buffer is empty *)

DeviceClose[dev];
```

Acquisition is callback-driven. The OS audio callback writes `float32` frames
to a lock-free single-producer/single-consumer ring buffer. A Wolfram Language
read only copies frames that are already available; there is no polling thread
and no float-to-double expansion on the raw read path.

## Build

Requirements are Wolfram Language 12 or newer and a platform C compiler:

- macOS: Xcode Command Line Tools (Clang)
- Linux: GCC or Clang
- Windows: Visual Studio Build Tools

Run the build script from any working directory. It derives every path from
its own location, which avoids shell and sandbox working-directory issues:

```sh
wolframscript -file "/absolute/path/to/AudioDriver/scripts/build.wls"
```

The script downloads the pinned miniaudio 0.11.25 header, verifies its SHA-256,
and puts the native result in `LibraryResources/$SystemID/`. The dependency has
no required development packages. On Linux, a running system audio service and
its runtime libraries (usually PipeWire/PulseAudio or ALSA) must still be
available.

For development, load the directory without copying it:

```wl
PacletDirectoryLoad["/absolute/path/to/AudioDriver"];
dev = DeviceOpen["Microphone"];
```

For a persistent local installation:

```wl
PacletInstall["/absolute/path/to/AudioDriver"]
```

To build an installable archive for the currently collected native targets:

```sh
wolframscript -file "/absolute/path/to/AudioDriver/scripts/package.wls"
```

The archive is written to `build/`.

Build once on every target architecture whose binary will be distributed, and
collect the resulting `$SystemID` subdirectories under `LibraryResources`.

## Configuration

Options can be supplied while opening:

```wl
dev = DeviceOpen["Microphone", <|
  "SampleRate" -> 48000,
  "Channels" -> 1,
  "BufferDuration" -> 2.,
  "PeriodSize" -> Automatic,
  "PerformanceProfile" -> "LowLatency"
|>];
```

They can also be changed later. Configuration is transactional: the native
stream is restarted, and the previous setup is restored if the new setup
cannot be opened.

```wl
DeviceConfigure[dev, {
  "SampleRate" -> 44100,
  "Channels" -> 1,
  "BufferDuration" -> .5,
  "PerformanceProfile" -> "Conservative"
}]
```

The configurable properties are:

| Property | Values | Default |
|---|---|---|
| `"SampleRate"` | `Automatic` or integer 8000–384000 | `Automatic` |
| `"Channels"` | `Automatic` or integer 1–32 | `Automatic` |
| `"BufferDuration"` | 0.01–60 seconds | 2 seconds |
| `"PeriodSize"` | `Automatic` or 1–262144 frames | `Automatic` |
| `"PerformanceProfile"` | `"LowLatency"` or `"Conservative"` | `"LowLatency"` |

`Automatic` lets the backend use the native format and avoids resampling or
channel conversion where possible. A requested non-native format is converted
by the native backend or miniaudio before it reaches the ring buffer.

The same configurable properties can be assigned through the device object,
for example `dev["BufferDuration"] = .25`. `DeviceConfigure` is preferable when
changing several properties because it performs a single restart.

## Reading and control

`DeviceReadBuffer` is nonblocking:

```wl
dev["AvailableFrames"]             (* current unread frame count *)
DeviceReadBuffer[dev]              (* all currently available frames *)
DeviceReadBuffer[dev, 1024]        (* at most 1024 frames *)
DeviceReadBuffer[dev, Quantity[50, "Milliseconds"]]
```

It returns a rank-2, interleaved `Real32` `NumericArray` with dimensions
`{frames, channels}`. When no frame is available it returns `{}`. The higher
level `DeviceRead` variants return an `Audio` object and return
`Missing["NotAvailable"]` for an empty buffer:

```wl
DeviceRead[dev]
DeviceRead[dev, 4096]
DeviceRead[dev, "Raw"]             (* equivalent raw-buffer convenience form *)
DeviceRead[dev, {"Raw", 4096}]
```

Diagnostics and stream control are available through `DeviceExecute`:

```wl
DeviceExecute[dev, "Information"]
DeviceExecute[dev, "AvailableFrames"]
DeviceExecute[dev, "ClearBuffer"]
DeviceExecute[dev, "Stop"]
DeviceExecute[dev, "Start"]
```

If the consumer falls behind, new frames are discarded rather than blocking
the real-time audio callback. Check `dev["DroppedFrames"]` to detect this and
increase `"BufferDuration"` if necessary.

The exact allocated capacity is available as `dev["BufferCapacityFrames"]`.

## Native backends

miniaudio selects the best available native backend at runtime. Typical desktop
paths are Core Audio on macOS, WASAPI on Windows, and PulseAudio/ALSA/JACK on
Linux. The chosen backend and device can be inspected with:

```wl
dev["Backend"]
dev["DeviceName"]
dev["NativeSampleRate"]
dev["NativeChannels"]
```

On macOS and Windows, the first open may trigger the operating system's
microphone permission prompt. Headless sessions must be granted access to the
actual kernel host executable (`wolframscript`, Wolfram Engine, or Mathematica).

## Test

After building, run the live smoke test:

```sh
wolframscript -file "/absolute/path/to/AudioDriver/scripts/test.wls"
```

The test opens the actual default input, verifies discovery and dynamic
properties, captures a `Real32` buffer, reconfigures the stream, and closes it.

## Implementation layout

- `Microphone.m` — Device Framework registration and Wolfram Language interface
- `native/microphone.c` — LibraryLink API, native capture, and ring buffer
- `scripts/build.wls` — cross-platform compiler and dependency bootstrap
- `scripts/test.wls` — live integration smoke test
- `scripts/package.wls` — installable paclet archive builder
- `PacletInfo.m` — discoverable `DeviceDriver_Microphone` paclet metadata

The Device Framework structure follows Wolfram's
[Developing Device Drivers](https://reference.wolfram.com/language/tutorial/DevelopingDeviceDrivers.html)
guide: the class-named driver file registers find, open, preconfigure,
configure, read, read-buffer, execute, close, property, status-label, and icon
handlers.
