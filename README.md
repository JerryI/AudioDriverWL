# Wolfram Device Framework audio drivers

This paclet exposes the operating system's default audio input and output as
Wolfram Device Framework devices.

## Microphone

```wl
PacletDirectoryLoad["path_to_this_repo_folder"];

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

## Speaker

Open the default system output and write either an `Audio` object or raw
sample frames:

```wl
speaker = DeviceOpen["Speaker"];

DeviceWrite[speaker, Audio[Sin[2 Pi 440 Range[0, 4799]/48000],
  SampleRate -> 48000]];

raw = NumericArray[ConstantArray[0., {1024, 1}], "Real32"];
DeviceWriteBuffer[speaker, raw];

DeviceClose[speaker];
```

`DeviceWrite` resamples `Audio` to the open speaker's sample rate when needed.
`DeviceWriteBuffer` accepts a numeric vector (mono) or a
`frames x channels` numeric matrix. Mono frames are duplicated for a
multichannel speaker, and multichannel frames are averaged when the speaker
is configured for mono. Raw buffers have no sample-rate metadata, so their
frames are always interpreted at `speaker["SampleRate"]`.

Playback is callback-driven and nonblocking. Writes enqueue `float32` frames
in a single-producer/single-consumer ring buffer, and the system output
callback drains that buffer. If a write is larger than the currently free
space, its excess frames are discarded. Queue diagnostics are available as:

```wl
speaker["BufferCapacityFrames"]
speaker["QueuedFrames"]
speaker["FreeFrames"]
speaker["DroppedFrames"]
speaker["UnderrunFrames"]

DeviceExecute[speaker, "ClearBuffer"]
DeviceExecute[speaker, "ResetStatistics"]
DeviceExecute[speaker, "Stop"]
DeviceExecute[speaker, "Start"]
```

### Synchronous live echo

For a synchronous microphone-to-speaker stream, open both devices at the same
sample rate and move each available raw block directly. The Wolfram kernel is
not a hard-real-time audio thread, so start playback only after building a
small jitter reserve. The short `Pause[0.005]` avoids a CPU-intensive tight
loop:

```wl
microphone = DeviceOpen["Microphone"];
speaker = DeviceOpen["Speaker", <|
  "SampleRate" -> microphone["SampleRate"],
  "BufferDuration" -> .25
|>];

DeviceExecute[speaker, "ClearBuffer"];
DeviceExecute[microphone, "ClearBuffer"];

While[True,
  block = DeviceReadBuffer[microphone, 1024];
  If[block === $Failed, Break[]];
  If[block =!= {},
    If[DeviceWriteBuffer[speaker, block] === $Failed, Break[]];
  ];
  Pause[0.005]
];
```

Use headphones for this example; an open microphone and nearby loudspeakers
can create acoustic feedback. The `.1` second reserve tolerates ordinary kernel
scheduling pauses at the cost of roughly 100 ms of added latency; reduce it only
if the workload can reliably keep the speaker queue nonempty. For continuous
streaming, keep each block below `speaker["FreeFrames"]` and monitor
`"DroppedFrames"`.

### Asynchronous live echo

The microphone can notify Wolfram when a complete block is available through
the Device Framework's asynchronous command API. This keeps all synchronous
`DeviceRead` and `DeviceReadBuffer` forms available and removes the need for a
scheduled polling task:

```wl
processBlock[_, "BufferReady", {availableFrames_}] := Module[{block},
  block = DeviceReadBuffer[microphone, Min[availableFrames, 256]];
  If[block =!= $Failed && block =!= {},
    DeviceWriteBuffer[speaker, block];
  ]
];

DeviceExecute[speaker, "ClearBuffer"];
DeviceExecute[microphone, "ClearBuffer"];

task = DeviceExecuteAsynchronous[
  microphone,
  "ReadBuffer",                 (* "BufferReady" is an equivalent alias *)
  256,                          (* minimum buffered frames *)
  processBlock
];

RemoveAsynchronousTask[task];
```

The threshold can also be supplied as
`<|"MinimumFrames" -> 256|>`. With no threshold, the native input period is
used. The audio callback only signals a sleeping native notification thread;
the Wolfram handler runs later on the kernel's event queue. Notifications are
coalesced until the handler consumes frames, so an unhandled event does not
produce an ever-growing event backlog.

An empty, healthy input buffer still returns `{}`. Actual native read or write
failures return `$Failed`; no separate readiness predicate is required.

## Prebuild binaries
⚠️ We need some time to collect binaries for all machines
- [x] MacOS Apple Silicon
- [ ] MacOS x64
- [x] Windows x64
- [ ] GNU/Linux x64
- [ ] GNU/Linux ARM64  

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
channel conversion where possible. When a non-native format is requested, the
ring buffer uses that requested format and miniaudio converts between it and
the hardware format.

The same configurable properties apply to both device classes and can be
assigned through the device object,
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

On macOS, capture runs in a small bundled helper process while the ring buffer
and Device Framework API remain in the Wolfram kernel. This avoids host-process
Core Audio attenuation observed with some USB interfaces (including iRig HD 2)
in embedded frontends such as WLJS. The Wolfram Language interface is unchanged
and no external runtime dependency is required.

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

The script downloads the pinned miniaudio 0.11.25 header, verifies its upstream
SHA-256, applies a checked Core Audio compatibility fix for padded 16-bit USB
formats, verifies the patched SHA-256, and puts the native libraries and macOS
capture helper in `LibraryResources/$SystemID/`. The dependency has no required
development packages. On Linux, a running system audio service and its runtime
libraries (usually PipeWire/PulseAudio or ALSA) must still be available.

For development, load the directory without copying it:

```wl
PacletDirectoryLoad["/absolute/path/to/AudioDriver"];
dev = DeviceOpen["Microphone"];
```

## Test

After building, run the live smoke test:

```sh
wolframscript -file "/absolute/path/to/AudioDriver/scripts/test.wls"
```

The test opens the actual default input and output, verifies registration and
dynamic properties, exercises `Audio` and `Real32` output writes, captures an
input buffer, reconfigures both streams, and closes them.
