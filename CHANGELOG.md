# Changelog

v1.1

- add: support `.kmz` input files by extracting the contained KML and converting it to GPX
- change: update program version output and package manifest to `1.1`
- change: document KML/KMZ usage and KMZ embedded-resource limitation

v1.0

- add: initial KML to GPX converter
- add: support Placemark Point, LineString, MultiGeometry, and gx:Track conversion
- add: optional OpenTopoData elevation lookup for missing elevations
- change: preserve KML altitude, name, description, and supported timestamp data in GPX output
