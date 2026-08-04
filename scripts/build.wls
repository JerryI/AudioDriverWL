root = DirectoryName[DirectoryName[ExpandFileName[$InputFileName]]];
vendorDirectory = FileNameJoin[{root, "vendor"}];
header = FileNameJoin[{vendorDirectory, "miniaudio.h"}];
headerURL = "https://raw.githubusercontent.com/mackron/miniaudio/0.11.25/miniaudio.h";
upstreamHeaderSHA256 = FromDigits["ac7af4de748b7e26b777f37e01cee313a308a7296a3eb080e2906b320cc55c89", 16];
patchedHeaderSHA256 = FromDigits["af63771146fb3b5477c4c34afb256ad475390b9889fe49d06c8449cce1e4f194", 16];

coreAudioS16Pattern = StringRiffle[{
  "            if (pDescription->mBitsPerChannel == 16) {",
  "                *pFormatOut = ma_format_s16;",
  "                return MA_SUCCESS;",
  "            } else if (pDescription->mBitsPerChannel == 24) {"
}, "\n"];
coreAudioS16Replacement = StringRiffle[{
  "            if (pDescription->mBitsPerChannel == 16) {",
  "                ma_uint32 expectedBytesPerFrame = sizeof(ma_int16);",
  "                if ((pDescription->mFormatFlags & kAudioFormatFlagIsNonInterleaved) == 0) {",
  "                    expectedBytesPerFrame *= pDescription->mChannelsPerFrame;",
  "                }",
  "",
  "                /*",
  "                Some USB interfaces advertise 16 significant bits in a 24-bit",
  "                container. ma_format_s16 is tightly packed, so accepting that",
  "                ASBD would make the Core Audio buffer three bytes per sample",
  "                while miniaudio advances it by two bytes per sample. Skip the",
  "                padded representation and let format selection choose a truly",
  "                packed format instead.",
  "                */",
  "                if (pDescription->mBytesPerFrame == expectedBytesPerFrame) {",
  "                    *pFormatOut = ma_format_s16;",
  "                    return MA_SUCCESS;",
  "                }",
  "                return MA_FORMAT_NOT_SUPPORTED;",
  "            } else if (pDescription->mBitsPerChannel == 24) {"
}, "\n"];

If[!DirectoryQ[vendorDirectory],
  CreateDirectory[vendorDirectory, CreateIntermediateDirectories -> True]
];

If[!FileExistsQ[header] ||
    !MemberQ[{upstreamHeaderSHA256, patchedHeaderSHA256}, FileHash[header, "SHA256"]],
  temporaryHeader = FileNameJoin[{vendorDirectory, "miniaudio.h.download"}];
  Print["Downloading pinned miniaudio 0.11.25..."];
  Quiet[DeleteFile[temporaryHeader]];
  Check[URLDownload[headerURL, temporaryHeader],
    Print["Unable to download ", headerURL]; Exit[2]
  ];
  If[FileHash[temporaryHeader, "SHA256"] =!= upstreamHeaderSHA256,
    Quiet[DeleteFile[temporaryHeader]];
    Print["Downloaded miniaudio.h failed its SHA-256 check."];
    Exit[3]
  ];
  RenameFile[temporaryHeader, header, OverwriteTarget -> True];
];

If[FileHash[header, "SHA256"] === upstreamHeaderSHA256,
  headerText = Import[header, "Text"];
  patchedHeaderText = StringReplace[headerText, coreAudioS16Pattern -> coreAudioS16Replacement];
  If[patchedHeaderText === headerText,
    Print["Unable to apply the Core Audio packed-s16 compatibility patch."];
    Exit[3]
  ];
  Export[header, patchedHeaderText, "Text"];
];


Needs["CCompilerDriver`"];
targetDirectory = FileNameJoin[{root, "LibraryResources", $SystemID}];
If[!DirectoryQ[targetDirectory],
  CreateDirectory[targetDirectory, CreateIntermediateDirectories -> True]
];

compileOptions = Switch[$OperatingSystem,
  "Windows", {"/O2", "/DNDEBUG"},
  _, {"-O3", "-DNDEBUG"}
];
extraLibraries = If[$SystemID === "Linux-ARM", {"atomic"}, {}];

libraries = Table[
  library = Check[
    CCompilerDriver`CreateLibrary[
      {FileNameJoin[{root, "native", name <> ".c"}]},
      name,
      "TargetDirectory" -> targetDirectory,
      "IncludeDirectories" -> {vendorDirectory},
      "CompileOptions" -> compileOptions,
      "Libraries" -> extraLibraries,
      "CleanIntermediate" -> True
    ],
    $Failed
  ];
  If[library === $Failed || !FileExistsQ[library],
    Print["Native library build failed for ", name, "."];
    Exit[4]
  ];
  Print["Built ", library];
  library,
  {name, {"microphone", "speaker"}}
];

If[$OperatingSystem === "MacOSX",
  helper = Check[
    CCompilerDriver`CreateExecutable[
      {FileNameJoin[{root, "native", "microphone_helper.c"}]},
      "microphone_helper",
      "TargetDirectory" -> targetDirectory,
      "IncludeDirectories" -> {vendorDirectory, FileNameJoin[{root, "native"}]},
      "CompileOptions" -> compileOptions,
      "Libraries" -> extraLibraries,
      "CleanIntermediate" -> True
    ],
    $Failed
  ];
  If[helper === $Failed || !FileExistsQ[helper],
    Print["Native microphone helper build failed for ", $SystemID, "."];
    Exit[4]
  ];
  Print["Built ", helper];
];
Exit[0];
