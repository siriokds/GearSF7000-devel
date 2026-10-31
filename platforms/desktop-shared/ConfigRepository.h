// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026  Saverio Russo
/* GearSF7000 desktop configuration storage.
 * Header-only so all desktop project manifests use the same implementation.
 * No SDL, GUI or core dependency; tests always supply a temporary root.
 */
#pragma once
#ifndef MINI_CASE_SENSITIVE
#define MINI_CASE_SENSITIVE
#endif
#include "mINI/ini.h"
#include <filesystem>
#include <functional>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace config_storage {
namespace fs = std::filesystem;
using Ini = mINI::INIStructure;
inline constexpr int globalVersion = 3;
inline constexpr int previousGlobalVersion = 2;
inline fs::path utf8Path(const std::string& path) {
    return fs::path(std::u8string(path.begin(), path.end()));
}
inline std::string utf8String(const fs::path& path) {
    const auto bytes = path.u8string();
    return std::string(bytes.begin(), bytes.end());
}
struct Family { const char* id; const char* file; int version; };
inline constexpr Family families[] = {
    {"sega-tms", "sega-tms.ini", 1}, {"sms", "sms.ini", 1},
    {"coleco", "coleco.ini", 1}, {"msx", "msx.ini", 1},
    {"sordm5", "sordm5.ini", 1}, {"gamegear", "gamegear.ini", 1}
};
inline constexpr int familyCount = sizeof(families) / sizeof(families[0]);
inline int owner(const std::string& section) {
    if (section == "Machine.SG1000" || section == "Machine.SC3000" ||
        section == "Machine.SK1100" || section == "Machine.SR1000" ||
        section == "Machine.SF7000" || section == "Machine.SegaAudio") return 0;
    if (section == "Machine.SMS") return 1;
    if (section == "Machine.Coleco") return 2;
    if (section == "Machine.MSX" || section == "Machine.MSXTape") return 3;
    if (section == "Machine.SordM5") return 4;
    if (section == "Machine.GameGear") return 5;
    return -1;
}
inline std::string diskSection(const std::string& s) { return s == "Machine.MSX" ? "MSX1" : s; }
inline std::string memorySection(const std::string& s, int f) { return f == 3 && s == "MSX1" ? "Machine.MSX" : s; }
inline std::string encode(const Ini& data) {
    std::string out;
    for (const auto& section : data) {
        out += "[" + section.first + "]\n";
        for (const auto& kv : section.second) {
            std::string key = kv.first;
            mINI::INIStringUtil::replace(key, "=", "\\=");
            out += key + "=" + kv.second + "\n";
        }
        out += "\n";
    }
    return out;
}
// Same key/value semantics as mINI, but reject malformed lines instead of
// silently dropping them. Repeated sections/keys merge, last value wins.
inline Ini decode(std::string bytes) {
    if (bytes.compare(0, 3, "\xef\xbb\xbf") == 0) bytes.erase(0, 3);
    if (bytes.find('\0') != std::string::npos) throw std::runtime_error("NUL in INI");
    Ini data;
    std::istringstream in(bytes);
    std::string line, section;
    while (std::getline(in, line)) {
        mINI::INIStringUtil::trim(line);
        if (!line.empty() && line[0] == '#') continue;
        mINI::INIParser::T_ParseValues pair;
        const auto type = mINI::INIParser::parseLine(line, pair);
        using T = mINI::INIParser::PDataType;
        if (type == T::PDATA_SECTION) {
            const auto close = line.find(']');
            std::string rest = line.substr(close + 1);
            mINI::INIStringUtil::trim(rest);
            if (pair.first.empty() || (!rest.empty() && rest[0] != ';'))
                throw std::runtime_error("invalid INI section");
            section = pair.first;
            data[section];
        } else if (type == T::PDATA_KEYVALUE && !section.empty() && !pair.first.empty())
            data[section][pair.first] = pair.second;
        else if (type != T::PDATA_NONE && type != T::PDATA_COMMENT)
            throw std::runtime_error("invalid INI line: " + line);
    }
    return data;
}
inline int version(const Ini& ini, int fallback) {
    if (!ini.get("Config").has("Version")) return fallback;
    const std::string v = ini.get("Config").get("Version");
    if (v.empty() || v.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error("invalid Config.Version");
    return std::stoi(v);
}
struct Bytes { bool exists = false; std::string value; };
inline Bytes read(const fs::path& path) {
    if (!fs::exists(path)) return {};
    if (!fs::is_regular_file(path)) throw std::runtime_error("not a regular file: " + utf8String(path));
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + utf8String(path));
    Bytes b{true, std::string(std::istreambuf_iterator<char>(in), {})};
    if (in.bad()) throw std::runtime_error("read failed: " + utf8String(path));
    return b;
}
inline std::string fingerprint(const std::string& bytes) {
    // Integrity/change detection, not a cryptographic signature.
    std::uint64_t hash = 14695981039346656037ull;
    for (unsigned char c : bytes) { hash ^= c; hash *= 1099511628211ull; }
    return std::to_string(bytes.size()) + ":" + std::to_string(hash);
}
inline bool equal(const Bytes& a, const Bytes& b) { return a.exists == b.exists && a.value == b.value; }
inline void rawWrite(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot write " + utf8String(path));
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    if (!out) throw std::runtime_error("write failed: " + utf8String(path));
}
// Lock is released by the OS on process exit, including a crash. All writers
// use it; byte comparisons also detect edits by tools which ignore the lock.
class Lock {
#ifdef _WIN32
    HANDLE fd = INVALID_HANDLE_VALUE;
#else
    int fd = -1;
#endif
public:
    explicit Lock(const fs::path& root) {
        fs::create_directories(root);
        const auto path = root / ".config.lock";
#ifdef _WIN32
        fd = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (fd == INVALID_HANDLE_VALUE) throw std::runtime_error("configuration is locked or not writable");
#else
        fd = ::open(path.c_str(), O_CREAT | O_RDWR, 0600);
        if (fd < 0) throw std::runtime_error("cannot open configuration lock");
        if (flock(fd, LOCK_EX | LOCK_NB) != 0) { close(fd); fd = -1; throw std::runtime_error("configuration is locked"); }
#endif
    }
    ~Lock() {
#ifdef _WIN32
        if (fd != INVALID_HANDLE_VALUE) CloseHandle(fd);
#else
        if (fd >= 0) { flock(fd, LOCK_UN); close(fd); }
#endif
    }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
};

class Repository {
    struct Document { fs::path relative; Bytes original; Ini data; };
    fs::path root;
    std::vector<Document> docs;
    bool blocked = false;
    // Optional deterministic failure injection for storage tests.
    std::function<void(const std::string&)> beforeWrite;
    std::function<void(Ini&)> normalize;
    void checkpoint(const std::string& stage) { if (beforeWrite) beforeWrite(stage); }
    void replace(const fs::path& path, const std::string& bytes) {
        operation = "write"; file = utf8String(path);
        checkpoint("write:" + path.filename().string());
        fs::path tmp = path; tmp += ".tmp";
        rawWrite(tmp, bytes);
        if (read(tmp).value != bytes) throw std::runtime_error("temporary file verification failed");
        operation = "replace";
        checkpoint("replace:" + path.filename().string());
#ifdef _WIN32
        if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("cannot replace " + utf8String(path));
#else
        fs::rename(tmp, path);
#endif
    }
    static void validateFamily(const Ini& data, int f) {
        if (version(data, -1) != families[f].version || data.get("Config").get("Family") != families[f].id)
            throw std::runtime_error(std::string("unsupported version or wrong family: ") + families[f].file);
        for (const auto& section : data) {
            const int owned = owner(memorySection(section.first, f));
            if (owned >= 0 && owned != f) throw std::runtime_error("section in wrong family: " + section.first);
        }
    }
    // A typo still present in the last monolithic format (v2).
    static void normalizeV2(Ini& data) {
        const auto tape = data.get("Machine.SR1000");
        for (const auto& kv : tape) if (kv.first.rfind("RecentCassettes", 0) == 0) {
            const std::string fixed = "RecentCassette" + kv.first.substr(std::string("RecentCassettes").size());
            if (!data.get("Machine.SR1000").has(fixed)) data["Machine.SR1000"][fixed] = kv.second;
            data["Machine.SR1000"].remove(kv.first);
        }
    }
    void recover() {
        const auto journal = read(root / ".config-migration-v3.ini");
        if (!journal.exists) return;
        const Ini index = decode(journal.value);
        const auto name = index.get("Migration").get("Backup");
        if (name.rfind("config-v", 0) != 0 || name.find_first_of("/\\.") != std::string::npos)
            throw std::runtime_error("invalid migration journal");
        const fs::path backup = root / "backups" / name;
        const Ini manifest = decode(read(backup / "manifest.ini").value);
        if (version(manifest, -1) != 1) throw std::runtime_error("invalid migration manifest");
        // Older format-3 journals predate M5 and contain four families. Recover
        // exactly those documents; the newly introduced family starts normally.
        const std::string countText = manifest.get("Config").get("FamilyCount");
        if (!countText.empty() && countText.find_first_not_of("0123456789") != std::string::npos)
            throw std::runtime_error("invalid migration family count");
        const int recordedFamilies = countText.empty() ? 4 : std::stoi(countText);
        if (recordedFamilies < 4 || recordedFamilies > familyCount)
            throw std::runtime_error("unsupported migration family count");
        // Verify ALL originals/destinations before publishing any next file.
        std::vector<std::pair<fs::path, Bytes>> pending;
        for (size_t index = 0; index < docs.size(); ++index) {
            if (index >= static_cast<size_t>(recordedFamilies) && index < familyCount) continue;
            const auto& doc = docs[index];
            const auto key = doc.relative.generic_string();
            if (!manifest.get(key).has("Existed")) throw std::runtime_error("incomplete migration manifest");
            Bytes original{manifest.get(key).get("Existed") == "1", {}};
            if (original.exists) {
                original = read(backup / "old" / doc.relative);
                if (!original.exists || fingerprint(original.value) != manifest.get(key).get("Original"))
                    throw std::runtime_error("damaged migration original: " + key);
            }
            const Bytes expected = read(backup / "new" / doc.relative);
            const Bytes current = read(root / doc.relative);
            if (!expected.exists || fingerprint(expected.value) != manifest.get(key).get("Expected"))
                throw std::runtime_error("missing or damaged staged configuration: " + key);
            if (!equal(current, original) && !equal(current, expected))
                throw std::runtime_error("configuration changed during migration: " + key);
            if (!equal(current, expected)) pending.push_back({doc.relative, expected});
        }
        // docs are ordered families first, global LAST: global version commits layout.
        for (const auto& p : pending) replace(root / p.first, p.second.value);
        checkpoint("finish-migration");
        fs::remove(root / ".config-migration-v3.ini");
        if (!notice.empty()) notice += "\n";
        notice += "Configuration migrated to format 3; backup: " + utf8String(backup);
    }
    void backupWrite(const fs::path& path, const std::string& bytes) {
        operation = "backup"; file = utf8String(path);
        checkpoint("backup-write:" + path.filename().string());
        rawWrite(path, bytes);
        if (read(path).value != bytes) throw std::runtime_error("backup verification failed");
    }
    void migrate(Ini data) {
        for (const auto& doc : docs)
            if (!equal(doc.original, read(root / doc.relative)))
                throw std::runtime_error("configuration changed before migration: " + doc.relative.generic_string());
        normalizeV2(data);
        if (normalize) normalize(data);
        std::vector<Ini> output(docs.size());
        for (int f = 0; f < familyCount; ++f) {
            output[f]["Config"]["Version"] = std::to_string(families[f].version);
            output[f]["Config"]["Family"] = families[f].id;
        }
        for (const auto& section : data) {
            const int f = owner(section.first);
            output[f < 0 ? familyCount : f][f < 0 ? section.first : diskSection(section.first)] = section.second;
        }
        output[familyCount]["Config"]["Version"] = std::to_string(globalVersion);
        for (int f = 0; f < familyCount; ++f) if (docs[f].original.exists) {
            validateFamily(docs[f].data, f);
            output[f] = docs[f].data;
        }
        // Backup is complete before its journal becomes visible. Unreferenced
        // directories left by an interrupted backup are harmless, never reused.
        const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
        std::string name = "config-v" + std::to_string(version(data, 1)) + "-" + std::to_string(stamp);
        fs::path backup = root / "backups" / name;
        fs::create_directories(root / "backups");
        while (!fs::create_directory(backup)) { name += "x"; backup = root / "backups" / name; }
        checkpoint("backup");
        Ini manifest;
        manifest["Config"]["Version"] = "1";
        manifest["Config"]["FamilyCount"] = std::to_string(familyCount);
        for (size_t i = 0; i < docs.size(); ++i) {
            const auto& doc = docs[i];
            manifest[doc.relative.generic_string()]["Existed"] = doc.original.exists ? "1" : "0";
            if (doc.original.exists) {
                backupWrite(backup / "old" / doc.relative, doc.original.value);
                manifest[doc.relative.generic_string()]["Original"] = fingerprint(doc.original.value);
            }
            const std::string bytes = i < familyCount && doc.original.exists ? doc.original.value : encode(output[i]);
            decode(bytes);
            manifest[doc.relative.generic_string()]["Expected"] = fingerprint(bytes);
            backupWrite(backup / "new" / doc.relative, bytes);
        }
        backupWrite(backup / "manifest.ini", encode(manifest));
        Ini journal; journal["Migration"]["Backup"] = name;
        replace(root / ".config-migration-v3.ini", encode(journal));
        recover();
    }
    static int documentVersion(const Ini& data) {
        try { return version(data, -1); } catch (...) { return -1; }
    }
    void resetDocument(Document& doc, int family) {
        Lock lock(root);
        if (!equal(doc.original, read(root / doc.relative)))
            throw std::runtime_error("configuration changed before reset: " + doc.relative.generic_string());
        const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
        fs::path backup = root / "backups" / ("config-reset-" + std::to_string(stamp));
        fs::create_directories(root / "backups");
        while (!fs::create_directory(backup)) backup += "x";
        backupWrite(backup / doc.relative, doc.original.value);
        Ini defaults;
        defaults["Config"]["Version"] = std::to_string(family < 0 ? globalVersion : families[family].version);
        if (family >= 0) defaults["Config"]["Family"] = families[family].id;
        const std::string bytes = encode(defaults);
        replace(root / doc.relative, bytes);
        doc.data = defaults; doc.original = {true, bytes};
        if (!notice.empty()) notice += "\n";
        notice += "Configuration version reset to defaults: " + doc.relative.generic_string() +
            "; backup: " + utf8String(backup);
    }

public:
    std::string error, notice, operation, file;
    explicit Repository(fs::path path, std::function<void(const std::string&)> hook = {},
                        std::function<void(Ini&)> normalizer = {})
        : root(std::move(path)), beforeWrite(std::move(hook)), normalize(std::move(normalizer)) {
        for (const auto& family : families) docs.push_back({fs::path("machines") / family.file, {}, {}});
        docs.push_back({"gearconfig.ini", {}, {}});
    }
    bool writable() const { return !blocked; }
    // Load produces the legacy in-memory section names used by config.cpp.
    bool load(Ini& merged) {
        merged.clear(); error.clear(); notice.clear(); blocked = false;
        operation = "load"; file = utf8String(root);
        try {
            if (fs::exists(root / ".config-migration-v3.ini")) { Lock lock(root); recover(); }
            for (auto& doc : docs) { file = utf8String(root / doc.relative); doc.original = read(root / doc.relative); doc.data = decode(doc.original.value); }
            // Only the immediately previous global format is migrated.
            // No config.ini/GearSystem fallback and no unversioned import.
            auto& global = docs[familyCount];
            const int v = documentVersion(global.data);
            if (global.original.exists && v != globalVersion && v != previousGlobalVersion)
                resetDocument(global, -1);
            for (int f = 0; f < familyCount; ++f) {
                if (!docs[f].original.exists) continue;
                file = utf8String(root / docs[f].relative);
                if (documentVersion(docs[f].data) != families[f].version) resetDocument(docs[f], f);
                validateFamily(docs[f].data, f);
            }
            if (global.original.exists && v == previousGlobalVersion) {
                // Keep readable v2 values if publication fails, but block saving.
                merged = global.data; normalizeV2(merged); if (normalize) normalize(merged);
                Lock lock(root);
                migrate(global.data);
                for (auto& doc : docs) { file = utf8String(root / doc.relative); doc.original = read(root / doc.relative); doc.data = decode(doc.original.value); }
            }
            merged = docs[familyCount].data;
            // Never recover machine values from an obsolete monolithic section.
            std::vector<std::string> remove;
            for (const auto& section : merged) if (owner(section.first) >= 0) remove.push_back(section.first);
            for (const auto& s : remove) merged.remove(s);
            for (int f = 0; f < familyCount; ++f) {
                file = utf8String(root / docs[f].relative);
                if (docs[f].original.exists) validateFamily(docs[f].data, f);
                else if (notice.empty()) notice = "Missing family configuration: using defaults";
                for (const auto& section : docs[f].data) {
                    const std::string name = memorySection(section.first, f);
                    if (owner(name) == f) merged[name] = section.second;
                }
            }
            return true;
        } catch (const std::exception& e) { error = e.what(); blocked = true; return false; }
    }
    bool save(const Ini& merged) {
        if (blocked) return false;
        operation = "save"; file = utf8String(root);
        try {
            Lock lock(root);
            // Check all files before any write, including a changed inactive family.
            for (const auto& doc : docs) {
                operation = "check-external-change"; file = utf8String(root / doc.relative);
                if (!equal(doc.original, read(root / doc.relative)))
                    throw std::runtime_error("configuration changed externally: " + doc.relative.generic_string());
            }
            std::vector<Ini> output;
            for (const auto& doc : docs) output.push_back(doc.data);
            output[familyCount] = merged;
            std::vector<std::string> transferred;
            for (const auto& section : merged) {
                const int f = owner(section.first);
                if (f >= 0) { output[f][diskSection(section.first)] = section.second; transferred.push_back(section.first); }
            }
            for (const auto& s : transferred) output[familyCount].remove(s);
            output[familyCount]["Config"]["Version"] = std::to_string(globalVersion);
            for (int f = 0; f < familyCount; ++f) {
                output[f]["Config"]["Version"] = std::to_string(families[f].version);
                output[f]["Config"]["Family"] = families[f].id;
            }
            for (size_t i = 0; i < docs.size(); ++i) {
                auto& doc = docs[i];
                if (doc.original.exists && encode(output[i]) == encode(doc.data)) continue;
                const std::string bytes = encode(output[i]);
                decode(bytes);
                replace(root / doc.relative, bytes);
                doc.original = {true, bytes}; doc.data = output[i];
            }
            error.clear(); return true;
        } catch (const std::exception& e) { error = e.what(); return false; }
    }
};
} // namespace config_storage
