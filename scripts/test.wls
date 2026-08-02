root = DirectoryName[DirectoryName[ExpandFileName[$InputFileName]]];
failures = {};

check[name_, condition_] := If[TrueQ[condition],
  Print["PASS  ", name],
  AppendTo[failures, name]; Print["FAIL  ", name]
];

PacletDirectoryLoad[root];
check["driver discovery",
  Length[DeviceFramework`FindDeviceDrivers["Microphone"]] >= 1
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

  DeviceClose[device];
  check["close", !DeviceOpenQ[device]];
];

If[failures === {},
  Print["All microphone integration checks passed."];
  Exit[0],
  Print["Failed checks: ", failures];
  Exit[1]
];

