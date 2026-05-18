# KML/KMZ to GPX Converter

Visual Studio C++ console project for `kml2gpx.exe`.

For full usage, mapping rules, validation commands, and sample notes, see the root project README:

```text
..\README.md
```

## Build

Open `Kml2Gpx.sln` in Visual Studio or build Release x64 with MSBuild.

The executable is written to:

```text
x64\Release\kml2gpx.exe
```

TinyXML2 is vendored in `third_party\tinyxml2`; this project does not require vcpkg. Elevation lookup uses the Windows WinHTTP API.
