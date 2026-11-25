#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
using IsoHandle = HANDLE;
#else
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#ifndef FILE_ATTRIBUTE_READONLY
#define FILE_ATTRIBUTE_READONLY  0x00000001u
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010u
#define FILE_ATTRIBUTE_NORMAL    0x00000080u
#endif
using IsoHandle = int;
#endif
#include "xiso_vfs.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>

namespace fs = std::filesystem;

namespace {
constexpr uint64_t XISO_SECTOR_SIZE = 2048;
constexpr uint64_t XISO_HEADER_OFFSET = 32ull * XISO_SECTOR_SIZE; // 0x10000
constexpr uint64_t kCandidatePartBases[] = {0ull, 0x18300000ull, 0x0FD90000ull};

std::mutex g_iso_mtx;
std::unordered_map<std::string, size_t> g_path_map;

std::string to_lower_ascii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string to_lower_bs(std::string s) {
    for (char& c : s) {
        if (c == '/') c = '\\';
        else c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    while (!s.empty() && s.front() == '\\') s.erase(s.begin());
    while (!s.empty() && s.back() == '\\') s.pop_back();
    return s;
}

bool wildcard_match_ci(const char* pat, const char* str) {
    if (!pat || !*pat || std::strcmp(pat, "*.*") == 0 || std::strcmp(pat, "*") == 0) return true;
    const char* s = str;
    const char* p = pat;
    const char* star = nullptr;
    const char* ss = s;
    while (*s) {
        if (std::tolower(static_cast<unsigned char>(*p)) == std::tolower(static_cast<unsigned char>(*s)) || *p == '?') {
            ++s; ++p;
        } else if (*p == '*') {
            star = p++;
            ss = s;
        } else if (star) {
            p = star + 1;
            s = ++ss;
        } else {
            return false;
        }
    }
    while (*p == '*') ++p;
    return *p == '\0';
}

IsoHandle open_iso_ro(const std::string& p) {
#ifdef _WIN32
    return CreateFileA(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
#else
    return ::open(p.c_str(), O_RDONLY);
#endif
}

bool valid_iso_handle(IsoHandle h) {
#ifdef _WIN32
    return h != INVALID_HANDLE_VALUE && h != nullptr;
#else
    return h >= 0;
#endif
}

void close_iso_handle(IsoHandle h) {
#ifdef _WIN32
    if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
#else
    if (h >= 0) ::close(h);
#endif
}

bool read_at(IsoHandle h, uint64_t off, void* dst, uint32_t size) {
#ifdef _WIN32
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(off & 0xFFFFFFFFu);
    ov.OffsetHigh = static_cast<DWORD>(off >> 32);
    DWORD rd = 0;
    if (!ReadFile(h, dst, size, &rd, &ov) || rd != size) return false;
    return true;
#else
    uint8_t* p = static_cast<uint8_t*>(dst);
    uint32_t total = 0;
    while (total < size) {
        ssize_t r = pread(h, p + total, size - total, static_cast<off_t>(off + total));
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (r == 0) return false;
        total += static_cast<uint32_t>(r);
    }
    return true;
#endif
}

bool detect_xdvdfs_base(IsoHandle h, uint64_t& out_base, uint32_t& out_root_sec, uint32_t& out_root_sz, std::string& out_kind) {
    uint8_t hdr[2048]{};
    for (uint64_t base : kCandidatePartBases) {
        if (read_at(h, base + XISO_HEADER_OFFSET, hdr, sizeof(hdr)) &&
            std::memcmp(hdr, "MICROSOFT*XBOX*MEDIA", 20) == 0) {
            std::memcpy(&out_root_sec, hdr + 20, 4);
            std::memcpy(&out_root_sz, hdr + 24, 4);
            out_base = base;
            out_kind = (base == 0ull) ? "Xbox XISO" : "Redump DVD (XGD1)";
            return true;
        }
    }
    return false;
}

void parse_dir_table(IsoHandle h, uint64_t base_off, uint32_t sec, uint32_t sz, const std::string& parent_norm,
                     std::vector<XisoEntry>& out_entries) {
    if (!sec || !sz || sz > 32 * 1024 * 1024) return;
    std::vector<uint8_t> buf(sz);
    if (!read_at(h, base_off + static_cast<uint64_t>(sec) * XISO_SECTOR_SIZE, buf.data(), sz)) return;

    std::vector<uint32_t> stack;
    stack.push_back(0);
    while (!stack.empty()) {
        uint32_t off = stack.back();
        stack.pop_back();
        if (off == 0xFFFFu || off * 4u + 14u > sz) continue;
        const uint8_t* p = buf.data() + off * 4u;
        uint16_t left = 0, right = 0;
        uint32_t fsec = 0, fsz = 0;
        std::memcpy(&left, p + 0, 2);
        std::memcpy(&right, p + 2, 2);
        std::memcpy(&fsec, p + 4, 4);
        std::memcpy(&fsz, p + 8, 4);
        uint8_t attr = p[12];
        uint8_t nlen = p[13];
        if (off * 4u + 14u + nlen > sz) continue;
        std::string name(reinterpret_cast<const char*>(p + 14), nlen);
        std::string child_norm = parent_norm.empty() ? to_lower_bs(name) : (parent_norm + "\\" + to_lower_bs(name));

        XisoEntry e;
        e.norm_path = child_norm;
        e.name = name;
        e.parent = parent_norm;
        e.iso_offset = base_off + static_cast<uint64_t>(fsec) * XISO_SECTOR_SIZE;
        e.size = fsz;
        e.is_dir = (attr & 0x10) != 0;
        e.attrs = e.is_dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_READONLY;
        out_entries.push_back(std::move(e));

        if (e.is_dir) {
            parse_dir_table(h, base_off, fsec, fsz, child_norm, out_entries);
        }
        if (left != 0xFFFFu && left != 0) stack.push_back(left);
        if (right != 0xFFFFu && right != 0) stack.push_back(right);
    }
}

bool find_xbe_in_xdvdfs(IsoHandle h, uint64_t base_off, uint32_t sec, uint32_t sz, uint64_t& out_off, uint32_t& out_sz) {
    if (!sec || !sz || sz > 8 * 1024 * 1024) return false;
    std::vector<uint8_t> buf(sz);
    if (!read_at(h, base_off + static_cast<uint64_t>(sec) * XISO_SECTOR_SIZE, buf.data(), sz)) return false;

    std::vector<uint32_t> stack;
    stack.push_back(0);
    while (!stack.empty()) {
        uint32_t off = stack.back();
        stack.pop_back();
        if (off == 0xFFFFu || off * 4u + 14u > sz) continue;
        const uint8_t* p = buf.data() + off * 4u;
        uint16_t left = 0, right = 0;
        uint32_t fsec = 0, fsz = 0;
        std::memcpy(&left, p + 0, 2);
        std::memcpy(&right, p + 2, 2);
        std::memcpy(&fsec, p + 4, 4);
        std::memcpy(&fsz, p + 8, 4);
        uint8_t attr = p[12];
        uint8_t nlen = p[13];
        if (off * 4u + 14u + nlen > sz) continue;
        std::string name(reinterpret_cast<const char*>(p + 14), nlen);
        if (!(attr & 0x10) && to_lower_ascii(name) == "default.xbe") {
            out_off = base_off + static_cast<uint64_t>(fsec) * XISO_SECTOR_SIZE;
            out_sz = fsz;
            return true;
        }
        if (left != 0xFFFFu && left != 0) stack.push_back(left);
        if (right != 0xFFFFu && right != 0) stack.push_back(right);
    }
    return false;
}

void parse_xbe_identity(const uint8_t* xbe, size_t len, XisoProbeResult& out) {
    if (len < 0x180 || std::memcmp(xbe, "XBEH", 4) != 0) {
        out.message = "default.xbe is missing a valid Xbox XBEH header.";
        return;
    }
    out.has_xbe = true;
    uint32_t base_addr = 0, cert_va = 0;
    std::memcpy(&base_addr, xbe + 0x104, 4);
    std::memcpy(&cert_va, xbe + 0x118, 4);
    if (cert_va >= base_addr) {
        uint32_t coff = cert_va - base_addr;
        if (static_cast<size_t>(coff) + 0x60 <= len) {
            std::memcpy(&out.title_id, xbe + coff + 8, 4);
            std::string title;
            for (int i = 0; i < 40; ++i) {
                uint16_t wc = 0;
                std::memcpy(&wc, xbe + coff + 12 + i * 2, 2);
                if (wc == 0) break;
                if (wc >= 0x20 && wc <= 0x7E) title.push_back(static_cast<char>(wc));
            }
            out.title_name = title;
        }
    }
    out.is_whacked = (out.title_id == 0x4D530027u);
    if (out.is_whacked) {
        out.message = "Verified " + (out.title_name.empty() ? "Whacked!" : out.title_name) + " (TitleID 0x4D530027) via " + out.format_kind;
    } else {
        char buf[192]{};
        std::snprintf(buf, sizeof(buf), "Wrong Xbox game: '%s' (TitleID 0x%08X, expected 0x4D530027 for Whacked!).",
                      out.title_name.empty() ? "Unknown" : out.title_name.c_str(), out.title_id);
        out.message = buf;
    }
}

void collect_folder_entries(const fs::path& root, const fs::path& cur, const std::string& parent_norm,
                            std::vector<XisoEntry>& out) {
    std::error_code ec;
    for (const auto& de : fs::directory_iterator(cur, ec)) {
        std::string name = de.path().filename().string();
        std::string norm = parent_norm.empty() ? to_lower_bs(name) : (parent_norm + "\\" + to_lower_bs(name));
        XisoEntry e{};
        e.name = name;
        e.norm_path = norm;
        e.parent = parent_norm;
        e.is_dir = de.is_directory(ec);
        e.attrs = e.is_dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_READONLY;
        if (!e.is_dir) {
            uintmax_t fsz = de.file_size(ec);
            e.size = ec ? 0u : static_cast<uint32_t>(std::min<uintmax_t>(fsz, 0xFFFFFFFFull));
            e.host_path = de.path().string();
        }
        out.push_back(e);
        if (e.is_dir) {
            collect_folder_entries(root, de.path(), norm, out);
        }
    }
}

std::string normalize_source_input(const std::string& path) {
    std::error_code ec;
    fs::path p(path);
    if (fs::is_regular_file(p, ec) && to_lower_ascii(p.filename().string()) == "default.xbe") {
        return p.parent_path().string();
    }
    return path;
}
} // namespace

XisoProbeResult xiso_probe_game(const std::string& raw_path) {
    XisoProbeResult res{};
    if (raw_path.empty()) {
        res.message = "No ISO or game folder path provided.";
        return res;
    }
    std::string path = normalize_source_input(raw_path);
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        res.message = "Path does not exist: " + path;
        return res;
    }
    res.readable = true;

    if (fs::is_directory(path, ec)) {
        res.format_kind = "Extracted Folder";
        fs::path xbe_path;
        for (const auto& de : fs::directory_iterator(path, ec)) {
            if (!de.is_directory(ec) && to_lower_ascii(de.path().filename().string()) == "default.xbe") {
                xbe_path = de.path();
                break;
            }
        }
        if (xbe_path.empty()) {
            res.message = "Folder does not contain default.xbe.";
            return res;
        }
        res.is_xbox_media = true;
        std::ifstream f(xbe_path, std::ios::binary);
        std::vector<uint8_t> hdr(16384);
        f.read(reinterpret_cast<char*>(hdr.data()), static_cast<std::streamsize>(hdr.size()));
        size_t got = static_cast<size_t>(f.gcount());
        hdr.resize(got);
        parse_xbe_identity(hdr.data(), hdr.size(), res);
        return res;
    }

    IsoHandle h = open_iso_ro(path);
    if (!valid_iso_handle(h)) {
        res.message = "Could not open image file.";
        return res;
    }
    uint64_t base_off = 0;
    uint32_t root_sec = 0, root_sz = 0;
    if (!detect_xdvdfs_base(h, base_off, root_sec, root_sz, res.format_kind)) {
        close_iso_handle(h);
        res.message = "Not an Original Xbox XDVDFS ISO (supported: XISO and Redump XGD1 DVD).";
        return res;
    }
    res.is_xbox_media = true;

    uint64_t xbe_off = 0;
    uint32_t xbe_sz = 0;
    if (!find_xbe_in_xdvdfs(h, base_off, root_sec, root_sz, xbe_off, xbe_sz)) {
        close_iso_handle(h);
        res.message = "default.xbe not found inside Xbox disc image.";
        return res;
    }
    std::vector<uint8_t> xbe_hdr(std::min<uint32_t>(xbe_sz, 16384u));
    if (!read_at(h, xbe_off, xbe_hdr.data(), static_cast<uint32_t>(xbe_hdr.size()))) {
        close_iso_handle(h);
        res.message = "Failed reading default.xbe header from ISO.";
        return res;
    }
    close_iso_handle(h);
    parse_xbe_identity(xbe_hdr.data(), xbe_hdr.size(), res);
    return res;
}

std::vector<std::string> xiso_auto_discover(const std::string& extra_dir) {
    std::vector<std::string> found;
    std::vector<fs::path> search_dirs;
    auto push_dir = [&](const fs::path& p) {
        std::error_code ec;
        if (!p.empty() && fs::is_directory(p, ec)) {
            for (const auto& existing : search_dirs) {
                if (existing == p) return;
            }
            search_dirs.push_back(p);
        }
    };

    if (!extra_dir.empty()) push_dir(fs::path(extra_dir));
    std::error_code ec;
    fs::path cwd = fs::current_path(ec);
    push_dir(cwd);
    push_dir(cwd.parent_path());
    push_dir(cwd.parent_path().parent_path());

#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    if (home && home[0]) {
        fs::path hp(home);
        push_dir(hp / "Games");
        push_dir(hp / "Games" / "Xbox");
        push_dir(hp / "Downloads");
        push_dir(hp / "Desktop");
        push_dir(hp / "Documents");
    }

    for (const auto& dir : search_dirs) {
        for (const auto& de : fs::directory_iterator(dir, ec)) {
            std::string fn = to_lower_ascii(de.path().filename().string());
            bool candidate = false;
            if (de.is_regular_file(ec)) {
                if (fn.ends_with(".xiso.iso") || fn.ends_with(".iso") || fn.ends_with(".xiso")) {
                    candidate = true;
                }
            } else if (de.is_directory(ec) && fn.find("whacked") != std::string::npos) {
                candidate = true;
            }
            if (candidate && xiso_probe_game(de.path().string()).is_whacked) {
                found.push_back(de.path().string());
            }
        }
    }
    return found;
}

XisoVfs& XisoVfs::instance() {
    static XisoVfs inst;
    return inst;
}

bool XisoVfs::mount_iso(const std::string& raw_path, const std::string& userdata_root) {
    unmount();
    std::lock_guard<std::mutex> lk(g_iso_mtx);

    userdata_root_ = userdata_root;
    std::error_code ec;
    fs::create_directories(fs::path(userdata_root_) / "TDATA", ec);
    fs::create_directories(fs::path(userdata_root_) / "UDATA", ec);
    fs::create_directories(fs::path(userdata_root_) / "Z_cache", ec);

    std::string iso_path = normalize_source_input(raw_path);
    if (fs::is_directory(iso_path, ec)) {
        entries_.clear();
        g_path_map.clear();
        collect_folder_entries(iso_path, iso_path, "", entries_);
        for (size_t i = 0; i < entries_.size(); ++i) {
            g_path_map[entries_[i].norm_path] = i;
        }
        if (g_path_map.find("default.xbe") == g_path_map.end()) {
            std::fprintf(stderr, "[XISO] Folder missing default.xbe: %s\n", iso_path.c_str());
            entries_.clear();
            g_path_map.clear();
            return false;
        }
        folder_mode_ = true;
        mounted_ = true;
        mounted_path_ = iso_path;
        mounted_format_ = "Extracted Folder";
        std::printf("[XISO] Mounted extracted folder %s (%zu entries)\n", iso_path.c_str(), entries_.size());
        return true;
    }

    IsoHandle h = open_iso_ro(iso_path);
    if (!valid_iso_handle(h)) {
        std::fprintf(stderr, "[XISO] Could not open ISO: %s\n", iso_path.c_str());
        return false;
    }

    uint64_t base_off = 0;
    uint32_t root_sec = 0, root_sz = 0;
    std::string kind;
    if (!detect_xdvdfs_base(h, base_off, root_sec, root_sz, kind)) {
        std::fprintf(stderr, "[XISO] Invalid XDVDFS header in %s\n", iso_path.c_str());
        close_iso_handle(h);
        return false;
    }

    entries_.clear();
    g_path_map.clear();
    iso_base_offset_ = base_off;
    parse_dir_table(h, base_off, root_sec, root_sz, "", entries_);
    for (size_t i = 0; i < entries_.size(); ++i) {
        g_path_map[entries_[i].norm_path] = i;
    }

#ifdef _WIN32
    file_handle_ = h;
#else
    iso_fd_ = h;
#endif
    folder_mode_ = false;
    mounted_ = true;
    mounted_path_ = iso_path;
    mounted_format_ = kind;
    std::printf("[XISO] Mounted %s [%s] (%zu entries)\n", iso_path.c_str(), kind.c_str(), entries_.size());
    return true;
}

void XisoVfs::unmount() {
    std::lock_guard<std::mutex> lk(g_iso_mtx);
#ifdef _WIN32
    if (file_handle_) {
        CloseHandle(static_cast<HANDLE>(file_handle_));
        file_handle_ = nullptr;
    }
#else
    if (iso_fd_ >= 0) {
        ::close(iso_fd_);
        iso_fd_ = -1;
    }
#endif
    entries_.clear();
    g_path_map.clear();
    mounted_ = false;
    folder_mode_ = false;
    iso_base_offset_ = 0;
    mounted_path_.clear();
    mounted_format_.clear();
}

bool XisoVfs::resolve_path(const std::string& raw_path, std::string& out_dev, std::string& out_rel) const {
    if (raw_path.empty()) return false;
    std::string p = raw_path;
    for (char& c : p) if (c == '/') c = '\\';

    auto istarts = [](const std::string& s, const char* pre) {
        size_t n = std::strlen(pre);
        if (s.size() < n) return false;
        for (size_t i = 0; i < n; ++i) {
            if (std::tolower(static_cast<unsigned char>(s[i])) !=
                std::tolower(static_cast<unsigned char>(pre[i]))) return false;
        }
        return true;
    };

    if (istarts(p, "\\??\\")) p = p.substr(4);
    if (istarts(p, "\\device\\cdrom0")) {
        p = p.substr(14);
        out_dev = "iso";
        out_rel = to_lower_bs(p);
        return true;
    }
    if (istarts(p, "\\device\\harddisk0\\partition")) {
        out_dev = "Z";
        if (p.find("TDATA") != std::string::npos || p.find("tdata") != std::string::npos) out_dev = "T";
        else if (p.find("UDATA") != std::string::npos || p.find("udata") != std::string::npos) out_dev = "U";
        size_t pos = p.find('\\', 25);
        out_rel = (pos != std::string::npos) ? to_lower_bs(p.substr(pos + 1)) : "";
        return true;
    }

    if (p.size() >= 2 && p[1] == ':') {
        char drv = static_cast<char>(std::toupper(static_cast<unsigned char>(p[0])));
        out_rel = to_lower_bs(p.substr(2));
        if (drv == 'D') { out_dev = "iso"; return true; }
        if (drv == 'T') { out_dev = "T";   return true; }
        if (drv == 'U') { out_dev = "U";   return true; }
        if (drv == 'Z' || drv == 'S' || drv == 'X' || drv == 'Y' || drv == 'C') {
            out_dev = "Z"; return true;
        }
        out_dev = "iso";
        return true;
    }

    if (!p.empty() && p[0] == '$') p = p.substr(1);
    out_dev = "iso";
    out_rel = to_lower_bs(p);
    return true;
}

const XisoEntry* XisoVfs::find_iso_entry(const std::string& norm_rel) const {
    std::string k = to_lower_bs(norm_rel);
    auto it = g_path_map.find(k);
    if (it == g_path_map.end()) return nullptr;
    return &entries_[it->second];
}

bool XisoVfs::read_iso_file(const std::string& norm_rel, std::vector<uint8_t>& out_data) {
    const XisoEntry* e = find_iso_entry(norm_rel);
    if (!e || e->is_dir) return false;
    out_data.resize(e->size);
    if (e->size == 0) return true;
    uint32_t got = 0;
    return read_iso_range(e, 0, out_data.data(), e->size, &got) && got == e->size;
}

bool XisoVfs::read_iso_range(const XisoEntry* entry, uint64_t file_offset, void* dst, uint32_t bytes, uint32_t* out_read) {
    if (out_read) *out_read = 0;
    if (!entry || entry->is_dir) return false;
    if (file_offset >= entry->size) return true;
    uint32_t avail = static_cast<uint32_t>(std::min<uint64_t>(bytes, entry->size - file_offset));
    if (avail == 0) return true;

    std::lock_guard<std::mutex> lk(g_iso_mtx);
    if (folder_mode_) {
        std::ifstream f(entry->host_path, std::ios::binary);
        if (!f.is_open()) return false;
        f.seekg(static_cast<std::streamoff>(file_offset));
        f.read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(avail));
        uint32_t got = static_cast<uint32_t>(f.gcount());
        if (out_read) *out_read = got;
        return got > 0;
    }

#ifdef _WIN32
    if (!file_handle_) return false;
    if (!read_at(static_cast<HANDLE>(file_handle_), entry->iso_offset + file_offset, dst, avail)) return false;
#else
    if (iso_fd_ < 0) return false;
    if (!read_at(iso_fd_, entry->iso_offset + file_offset, dst, avail)) return false;
#endif
    if (out_read) *out_read = avail;
    return true;
}

std::vector<const XisoEntry*> XisoVfs::list_iso_dir(const std::string& dir_norm_rel, const std::string& pattern) const {
    std::string parent = to_lower_bs(dir_norm_rel);
    std::vector<const XisoEntry*> res;
    for (const auto& e : entries_) {
        if (e.parent == parent && wildcard_match_ci(pattern.c_str(), e.name.c_str())) {
            res.push_back(&e);
        }
    }
    return res;
}

std::string XisoVfs::host_path_for(const std::string& dev, const std::string& rel) const {
    std::string sub = "Z_cache";
    if (dev == "T") sub = "TDATA";
    else if (dev == "U") sub = "UDATA";
    fs::path p = fs::path(userdata_root_) / sub;
    if (!rel.empty()) {
        std::string norm = rel;
        for (char& c : norm) if (c == '\\') c = '/';
        p /= fs::path(norm);
    }
    return p.string();
}
