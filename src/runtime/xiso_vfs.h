#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct XisoEntry {
    std::string norm_path; // lowercase backslash path relative to root, e.g. "global\\anm\\chars.pak"
    std::string name;      // original filename
    std::string parent;    // normalized parent directory
    std::string host_path; // non-empty when mounted from an extracted game folder
    uint64_t iso_offset = 0;
    uint32_t size = 0;
    bool is_dir = false;
    uint32_t attrs = 0x80; // FILE_ATTRIBUTE_NORMAL / DIRECTORY
};

struct XisoProbeResult {
    bool readable = false;
    bool is_xbox_media = false;
    bool has_xbe = false;
    bool is_whacked = false;
    uint32_t title_id = 0;
    std::string title_name;
    std::string format_kind; // "Xbox XISO", "Redump DVD (XGD1)", "Extracted Folder"
    std::string message;
};

XisoProbeResult xiso_probe_game(const std::string& path);
std::vector<std::string> xiso_auto_discover(const std::string& extra_dir = {});

class XisoVfs {
public:
    static XisoVfs& instance();

    bool mount_iso(const std::string& iso_path, const std::string& userdata_root);
    void unmount();

    bool is_mounted() const { return mounted_; }
    const std::string& mounted_source() const { return mounted_path_; }
    const std::string& mounted_format() const { return mounted_format_; }

    // Path normalization: maps Xbox paths ("D:\\...", "\\??\\D:\\...", "$...", "\\Device\\Cdrom0\\...")
    // Returns type: "iso", "T", "U", "Z", or "host" and normalized relative path
    bool resolve_path(const std::string& xbox_path, std::string& out_dev, std::string& out_rel) const;

    const XisoEntry* find_iso_entry(const std::string& norm_rel) const;
    bool read_iso_file(const std::string& norm_rel, std::vector<uint8_t>& out_data);
    bool read_iso_range(const XisoEntry* entry, uint64_t file_offset, void* dst, uint32_t bytes, uint32_t* out_read);

    std::vector<const XisoEntry*> list_iso_dir(const std::string& dir_norm_rel, const std::string& pattern) const;
    std::string host_path_for(const std::string& dev, const std::string& rel) const;
    void set_userdata_root(const std::string& root) { userdata_root_ = root; }

private:
    bool mounted_ = false;
    bool folder_mode_ = false;
    uint64_t iso_base_offset_ = 0;
    void* file_handle_ = nullptr; // HANDLE on Windows
    int iso_fd_ = -1;             // POSIX fd on Linux/Unix
    std::string mounted_path_;
    std::string mounted_format_;
    std::string userdata_root_;
    std::vector<XisoEntry> entries_;
};
