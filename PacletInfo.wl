PacletObject[
  <|
    "Name" -> "DeviceMicrophone_Driver",
    "Version" -> "1.2.2",
    "WolframVersion" -> "13.0+",
    "Description" -> "Cross-platform microphone and speaker drivers for the Wolfram Device Framework",
    "Extensions" -> {
      {
        "Kernel",
        "Root" -> "Kernel",
        "Context" -> {
            {"DeviceAPI`Drivers`Microphone`", "Microphone.wl"},
            {"DeviceAPI`Drivers`Speaker`", "Speaker.wl"},
        },
        "Loading" -> "Startup"
      },
      {
        "LibraryLink"
      }
    }
  |>
]
