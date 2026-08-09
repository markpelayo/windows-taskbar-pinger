// settings.cpp — INI persistence, profiles, and the small formatting helpers.

#include "settings.h"

#include <windows.h>
// knownfolders.h and shlobj.h must follow windows.h.
#include <knownfolders.h>
#include <objbase.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <string>
#include <vector>

// Deliberately no <fstream> or <sstream>.
//
// Those two headers were, by a wide margin, the largest single contributor to
// this binary: between them they instantiate basic_filebuf, basic_stringbuf,
// both narrow and wide istream/ostream, and two complete sets of locale facets,
// which construct at startup and stay resident. Tens of kilobytes for what this
// file actually needs, which is: read a file, split it on newlines, write a
// file. The code already did its own UTF-8 conversion and its own BOM handling,
// so the streams were contributing nothing but byte transport.

#include "raii.h"

namespace pinger {

// ------------------------------------------------------------ colour helpers

bool ColorFromHex(const std::wstring& hex, COLORREF* out) {
    if (!out) return false;

    std::wstring s = hex;
    // Trim whitespace at both ends.
    const wchar_t* kSpace = L" \t\r\n";
    const size_t first = s.find_first_not_of(kSpace);
    if (first == std::wstring::npos) return false;
    const size_t last = s.find_last_not_of(kSpace);
    s = s.substr(first, last - first + 1);

    if (!s.empty() && s[0] == L'#') s.erase(0, 1);
    if (s.size() != 6) return false;

    for (wchar_t c : s) {
        if (!iswxdigit(c)) return false;
    }

    const unsigned long value = wcstoul(s.c_str(), nullptr, 16);
    *out = RGB((value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF);
    return true;
}

std::wstring ColorToHex(COLORREF color) {
    wchar_t buffer[8];
    swprintf(buffer, 8, L"#%02X%02X%02X", GetRValue(color), GetGValue(color),
             GetBValue(color));
    return buffer;
}

// ------------------------------------------------------------------ sanitise

void MonitorSettings::Sanitise() {
    if (host.empty()) host = defaults::kHost;

    rows     = std::clamp(rows, 1, 16);
    columns  = std::clamp(columns, 1, 64);
    cell     = std::clamp(cell, 1, 32);
    gap      = std::clamp(gap, 0, 8);
    textSize = std::clamp(textSize, 6, 24);

    if (!(interval >= defaults::kMinInterval && interval <= defaults::kMaxInterval)) {
        interval = defaults::kInterval;
    }
}

// ------------------------------------------------------------------ interval

std::wstring FormatInterval(double seconds) {
    wchar_t buffer[32];

    if (seconds == static_cast<double>(static_cast<long long>(seconds))) {
        swprintf(buffer, 32, L"%lld s", static_cast<long long>(seconds));
        return buffer;
    }

    swprintf(buffer, 32, L"%.2f", seconds);
    std::wstring text = buffer;
    while (!text.empty() && text.back() == L'0') text.pop_back();
    if (!text.empty() && text.back() == L'.') text.pop_back();
    return text + L" s";
}

// ------------------------------------------------------------ natural compare

bool NaturalLess(const std::wstring& a, const std::wstring& b) {
    size_t i = 0, j = 0;

    while (i < a.size() && j < b.size()) {
        if (iswdigit(a[i]) && iswdigit(b[j])) {
            size_t startA = i, startB = j;
            while (i < a.size() && iswdigit(a[i])) ++i;
            while (j < b.size() && iswdigit(b[j])) ++j;

            std::wstring numA = a.substr(startA, i - startA);
            std::wstring numB = b.substr(startB, j - startB);

            // Strip leading zeros so "007" and "7" compare equal in magnitude.
            numA.erase(0, std::min(numA.find_first_not_of(L'0'), numA.size() - 1));
            numB.erase(0, std::min(numB.find_first_not_of(L'0'), numB.size() - 1));

            if (numA.size() != numB.size()) return numA.size() < numB.size();
            if (numA != numB) return numA < numB;
        } else {
            const wchar_t ca = towupper(a[i]);
            const wchar_t cb = towupper(b[j]);
            if (ca != cb) return ca < cb;
            ++i;
            ++j;
        }
    }

    return (a.size() - i) < (b.size() - j);
}

// ------------------------------------------------------------------- INI I/O

namespace {

// One parsed INI section: its name and its key/value pairs, in file order.
struct IniSection {
    std::wstring name;
    std::vector<std::pair<std::wstring, std::wstring>> values;

    const std::wstring* Find(const std::wstring& key) const {
        for (const auto& pair : values) {
            if (pair.first == key) return &pair.second;
        }
        return nullptr;
    }
};

std::wstring TrimCopy(const std::wstring& text) {
    const wchar_t* kSpace = L" \t\r\n";
    const size_t first = text.find_first_not_of(kSpace);
    if (first == std::wstring::npos) return L"";
    const size_t last = text.find_last_not_of(kSpace);
    return text.substr(first, last - first + 1);
}

// Deliberately forgiving: unknown keys, blank lines, `;` and `#` comments and
// stray text are all skipped rather than treated as errors.
std::vector<IniSection> ParseIni(const std::wstring& text) {
    std::vector<IniSection> sections;

    // Hand-rolled line split. TrimCopy already strips the trailing \r, so CRLF
    // and LF files both work without a second pass.
    //
    // `done` is set before the body runs, so the `continue`s below still
    // terminate the loop — testing for the last line at the bottom would be
    // skipped by every one of them.
    size_t start = 0;
    bool done = false;

    while (!done) {
        size_t end = text.find(L'\n', start);
        if (end == std::wstring::npos) {
            end = text.size();
            done = true;
        }

        const std::wstring line = text.substr(start, end - start);
        start = end + 1;

        const std::wstring trimmed = TrimCopy(line);
        if (trimmed.empty() || trimmed[0] == L';' || trimmed[0] == L'#') continue;

        if (trimmed.front() == L'[' && trimmed.back() == L']') {
            IniSection section;
            section.name = trimmed.substr(1, trimmed.size() - 2);
            sections.push_back(std::move(section));
            continue;
        }

        const size_t equals = trimmed.find(L'=');
        if (equals == std::wstring::npos || sections.empty()) continue;

        sections.back().values.emplace_back(TrimCopy(trimmed.substr(0, equals)),
                                            TrimCopy(trimmed.substr(equals + 1)));
    }

    return sections;
}

int ReadInt(const IniSection& section, const wchar_t* key, int fallback) {
    const std::wstring* value = section.Find(key);
    if (!value || value->empty()) return fallback;
    return static_cast<int>(wcstol(value->c_str(), nullptr, 10));
}

double ReadDouble(const IniSection& section, const wchar_t* key, double fallback) {
    const std::wstring* value = section.Find(key);
    if (!value || value->empty()) return fallback;
    return wcstod(value->c_str(), nullptr);
}

bool ReadBool(const IniSection& section, const wchar_t* key, bool fallback) {
    const std::wstring* value = section.Find(key);
    if (!value || value->empty()) return fallback;
    return *value == L"1" || *value == L"true" || *value == L"yes";
}

std::wstring ReadString(const IniSection& section, const wchar_t* key,
                        const std::wstring& fallback) {
    const std::wstring* value = section.Find(key);
    return (value && !value->empty()) ? *value : fallback;
}

// The settings file wants a round-trippable number, not the menu's "1 s".
std::wstring FormatIntervalRaw(double seconds) {
    wchar_t buffer[32];
    swprintf(buffer, 32, L"%g", seconds);
    return buffer;
}

MonitorSettings ReadSettings(const IniSection& section) {
    MonitorSettings settings;
    settings.host = ReadString(section, L"host", defaults::kHost);

    COLORREF color = 0;
    if (ColorFromHex(ReadString(section, L"success", defaults::kSuccessHex), &color)) {
        settings.success = color;
    }
    if (ColorFromHex(ReadString(section, L"failure", defaults::kFailureHex), &color)) {
        settings.failure = color;
    }

    settings.rows        = ReadInt(section, L"rows", defaults::kRows);
    settings.columns     = ReadInt(section, L"columns", defaults::kColumns);
    settings.cell        = ReadInt(section, L"cell", defaults::kCell);
    settings.gap         = ReadInt(section, L"gap", defaults::kGap);
    settings.interval    = ReadDouble(section, L"interval", defaults::kInterval);
    settings.showLatency = ReadBool(section, L"showLatency", defaults::kShowLatency);
    settings.textSize    = ReadInt(section, L"textSize", defaults::kTextSize);
    settings.fillVertical = ReadBool(section, L"fillVertical", defaults::kFillVertical);

    settings.Sanitise();
    return settings;
}

// Appends "key=value\n". The integer form goes through swprintf rather than a
// stream inserter, which is what let the iostream dependency go.
void AppendKey(std::wstring& out, const wchar_t* key, const std::wstring& value) {
    out += key;
    out += L'=';
    out += value;
    out += L'\n';
}

void AppendKey(std::wstring& out, const wchar_t* key, int value) {
    wchar_t buffer[16];
    swprintf(buffer, 16, L"%d", value);
    AppendKey(out, key, buffer);
}

void WriteSettings(std::wstring& out, const MonitorSettings& settings) {
    AppendKey(out, L"host", settings.host);
    AppendKey(out, L"success", ColorToHex(settings.success));
    AppendKey(out, L"failure", ColorToHex(settings.failure));
    AppendKey(out, L"rows", settings.rows);
    AppendKey(out, L"columns", settings.columns);
    AppendKey(out, L"cell", settings.cell);
    AppendKey(out, L"gap", settings.gap);
    AppendKey(out, L"interval", FormatIntervalRaw(settings.interval));
    AppendKey(out, L"showLatency", settings.showLatency ? L"1" : L"0");
    AppendKey(out, L"textSize", settings.textSize);
    AppendKey(out, L"fillVertical", settings.fillVertical ? L"1" : L"0");
}

// Reads a whole file into a byte string. Returns false if it cannot be opened;
// an empty file reads as success with empty contents.
bool ReadWholeFile(const std::wstring& path, std::string* out) {
    if (!out) return false;
    out->clear();

    ScopedHandle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) return false;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size)) return false;

    // A settings file this large is not ours. Refuse rather than allocate it.
    if (size.QuadPart <= 0 || size.QuadPart > 4 * 1024 * 1024) return size.QuadPart == 0;

    out->resize(static_cast<size_t>(size.QuadPart));

    DWORD read = 0;
    if (!ReadFile(file.get(), out->data(), static_cast<DWORD>(out->size()), &read,
                  nullptr)) {
        out->clear();
        return false;
    }

    out->resize(read);
    return true;
}

bool WriteWholeFile(const std::wstring& path, const std::string& bytes) {
    ScopedHandle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) return false;

    if (bytes.empty()) return true;

    DWORD written = 0;
    if (!WriteFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                   nullptr)) {
        return false;
    }

    return written == bytes.size();
}

std::wstring DirectoryPath() {
    PWSTR roaming = nullptr;
    std::wstring path;

    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming))) {
        path = roaming;
        CoTaskMemFree(roaming);
    }

    if (path.empty()) return L"";
    return path + L"\\Pinger";
}

std::wstring GenerateId() {
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) {
        // Good enough as a fallback: this only needs to be unique among at most
        // eight monitors in one settings file.
        wchar_t buffer[32];
        swprintf(buffer, 32, L"m%lu", GetTickCount());
        return buffer;
    }

    wchar_t buffer[40];
    swprintf(buffer, 40, L"%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
             guid.Data1, guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1],
             guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5],
             guid.Data4[6], guid.Data4[7]);
    return buffer;
}

bool g_loaded = false;
SettingsDocument g_document;

}  // namespace

namespace store {

std::wstring FilePath() {
    const std::wstring directory = DirectoryPath();
    if (directory.empty()) return L"";
    return directory + L"\\settings.ini";
}

SettingsDocument& Document() {
    if (g_loaded) return g_document;
    g_loaded = true;

    const std::wstring path = FilePath();
    if (path.empty()) return g_document;

    // Read as UTF-8 and widen; hosts can be IDNs and profile names are free text.
    std::string utf8;
    if (!ReadWholeFile(path, &utf8)) return g_document;

    // Skip a UTF-8 BOM if a text editor added one.
    if (utf8.size() >= 3 && static_cast<unsigned char>(utf8[0]) == 0xEF &&
        static_cast<unsigned char>(utf8[1]) == 0xBB &&
        static_cast<unsigned char>(utf8[2]) == 0xBF) {
        utf8.erase(0, 3);
    }

    if (utf8.empty()) return g_document;

    const int wide = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                         static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring text(static_cast<size_t>(wide), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
                        text.data(), wide);

    for (const IniSection& section : ParseIni(text)) {
        if (section.name.rfind(L"monitor:", 0) == 0) {
            MonitorRecord record;
            record.id = section.name.substr(8);
            record.settings = ReadSettings(section);
            if (!record.id.empty()) g_document.monitors.push_back(std::move(record));
        } else if (section.name == L"widget") {
            g_document.placement.manual = ReadBool(section, L"manualPosition", false);
            g_document.placement.offsetFromRight =
                ReadInt(section, L"offsetFromRight", 0);
        } else if (section.name.rfind(L"profile:", 0) == 0) {
            const std::wstring name = ReadString(section, L"name", L"");
            if (!name.empty()) {
                g_document.profiles.emplace_back(name, ReadSettings(section));
            }
        }
    }

    return g_document;
}

void Save() {
    if (!g_loaded) return;

    const std::wstring directory = DirectoryPath();
    const std::wstring path = FilePath();
    if (directory.empty() || path.empty()) return;

    SHCreateDirectoryExW(nullptr, directory.c_str(), nullptr);

    std::wstring text;
    // Roughly what a typical file comes to, so the appends below rarely realloc.
    text.reserve(1024);

    text += L"; ";
    text += defaults::kProjectName;
    text += L" settings\n; Edited by hand at your own risk; unknown keys are ignored.\n\n";

    text += L"[widget]\n";
    AppendKey(text, L"manualPosition", g_document.placement.manual ? L"1" : L"0");
    AppendKey(text, L"offsetFromRight", g_document.placement.offsetFromRight);
    text += L'\n';

    for (const MonitorRecord& record : g_document.monitors) {
        text += L"[monitor:";
        text += record.id;
        text += L"]\n";
        WriteSettings(text, record.settings);
        text += L'\n';
    }

    int index = 0;
    for (const auto& profile : g_document.profiles) {
        wchar_t header[32];
        swprintf(header, 32, L"[profile:%d]\n", index++);
        text += header;
        AppendKey(text, L"name", profile.first);
        WriteSettings(text, profile.second);
        text += L'\n';
    }

    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                          static_cast<int>(text.size()), nullptr, 0,
                                          nullptr, nullptr);
    std::string utf8(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                        utf8.data(), bytes, nullptr, nullptr);

    // Write to a temp file and swap, so an interrupted write cannot truncate
    // the real settings file.
    const std::wstring temp = path + L".tmp";
    if (!WriteWholeFile(temp, utf8)) return;

    MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

std::vector<MonitorRecord> LoadMonitors() {
    SettingsDocument& document = Document();

    if (document.monitors.empty()) {
        MonitorRecord first;
        first.id = GenerateId();
        document.monitors.push_back(first);
        Save();
    }

    return document.monitors;
}

void PersistMonitors(const std::vector<MonitorRecord>& monitors) {
    Document().monitors = monitors;
    Save();
}

void PersistPlacement(const WidgetPlacement& placement) {
    Document().placement = placement;
    Save();
}

}  // namespace store

namespace profiles {

std::vector<std::wstring> Names() {
    std::vector<std::wstring> names;
    for (const auto& profile : store::Document().profiles) {
        names.push_back(profile.first);
    }
    std::sort(names.begin(), names.end(), NaturalLess);
    return names;
}

bool Snapshot(const std::wstring& name, MonitorSettings* out) {
    if (!out) return false;
    for (const auto& profile : store::Document().profiles) {
        if (profile.first == name) {
            *out = profile.second;
            return true;
        }
    }
    return false;
}

void Save(const std::wstring& name, const MonitorSettings& settings) {
    auto& list = store::Document().profiles;
    for (auto& profile : list) {
        if (profile.first == name) {
            profile.second = settings;
            store::Save();
            return;
        }
    }
    list.emplace_back(name, settings);
    store::Save();
}

void Delete(const std::wstring& name) {
    auto& list = store::Document().profiles;
    for (auto it = list.begin(); it != list.end(); ++it) {
        if (it->first == name) {
            list.erase(it);
            store::Save();
            return;
        }
    }
}

}  // namespace profiles

}  // namespace pinger
