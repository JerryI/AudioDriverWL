BeginPackage["DeviceAPI`Drivers`Speaker`"];

DeviceFramework`Devices`Speaker::lib =
  "The native speaker library could not be loaded. Run scripts/build.wls for system `1`.";
DeviceFramework`Devices`Speaker::open =
  "The system default output device could not be opened: `1`.";
DeviceFramework`Devices`Speaker::config =
  "Invalid speaker configuration: `1`.";
DeviceFramework`Devices`Speaker::native =
  "The native audio operation failed: `1`.";
DeviceFramework`Devices`Speaker::write =
  "DeviceWrite expects one Audio object; received `1`.";
DeviceFramework`Devices`Speaker::buffer =
  "Audio buffer `1` must be a numeric vector or frames-by-channels matrix. The speaker has `2` channels.";
DeviceFramework`Devices`Speaker::command =
  "Unknown speaker command `1`. Use \"Information\", \"QueuedFrames\", \"FreeFrames\", \"ClearBuffer\", \"ResetStatistics\", \"Start\", or \"Stop\".";

Begin["`Private`"];

$driverRoot = DirectoryName[ExpandFileName[$InputFileName]] // ParentDirectory;
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
  "BufferCapacityFrames", "QueuedFrames", "FreeFrames", "DroppedFrames", "UnderrunFrames",
  "Backend", "DeviceName", "State", "NativeSampleRate", "NativeChannels"
};
$readOnlyProperties = $dynamicProperties;

resolveNativeLibrary[] := Module[{directory, candidates, found},
  directory = FileNameJoin[{$driverRoot, "LibraryResources", $SystemID}];
  candidates = If[DirectoryQ[directory], FileNames["speaker.*", directory], {}];
  found = Quiet[Check[FindLibrary["speaker"], $Failed]];
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
    Message[DeviceFramework`Devices`Speaker::lib, $SystemID];
    Return[False]
  ];
  If[!TrueQ[Quiet[Check[
      $nativeOpen = LibraryFunctionLoad[library, "speakerOpen",
        {Integer, Integer, Integer, Integer, Integer}, Integer];
      $nativeClose = LibraryFunctionLoad[library, "speakerClose", {Integer}, Integer];
      $nativeConfigure = LibraryFunctionLoad[library, "speakerConfigure",
        {Integer, Integer, Integer, Integer, Integer, Integer}, Integer];
      $nativeWrite = LibraryFunctionLoad[library, "speakerWrite",
        {Integer, LibraryDataType[NumericArray]}, Integer];
      $nativeGetInteger = LibraryFunctionLoad[library, "speakerGetInteger",
        {Integer, Integer}, Integer];
      $nativeGetString = LibraryFunctionLoad[library, "speakerGetString",
        {Integer, Integer}, "UTF8String"];
      $nativeDescribeResult = LibraryFunctionLoad[library, "speakerDescribeResult",
        {Integer}, "UTF8String"];
      $nativeControl = LibraryFunctionLoad[library, "speakerControl",
        {Integer, Integer}, Integer];
      True,
      False
    ]]],
    Message[DeviceFramework`Devices`Speaker::lib, $SystemID];
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

SpeakerHandle /: MakeBoxes[SpeakerHandle[id_], format_] :=
  InterpretationBox[RowBox[{"SpeakerHandle", "[", ToBoxes[id, format], "]"}], SpeakerHandle[id]];

nativeID[SpeakerHandle[id_Integer]] := id;
nativeID[_] := $Failed;

open[_, arguments___] := Module[{configuration, id},
  If[!loadNative[], Return[$Failed]];
  configuration = configurationFrom[$defaultConfiguration, {arguments}];
  If[FailureQ[configuration],
    Message[DeviceFramework`Devices`Speaker::config,
      Lookup[configuration[[2]], "Message", "expected an Association or rules"]];
    Return[$Failed]
  ];
  id = $nativeOpen @@ nativeConfiguration[configuration];
  If[!IntegerQ[id] || id <= 0,
    Message[DeviceFramework`Devices`Speaker::open, $nativeDescribeResult[0]];
    Return[$Failed]
  ];
  SpeakerHandle[id]
];

close[{_, handle_}] := Module[{id = nativeID[handle], result},
  If[id === $Failed || !TrueQ[$nativeLoaded], Return[Null]];
  result = $nativeClose[id];
  If[result =!= 0, Message[DeviceFramework`Devices`Speaker::native, $nativeDescribeResult[result]]];
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
  period = $nativeGetInteger[id, 7];
  profile = If[$nativeGetInteger[id, 9] == 1, "Conservative", "LowLatency"];
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
    Message[DeviceFramework`Devices`Speaker::config,
      Lookup[configuration[[2]], "Message", "expected an Association or rules"]];
    Return[$Failed]
  ];
  result = $nativeConfigure[id, Sequence @@ nativeConfiguration[configuration]];
  If[result =!= 0,
    Message[DeviceFramework`Devices`Speaker::native, $nativeDescribeResult[result]];
    Return[$Failed]
  ];
  syncProperties[device];
  Null
];

channelConvert[data_, channels_Integer] := Module[{sourceChannels},
  sourceChannels = If[data === {}, 0, Last[Dimensions[data]]];
  Which[
    data === {}, {},
    sourceChannels === channels, data,
    sourceChannels === 1, Map[ConstantArray[First[#], channels] &, data],
    channels === 1, Map[{Mean[#]} &, data],
    True, $Failed
  ]
];

rawNumericArray[data_NumericArray, channels_Integer] /;
    NumericArrayType[data] === "Real32" &&
    Length[Dimensions[data]] === 2 && Last[Dimensions[data]] === channels := data;
rawNumericArray[data_, channels_Integer] := Module[{dimensions, matrix, converted},
  dimensions = Quiet[Check[Dimensions[data], $Failed]];
  If[dimensions === $Failed || !VectorQ[Flatten[Normal[data]], NumericQ], Return[$Failed]];
  matrix = Switch[Length[dimensions],
    1, List /@ Normal[data],
    2, Normal[data],
    _, Return[$Failed]
  ];
  If[matrix === {}, Return[{}]];
  converted = channelConvert[matrix, channels];
  If[converted === $Failed, Return[$Failed]];
  Quiet[Check[NumericArray[converted, "Real32"], $Failed]]
];

writeBuffer[{_, handle_}, data_] := Module[{id, channels, array, written},
  id = nativeID[handle];
  channels = $nativeGetInteger[id, 1];
  array = rawNumericArray[data, channels];
  If[array === $Failed,
    Message[DeviceFramework`Devices`Speaker::buffer, Dimensions[data], channels];
    Return[$Failed]
  ];
  If[array === {}, Return[Null]];
  written = Quiet[Check[$nativeWrite[id, array], -1]];
  If[!IntegerQ[written] || written < 0,
    Message[DeviceFramework`Devices`Speaker::native, "the audio buffer could not be queued"];
    Return[$Failed]
  ];
  Null
];
writeBuffer[{_, handle_}, arguments___] := (
  Message[DeviceFramework`Devices`Speaker::buffer, HoldForm[{arguments}],
    $nativeGetInteger[nativeID[handle], 1]];
  $Failed
);

audioSampleRate[audio_] := Quiet[Check[
  Round[QuantityMagnitude[UnitConvert[AudioSampleRate[audio], "Hertz"]]],
  $Failed
]];

write[handles : {_, handle_}, audio_?AudioQ] := Module[
  {sampleRate, prepared, samples},
  sampleRate = $nativeGetInteger[nativeID[handle], 0];
  prepared = If[audioSampleRate[audio] === sampleRate,
    audio,
    Quiet[Check[AudioResample[audio, sampleRate], $Failed]]
  ];
  If[prepared === $Failed,
    Message[DeviceFramework`Devices`Speaker::write, HoldForm[audio]];
    Return[$Failed]
  ];
  samples = Transpose[AudioData[prepared, "Real32"]];
  writeBuffer[handles, samples]
];
write[_, arguments___] := (
  Message[DeviceFramework`Devices`Speaker::write, HoldForm[{arguments}]];
  $Failed
);

integerPropertyCode = <|
  "SampleRate" -> 0,
  "Channels" -> 1,
  "BufferCapacityFrames" -> 2,
  "QueuedFrames" -> 3,
  "FreeFrames" -> 4,
  "DroppedFrames" -> 5,
  "UnderrunFrames" -> 6,
  "PeriodSize" -> 7,
  "NativeSampleRate" -> 10,
  "NativeChannels" -> 11
|>;
stringPropertyCode = <|"DeviceName" -> 0, "Backend" -> 1, "State" -> 2|>;

getProperty[device_, property_] /; KeyExistsQ[integerPropertyCode, property] && DeviceOpenQ[device] :=
  $nativeGetInteger[nativeID[DeviceFramework`DeviceHandle[device]], integerPropertyCode[property]];
getProperty[device_, "BufferDuration"] /; DeviceOpenQ[device] :=
  N[$nativeGetInteger[nativeID[DeviceFramework`DeviceHandle[device]], 2]/
    $nativeGetInteger[nativeID[DeviceFramework`DeviceHandle[device]], 0]];
getProperty[device_, "PerformanceProfile"] /; DeviceOpenQ[device] :=
  If[$nativeGetInteger[nativeID[DeviceFramework`DeviceHandle[device]], 9] == 1,
    "Conservative", "LowLatency"];
getProperty[device_, property_] /; KeyExistsQ[stringPropertyCode, property] && DeviceOpenQ[device] :=
  $nativeGetString[nativeID[DeviceFramework`DeviceHandle[device]], stringPropertyCode[property]];
getProperty[device_, property_] := DeviceFramework`DeviceGetProperty[device, property];

setProperty[device_, property_, _] /; MemberQ[$readOnlyProperties, property] && DeviceOpenQ[device] :=
  Message[DeviceObject::ronly, property, "Speaker"];
setProperty[device_, property_, value_] /; MemberQ[$configurableProperties, property] && DeviceOpenQ[device] :=
  configure[{Null, DeviceFramework`DeviceHandle[device]}, property -> value];
setProperty[device_, property_, value_] := DeviceFramework`DeviceSetProperty[device, property, value];

information[handle_] := Association[
  "DeviceName" -> $nativeGetString[nativeID[handle], 0],
  "Backend" -> $nativeGetString[nativeID[handle], 1],
  "State" -> $nativeGetString[nativeID[handle], 2],
  "SampleRate" -> $nativeGetInteger[nativeID[handle], 0],
  "Channels" -> $nativeGetInteger[nativeID[handle], 1],
  "NativeSampleRate" -> $nativeGetInteger[nativeID[handle], 10],
  "NativeChannels" -> $nativeGetInteger[nativeID[handle], 11],
  "PeriodSize" -> $nativeGetInteger[nativeID[handle], 7],
  "BufferCapacityFrames" -> $nativeGetInteger[nativeID[handle], 2],
  "QueuedFrames" -> $nativeGetInteger[nativeID[handle], 3],
  "FreeFrames" -> $nativeGetInteger[nativeID[handle], 4],
  "DroppedFrames" -> $nativeGetInteger[nativeID[handle], 5],
  "UnderrunFrames" -> $nativeGetInteger[nativeID[handle], 6]
];

execute[{_, handle_}, "Information"] := information[handle];
execute[{_, handle_}, "QueuedFrames"] := $nativeGetInteger[nativeID[handle], 3];
execute[{_, handle_}, "FreeFrames"] := $nativeGetInteger[nativeID[handle], 4];
execute[{_, handle_}, command : ("Start" | "Stop" | "ClearBuffer" | "ResetStatistics")] := Module[
  {code = <|"Start" -> 0, "Stop" -> 1, "ClearBuffer" -> 2, "ResetStatistics" -> 3|>[command], result},
  result = $nativeControl[nativeID[handle], code];
  If[result =!= 0,
    Message[DeviceFramework`Devices`Speaker::native, $nativeDescribeResult[result]];
    $Failed,
    Switch[command,
      "ClearBuffer", 0,
      "ResetStatistics", 0,
      _, $nativeGetString[nativeID[handle], 2]
    ]
  ]
];
execute[_, command_, ___] := (
  Message[DeviceFramework`Devices`Speaker::command, command];
  $Failed
);

speakerIcon[___] := Graphics[
  {
    Directive[RGBColor[0.18, 0.45, 0.82]],
    Polygon[{{0.2, 0.42}, {0.37, 0.42}, {0.58, 0.62}, {0.58, 0.18}, {0.37, 0.38}, {0.2, 0.38}}],
    Directive[RGBColor[0.12, 0.25, 0.45], Thickness[0.07], CapForm["Round"]],
    BezierCurve[{{0.64, 0.29}, {0.78, 0.4}, {0.64, 0.51}}],
    BezierCurve[{{0.7, 0.19}, {0.94, 0.4}, {0.7, 0.61}}]
  },
  PlotRange -> {{0, 1}, {0, 0.8}}, ImageSize -> 48
];

DeviceFramework`DeviceClassRegister[
  "Speaker",
  "FindFunction" -> ({{}} &),
  "OpenFunction" -> open,
  "PreconfigureFunction" -> preconfigure,
  "ConfigureFunction" -> configure,
  "WriteFunction" -> write,
  "WriteBufferFunction" -> writeBuffer,
  "ExecuteFunction" -> execute,
  "CloseFunction" -> close,
  "Properties" -> Join[
    Normal[$defaultConfiguration],
    Thread[$dynamicProperties -> Missing["NotAvailable"]]
  ],
  "GetPropertyFunction" -> getProperty,
  "SetPropertyFunction" -> setProperty,
  "StatusLabelFunction" -> ({"Playing system output", "Speaker closed"} &),
  "DeviceIconFunction" -> speakerIcon,
  "Singleton" -> True,
  "DeregisterOnClose" -> True,
  "DriverVersion" -> 0.1
];

End[];
EndPackage[];
