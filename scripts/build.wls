root = DirectoryName[DirectoryName[ExpandFileName[$InputFileName]]];
vendorDirectory = FileNameJoin[{root, "vendor"}];
header = FileNameJoin[{vendorDirectory, "miniaudio.h"}];
headerURL = "https://raw.githubusercontent.com/mackron/miniaudio/0.11.25/miniaudio.h";
headerSHA256 = FromDigits["ac7af4de748b7e26b777f37e01cee313a308a7296a3eb080e2906b320cc55c89", 16];

If[!DirectoryQ[vendorDirectory],
  CreateDirectory[vendorDirectory, CreateIntermediateDirectories -> True]
];

If[!FileExistsQ[header] || FileHash[header, "SHA256"] =!= headerSHA256,
  temporaryHeader = FileNameJoin[{vendorDirectory, "miniaudio.h.download"}];
  Print["Downloading pinned miniaudio 0.11.25..."];
  Quiet[DeleteFile[temporaryHeader]];
  Check[URLDownload[headerURL, temporaryHeader],
    Print["Unable to download ", headerURL]; Exit[2]
  ];
  If[FileHash[temporaryHeader, "SHA256"] =!= headerSHA256,
    Quiet[DeleteFile[temporaryHeader]];
    Print["Downloaded miniaudio.h failed its SHA-256 check."];
    Exit[3]
  ];
  RenameFile[temporaryHeader, header, OverwriteTarget -> True];
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
Exit[0];
