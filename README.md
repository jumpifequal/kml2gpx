# KML/KMZ to GPX

Standalone external C++ converter for Google Earth KML and KMZ files.

The program follows the same simple positional command-line shape used by the other GPX conversion utilities:

```text
kml2gpx.exe <input.kml|input.kmz> [output.gpx] [options]
```

If `[output.gpx]` is omitted, the output path is derived from the input path by replacing the extension with `.gpx`.

## Build

Open and build:

```text
Kml2Gpx\Kml2Gpx.sln
```

Release x64 output:

```text
Kml2Gpx\x64\Release\kml2gpx.exe
```

TinyXML2 is vendored in `Kml2Gpx\third_party\tinyxml2`, so no vcpkg setup is required.

## Usage

```powershell
kml2gpx.exe samples\point.kml
kml2gpx.exe samples\point.kmz
kml2gpx.exe samples\line.kml samples\line.gpx
kml2gpx.exe "samples\Sentieri della Collina di Torino (1.0.6).kml"
```

Options:

| Option                            | Description                                                                                                              |
| --------------------------------- | ------------------------------------------------------------------------------------------------------------------------ |
| `-h`, `--help`                    | Show help.                                                                                                               |
| `-v`, `--version`                 | Show version.                                                                                                            |
| `--do-not-fetch-elevation`        | Do not call OpenTopoData for missing elevations.                                                                         |
| `--elevation-dataset <name\|csv>` | Dataset ID or comma-separated list. Default: `srtm90m`.                                                                  |
| `--force`                         | Fetch elevation for all points; if the KML already has elevation, overwrite it only when fetched elevation is available. |

## Elevation Sources

By default, `kml2gpx` preserves KML altitude values and calls OpenTopoData only for points where elevation is missing.

Use `--do-not-fetch-elevation` to disable network elevation lookup. Use `--force` to fetch elevation for every point; when the source KML already has elevation, `--force` clearly reports that it is overwriting KML elevations when fetched elevation is available.

Multiple datasets can be specified as a comma-separated list:

```powershell
Kml2Gpx\x64\Release\kml2gpx.exe route.kml --elevation-dataset srtm30m,eudem25m
Kml2Gpx\x64\Release\kml2gpx.exe route.kml --force --elevation-dataset eudem25m
```

| Dataset     | Description                                                                               | Coverage                                      |
| ----------- | ----------------------------------------------------------------------------------------- | --------------------------------------------- |
| `srtm90m`   | Global baseline ~90 m / 3 arc-sec SRTM. Good default when detail is not critical.         | Global land roughly between 60N and 56S       |
| `srtm30m`   | Higher-detail SRTM ~30 m / SRTMGL1 v3. Fewer voids than older SRTM90.                     | Near-global land, within SRTM latitude limits |
| `aster30m`  | ASTER GDEM ~30 m. Good detail in mountains, but may contain artifacts in forests or snow. | Global land                                   |
| `eudem25m`  | EU-DEM ~25 m. Balanced and clean DEM for Europe.                                          | Europe / EU / EEA                             |
| `ned10m`    | US NED/3DEP ~10 m. Higher detail, suitable for U.S. hiking and biking routes.             | United States; CONUS, AK/HI varies            |
| `nzdem8m`   | New Zealand 8 m DEM. Very detailed for local routes.                                      | New Zealand                                   |
| `etopo1`    | Coarse 1 arc-min ~1.8 km topography and bathymetry. Stable global fallback.               | Global land and ocean                         |
| `gebco2020` | Global bathymetry/topography ~15 arc-sec / ~500 m. Useful near coasts and ocean.          | Global land and ocean                         |
| `emod2018`  | European marine bathymetry composite. Better sea-floor detail than global datasets.       | European seas                                 |
| `mapzen`    | Composite terrain tiles. Reasonable global fallback where other datasets are sparse.      | Global composite                              |
| `bkg200`    | Germany DEM at ~200 m. Lightweight background dataset.                                    | Germany                                       |

## KML Support

`.kmz` input is supported by extracting the first `.kml` file found in the archive and then applying the same conversion rules below. Embedded resources are ignored.

Supported source structures:

- `Placemark`
- `Point`
- `LineString`
- `MultiGeometry` containing supported geometries
- `gx:Track`

Coordinate formats:

- `Point` and `LineString`: `longitude,latitude[,altitude]`
- `gx:Track`: `gx:coord` values in `longitude latitude [altitude]`

## GPX Mapping

| KML                                   | GPX                                            |
| ------------------------------------- | ---------------------------------------------- |
| `Point`                               | `wpt`                                          |
| `LineString`                          | `trk` with one `trkseg`                        |
| each KML `LineString`                 | separate GPX `trk`                             |
| `gx:Track`                            | `trk` with timed `trkpt` entries               |
| altitude                              | `ele`                                          |
| `name`                                | `name`                                         |
| `description`                         | `desc`                                         |
| `TimeStamp/when` on a point placemark | waypoint `time`                                |
| `gx:Track/when`                       | track point `time`                             |
| timed consecutive track points        | derived `<extensions><speed>` in meters/second |

Plain `LineString` data usually has no per-point time. In that case the converter preserves track and elevation, but cannot derive speed or time.

# 

## Limitations

Not currently converted:

- `Polygon`
- styles and icons
- folders as GPX groups
- embedded resources from KMZ archives
- speed/time for untimed `LineString` coordinates

Malformed coordinate data fails with a clear `[kml]` error and a non-zero exit code.
