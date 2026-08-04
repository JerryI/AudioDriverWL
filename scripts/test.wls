root = DirectoryName[DirectoryName[ExpandFileName[$InputFileName]]];
failures = {};

check[name_, condition_] := If[TrueQ[condition],
  Print["PASS  ", name],
  AppendTo[failures, name]; Print["FAIL  ", name]
];

PacletDirectoryLoad[root];
check["microphone driver registration",
  NumericQ[DeviceFramework`DeviceDriverVersion["Microphone"]]
];

device = Quiet[Check[DeviceOpen["Microphone"], $Failed]];
check["open default input", Head[device] === DeviceObject];

If[Head[device] === DeviceObject,
  info = DeviceExecute[device, "Information"];
  check["information association", AssociationQ[info]];
  check["positive sample rate", IntegerQ[info["SampleRate"]] && info["SampleRate"] > 0];
  check["positive channel count", IntegerQ[info["Channels"]] && info["Channels"] > 0];
  check["backend name", StringQ[info["Backend"]] && StringLength[info["Backend"]] > 0];

  Pause[.15];
  buffer = DeviceReadBuffer[device, 1024];
  check["raw read type", buffer === {} || Head[buffer] === NumericArray];
  check["raw read shape", buffer === {} ||
    (Length[Dimensions[buffer]] == 2 && Last[Dimensions[buffer]] == info["Channels"])
  ];
  check["raw read precision", buffer === {} || NumericArrayType[buffer] === "Real32"];

  DeviceConfigure[device, {"BufferDuration" -> .1, "PeriodSize" -> Automatic}];
  check["configuration", Abs[device["BufferDuration"] - .1] < .001];

  check["stop", DeviceExecute[device, "Stop"] === "Stopped"];
  check["clear", DeviceExecute[device, "ClearBuffer"] === 0];
  check["empty buffer", DeviceReadBuffer[device] === {}];
  check["start", DeviceExecute[device, "Start"] === "Running"];

  DeviceExecute[device, "ClearBuffer"];
  asynchronousEvents = {};
  asynchronousHandler[_, "BufferReady", {available_}] :=
    AppendTo[asynchronousEvents, available];
  asynchronousTask = DeviceExecuteAsynchronous[
    device, "ReadBuffer", 128, asynchronousHandler];
  check["asynchronous task object", Head[asynchronousTask] === AsynchronousTaskObject];
  Pause[.1];
  check["asynchronous buffer-ready event",
    Length[asynchronousEvents] === 1 && First[asynchronousEvents] >= 128];
  If[Head[asynchronousTask] === AsynchronousTaskObject,
    RemoveAsynchronousTask[asynchronousTask]
  ];
  DeviceExecute[device, "ClearBuffer"];

  DeviceClose[device];
  check["close", !DeviceOpenQ[device]];
  check["closed microphone read fails", Quiet[DeviceReadBuffer[device]] === $Failed];
];

check["speaker driver registration",
  NumericQ[DeviceFramework`DeviceDriverVersion["Speaker"]]
];

speaker = Quiet[Check[DeviceOpen["Speaker"], $Failed]];
check["open default output", Head[speaker] === DeviceObject];

If[Head[speaker] === DeviceObject,
  speakerInfo = DeviceExecute[speaker, "Information"];
  check["speaker information association", AssociationQ[speakerInfo]];
  check["speaker sample rate", IntegerQ[speakerInfo["SampleRate"]] && speakerInfo["SampleRate"] > 0];
  check["speaker channel count", IntegerQ[speakerInfo["Channels"]] && speakerInfo["Channels"] > 0];
  check["speaker backend name", StringQ[speakerInfo["Backend"]] && StringLength[speakerInfo["Backend"]] > 0];

  check["speaker stop", DeviceExecute[speaker, "Stop"] === "Stopped"];
  check["speaker clear", DeviceExecute[speaker, "ClearBuffer"] === 0];

  rawOutput = NumericArray[ConstantArray[0., {256, 1}], "Real32"];
  DeviceWriteBuffer[speaker, rawOutput];
  check["raw output queued", speaker["QueuedFrames"] === 256];
  check["raw mono channel conversion", speaker["FreeFrames"] ===
    speaker["BufferCapacityFrames"] - 256];

  DeviceExecute[speaker, "ClearBuffer"];
  silentAudio = Audio[ConstantArray[0., {1, 128}], SampleRate -> speaker["SampleRate"]];
  DeviceWrite[speaker, silentAudio];
  check["Audio output queued", speaker["QueuedFrames"] === 128];

  DeviceConfigure[speaker, {"BufferDuration" -> .1, "PeriodSize" -> Automatic}];
  check["speaker configuration", Abs[speaker["BufferDuration"] - .1] < .001];
  check["speaker reset statistics", DeviceExecute[speaker, "ResetStatistics"] === 0];
  check["speaker start", DeviceExecute[speaker, "Start"] === "Running"];

  DeviceClose[speaker];
  check["speaker close", !DeviceOpenQ[speaker]];
  check["closed speaker write fails",
    Quiet[DeviceWriteBuffer[speaker, NumericArray[{{0.}}, "Real32"]]] === $Failed];
];

If[failures === {},
  Print["All audio device integration checks passed."];
  Exit[0],
  Print["Failed checks: ", failures];
  Exit[1]
];
