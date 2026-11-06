// Xbox path resolution with the mods/ override layer (see vfs.h).

#include "hle/kernel/vfs.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <unordered_map>

namespace hp2k::vfs {

namespace {

Config g_cfg;
std::mutex g_mu;
std::map<std::string, std::string> g_links;  // lower(link) -> target
// Read-only trees (disc, mods): directory -> lower(name) -> actual name.
std::unordered_map<std::string, std::unordered_map<std::string, std::string>> g_ro_cache;

std::vector<std::string> Split(const std::string& p) {
  std::vector<std::string> out;
  std::string cur;
  for (char ch : p) {
    if (ch == '\\' || ch == '/') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else {
      cur += ch;
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

bool IsDir(const std::string& p) {
  struct stat st;
  return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
bool Exists(const std::string& p) {
  struct stat st;
  return stat(p.c_str(), &st) == 0;
}

std::vector<std::string> ReadDir(const std::string& dir) {
  std::vector<std::string> names;
  if (DIR* d = opendir(dir.c_str())) {
    while (dirent* e = readdir(d)) {
      std::string n = e->d_name;
      if (n != "." && n != "..") names.push_back(n);
    }
    closedir(d);
  }
  return names;
}

// Actual name of `name` in `dir` (case-insensitive), or "".
std::string LookupName(const std::string& dir, const std::string& name, bool cached) {
  std::string ln = Lower(name);
  if (cached) {
    auto it = g_ro_cache.find(dir);
    if (it == g_ro_cache.end()) {
      auto& m = g_ro_cache[dir];
      for (auto& n : ReadDir(dir)) m.emplace(Lower(n), n);
      it = g_ro_cache.find(dir);
    }
    auto f = it->second.find(ln);
    return f == it->second.end() ? "" : f->second;
  }
  if (Exists(dir + "/" + name)) return name;  // fast path; may still differ in case on case-sensitive FS
  for (auto& n : ReadDir(dir))
    if (Lower(n) == ln) return n;
  return "";
}

// Walks `rel` under `root` case-insensitively; "" if a component is missing.
std::string Walk(const std::string& root, const std::string& rel, bool cached) {
  std::string p = root;
  for (auto& comp : Split(rel)) {
    std::string n = LookupName(p, comp, cached);
    if (n.empty()) return "";
    p += "/" + n;
  }
  return p;
}

std::vector<std::string> DiscRoots() {
  std::vector<std::string> r = g_cfg.mods_dirs;
  r.push_back(g_cfg.disc_dir);
  return r;
}

void MakeDirs(const std::string& p) {
  std::string cur;
  for (size_t i = 0; i <= p.size(); i++) {
    if (i == p.size() || p[i] == '/') {
      if (!cur.empty()) mkdir(cur.c_str(), 0755);
    }
    if (i < p.size()) cur += p[i];
  }
}

}  // namespace

std::string Lower(std::string s) {
  for (auto& ch : s) ch = char(std::tolower(static_cast<unsigned char>(ch)));
  return s;
}

void Init(const Config& cfg) {
  std::lock_guard<std::mutex> l(g_mu);
  g_cfg = cfg;
  g_links.clear();
  g_ro_cache.clear();
  g_links["\\??\\d:"] = "\\Device\\CdRom0";
  MakeDirs(cfg.data_dir + "/hdd/partition1");
  for (int i = 3; i <= 5; i++) MakeDirs(cfg.data_dir + "/hdd/partition" + std::to_string(i));
}

const Config& Cfg() { return g_cfg; }

bool CreateLink(const std::string& link, const std::string& target) {
  std::lock_guard<std::mutex> l(g_mu);
  std::string k = Lower(link);
  while (!k.empty() && k.back() == '\\') k.pop_back();
  if (g_links.count(k)) return false;
  g_links[k] = target;
  return true;
}

bool GetLink(const std::string& link, std::string* target) {
  std::lock_guard<std::mutex> l(g_mu);
  std::string k = Lower(link);
  while (!k.empty() && k.back() == '\\') k.pop_back();
  auto it = g_links.find(k);
  if (it == g_links.end()) return false;
  *target = it->second;
  return true;
}

bool Resolve(const std::string& in, Resolved* out) {
  std::string p = in;
  for (auto& ch : p)
    if (ch == '/') ch = '\\';
  // "D:\x" -> "\??\D:\x"
  if (p.size() >= 2 && p[1] == ':' && std::isalpha(static_cast<unsigned char>(p[0]))) p = "\\??\\" + p;
  if (p.rfind("\\??\\", 0) == 0) {
    size_t colon = p.find(':');
    if (colon == std::string::npos) return false;
    std::string target;
    if (!GetLink(p.substr(0, colon + 1), &target)) return false;
    p = target + "\\" + p.substr(colon + 1);
  }
  auto parts = Split(p);
  // Resolve "." and ".." against the components.
  std::vector<std::string> clean;
  for (auto& s : parts) {
    if (s == ".") continue;
    if (s == "..") {
      if (clean.size() > 2) clean.pop_back();
      continue;
    }
    clean.push_back(s);
  }
  if (clean.size() < 2 || Lower(clean[0]) != "device") return false;
  Resolved r;
  size_t rest = 0;
  std::string dev = Lower(clean[1]);
  if (dev == "cdrom0") {
    r.dev = Device::Disc;
    rest = 2;
    r.nt = "\\Device\\CdRom0";
  } else if (dev == "harddisk0") {
    if (clean.size() < 3) return false;
    std::string part = Lower(clean[2]);
    if (part.rfind("partition", 0) != 0) return false;
    int n = std::atoi(part.c_str() + 9);
    rest = 3;
    r.nt = "\\Device\\Harddisk0\\Partition" + std::to_string(n);
    if (n == 0) {
      r.dev = Device::RawDisk;
    } else if (n == 1 || n >= 3) {
      r.dev = Device::Partition;
      r.partition = n;
    } else {
      return false;  // partition 2: the dashboard's C:, not ours
    }
  } else {
    return false;
  }
  for (size_t i = rest; i < clean.size(); i++) {
    if (!r.rel.empty()) r.rel += "/";
    r.rel += clean[i];
    r.nt += "\\" + clean[i];
  }
  *out = r;
  return true;
}

std::string DeviceRoot(const Resolved& r) {
  switch (r.dev) {
    case Device::Disc: return g_cfg.disc_dir;
    case Device::Partition: return g_cfg.data_dir + "/hdd/partition" + std::to_string(r.partition);
    case Device::RawDisk: return g_cfg.data_dir + "/hdd";
    default: return "";
  }
}

std::string FindExisting(const Resolved& r) {
  std::lock_guard<std::mutex> l(g_mu);
  if (r.dev == Device::Disc) {
    for (auto& root : DiscRoots()) {
      if (root.empty() || !IsDir(root)) continue;
      std::string p = r.rel.empty() ? root : Walk(root, r.rel, true);
      if (!p.empty()) return p;
    }
    return "";
  }
  std::string root = DeviceRoot(r);
  if (root.empty()) return "";
  return r.rel.empty() ? root : Walk(root, r.rel, false);
}

std::string PathForCreate(const Resolved& r) {
  if (r.dev != Device::Partition) return "";
  std::lock_guard<std::mutex> l(g_mu);
  std::string p = DeviceRoot(r);
  auto comps = Split(r.rel);
  for (size_t i = 0; i < comps.size(); i++) {
    std::string n = LookupName(p, comps[i], false);
    p += "/" + (n.empty() ? comps[i] : n);
  }
  return p;
}

std::vector<DirEntry> List(const Resolved& r) {
  std::vector<std::string> dirs;
  {
    std::lock_guard<std::mutex> l(g_mu);
    if (r.dev == Device::Disc) {
      for (auto& root : DiscRoots()) {
        if (root.empty() || !IsDir(root)) continue;
        std::string p = r.rel.empty() ? root : Walk(root, r.rel, true);
        if (!p.empty() && IsDir(p)) dirs.push_back(p);
      }
    } else {
      std::string root = DeviceRoot(r);
      std::string p = r.rel.empty() ? root : Walk(root, r.rel, false);
      if (!p.empty() && IsDir(p)) dirs.push_back(p);
    }
  }
  std::map<std::string, DirEntry> merged;  // lower name -> entry (first root wins)
  for (auto& d : dirs)
    for (auto& n : ReadDir(d)) {
      std::string ln = Lower(n);
      if (merged.count(ln)) continue;
      DirEntry e;
      e.name = n;
      e.host_path = d + "/" + n;
      struct stat st;
      if (stat(e.host_path.c_str(), &st) != 0) continue;
      e.dir = S_ISDIR(st.st_mode);
      e.size = e.dir ? 0 : uint64_t(st.st_size);
      e.mtime = st.st_mtime;
      merged.emplace(ln, e);
    }
  std::vector<DirEntry> out;
  for (auto& [k, e] : merged) out.push_back(e);
  return out;
}

bool MatchMask(const std::string& mask, const std::string& name) {
  if (mask.empty() || mask == "*" || mask == "*.*") return true;
  std::string m = Lower(mask), n = Lower(name);
  // Iterative glob with backtracking.
  size_t mi = 0, ni = 0, star = std::string::npos, mark = 0;
  while (ni < n.size()) {
    if (mi < m.size() && (m[mi] == '?' || m[mi] == n[ni])) {
      mi++;
      ni++;
    } else if (mi < m.size() && m[mi] == '*') {
      star = mi++;
      mark = ni;
    } else if (star != std::string::npos) {
      mi = star + 1;
      ni = ++mark;
    } else {
      return false;
    }
  }
  while (mi < m.size() && m[mi] == '*') mi++;
  // "name.*" also matches "name" without an extension (DOS semantics).
  if (mi + 2 == m.size() && m[mi] == '.' && m[mi + 1] == '*') return true;
  return mi == m.size();
}

}  // namespace hp2k::vfs
