#include "hf-cache.h"

#include "build-info.h"
#include "common.h"
#include "log.h"
#include "http.h"

#define JSON_ASSERT GGML_ASSERT
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <atomic>
#include <string>
#include <string_view>
#include <stdexcept>

namespace nl = nlohmann;

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define HOME_DIR "USERPROFILE"
#include <windows.h>
#else
#define HOME_DIR "HOME"
#include <unistd.h>
#include <pwd.h>
#endif

namespace hf_cache {

static std::string repo_to_folder_name(const std::string & repo_id);

#if defined(_WIN32)
static bool win_path_exists(const std::string & path) {
    std::ifstream file(path, std::ios::binary);
    return file.good();
}

static bool win_is_directory(const std::string & path) {
    return fs_is_directory(path);
}

static std::string win_join_path(const std::string & a, const std::string & b) {
    if (a.empty()) {
        return b;
    }
    if (b.empty()) {
        return a;
    }
    if (a.back() == '/' || a.back() == '\\') {
        return a + b;
    }
    return a + "\\" + b;
}

static std::string win_parent_path(const std::string & path) {
    const size_t pos = path.find_last_of("/\\");
    return pos == std::string::npos ? std::string() : path.substr(0, pos);
}

static bool win_is_valid_subpath(const std::string & subpath) {
    if (subpath.empty() || subpath.find(':') != std::string::npos) {
        return false;
    }
    if (subpath[0] == '/' || subpath[0] == '\\') {
        return false;
    }
    for (size_t pos = 0; pos < subpath.size();) {
        size_t end = subpath.find_first_of("/\\", pos);
        std::string part = subpath.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        if (part.empty() || part == "." || part == "..") {
            return false;
        }
        if (end == std::string::npos) {
            break;
        }
        pos = end + 1;
    }
    return true;
}

static std::string win_get_cache_directory() {
    static const std::string cache = []() {
        struct entry {
            const char * var;
            const char * suffix;
        } entries[] = {
            {"LLAMA_CACHE",           ""},
            {"HF_HUB_CACHE",          ""},
            {"HUGGINGFACE_HUB_CACHE", ""},
            {"HF_HOME",               "hub"},
            {"XDG_CACHE_HOME",        "huggingface\\hub"},
            {HOME_DIR,                ".cache\\huggingface\\hub"}
        };
        for (const auto & entry : entries) {
            if (auto * p = std::getenv(entry.var); p && *p) {
                return entry.suffix[0] == '\0' ? std::string(p) : win_join_path(p, entry.suffix);
            }
        }
        throw std::runtime_error("Failed to determine HF cache directory");
    }();

    return cache;
}

static std::string win_get_repo_path(const std::string & repo_id) {
    return win_join_path(win_get_cache_directory(), repo_to_folder_name(repo_id));
}

static void win_safe_write_file(const std::string & path, const std::string & data) {
    const std::string path_tmp = path + ".tmp";
    const std::string parent = win_parent_path(path);
    if (!parent.empty()) {
        fs_create_directory_with_parents(parent);
    }

    std::ofstream file(path_tmp, std::ios::binary);
    file << data;
    file.close();

    if (!file.fail() && std::rename(path_tmp.c_str(), path.c_str()) == 0) {
        return;
    }

    remove(path_tmp.c_str());
    throw std::runtime_error("failed to write file: " + path);
}

static bool win_remove_all(const std::string & path) {
    bool removed = false;
    if (win_is_directory(path)) {
        for (const auto & entry : fs_list(path, true)) {
            removed = win_remove_all(entry.path) || removed;
        }
        if (RemoveDirectoryA(path.c_str()) != 0) {
            removed = true;
        }
    } else if (remove(path.c_str()) == 0) {
        removed = true;
    }
    return removed;
}
#else
namespace fs = std::filesystem;
#endif

#if !defined(_WIN32)
static fs::path get_cache_directory() {
    static const fs::path cache = []() {
        struct {
            const char * var;
            fs::path path;
        } entries[] = {
            {"LLAMA_CACHE",           fs::path()},
            {"HF_HUB_CACHE",          fs::path()},
            {"HUGGINGFACE_HUB_CACHE", fs::path()},
            {"HF_HOME",               fs::path("hub")},
            {"XDG_CACHE_HOME",        fs::path("huggingface") / "hub"},
            {HOME_DIR,                fs::path(".cache") / "huggingface" / "hub"}
        };
        for (const auto & entry : entries) {
            if (auto * p = std::getenv(entry.var); p && *p) {
                fs::path base(p);
                return entry.path.empty() ? base : base / entry.path;
            }
        }
#ifndef _WIN32
        const struct passwd * pw = getpwuid(getuid());

        if (pw && pw->pw_dir && *pw->pw_dir) {
            return fs::path(pw->pw_dir) / ".cache" / "huggingface" / "hub";
        }
#endif
        throw std::runtime_error("Failed to determine HF cache directory");
    }();

    return cache;
}
#endif

static std::string folder_name_to_repo(const std::string & folder) {
    constexpr std::string_view prefix = "models--";
    if (folder.rfind(prefix, 0)) {
        return {};
    }
    std::string result = folder.substr(prefix.length());
    string_replace_all(result, "--", "/");
    return result;
}

static std::string repo_to_folder_name(const std::string & repo_id) {
    constexpr std::string_view prefix = "models--";
    std::string result = std::string(prefix) + repo_id;
    string_replace_all(result, "/", "--");
    return result;
}

#if !defined(_WIN32)
static fs::path get_repo_path(const std::string & repo_id) {
    return get_cache_directory() / repo_to_folder_name(repo_id);
}
#endif

static bool is_hex_char(const char c) {
    return (c >= 'A' && c <= 'F') ||
           (c >= 'a' && c <= 'f') ||
           (c >= '0' && c <= '9');
}

static bool is_hex_string(const std::string & s, size_t expected_len) {
    if (s.length() != expected_len) {
        return false;
    }
    for (const char c : s) {
        if (!is_hex_char(c)) {
            return false;
        }
    }
    return true;
}

static bool is_alphanum(const char c) {
    return (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9');
}

static bool is_special_char(char c) {
    return c == '/' || c == '.' || c == '-';
}

// base chars [A-Za-z0-9_] are always valid
// special chars [/.-] must be surrounded by base chars
// exactly one '/' required
static bool is_valid_repo_id(const std::string & repo_id) {
    if (repo_id.empty() || repo_id.length() > 256) {
        return false;
    }
    int slash = 0;
    bool special = true;

    for (const char c : repo_id) {
        if (is_alphanum(c) || c == '_') {
            special = false;
        } else if (is_special_char(c)) {
            if (special) {
                return false;
            }
            slash += (c == '/');
            special = true;
        } else {
            return false;
        }
    }
    return !special && slash == 1;
}

static bool is_valid_hf_token(const std::string & token) {
    if (token.length() < 37 || token.length() > 256 ||
        !string_starts_with(token, "hf_")) {
        return false;
    }
    for (size_t i = 3; i < token.length(); ++i) {
        if (!is_alphanum(token[i])) {
            return false;
        }
    }
    return true;
}

static bool is_valid_commit(const std::string & hash) {
    return is_hex_string(hash, 40);
}

static bool is_valid_oid(const std::string & oid) {
    return is_hex_string(oid, 40) || is_hex_string(oid, 64);
}

#if !defined(_WIN32)
static bool is_valid_subpath(const fs::path & path, const fs::path & subpath) {
    if (subpath.is_absolute()) {
        return false; // never do a / b with b absolute
    }
    auto b = fs::absolute(path).lexically_normal();
    auto t = (b / subpath).lexically_normal();
    auto [b_end, _] = std::mismatch(b.begin(), b.end(), t.begin(), t.end());

    return b_end == b.end();
}
#endif

#if !defined(_WIN32)
static void safe_write_file(const fs::path & path, const std::string & data) {
    fs::path path_tmp = path.string() + ".tmp";

    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path());
    }

    std::ofstream file(path_tmp);
    file << data;
    file.close();

    std::error_code ec;

    if (!file.fail()) {
        fs::rename(path_tmp, path, ec);
    }
    if (file.fail() || ec) {
        fs::remove(path_tmp, ec);
        throw std::runtime_error("failed to write file: " + path.string());
    }
}
#endif

static nl::json api_get(const std::string & url,
                        const std::string & token) {
    auto [cli, parts] = common_http_client(url);

    httplib::Headers headers = {
        {"User-Agent", "llama-cpp/" + std::string(llama_build_info())},
        {"Accept", "application/json"}
    };

    if (is_valid_hf_token(token)) {
        headers.emplace("Authorization", "Bearer " + token);
    } else if (!token.empty()) {
        LOG_WRN("%s: invalid token, authentication disabled\n", __func__);
    }

    if (auto res = cli.Get(parts.path, headers)) {
        auto body = res->body;

        if (res->status == 200) {
            return nl::json::parse(res->body);
        }
        try {
            body = nl::json::parse(res->body)["error"].get<std::string>();
        } catch (...) { }

        throw std::runtime_error("GET failed (" + std::to_string(res->status) + "): " + body);
    } else {
        throw std::runtime_error("HTTPLIB failed: " + httplib::to_string(res.error()));
    }
}

static std::string get_repo_commit(const std::string & repo_id,
                                   const std::string & token) {
    try {
        auto endpoint = common_get_model_endpoint();
        auto json = api_get(endpoint + "api/models/" + repo_id + "/refs", token);

        if (!json.is_object() ||
            !json.contains("branches") || !json["branches"].is_array()) {
            LOG_WRN("%s: missing 'branches' for '%s'\n", __func__, repo_id.c_str());
            return {};
        }

    #ifdef _WIN32
        const std::string refs_path = win_join_path(win_get_repo_path(repo_id), "refs");
    #else
        fs::path refs_path = get_repo_path(repo_id) / "refs";
    #endif
        std::string name;
        std::string commit;

        for (const auto & branch : json["branches"]) {
            if (!branch.is_object() ||
                !branch.contains("name") || !branch["name"].is_string() ||
                !branch.contains("targetCommit") || !branch["targetCommit"].is_string()) {
                continue;
            }
            std::string _name = branch["name"].get<std::string>();
            std::string _commit = branch["targetCommit"].get<std::string>();

#ifdef _WIN32
            if (!win_is_valid_subpath(_name)) {
#else
            if (!is_valid_subpath(refs_path, _name)) {
#endif
                LOG_WRN("%s: skip invalid branch: %s\n", __func__, _name.c_str());
                continue;
            }
            if (!is_valid_commit(_commit)) {
                LOG_WRN("%s: skip invalid commit: %s\n", __func__, _commit.c_str());
                continue;
            }

            if (_name == "main") {
                name = _name;
                commit = _commit;
                break;
            }

            if (name.empty() || commit.empty()) {
                name = _name;
                commit = _commit;
            }
        }

        if (name.empty() || commit.empty()) {
            LOG_WRN("%s: no valid branch for '%s'\n", __func__, repo_id.c_str());
            return {};
        }

    #ifdef _WIN32
        win_safe_write_file(win_join_path(refs_path, name), commit);
    #else
        safe_write_file(refs_path / name, commit);
    #endif
        return commit;

    } catch (const nl::json::exception & e) {
        LOG_ERR("%s: JSON error: %s\n", __func__, e.what());
    } catch (const std::exception & e) {
        LOG_ERR("%s: error: %s\n", __func__, e.what());
    }
    return {};
}

hf_files get_repo_files(const std::string & repo_id,
                        const std::string & token) {
    if (!is_valid_repo_id(repo_id)) {
        LOG_WRN("%s: invalid repository: %s\n", __func__, repo_id.c_str());
        return {};
    }

    std::string commit = get_repo_commit(repo_id, token);
    if (commit.empty()) {
        LOG_WRN("%s: failed to resolve commit for %s\n", __func__, repo_id.c_str());
        return {};
    }

#ifdef _WIN32
    const std::string repo_path = win_get_repo_path(repo_id);
    const std::string blobs_path = win_join_path(repo_path, "blobs");
    const std::string commit_path = win_join_path(win_join_path(repo_path, "snapshots"), commit);
#else
    fs::path blobs_path = get_repo_path(repo_id) / "blobs";
    fs::path commit_path = get_repo_path(repo_id) / "snapshots" / commit;
#endif

    hf_files files;

    try {
        auto endpoint = common_get_model_endpoint();
        auto json = api_get(endpoint + "api/models/" + repo_id + "/tree/" + commit + "?recursive=true", token);

        if (!json.is_array()) {
            LOG_WRN("%s: response is not an array for '%s'\n", __func__, repo_id.c_str());
            return {};
        }

        for (const auto & item : json) {
            if (!item.is_object() ||
                !item.contains("type") || !item["type"].is_string() || item["type"] != "file" ||
                !item.contains("path") || !item["path"].is_string()) {
                continue;
            }

            hf_file file;
            file.repo_id = repo_id;
            file.path = item["path"].get<std::string>();

#ifdef _WIN32
            if (!win_is_valid_subpath(file.path)) {
#else
            if (!is_valid_subpath(commit_path, file.path)) {
#endif
                LOG_WRN("%s: skip invalid path: %s\n", __func__, file.path.c_str());
                continue;
            }

            if (item.contains("lfs") && item["lfs"].is_object()) {
                if (item["lfs"].contains("oid") && item["lfs"]["oid"].is_string()) {
                    file.oid = item["lfs"]["oid"].get<std::string>();
                }
            } else if (item.contains("oid") && item["oid"].is_string()) {
                file.oid = item["oid"].get<std::string>();
            }

            if (!file.oid.empty() && !is_valid_oid(file.oid)) {
                LOG_WRN("%s: skip invalid oid: %s\n", __func__, file.oid.c_str());
                continue;
            }

            file.url = endpoint + repo_id + "/resolve/" + commit + "/" + file.path;

#ifdef _WIN32
            std::string final_path = win_join_path(commit_path, file.path);
            file.final_path = final_path;

            if (!file.oid.empty() && !win_path_exists(final_path)) {
                file.local_path = win_join_path(blobs_path, file.oid);
            } else {
                file.local_path = file.final_path;
            }
#else
            fs::path final_path = commit_path / file.path;
            file.final_path = final_path.string();

            if (!file.oid.empty() && !fs::exists(final_path)) {
                fs::path local_path = blobs_path / file.oid;
                file.local_path = local_path.string();
            } else {
                file.local_path = file.final_path;
            }
#endif

            files.push_back(file);
        }
    } catch (const nl::json::exception & e) {
        LOG_ERR("%s: JSON error: %s\n", __func__, e.what());
    } catch (const std::exception & e) {
        LOG_ERR("%s: error: %s\n", __func__, e.what());
    }
    return files;
}

#ifdef _WIN32
static std::string get_cached_ref(const std::string & repo_path) {
    const std::string refs_path = win_join_path(repo_path, "refs");
    if (!win_is_directory(refs_path)) {
        return {};
    }
    std::string fallback;

    for (const auto & entry : fs_list(refs_path, false)) {
        std::ifstream f(entry.path);
        std::string commit;
        if (!f || !std::getline(f, commit) || commit.empty()) {
            continue;
        }
        if (!is_valid_commit(commit)) {
            LOG_WRN("%s: skip invalid commit: %s\n", __func__, commit.c_str());
            continue;
        }
        if (entry.name == "main") {
            return commit;
        }
        if (fallback.empty()) {
            fallback = commit;
        }
    }
    return fallback;
}
#else
static std::string get_cached_ref(const fs::path & repo_path) {
    fs::path refs_path = repo_path / "refs";
    if (!fs::is_directory(refs_path)) {
        return {};
    }
    std::string fallback;

    for (const auto & entry : fs::directory_iterator(refs_path)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        std::ifstream f(entry.path());
        std::string commit;
        if (!f || !std::getline(f, commit) || commit.empty()) {
            continue;
        }
        if (!is_valid_commit(commit)) {
            LOG_WRN("%s: skip invalid commit: %s\n", __func__, commit.c_str());
            continue;
        }
        if (entry.path().filename() == "main") {
            return commit;
        }
        if (fallback.empty()) {
            fallback = commit;
        }
    }
    return fallback;
}
#endif

hf_files get_cached_files(const std::string & repo_id) {
#ifdef _WIN32
    const std::string cache_dir = win_get_cache_directory();
    if (!win_is_directory(cache_dir)) {
        return {};
    }

    if (!repo_id.empty() && !is_valid_repo_id(repo_id)) {
        LOG_WRN("%s: invalid repository: %s\n", __func__, repo_id.c_str());
        return {};
    }

    hf_files files;

    for (const auto & repo : fs_list(cache_dir, true)) {
        if (!repo.is_dir) {
            continue;
        }
        const std::string snapshots_path = win_join_path(repo.path, "snapshots");
        if (!win_is_directory(snapshots_path)) {
            continue;
        }
        std::string _repo_id = folder_name_to_repo(repo.name);

        if (!is_valid_repo_id(_repo_id)) {
            continue;
        }
        if (!repo_id.empty() && _repo_id != repo_id) {
            continue;
        }
        std::string commit = get_cached_ref(repo.path);
        const std::string commit_path = win_join_path(snapshots_path, commit);

        if (commit.empty() || !win_is_directory(commit_path)) {
            continue;
        }

        std::vector<std::pair<std::string, std::string>> stack = {{commit_path, std::string()}};
        while (!stack.empty()) {
            auto [dir, rel] = stack.back();
            stack.pop_back();
            for (const auto & entry : fs_list(dir, true)) {
                const std::string entry_rel = rel.empty() ? entry.name : rel + "/" + entry.name;
                if (entry.is_dir) {
                    stack.push_back({entry.path, entry_rel});
                    continue;
                }
                hf_file file;
                file.repo_id = _repo_id;
                file.path = entry_rel;
                file.local_path = entry.path;
                file.final_path = file.local_path;
                files.push_back(std::move(file));
            }
        }
    }

    return files;
#else
    fs::path cache_dir = get_cache_directory();
    if (!fs::exists(cache_dir)) {
        return {};
    }

    if (!repo_id.empty() && !is_valid_repo_id(repo_id)) {
        LOG_WRN("%s: invalid repository: %s\n", __func__, repo_id.c_str());
        return {};
    }

    hf_files files;

    for (const auto & repo : fs::directory_iterator(cache_dir)) {
        if (!repo.is_directory()) {
            continue;
        }
        fs::path snapshots_path = repo.path() / "snapshots";

        if (!fs::exists(snapshots_path)) {
            continue;
        }
        std::string _repo_id = folder_name_to_repo(repo.path().filename().string());

        if (!is_valid_repo_id(_repo_id)) {
            continue;
        }
        if (!repo_id.empty() && _repo_id != repo_id) {
            continue;
        }
        std::string commit = get_cached_ref(repo.path());
        fs::path commit_path = snapshots_path / commit;

        if (commit.empty() || !fs::is_directory(commit_path)) {
            continue;
        }
        for (const auto & entry : fs::recursive_directory_iterator(commit_path)) {
            if (!entry.is_regular_file() && !entry.is_symlink()) {
                continue;
            }
            fs::path path = entry.path().lexically_relative(commit_path);

            if (!path.empty()) {
                hf_file file;
                file.repo_id = _repo_id;
                file.path = path.generic_string();
                file.local_path = entry.path().string();
                file.final_path = file.local_path;
                files.push_back(std::move(file));
            }
        }
    }

    return files;
#endif
}

std::string finalize_file(const hf_file & file) {
#ifdef _WIN32
    const std::string & local_path = file.local_path;
    const std::string & final_path = file.final_path;

    if (local_path == final_path || win_path_exists(final_path)) {
        return file.final_path;
    }

    if (!win_path_exists(local_path)) {
        return file.final_path;
    }

    const std::string parent = win_parent_path(final_path);
    if (!parent.empty()) {
        fs_create_directory_with_parents(parent);
    }

    if (std::rename(local_path.c_str(), final_path.c_str()) != 0) {
        LOG_WRN("%s: failed to move file to snapshots\n", __func__);
        std::ifstream src(local_path, std::ios::binary);
        std::ofstream dst(final_path, std::ios::binary);
        dst << src.rdbuf();
        if (!src.good() && !src.eof()) {
            LOG_ERR("%s: failed to read local file\n", __func__);
        }
        if (!dst.good()) {
            LOG_ERR("%s: failed to copy file to snapshots\n", __func__);
        }
    }
    return file.final_path;
#else
    static std::atomic<bool> symlinks_disabled{false};

    std::error_code ec;
    fs::path local_path(file.local_path);
    fs::path final_path(file.final_path);

    if (local_path == final_path || fs::exists(final_path, ec)) {
        return file.final_path;
    }

    if (!fs::exists(local_path, ec)) {
        return file.final_path;
    }

    fs::create_directories(final_path.parent_path(), ec);

    if (!symlinks_disabled) {
        fs::path target = fs::relative(local_path, final_path.parent_path(), ec);
        if (!ec) {
            fs::create_symlink(target, final_path, ec);
        }
        if (!ec) {
            return file.final_path;
        }
    }

    if (!symlinks_disabled.exchange(true)) {
        LOG_WRN("%s: failed to create symlink: %s\n", __func__, ec.message().c_str());
        LOG_WRN("%s: switching to degraded mode\n", __func__);
    }

    fs::rename(local_path, final_path, ec);
    if (ec) {
        LOG_WRN("%s: failed to move file to snapshots: %s\n", __func__, ec.message().c_str());
        fs::copy(local_path, final_path, ec);
        if (ec) {
            LOG_ERR("%s: failed to copy file to snapshots: %s\n", __func__, ec.message().c_str());
        }
    }
    return file.final_path;
#endif
}

bool remove_cached_repo(const std::string & repo_id) {
    if (!is_valid_repo_id(repo_id)) {
        LOG_WRN("%s: invalid repository: %s\n", __func__, repo_id.c_str());
        return false;
    }
#ifdef _WIN32
    const std::string repo_path = win_get_repo_path(repo_id);
    if (!win_path_exists(repo_path) && !win_is_directory(repo_path)) {
        return false;
    }
    if (!win_remove_all(repo_path)) {
        LOG_ERR("%s: failed to remove repo cache %s\n", __func__, repo_path.c_str());
        return false;
    }
    return true;
#else
    fs::path repo_path = get_repo_path(repo_id);
    std::error_code ec;
    auto removed = fs::remove_all(repo_path, ec);
    if (ec) {
        LOG_ERR("%s: failed to remove repo cache %s: %s\n", __func__, repo_path.string().c_str(), ec.message().c_str());
        return false;
    }
    return removed > 0;
#endif
}

} // namespace hf_cache
