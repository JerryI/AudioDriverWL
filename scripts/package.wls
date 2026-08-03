root = DirectoryName[DirectoryName[ExpandFileName[$InputFileName]]];
target = FileNameJoin[{root, "build"}];
libraryDirectory = FileNameJoin[{root, "LibraryResources", $SystemID}];

If[!DirectoryQ[libraryDirectory] ||
   AnyTrue[{"microphone", "speaker"}, FileNames[# <> ".*", libraryDirectory] === {} &],
  Print["One or more native audio libraries are missing for ", $SystemID,
    ". Run scripts/build.wls first."];
  Exit[2]
];

If[!DirectoryQ[target], CreateDirectory[target, CreateIntermediateDirectories -> True]];
Needs["PacletTools`"];
result = PacletTools`PacletBuild[root, target];
Print[result];
If[MatchQ[result, _Success], Exit[0], Exit[3]];
