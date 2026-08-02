BeginPackage["DeviceAPI`Drivers`Microphone`"];

DeviceFramework`Devices`Microphone::lib =
  "The native microphone library could not be loaded. Run scripts/build.wls for system `1`.";
DeviceFramework`Devices`Microphone::open =
  "The system default input device could not be opened: `1`.";
DeviceFramework`Devices`Microphone::config =
  "Invalid microphone configuration: `1`.";
DeviceFramework`Devices`Microphone::native =
  "The native audio operation failed: `1`.";
DeviceFramework`Devices`Microphone::read =
  "The read criterion `1` must be Automatic, All, a non-negative frame count, or a time Quantity.";
DeviceFramework`Devices`Microphone::command =
  "Unknown microphone command `1`. Use \"Information\", \"AvailableFrames\", \"ClearBuffer\", \"Start\", or \"Stop\".";

Begin["`Private`"];

$driverRoot = DirectoryName[ExpandFileName[$InputFileName]];
$nativeLoaded = False;
$nativeLibrary = None;

$defaultConfiguration = <|
  "SampleRate" -> Automatic,
  "Channels" -> Automatic,
  "BufferDuration" -> 2.,
  "PeriodSize" -> Automatic,
  "PerformanceProfile" -> "LowLatency"
|>;

$configurableProperties = Keys[$defaultConfiguration];
$dynamicProperties = {
  "BufferCapacityFrames", "AvailableFrames", "DroppedFrames", "Backend", "DeviceName", "State",
  "NativeSampleRate", "NativeChannels"
};
$readOnlyProperties = $dynamicProperties;

resolveNativeLibrary[] := Module[{directory, candidates, found},
  directory = FileNameJoin[{$driverRoot, "LibraryResources", $SystemID}];
  candidates = If[DirectoryQ[directory], FileNames["microphone.*", directory], {}];
  found = Quiet[Check[FindLibrary["microphone"], $Failed]];
  SelectFirst[
    DeleteDuplicates[Join[candidates, If[StringQ[found], {found}, {}]]],
    StringQ[#] && FileExistsQ[#] &,
    $Failed
  ]
];

loadNative[] := Module[{library},
  If[TrueQ[$nativeLoaded], Return[True]];
  library = resolveNativeLibrary[];
  If[library === $Failed,
    Message[DeviceFramework`Devices`Microphone::lib, $SystemID];
    Return[False]
  ];
  If[!TrueQ[Quiet[Check[
      $nativeOpen = LibraryFunctionLoad[library, "microphoneOpen",
        {Integer, Integer, Integer, Integer, Integer}, Integer];
      $nativeClose = LibraryFunctionLoad[library, "microphoneClose", {Integer}, Integer];
      $nativeConfigure = LibraryFunctionLoad[library, "microphoneConfigure",
        {Integer, Integer, Integer, Integer, Integer, Integer}, Integer];
      $nativeRead = LibraryFunctionLoad[library, "microphoneRead",
        {Integer, Integer}, LibraryDataType[NumericArray]];
      $nativeGetInteger = LibraryFunctionLoad[library, "microphoneGetInteger",
        {Integer, Integer}, Integer];
      $nativeGetString = LibraryFunctionLoad[library, "microphoneGetString",
        {Integer, Integer}, "UTF8String"];
      $nativeDescribeResult = LibraryFunctionLoad[library, "microphoneDescribeResult",
        {Integer}, "UTF8String"];
      $nativeControl = LibraryFunctionLoad[library, "microphoneControl",
        {Integer, Integer}, Integer];
      True,
      False
    ]]],
    Message[DeviceFramework`Devices`Microphone::lib, $SystemID];
    Return[False]
  ];
  $nativeLibrary = library;
  $nativeLoaded = True;
  True
];

rulesAssociation[{}] := <||>;
rulesAssociation[{association_Association}] := association;
rulesAssociation[arguments_List] /; AllTrue[arguments, MatchQ[_Rule | _RuleDelayed]] :=
  Association[arguments];
rulesAssociation[{arguments_List}] /; AllTrue[arguments, MatchQ[_Rule | _RuleDelayed]] :=
  Association[arguments];
rulesAssociation[other_] := Failure["Arguments", <|"Arguments" -> other|>];

validSampleRateQ[Automatic] := True;
validSampleRateQ[value_] := IntegerQ[value] && 8000 <= value <= 384000;
validChannelsQ[Automatic] := True;
validChannelsQ[value_] := IntegerQ[value] && 1 <= value <= 32;
validBufferDurationQ[value_] := NumericQ[value] && 0.01 <= value <= 60;
validPeriodSizeQ[Automatic] := True;
validPeriodSizeQ[value_] := IntegerQ[value] && 1 <= value <= 262144;
validPerformanceProfileQ[value_] := MemberQ[{"LowLatency", "Conservative"}, value];

validateConfiguration[configuration_Association] := Module[{unknown, invalid},
  unknown = Complement[Keys[configuration], $configurableProperties];
  If[unknown =!= {}, Return["unknown properties " <> ToString[unknown, InputForm]]];
  invalid = Select[
    {
      "SampleRate" -> validSampleRateQ[configuration["SampleRate"]],
      "Channels" -> validChannelsQ[configuration["Channels"]],
      "BufferDuration" -> validBufferDurationQ[configuration["BufferDuration"]],
      "PeriodSize" -> validPeriodSizeQ[configuration["PeriodSize"]],
      "PerformanceProfile" -> validPerformanceProfileQ[configuration["PerformanceProfile"]]
    },
    Last[#] =!= True &
  ];
  If[invalid === {}, True, "invalid value for " <> First[First[invalid]]]
];

configurationFrom[base_Association, arguments_List] := Module[{updates, result, validation},
  updates = rulesAssociation[arguments];
  If[FailureQ[updates], Return[updates]];
  If[Complement[Keys[updates], $configurableProperties] =!= {},
    Return[Failure["Configuration", <|
      "Message" -> ("unknown properties " <>
        ToString[Complement[Keys[updates], $configurableProperties], InputForm])
    |>]]
  ];
  result = Join[base, updates];
  validation = validateConfiguration[result];
  If[validation === True,
    result,
    Failure["Configuration", <|"Message" -> validation|>]
  ]
];

nativeConfiguration[configuration_Association] := {
  Replace[configuration["SampleRate"], Automatic -> 0],
  Replace[configuration["Channels"], Automatic -> 0],
  Round[1000 N[configuration["BufferDuration"]]],
  Replace[configuration["PeriodSize"], Automatic -> 0],
  If[configuration["PerformanceProfile"] === "Conservative", 1, 0]
};

MicrophoneHandle /: MakeBoxes[MicrophoneHandle[id_], format_] :=
  InterpretationBox[RowBox[{"MicrophoneHandle", "[", ToBoxes[id, format], "]"}], MicrophoneHandle[id]];

nativeID[MicrophoneHandle[id_Integer]] := id;
nativeID[_] := $Failed;

open[_, arguments___] := Module[{configuration, id},
  If[!loadNative[], Return[$Failed]];
  configuration = configurationFrom[$defaultConfiguration, {arguments}];
  If[FailureQ[configuration],
    Message[DeviceFramework`Devices`Microphone::config,
      Lookup[configuration[[2]], "Message", "expected an Association or rules"]];
    Return[$Failed]
  ];
  id = $nativeOpen @@ nativeConfiguration[configuration];
  If[!IntegerQ[id] || id <= 0,
    Message[DeviceFramework`Devices`Microphone::open, $nativeDescribeResult[0]];
    Return[$Failed]
  ];
  MicrophoneHandle[id]
];

close[{_, handle_}] := Module[{id = nativeID[handle], result},
  If[id === $Failed || !TrueQ[$nativeLoaded], Return[Null]];
  result = $nativeClose[id];
  If[result =!= 0, Message[DeviceFramework`Devices`Microphone::native, $nativeDescribeResult[result]]];
  Null
];

setStoredProperty[device_, property_, value_] :=
  DeviceFramework`DeviceSetProperty[device, property, value];

syncProperties[device_] := Module[{id, sampleRate, channels, capacity, period, profile},
  id = nativeID[DeviceFramework`DeviceHandle[device]];
  If[id === $Failed, Return[$Failed]];
  sampleRate = $nativeGetInteger[id, 0];
  channels = $nativeGetInteger[id, 1];
  capacity = $nativeGetInteger[id, 2];
  period = $nativeGetInteger[id, 5];
  profile = If[$nativeGetInteger[id, 7] == 1, "Conservative", "LowLatency"];
  Scan[
    Function[rule, setStoredProperty[device, First[rule], Last[rule]]],
    {
      "SampleRate" -> sampleRate,
      "Channels" -> channels,
      "BufferDuration" -> N[capacity/sampleRate],
      "PeriodSize" -> period,
      "PerformanceProfile" -> profile
    }
  ];
  Null
];

preconfigure[device_] := (
  syncProperties[device];
  Join[$configurableProperties, $dynamicProperties]
);

storedConfiguration[device_] := AssociationMap[
  getProperty[device, #] &,
  $configurableProperties
];

configure[{_, handle_}, arguments___] := Module[
  {id = nativeID[handle], device, configuration, result},
  If[id === $Failed, Return[$Failed]];
  device = DeviceFramework`DeviceObjectFromHandle[handle];
  configuration = configurationFrom[storedConfiguration[device], {arguments}];
  If[FailureQ[configuration],
    Message[DeviceFramework`Devices`Microphone::config,
      Lookup[configuration[[2]], "Message", "expected an Association or rules"]];
    Return[$Failed]
  ];
  result = $nativeConfigure[id, Sequence @@ nativeConfiguration[configuration]];
  If[result =!= 0,
    Message[DeviceFramework`Devices`Microphone::native, $nativeDescribeResult[result]];
    Return[$Failed]
  ];
  syncProperties[device];
  Null
];

frameLimit[_, Automatic | All] := -1;
frameLimit[_, frames_Integer?NonNegative] := frames;
frameLimit[handle_, duration_Quantity] := Module[{seconds, rate},
  seconds = Quiet[Check[QuantityMagnitude[UnitConvert[duration, "Seconds"]], $Failed]];
  rate = $nativeGetInteger[nativeID[handle], 0];
  If[NumericQ[seconds] && NonNegative[seconds], Round[seconds rate], $Failed]
];
frameLimit[_, _] := $Failed;

readBuffer[{_, handle_}, criterion_, parameters_: Automatic] := Module[{id, limit, available, frames},
  If[parameters =!= Automatic,
    Message[DeviceFramework`Devices`Microphone::read, parameters];
    Return[$Failed]
  ];
  limit = frameLimit[handle, criterion];
  If[limit === $Failed,
    Message[DeviceFramework`Devices`Microphone::read, criterion];
    Return[$Failed]
  ];
  id = nativeID[handle];
  available = $nativeGetInteger[id, 3];
  If[available <= 0 || limit == 0, Return[{}]];
  frames = If[limit < 0, available, Min[limit, available]];
  Quiet[Check[$nativeRead[id, frames], $Failed]]
];

audioFromBuffer[handle_, buffer_] := Module[{dimensions, sampleRate, data},
  If[buffer === $Failed, Return[$Failed]];
  dimensions = Dimensions[buffer];
  If[dimensions === {} || First[dimensions] == 0, Return[Missing["NotAvailable"]]];
  sampleRate = $nativeGetInteger[nativeID[handle], 0];
  data = If[Last[dimensions] == 1, Flatten[buffer], Transpose[buffer]];
  Audio[data, SampleRate -> sampleRate]
];

read[handles : {_, handle_}] :=
  audioFromBuffer[handle, readBuffer[handles, Automatic]];
read[handles : {_, _}, "Raw"] := readBuffer[handles, Automatic];
read[handles : {_, _}, "Raw", criterion_] := readBuffer[handles, criterion];
read[handles : {_, handle_}, criterion_] :=
  audioFromBuffer[handle, readBuffer[handles, criterion]];
read[_, arguments___] := (
  Message[DeviceFramework`Devices`Microphone::read, HoldForm[{arguments}]];
  $Failed
);

integerPropertyCode = <|
  "SampleRate" -> 0,
  "Channels" -> 1,
  "BufferCapacityFrames" -> 2,
  "AvailableFrames" -> 3,
  "DroppedFrames" -> 4,
  "PeriodSize" -> 5,
  "NativeSampleRate" -> 8,
  "NativeChannels" -> 9
|>;
stringPropertyCode = <|"DeviceName" -> 0, "Backend" -> 1, "State" -> 2|>;

getProperty[device_, property_] /; KeyExistsQ[integerPropertyCode, property] && DeviceOpenQ[device] :=
  $nativeGetInteger[nativeID[DeviceFramework`DeviceHandle[device]], integerPropertyCode[property]];
getProperty[device_, "BufferDuration"] /; DeviceOpenQ[device] :=
  N[$nativeGetInteger[nativeID[DeviceFramework`DeviceHandle[device]], 2]/
    $nativeGetInteger[nativeID[DeviceFramework`DeviceHandle[device]], 0]];
getProperty[device_, "PerformanceProfile"] /; DeviceOpenQ[device] :=
  If[$nativeGetInteger[nativeID[DeviceFramework`DeviceHandle[device]], 7] == 1,
    "Conservative", "LowLatency"];
getProperty[device_, property_] /; KeyExistsQ[stringPropertyCode, property] && DeviceOpenQ[device] :=
  $nativeGetString[nativeID[DeviceFramework`DeviceHandle[device]], stringPropertyCode[property]];
getProperty[device_, property_] := DeviceFramework`DeviceGetProperty[device, property];

setProperty[device_, property_, _] /; MemberQ[$readOnlyProperties, property] && DeviceOpenQ[device] :=
  Message[DeviceObject::ronly, property, "Microphone"];
setProperty[device_, property_, value_] /; MemberQ[$configurableProperties, property] && DeviceOpenQ[device] :=
  configure[{Null, DeviceFramework`DeviceHandle[device]}, property -> value];
setProperty[device_, property_, value_] := DeviceFramework`DeviceSetProperty[device, property, value];

information[handle_] := Association[
  "DeviceName" -> $nativeGetString[nativeID[handle], 0],
  "Backend" -> $nativeGetString[nativeID[handle], 1],
  "State" -> $nativeGetString[nativeID[handle], 2],
  "SampleRate" -> $nativeGetInteger[nativeID[handle], 0],
  "Channels" -> $nativeGetInteger[nativeID[handle], 1],
  "NativeSampleRate" -> $nativeGetInteger[nativeID[handle], 8],
  "NativeChannels" -> $nativeGetInteger[nativeID[handle], 9],
  "PeriodSize" -> $nativeGetInteger[nativeID[handle], 5],
  "BufferCapacityFrames" -> $nativeGetInteger[nativeID[handle], 2],
  "AvailableFrames" -> $nativeGetInteger[nativeID[handle], 3],
  "DroppedFrames" -> $nativeGetInteger[nativeID[handle], 4]
];

execute[{_, handle_}, "Information"] := information[handle];
execute[{_, handle_}, "AvailableFrames"] := $nativeGetInteger[nativeID[handle], 3];
execute[{_, handle_}, command : ("Start" | "Stop" | "ClearBuffer")] := Module[
  {code = <|"Start" -> 0, "Stop" -> 1, "ClearBuffer" -> 2|>[command], result},
  result = $nativeControl[nativeID[handle], code];
  If[result =!= 0,
    Message[DeviceFramework`Devices`Microphone::native, $nativeDescribeResult[result]];
    $Failed,
    If[command === "ClearBuffer", 0, $nativeGetString[nativeID[handle], 2]]
  ]
];
execute[_, command_, ___] := (
  Message[DeviceFramework`Devices`Microphone::command, command];
  $Failed
);

microphoneIcon[___] := Graphics[
  {
    Directive[RGBColor[0.18, 0.45, 0.82], Thickness[0.09], CapForm["Round"]],
    Line[{{0.35, 0.53}, {0.35, 0.66}}],
    Line[{{0.65, 0.53}, {0.65, 0.66}}],
    Line[{{0.35, 0.53}, {0.38, 0.38}, {0.5, 0.32}, {0.62, 0.38}, {0.65, 0.53}}],
    Directive[RGBColor[0.12, 0.25, 0.45], Thickness[0.08]],
    Line[{{0.5, 0.3}, {0.5, 0.16}}],
    Line[{{0.38, 0.14}, {0.62, 0.14}}]
  },
  PlotRange -> {{0, 1}, {0, 1}}, ImageSize -> 48
];

DeviceFramework`DeviceClassRegister[
  "Microphone",
  "FindFunction" -> ({{}} &),
  "OpenFunction" -> open,
  "PreconfigureFunction" -> preconfigure,
  "ConfigureFunction" -> configure,
  "ReadFunction" -> read,
  "ReadBufferFunction" -> readBuffer,
  "ExecuteFunction" -> execute,
  "CloseFunction" -> close,
  "Properties" -> Join[
    Normal[$defaultConfiguration],
    Thread[$dynamicProperties -> Missing["NotAvailable"]]
  ],
  "GetPropertyFunction" -> getProperty,
  "SetPropertyFunction" -> setProperty,
  "StatusLabelFunction" -> ({"Recording system input", "Microphone closed"} &),
  "DeviceIconFunction" -> microphoneIcon,
  "Singleton" -> True,
  "DeregisterOnClose" -> True,
  "DriverVersion" -> 0.1
];

End[];
EndPackage[];
