#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <set>

#include <windows.h>
#include <winhttp.h>

#include "third_party/tinyxml2/tinyxml2.h"

#ifdef _MSC_VER
#pragma comment(lib, "winhttp.lib")
#endif

struct Coordinate {
    double lon = 0.0;
    double lat = 0.0;
    double ele = 0.0;
    bool has_ele = false;
    double speed = 0.0; // meters/second, derived when timed track data is available
    bool has_speed = false;
    std::string time;
};

struct Waypoint {
    Coordinate coord;
    std::string name;
    std::string desc;
};

struct Track {
    std::vector<Coordinate> points;
    std::string name;
    std::string desc;
};

struct ConversionResult {
    std::vector<Waypoint> waypoints;
    std::vector<Track> tracks;
};

struct Args {
    std::string kmlFile;
    std::string gpxFile;
    bool fetchElevation = true;
    bool forceElevation = false;
    std::string dataset_csv = "srtm90m";
    bool showHelp = false;
};

struct ElevationTarget {
    Coordinate* coord = nullptr;
};

struct DatasetInfo {
    const char* id;
    const char* description;
    const char* coverage;
};

static const DatasetInfo kDatasetInfos[] = {
    { "srtm90m", "Global baseline ~90 m / 3 arc-sec SRTM. Good default when detail is not critical.", "Global land roughly between 60N and 56S" },
    { "srtm30m", "Higher-detail SRTM ~30 m / SRTMGL1 v3. Fewer voids than older SRTM90.", "Near-global land, within SRTM latitude limits" },
    { "aster30m", "ASTER GDEM ~30 m. Good detail in mountains, but may contain artifacts in forests or snow.", "Global land" },
    { "eudem25m", "EU-DEM ~25 m. Balanced and clean DEM for Europe.", "Europe / EU / EEA" },
    { "ned10m", "US NED/3DEP ~10 m. Higher detail, suitable for U.S. hiking and biking routes.", "United States; CONUS, AK/HI varies" },
    { "nzdem8m", "New Zealand 8 m DEM. Very detailed for local routes.", "New Zealand" },
    { "etopo1", "Coarse 1 arc-min ~1.8 km topography and bathymetry. Stable global fallback.", "Global land and ocean" },
    { "gebco2020", "Global bathymetry/topography ~15 arc-sec / ~500 m. Useful near coasts and ocean.", "Global land and ocean" },
    { "emod2018", "European marine bathymetry composite. Better sea-floor detail than global datasets.", "European seas" },
    { "mapzen", "Composite terrain tiles. Reasonable global fallback where other datasets are sparse.", "Global composite" },
    { "bkg200", "Germany DEM at ~200 m. Lightweight background dataset.", "Germany" }
};

static const size_t kDatasetInfosCount = sizeof(kDatasetInfos) / sizeof(kDatasetInfos[0]);
static const size_t kElevationBatchSize = 100;

static std::string DeriveGpxPath(const std::string& inPath) {
    const std::string::size_type sep = inPath.find_last_of("/\\");
    const std::string::size_type dot = inPath.rfind('.');
    const bool hasExt = (dot != std::string::npos) && (sep == std::string::npos || dot > sep);
    if (hasExt) return inPath.substr(0, dot) + ".gpx";
    return inPath + ".gpx";
}

static std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

static std::string NormalizePath(std::string p) {
    p = ToLower(p);
    std::replace(p.begin(), p.end(), '\\', '/');
    return p;
}

static std::vector<std::string> SplitChar(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == delim) {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        }
        else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

static const std::set<std::string>& AllowedDatasets() {
    static std::set<std::string> s;
    if (s.empty()) {
        for (size_t i = 0; i < kDatasetInfosCount; ++i) s.insert(kDatasetInfos[i].id);
    }
    return s;
}

static bool ValidateDatasetsCSV(const std::string& csv, std::string& badToken) {
    const std::vector<std::string> parts = SplitChar(csv, ',');
    if (parts.empty()) return false;
    const std::set<std::string>& allowed = AllowedDatasets();
    for (const std::string& p : parts) {
        if (allowed.find(p) == allowed.end()) {
            badToken = p;
            return false;
        }
    }
    return true;
}

static void PrintAllowedDatasetsDetailed() {
    std::cout << "Allowed elevation datasets (OpenTopoData public API):\n";
    for (size_t i = 0; i < kDatasetInfosCount; ++i) {
        std::cout << "  - " << kDatasetInfos[i].id
            << " -> " << kDatasetInfos[i].description
            << " [" << kDatasetInfos[i].coverage << "]\n";
    }
    std::cout << "Multiple datasets can be comma-separated, for example: srtm30m,eudem25m\n";
}

static const char* LocalName(const tinyxml2::XMLElement* elem) {
    if (!elem || !elem->Name()) return "";
    const char* name = elem->Name();
    const char* colon = std::strrchr(name, ':');
    return colon ? colon + 1 : name;
}

static bool IsElement(const tinyxml2::XMLElement* elem, const char* localName) {
    return std::strcmp(LocalName(elem), localName) == 0;
}

static std::string ElementText(const tinyxml2::XMLElement* elem) {
    const char* text = elem ? elem->GetText() : nullptr;
    return text ? std::string(text) : std::string();
}

static const tinyxml2::XMLElement* FirstChildLocal(const tinyxml2::XMLElement* parent, const char* localName) {
    if (!parent) return nullptr;
    for (const tinyxml2::XMLElement* child = parent->FirstChildElement(); child; child = child->NextSiblingElement()) {
        if (IsElement(child, localName)) return child;
    }
    return nullptr;
}

static void FindDescendantsLocal(const tinyxml2::XMLElement* parent,
                                 const char* localName,
                                 std::vector<const tinyxml2::XMLElement*>& out) {
    if (!parent) return;
    for (const tinyxml2::XMLElement* child = parent->FirstChildElement(); child; child = child->NextSiblingElement()) {
        if (IsElement(child, localName)) out.push_back(child);
        FindDescendantsLocal(child, localName, out);
    }
}

static bool ParseDoubleStrict(const std::string& s, double& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return end && *end == '\0';
}

static std::vector<std::string> SplitComma(const std::string& s) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : s) {
        if (c == ',') {
            parts.push_back(cur);
            cur.clear();
        }
        else {
            cur.push_back(c);
        }
    }
    parts.push_back(cur);
    return parts;
}

static std::vector<std::string> SplitWhitespace(const std::string& s) {
    std::vector<std::string> parts;
    std::istringstream iss(s);
    std::string part;
    while (iss >> part) parts.push_back(part);
    return parts;
}

static Coordinate ParseCoordinateTuple(const std::string& tuple) {
    const std::vector<std::string> parts = SplitComma(tuple);
    if (parts.size() < 2 || parts.size() > 3) {
        throw std::runtime_error("[kml] Malformed coordinate tuple: " + tuple);
    }

    Coordinate c;
    if (!ParseDoubleStrict(parts[0], c.lon) || !ParseDoubleStrict(parts[1], c.lat)) {
        throw std::runtime_error("[kml] Invalid longitude/latitude in coordinate tuple: " + tuple);
    }
    if (parts.size() == 3) {
        if (!ParseDoubleStrict(parts[2], c.ele)) {
            throw std::runtime_error("[kml] Invalid altitude in coordinate tuple: " + tuple);
        }
        c.has_ele = true;
    }
    if (c.lon < -180.0 || c.lon > 180.0 || c.lat < -90.0 || c.lat > 90.0) {
        throw std::runtime_error("[kml] Coordinate out of range: " + tuple);
    }
    return c;
}

static Coordinate ParseGxCoordText(const std::string& text) {
    const std::vector<std::string> parts = SplitWhitespace(text);
    if (parts.size() < 2 || parts.size() > 3) {
        throw std::runtime_error("[kml] Malformed gx:coord tuple: " + text);
    }

    Coordinate c;
    if (!ParseDoubleStrict(parts[0], c.lon) || !ParseDoubleStrict(parts[1], c.lat)) {
        throw std::runtime_error("[kml] Invalid longitude/latitude in gx:coord tuple: " + text);
    }
    if (parts.size() == 3) {
        if (!ParseDoubleStrict(parts[2], c.ele)) {
            throw std::runtime_error("[kml] Invalid altitude in gx:coord tuple: " + text);
        }
        c.has_ele = true;
    }
    if (c.lon < -180.0 || c.lon > 180.0 || c.lat < -90.0 || c.lat > 90.0) {
        throw std::runtime_error("[kml] gx:coord out of range: " + text);
    }
    return c;
}

static bool ParseIso8601UtcSeconds(const std::string& text, std::time_t& out) {
    if (text.size() < 20 || text[4] != '-' || text[7] != '-' ||
        text[10] != 'T' || text[13] != ':' || text[16] != ':') {
        return false;
    }

    std::tm tm{};
    try {
        tm.tm_year = std::stoi(text.substr(0, 4)) - 1900;
        tm.tm_mon = std::stoi(text.substr(5, 2)) - 1;
        tm.tm_mday = std::stoi(text.substr(8, 2));
        tm.tm_hour = std::stoi(text.substr(11, 2));
        tm.tm_min = std::stoi(text.substr(14, 2));
        tm.tm_sec = std::stoi(text.substr(17, 2));
    }
    catch (...) {
        return false;
    }

    out = _mkgmtime(&tm);
    return out != static_cast<std::time_t>(-1);
}

static double DegreesToRadians(double degrees) {
    return degrees * 3.14159265358979323846 / 180.0;
}

static double HaversineMeters(const Coordinate& a, const Coordinate& b) {
    const double earthRadiusMeters = 6371000.0;
    const double lat1 = DegreesToRadians(a.lat);
    const double lat2 = DegreesToRadians(b.lat);
    const double dLat = DegreesToRadians(b.lat - a.lat);
    const double dLon = DegreesToRadians(b.lon - a.lon);
    const double s1 = std::sin(dLat / 2.0);
    const double s2 = std::sin(dLon / 2.0);
    const double h = s1 * s1 + std::cos(lat1) * std::cos(lat2) * s2 * s2;
    return earthRadiusMeters * 2.0 * std::atan2(std::sqrt(h), std::sqrt(1.0 - h));
}

static void DeriveSpeeds(std::vector<Coordinate>& coords) {
    for (size_t i = 1; i < coords.size(); ++i) {
        std::time_t prevTime = 0;
        std::time_t curTime = 0;
        if (!ParseIso8601UtcSeconds(coords[i - 1].time, prevTime) ||
            !ParseIso8601UtcSeconds(coords[i].time, curTime)) {
            continue;
        }

        const double seconds = std::difftime(curTime, prevTime);
        if (seconds <= 0.0) continue;
        coords[i].speed = HaversineMeters(coords[i - 1], coords[i]) / seconds;
        coords[i].has_speed = true;
    }
}

static std::wstring WidenAscii(const std::string& s) {
    return std::wstring(s.begin(), s.end());
}

static std::string BuildElevationLocationsQuery(const std::vector<ElevationTarget>& batch) {
    std::ostringstream oss;
    for (size_t i = 0; i < batch.size(); ++i) {
        if (i != 0) oss << "%7C";
        oss << std::setprecision(17) << batch[i].coord->lat
            << "," << std::setprecision(17) << batch[i].coord->lon;
    }
    return oss.str();
}

static bool HttpGetWinHttp(const std::string& pathAndQuery, std::string& response) {
    response.clear();

    HINTERNET session = WinHttpOpen(L"kml2gpx/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!session) return false;

    HINTERNET connect = WinHttpConnect(session, L"api.opentopodata.org", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        return false;
    }

    const std::wstring path = WidenAscii(pathAndQuery);
    HINTERNET request = WinHttpOpenRequest(connect,
        L"GET",
        path.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    BOOL ok = WinHttpSendRequest(request,
        WINHTTP_NO_ADDITIONAL_HEADERS,
        0,
        WINHTTP_NO_REQUEST_DATA,
        0,
        0,
        0);
    if (ok) ok = WinHttpReceiveResponse(request, nullptr);

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (ok) {
        ok = WinHttpQueryHeaders(request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &status,
            &statusSize,
            WINHTTP_NO_HEADER_INDEX);
    }

    while (ok) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;

        std::string chunk;
        chunk.resize(available);
        DWORD read = 0;
        if (!WinHttpReadData(request, &chunk[0], available, &read)) {
            ok = FALSE;
            break;
        }
        chunk.resize(read);
        response += chunk;
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    return ok && status >= 200 && status < 300;
}

static std::vector<Coordinate*> CollectElevationTargets(ConversionResult& data, bool forceElevation) {
    std::vector<Coordinate*> targets;
    for (Waypoint& w : data.waypoints) {
        if (forceElevation || !w.coord.has_ele) targets.push_back(&w.coord);
    }
    for (Track& t : data.tracks) {
        for (Coordinate& c : t.points) {
            if (forceElevation || !c.has_ele) targets.push_back(&c);
        }
    }
    return targets;
}

static bool HasExistingElevation(const ConversionResult& data) {
    for (const Waypoint& w : data.waypoints) {
        if (w.coord.has_ele) return true;
    }
    for (const Track& t : data.tracks) {
        for (const Coordinate& c : t.points) {
            if (c.has_ele) return true;
        }
    }
    return false;
}

static std::vector<double> ParseElevationValues(const std::string& json) {
    std::vector<double> values;
    size_t pos = 0;
    const std::string key = "\"elevation\"";
    while ((pos = json.find(key, pos)) != std::string::npos) {
        pos = json.find(':', pos + key.size());
        if (pos == std::string::npos) break;
        ++pos;
        while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
        if (json.compare(pos, 4, "null") == 0) {
            values.push_back(HUGE_VAL);
            pos += 4;
            continue;
        }

        char* end = nullptr;
        const double value = std::strtod(json.c_str() + pos, &end);
        if (end == json.c_str() + pos) {
            values.push_back(HUGE_VAL);
        }
        else {
            values.push_back(value);
            pos = static_cast<size_t>(end - json.c_str());
        }
    }
    return values;
}

static size_t FetchElevationBatch(const std::vector<ElevationTarget>& batch,
                                  const std::string& dataset_csv) {
    if (batch.empty()) return 0;

    const std::string path = "/v1/" + dataset_csv + "?locations=" + BuildElevationLocationsQuery(batch);
    std::string response;
    if (!HttpGetWinHttp(path, response)) {
        std::cerr << "[elev] HTTP error contacting OpenTopoData dataset(s) '" << dataset_csv << "'\n";
        return 0;
    }

    const std::vector<double> elevations = ParseElevationValues(response);
    size_t changed = 0;
    for (size_t i = 0; i < batch.size() && i < elevations.size(); ++i) {
        if (elevations[i] == HUGE_VAL) continue;
        batch[i].coord->ele = elevations[i];
        batch[i].coord->has_ele = true;
        ++changed;
    }
    return changed;
}

static size_t FetchElevations(ConversionResult& data, const Args& args) {
    std::vector<Coordinate*> targets = CollectElevationTargets(data, args.forceElevation);
    if (targets.empty()) {
        std::cout << "[elev] No elevation fetch needed\n";
        return 0;
    }

    std::cout << "[elev] Fetching " << targets.size()
        << " elevations via dataset(s) '" << args.dataset_csv << "'\n";
    if (args.forceElevation && HasExistingElevation(data)) {
        std::cout << "[elev] Force enabled: overwriting KML elevations when fetched elevation is available\n";
    }
    else if (args.forceElevation) {
        std::cout << "[elev] Force enabled: fetching elevations for all points; no KML elevations to overwrite\n";
    }

    size_t changed = 0;
    const size_t totalBatches = (targets.size() + kElevationBatchSize - 1) / kElevationBatchSize;
    for (size_t i = 0; i < targets.size(); i += kElevationBatchSize) {
        std::vector<ElevationTarget> batch;
        const size_t end = std::min(i + kElevationBatchSize, targets.size());
        for (size_t j = i; j < end; ++j) {
            ElevationTarget t;
            t.coord = targets[j];
            batch.push_back(t);
        }
        changed += FetchElevationBatch(batch, args.dataset_csv);
        std::cout << "[elev] Batch " << (i / kElevationBatchSize + 1)
            << "/" << totalBatches << " done\n";
    }
    std::cout << "[elev] Applied " << changed << " fetched elevations\n";
    return changed;
}

static std::vector<Coordinate> ParseCoordinatesText(const std::string& text) {
    std::vector<Coordinate> coords;
    std::istringstream iss(text);
    std::string tuple;
    while (iss >> tuple) {
        coords.push_back(ParseCoordinateTuple(tuple));
    }
    if (coords.empty()) {
        throw std::runtime_error("[kml] Empty coordinates element");
    }
    return coords;
}

static std::vector<Coordinate> ParseGeometryCoordinates(const tinyxml2::XMLElement* geometry) {
    const tinyxml2::XMLElement* coordinates = FirstChildLocal(geometry, "coordinates");
    if (!coordinates) {
        throw std::runtime_error(std::string("[kml] Missing coordinates in ") + LocalName(geometry));
    }
    return ParseCoordinatesText(ElementText(coordinates));
}

static std::string PlacemarkTime(const tinyxml2::XMLElement* placemark) {
    const tinyxml2::XMLElement* timeStamp = FirstChildLocal(placemark, "TimeStamp");
    const tinyxml2::XMLElement* when = FirstChildLocal(timeStamp, "when");
    return ElementText(when);
}

static Track ParseGxTrack(const tinyxml2::XMLElement* gxTrack,
                          const std::string& name,
                          const std::string& desc) {
    std::vector<std::string> times;
    std::vector<Coordinate> coords;

    for (const tinyxml2::XMLElement* child = gxTrack->FirstChildElement(); child; child = child->NextSiblingElement()) {
        if (IsElement(child, "when")) {
            times.push_back(ElementText(child));
        }
        else if (IsElement(child, "coord")) {
            coords.push_back(ParseGxCoordText(ElementText(child)));
        }
    }

    if (coords.size() < 2) {
        throw std::runtime_error("[kml] gx:Track must contain at least two gx:coord elements");
    }
    if (!times.empty() && times.size() != coords.size()) {
        throw std::runtime_error("[kml] gx:Track when/gx:coord counts do not match");
    }

    for (size_t i = 0; i < coords.size() && i < times.size(); ++i) {
        coords[i].time = times[i];
    }
    DeriveSpeeds(coords);

    Track trk;
    trk.points = coords;
    trk.name = name;
    trk.desc = desc;
    return trk;
}

static void ConvertPlacemark(const tinyxml2::XMLElement* placemark, ConversionResult& out) {
    const std::string name = ElementText(FirstChildLocal(placemark, "name"));
    const std::string desc = ElementText(FirstChildLocal(placemark, "description"));
    const std::string placemarkTime = PlacemarkTime(placemark);

    std::vector<const tinyxml2::XMLElement*> points;
    std::vector<const tinyxml2::XMLElement*> lineStrings;
    std::vector<const tinyxml2::XMLElement*> gxTracks;
    FindDescendantsLocal(placemark, "Point", points);
    FindDescendantsLocal(placemark, "LineString", lineStrings);
    FindDescendantsLocal(placemark, "Track", gxTracks);

    for (const tinyxml2::XMLElement* point : points) {
        const std::vector<Coordinate> coords = ParseGeometryCoordinates(point);
        if (coords.size() != 1) {
            throw std::runtime_error("[kml] Point geometry must contain exactly one coordinate");
        }
        Waypoint wpt;
        wpt.coord = coords[0];
        wpt.coord.time = placemarkTime;
        wpt.name = name;
        wpt.desc = desc;
        out.waypoints.push_back(wpt);
    }

    for (const tinyxml2::XMLElement* line : lineStrings) {
        const std::vector<Coordinate> coords = ParseGeometryCoordinates(line);
        if (coords.size() < 2) {
            throw std::runtime_error("[kml] LineString geometry must contain at least two coordinates");
        }
        Track trk;
        trk.points = coords;
        trk.name = name;
        trk.desc = desc;
        out.tracks.push_back(trk);
    }

    for (const tinyxml2::XMLElement* gxTrack : gxTracks) {
        out.tracks.push_back(ParseGxTrack(gxTrack, name, desc));
    }
}

static ConversionResult ReadKml(const std::string& inPath) {
    tinyxml2::XMLDocument doc;
    const tinyxml2::XMLError rc = doc.LoadFile(inPath.c_str());
    if (rc != tinyxml2::XML_SUCCESS) {
        std::ostringstream oss;
        oss << "[kml] XML parse/load error code=" << rc;
        throw std::runtime_error(oss.str());
    }

    const tinyxml2::XMLElement* root = doc.RootElement();
    if (!root) {
        throw std::runtime_error("[kml] Empty XML document");
    }

    std::vector<const tinyxml2::XMLElement*> placemarks;
    if (IsElement(root, "Placemark")) placemarks.push_back(root);
    FindDescendantsLocal(root, "Placemark", placemarks);

    ConversionResult result;
    for (const tinyxml2::XMLElement* placemark : placemarks) {
        ConvertPlacemark(placemark, result);
    }

    if (result.waypoints.empty() && result.tracks.empty()) {
        throw std::runtime_error("[kml] No supported Point, LineString, or gx:Track geometries found");
    }
    return result;
}

static void AddTextElement(tinyxml2::XMLDocument& doc,
                           tinyxml2::XMLElement* parent,
                           const char* name,
                           const std::string& value) {
    if (value.empty()) return;
    tinyxml2::XMLElement* elem = doc.NewElement(name);
    elem->SetText(value.c_str());
    parent->InsertEndChild(elem);
}

static void AddEle(tinyxml2::XMLDocument& doc, tinyxml2::XMLElement* parent, const Coordinate& c) {
    if (!c.has_ele) return;
    tinyxml2::XMLElement* ele = doc.NewElement("ele");
    ele->SetText(c.ele);
    parent->InsertEndChild(ele);
}

static void AddTime(tinyxml2::XMLDocument& doc, tinyxml2::XMLElement* parent, const Coordinate& c) {
    AddTextElement(doc, parent, "time", c.time);
}

static void AddSpeedExtension(tinyxml2::XMLDocument& doc, tinyxml2::XMLElement* parent, const Coordinate& c) {
    if (!c.has_speed) return;
    tinyxml2::XMLElement* extensions = doc.NewElement("extensions");
    tinyxml2::XMLElement* speed = doc.NewElement("speed");
    speed->SetText(c.speed);
    extensions->InsertEndChild(speed);
    parent->InsertEndChild(extensions);
}

static bool WriteGpx(const std::string& outPath, const ConversionResult& data) {
    tinyxml2::XMLDocument doc;
    doc.InsertFirstChild(doc.NewDeclaration(R"(xml version="1.0" encoding="UTF-8")"));

    tinyxml2::XMLElement* gpx = doc.NewElement("gpx");
    gpx->SetAttribute("version", "1.1");
    gpx->SetAttribute("creator", "KML to GPX Converter (C++/TinyXML2)");
    gpx->SetAttribute("xmlns", "http://www.topografix.com/GPX/1/1");
    doc.InsertEndChild(gpx);

    for (const Waypoint& w : data.waypoints) {
        tinyxml2::XMLElement* wpt = doc.NewElement("wpt");
        wpt->SetAttribute("lat", w.coord.lat);
        wpt->SetAttribute("lon", w.coord.lon);
        AddEle(doc, wpt, w.coord);
        AddTime(doc, wpt, w.coord);
        AddTextElement(doc, wpt, "name", w.name);
        AddTextElement(doc, wpt, "desc", w.desc);
        gpx->InsertEndChild(wpt);
    }

    for (const Track& t : data.tracks) {
        tinyxml2::XMLElement* trk = doc.NewElement("trk");
        AddTextElement(doc, trk, "name", t.name);
        AddTextElement(doc, trk, "desc", t.desc);

        tinyxml2::XMLElement* seg = doc.NewElement("trkseg");
        for (const Coordinate& c : t.points) {
            tinyxml2::XMLElement* trkpt = doc.NewElement("trkpt");
            trkpt->SetAttribute("lat", c.lat);
            trkpt->SetAttribute("lon", c.lon);
            AddEle(doc, trkpt, c);
            AddTime(doc, trkpt, c);
            AddSpeedExtension(doc, trkpt, c);
            seg->InsertEndChild(trkpt);
        }
        trk->InsertEndChild(seg);
        gpx->InsertEndChild(trk);
    }

    const tinyxml2::XMLError rc = doc.SaveFile(outPath.c_str());
    if (rc != tinyxml2::XML_SUCCESS) {
        std::cerr << "[gpx] SaveFile error code=" << rc << "\n";
        return false;
    }
    return true;
}

static void ShowHelp(const char* exe) {
    std::cout <<
        "KML -> GPX converter\n\n"
        "Usage:\n"
        "  " << exe << " <input.kml> [output.gpx] [options]\n\n"
        "If [output.gpx] is omitted, it will be set automatically to <input>.gpx\n\n"
        "Options:\n"
        "  --do-not-fetch-elevation          Do not call OpenTopoData for missing elevations\n"
        "  --elevation-dataset <name|csv>    Dataset id or comma-separated list (default: srtm90m)\n"
        "  --force                           Fetch elevation for all points; overwrite KML elevation when fetched\n"
        "  -h, --help                        Show this help\n"
        "  -v, --version                     Show the version and exit\n\n"
        "Allowed elevation datasets (OpenTopoData public API):\n"
        "  - srtm90m   -> Global baseline ~90 m / 3 arc-sec SRTM. [Global land roughly between 60N and 56S]\n"
        "  - srtm30m   -> Higher-detail SRTM ~30 m / SRTMGL1 v3. [Near-global land, within SRTM latitude limits]\n"
        "  - aster30m  -> ASTER GDEM ~30 m; detailed but may contain artifacts. [Global land]\n"
        "  - eudem25m  -> EU-DEM ~25 m; balanced and clean DEM for Europe. [Europe / EU / EEA]\n"
        "  - ned10m    -> US NED/3DEP ~10 m; higher detail for U.S. routes. [United States]\n"
        "  - nzdem8m   -> New Zealand 8 m DEM. [New Zealand]\n"
        "  - etopo1    -> Coarse 1 arc-min topography and bathymetry. [Global land and ocean]\n"
        "  - gebco2020 -> Global bathymetry/topography ~15 arc-sec / ~500 m. [Global land and ocean]\n"
        "  - emod2018  -> European marine bathymetry composite. [European seas]\n"
        "  - mapzen    -> Composite terrain tiles. [Global composite]\n"
        "  - bkg200    -> Germany DEM at ~200 m. [Germany]\n"
        "By default, KML altitude is preserved and missing elevations are fetched.\n"
        "With --force, fetched elevation overwrites KML elevation only when the fetch returns a value.\n\n"
        "Supported KML geometry:\n"
        "  Placemark Point, LineString, gx:Track, and MultiGeometry containing supported shapes.\n"
        "  Point TimeStamp is copied to GPX waypoint time. gx:Track when/gx:coord pairs\n"
        "  are copied to GPX track point time/elevation, with speed derived when possible.\n\n"
        "Examples:\n"
        "  " << exe << " route.kml\n"
        "  " << exe << " route.kml route.gpx\n";
}

static bool ParseArgs(int argc, char** argv, Args& a) {
    if (argc < 2) {
        a.showHelp = true;
        return false;
    }

    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];

        if (s == "--version" || s == "-v") {
            std::cout << "kml2gpx 1.0.0\n";
            std::exit(0);
        }
        if (s == "-h" || s == "--help") {
            a.showHelp = true;
            return false;
        }
        if (s == "--do-not-fetch-elevation") {
            a.fetchElevation = false;
            continue;
        }
        if (s == "--elevation-dataset") {
            if ((i + 1) >= argc) {
                std::cerr << "[args] Missing value for --elevation-dataset\n\n";
                a.showHelp = true;
                return false;
            }
            a.dataset_csv = argv[++i];
            if (a.dataset_csv.empty()) {
                std::cerr << "[args] Empty value for --elevation-dataset\n\n";
                a.showHelp = true;
                return false;
            }
            std::string badToken;
            if (!ValidateDatasetsCSV(a.dataset_csv, badToken)) {
                std::cerr << "[args] Unknown elevation dataset: " << badToken << "\n\n";
                PrintAllowedDatasetsDetailed();
                return false;
            }
            continue;
        }
        if (s == "--force") {
            a.forceElevation = true;
            continue;
        }

        if (!s.empty() && s[0] != '-') {
            if (a.kmlFile.empty()) {
                a.kmlFile = s;
            }
            else if (a.gpxFile.empty()) {
                a.gpxFile = s;
            }
            else {
                std::cerr << "[args] Unexpected extra positional argument: " << s << "\n\n";
                a.showHelp = true;
                return false;
            }
            continue;
        }

        std::cerr << "[args] Unknown argument: " << s << "\n\n";
        a.showHelp = true;
        return false;
    }

    if (a.kmlFile.empty()) {
        a.showHelp = true;
        return false;
    }
    if (a.gpxFile.empty()) {
        a.gpxFile = DeriveGpxPath(a.kmlFile);
    }
    if (NormalizePath(a.gpxFile) == NormalizePath(a.kmlFile)) {
        a.gpxFile = a.kmlFile + ".converted.gpx";
    }
    return true;
}

int main(int argc, char** argv) {
    const auto t0 = std::chrono::steady_clock::now();

    Args args;
    if (!ParseArgs(argc, argv, args)) {
        ShowHelp(argv[0]);
        return args.showHelp ? 0 : 1;
    }

    std::cout << "[kml] Reading: " << args.kmlFile << "\n";
    std::cout << "[opts] Elevation fetch: " << (args.fetchElevation ? "enabled" : "disabled") << "\n";
    if (args.fetchElevation) {
        std::cout << "[opts] Elevation dataset(s): " << args.dataset_csv << "\n";
        std::cout << "[opts] Force elevation overwrite: " << (args.forceElevation ? "enabled" : "disabled") << "\n";
    }

    try {
        ConversionResult data = ReadKml(args.kmlFile);
        std::cout << "[kml] Waypoints: " << data.waypoints.size() << "\n";
        std::cout << "[kml] Tracks: " << data.tracks.size() << "\n";

        if (args.fetchElevation) {
            FetchElevations(data, args);
        }

        if (!WriteGpx(args.gpxFile, data)) {
            return 1;
        }
    }
    catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }

    const auto t1 = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(t1 - t0).count();
    std::cout << "[gpx] Saved: " << args.gpxFile << "\n";
    std::cout << std::fixed << std::setprecision(3)
        << "[time] Conversion took " << secs << " s\n";

    return 0;
}
