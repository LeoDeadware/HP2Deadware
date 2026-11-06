// File I/O: Nt*File, symbolic links, volume/disk queries, on top of vfs.
// All I/O completes synchronously (never STATUS_PENDING).

#include <fcntl.h>
#include <mach-o/dyld.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <cstdlib>
#include <set>
#include <vector>

#include "hle/kernel/kernel_internal.h"
#include "hle/kernel/vfs.h"

namespace hp2k {

namespace {

constexpr uint32_t kFileSupersede = 0, kFileOpen = 1, kFileCreate = 2,
                   kFileOverwrite = 4, kFileOverwriteIf = 5;
constexpr uint32_t kOptDirectory = 0x1, kOptNonDirectory = 0x40, kOptDeleteOnClose = 0x1000;
constexpr uint32_t kInfoSuperseded = 0, kInfoOpened = 1, kInfoCreated = 2,
                   kInfoOverwritten = 3, kInfoDoesNotExist = 5;
constexpr uint32_t kAttrReadOnly = 0x1, kAttrDirectory = 0x10, kAttrNormal = 0x80;
constexpr uint32_t kClusterSectors = 32, kSectorSize = 512;  // 16 KB clusters (XAPI checks)

bool g_zcache = false;

enum class FKind { File, Dir, Volume, RawDisk };

struct FileObject : KObject {
  const char* Kind() const override { return "file"; }
  FKind kind = FKind::File;
  vfs::Resolved where;
  std::string host;
  int fd = -1;
  bool readonly = false;
  bool delete_on_close = false;
  uint64_t pos = 0;
  std::mutex mu;
  // Directory enumeration.
  std::vector<vfs::DirEntry> entries;
  size_t next = 0;
  std::string mask;
  bool listed = false;
  ~FileObject() override {
    if (fd >= 0) close(fd);
    if (delete_on_close && !host.empty()) {
      if (kind == FKind::Dir) rmdir(host.c_str());
      else unlink(host.c_str());
    }
  }
};

struct LinkObject : KObject {
  const char* Kind() const override { return "symlink"; }
  std::string target;
};

std::shared_ptr<FileObject> FileOf(uint32_t h) {
  return std::dynamic_pointer_cast<FileObject>(GetHandle(h));
}

void SetIosb(uint32_t iosb, uint32_t status, uint32_t info) {
  if (!iosb) return;
  Wr32(iosb, status);
  Wr32(iosb + 4, info);
}

// Full object path from OBJECT_ATTRIBUTES {RootDirectory, ObjectName, Attributes}.
bool ObjectPath(uint32_t oa, std::string* out) {
  if (!oa) return false;
  std::string name = AnsiString(Rd32(oa + 4));
  uint32_t root = Rd32(oa);
  if (root && root != 0xFFFFFFFD) {  // -3 = ObDosDevicesDirectory: name is "\??\"-relative
    auto f = FileOf(root);
    if (!f) return false;
    *out = f->where.nt + "\\" + name;
    return true;
  }
  if (root == 0xFFFFFFFD) name = "\\??\\" + name;
  *out = name;
  return true;
}

uint64_t ToFileTime(int64_t unix_s) { return 116444736000000000ull + uint64_t(unix_s) * 10000000ull; }

struct Stat {
  bool dir = false;
  uint64_t size = 0;
  int64_t mtime = 0, atime = 0, ctime = 0;
};

bool StatOf(FileObject& f, Stat* s) {
  struct stat st;
  int r = f.fd >= 0 ? fstat(f.fd, &st) : stat(f.host.c_str(), &st);
  if (r != 0) return false;
  s->dir = S_ISDIR(st.st_mode);
  s->size = s->dir ? 0 : uint64_t(st.st_size);
  s->mtime = st.st_mtime;
  s->atime = st.st_atime;
  s->ctime = st.st_ctime;
  return true;
}

uint32_t Attributes(const FileObject& f, bool dir) {
  if (dir) return kAttrDirectory | (f.readonly ? kAttrReadOnly : 0);
  return f.readonly ? kAttrReadOnly : kAttrNormal;
}

uint64_t AllocSize(uint64_t size) {
  uint64_t cl = kClusterSectors * kSectorSize;
  return (size + cl - 1) / cl * cl;
}

// Open/create; returns NTSTATUS, sets *info (IoStatusBlock.Information).
uint32_t OpenPath(const std::string& path, uint32_t access, uint32_t disposition,
                  uint32_t options, std::shared_ptr<FileObject>* out, uint32_t* info) {
  *info = kInfoDoesNotExist;
  vfs::Resolved r;
  if (!vfs::Resolve(path, &r)) {
    Warn("open '%s': no such device/link", path.c_str());
    return kObjectPathNotFound;
  }
  auto f = std::make_shared<FileObject>();
  f->where = r;
  if (r.dev == vfs::Device::RawDisk) {
    if (!r.rel.empty()) return kObjectNameNotFound;
    f->kind = FKind::RawDisk;
    f->host = vfs::DeviceRoot(r) + "/partition0.bin";
    f->fd = open(f->host.c_str(), O_RDWR | O_CREAT, 0644);
    if (f->fd < 0) return kAccessDenied;
    *out = f;
    *info = kInfoOpened;
    return kSuccess;
  }
  if (r.volume() && !(options & kOptDirectory)) {
    // A volume open (\Device\Harddisk0\PartitionN, \Device\CdRom0): queries,
    // raw writes from XAPI's cache-partition format (discarded).
    f->kind = FKind::Volume;
    f->host = vfs::DeviceRoot(r);
    f->readonly = r.dev == vfs::Device::Disc;
    *out = f;
    *info = kInfoOpened;
    return kSuccess;
  }
  bool ro_dev = r.dev == vfs::Device::Disc;
  f->readonly = ro_dev;
  std::string host = vfs::FindExisting(r);
  bool exists = !host.empty();
  struct stat st;
  bool is_dir = exists && stat(host.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
  if (exists) {
    if (disposition == kFileCreate) return kObjectNameCollision;
    if ((options & kOptDirectory) && !is_dir) return kNotADirectory;
    if ((options & kOptNonDirectory) && is_dir) return kFileIsADirectory;
  } else {
    if (disposition == kFileOpen || disposition == kFileOverwrite) {
      // Parent missing -> path not found, else name not found.
      vfs::Resolved parent = r;
      size_t slash = parent.rel.rfind('/');
      parent.rel = slash == std::string::npos ? "" : parent.rel.substr(0, slash);
      return vfs::FindExisting(parent).empty() ? kObjectPathNotFound : kObjectNameNotFound;
    }
    if (ro_dev) return kMediaWriteProtected;
    host = vfs::PathForCreate(r);
    if (host.empty()) return kAccessDenied;
    is_dir = options & kOptDirectory;
  }
  f->host = host;
  f->delete_on_close = (options & kOptDeleteOnClose) && !ro_dev;
  if (is_dir) {
    if (!exists && mkdir(host.c_str(), 0755) != 0) return kObjectPathNotFound;
    f->kind = FKind::Dir;
    *info = exists ? kInfoOpened : kInfoCreated;
    *out = f;
    return kSuccess;
  }
  int flags = ro_dev ? O_RDONLY : O_RDWR;
  if (!exists) flags |= O_CREAT;
  bool trunc = exists && (disposition == kFileOverwrite || disposition == kFileOverwriteIf ||
                          disposition == kFileSupersede);
  if (trunc && ro_dev) return kMediaWriteProtected;
  if (trunc) flags |= O_TRUNC;
  f->fd = open(host.c_str(), flags, 0644);
  if (f->fd < 0 && !ro_dev && exists) {  // a read-only host file: open it read-only
    f->fd = open(host.c_str(), O_RDONLY);
    f->readonly = true;
  }
  if (f->fd < 0) {
    if (!exists) return kObjectPathNotFound;
    return kAccessDenied;
  }
  (void)access;
  *info = !exists ? kInfoCreated
          : trunc ? (disposition == kFileSupersede ? kInfoSuperseded : kInfoOverwritten)
                  : kInfoOpened;
  *out = f;
  return kSuccess;
}

uint32_t CreateCommon(Ctx& c, uint32_t ph, uint32_t access, uint32_t oa, uint32_t iosb,
                      uint32_t disposition, uint32_t options) {
  std::string path;
  if (!ObjectPath(oa, &path)) return kObjectNameInvalid;
  std::shared_ptr<FileObject> f;
  uint32_t info = 0;
  uint32_t st = OpenPath(path, access, disposition, options, &f, &info);
  bool first_miss = false;  // an unresolved path: logged once (the game retries some)
  if (st == kObjectPathNotFound) {
    static std::mutex mu;
    static std::set<std::string> seen;
    std::lock_guard<std::mutex> l(mu);
    first_miss = seen.insert(path).second;
  }
  if (g_trace || first_miss)
    Log("open %s (disp %u opt %X) -> %08X%s%s%s", path.c_str(), disposition, options, st,
        f ? " " : "", f ? f->host.c_str() : "", first_miss && !g_trace ? " (logged once)" : "");
  SetIosb(iosb, st, info);
  if (st != kSuccess) return st;
  Wr32(ph, NewHandle(f));
  return kSuccess;
}

void SignalCompletion(uint32_t event_handle, uint32_t apc) {
  if (event_handle)
    if (auto o = GetHandle(event_handle))
      if (o->disp_va) SetEventVa(o->disp_va);
  if (apc) Warn("I/O APC routines are not supported (ignored)");
}

// Root that holds game/extracted and mods/: $HP2_DATA, else the nearest
// ancestor of the executable (or its app bundle's Contents/Resources) with an
// extracted disc (game/extracted/default.xbe), else the current directory.
std::string DataRoot() {
  if (const char* d = std::getenv("HP2_DATA")) return d;
  char buf[4096];
  uint32_t n = sizeof buf;
  if (_NSGetExecutablePath(buf, &n) == 0) {
    char real[PATH_MAX];
    std::string dir = realpath(buf, real) ? real : buf;
    while (dir.size() > 1) {
      dir = dir.substr(0, dir.rfind('/'));
      if (dir.empty()) break;
      for (const std::string& cand : {dir, dir + "/Resources"}) {
        struct stat st;
        if (stat((cand + "/game/extracted/default.xbe").c_str(), &st) == 0) return cand;
      }
    }
  }
  Warn("HP2_DATA unset and no game/extracted next to the executable; using the current directory");
  return ".";
}

}  // namespace

std::string g_data_dir;  // per-profile root of T:\ U:\ Z:\ (and hp2.ini)

// The hybrid slot (Ukrainian text, English speech) exists when the mods give
// the disc a _UKT folder (tools/loc/build_mod.py). By device path: the game's
// XAPI links D: only later.
bool HybridSlotAvailable() {
  vfs::Resolved r;
  return vfs::Resolve("\\Device\\CdRom0\\_UKT\\text.bin", &r) && !vfs::FindExisting(r).empty();
}

// Language key: $HP2_LANG, else Hp2KernelConfig::language, else
// "language=<key>" in <profile data dir>/hp2.ini, else "en". Keys: en (_USA),
// uk (_UK slot + mods/uk: the Ukrainian translation), uk_text (_UKT: the
// Ukrainian text with the original English speech; uk without _UKT). The port
// ships only these (the game's picker lists only them, misc.cpp); others -> en.
std::string LanguageKey() {
  static std::mutex mu;
  static std::string logged;
  std::string key = [] {
    static const char* const kKeys[] = {"en", "uk", "uk_text"};
    std::string k, from;
    if (const char* e = std::getenv("HP2_LANG"); e && *e) k = e, from = "HP2_LANG";
    else if (g_cfg.language) k = g_cfg.language, from = "config";
    else if (FILE* f = std::fopen((g_data_dir + "/hp2.ini").c_str(), "r")) {
      char line[256];
      while (std::fgets(line, sizeof line, f)) {
        std::string l = line;
        while (!l.empty() && std::isspace(uint8_t(l.back()))) l.pop_back();
        if (l.rfind("language=", 0) == 0) k = l.substr(9), from = g_data_dir + "/hp2.ini";
      }
      std::fclose(f);
    }
    if (k.empty()) return std::string("en (default)");
    if (k == "uk_text" && !HybridSlotAvailable()) {
      static bool warned = false;
      if (!warned) Warn("language uk_text (from %s): no _UKT\\text.bin in the mods: using uk", from.c_str());
      warned = true;
      return std::string("uk (no _UKT for uk_text)");
    }
    for (const char* known : kKeys)
      if (k == known) return k + " (from " + from + ")";
    Warn("unknown language '%s' (from %s): using en", k.c_str(), from.c_str());
    return std::string("en (unknown '") + k + "')";
  }();
  std::lock_guard<std::mutex> l(mu);
  if (key != logged) {
    logged = key;
    Log("language %s", key.c_str());
  }
  return key.substr(0, key.find(' '));
}

namespace {
// Writes the profile's hp2.ini template and README.txt when they are missing
// (first run, or the player deleted them).
void WriteProfileTemplates(const std::string& dir) {
  struct stat st;
  auto write = [&](const char* name, const char* text) {
    std::string path = dir + "/" + name;
    if (stat(path.c_str(), &st) == 0) return;
    if (FILE* f = std::fopen(path.c_str(), "w")) {
      std::fputs(text, f);
      std::fclose(f);
    } else {
      Warn("cannot write %s", path.c_str());
    }
  };
  write("hp2.ini",
        "# HP2Port settings for this profile. HP2_* environment variables override them.\n"
        "#\n"
        "# Game language: en = English (US, default), uk = Ukrainian, uk_text = Ukrainian\n"
        "# text with the original English speech.\n"
        "# The game's own Options -> Language Selection (flags) switches too.\n"
        "#language=en\n");
  write("README.txt",
        "HP2Port profile\n"
        "\n"
        "hp2.ini                  settings (language)\n"
        "hdd/partition1/UDATA/    save games (U:\\), one folder per save; back this up\n"
        "hdd/partition1/TDATA/    title data (T:\\)\n"
        "hdd/partition3..5/       cache partitions (X:\\ Y:\\ Z:\\), safe to delete\n"
        "hdd/partition0.bin       the cache partition table (XAPI), safe to delete\n"
        "\n"
        "Another profile: HP2_PROFILE=<name>. Crash reports: ~/Library/Logs/hp2/.\n");
}
}  // namespace

void FileInit() {
  std::string hp2_data = DataRoot();
  vfs::Config vc;
  vc.disc_dir = g_cfg.disc_dir ? g_cfg.disc_dir : hp2_data + "/game/extracted";
  if (g_cfg.data_dir) {
    vc.data_dir = g_cfg.data_dir;
  } else {
    const char* home = std::getenv("HOME");
    const char* profile = std::getenv("HP2_PROFILE");
    vc.data_dir = std::string(home ? home : "/tmp") + "/Library/Application Support/HP2Port/" +
                  (profile ? profile : "default");
  }
  g_data_dir = vc.data_dir;
  // Mods: $HP2_MODS, else the config, else mods/uk when it exists, whatever
  // the language: it only replaces the _UK slot's text and adds glyphs to
  // the fonts, so English is unchanged and the game's own picker (Options ->
  // flags) can switch to Ukrainian at any time.
  std::string mods;
  struct stat st;
  if (const char* m = std::getenv("HP2_MODS")) mods = m;
  else if (g_cfg.mods_dirs) mods = g_cfg.mods_dirs;
  else if (std::string uk = hp2_data + "/mods/uk";
           stat(uk.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
    mods = uk;
  size_t at = 0;
  while (at < mods.size()) {
    size_t colon = mods.find(':', at);
    if (colon == std::string::npos) colon = mods.size();
    if (colon > at) vc.mods_dirs.push_back(mods.substr(at, colon - at));
    at = colon + 1;
  }
  const char* z = std::getenv("HP2_ZCACHE");
  g_zcache = z && *z == '1';
  bool first_run = stat(vc.data_dir.c_str(), &st) != 0;
  vfs::Init(vc);  // creates the profile's hdd/partition* directories
  if (first_run) Log("first run: created profile %s", vc.data_dir.c_str());
  WriteProfileTemplates(vc.data_dir);
  Log("root %s, disc %s, data %s, %zu mods dir(s)%s%s, Z: cache %s", hp2_data.c_str(),
      vc.disc_dir.c_str(), vc.data_dir.c_str(), vc.mods_dirs.size(), mods.empty() ? "" : " ",
      mods.c_str(), g_zcache ? "on" : "off");
}

// NtCreateFile(PHANDLE, DesiredAccess, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
//   PLARGE_INTEGER AllocationSize, FileAttributes, ShareAccess,
//   CreateDisposition, CreateOptions)
KEXPORT(190, NtCreateFile, 9) {
  return CreateCommon(c, Arg(c, 0), Arg(c, 1), Arg(c, 2), Arg(c, 3), Arg(c, 7), Arg(c, 8));
}

// NtOpenFile(PHANDLE, DesiredAccess, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
//   ShareAccess, OpenOptions)
KEXPORT(202, NtOpenFile, 6) {
  return CreateCommon(c, Arg(c, 0), Arg(c, 1), Arg(c, 2), Arg(c, 3), kFileOpen, Arg(c, 5));
}

// NtReadFile(Handle, Event, ApcRoutine, ApcContext, PIO_STATUS_BLOCK, Buffer,
//   Length, PLARGE_INTEGER ByteOffset)
KEXPORT(219, NtReadFile, 8) {
  auto f = FileOf(Arg(c, 0));
  uint32_t iosb = Arg(c, 4), buf = Arg(c, 5), len = Arg(c, 6), poff = Arg(c, 7);
  if (!f) return kInvalidHandle;
  std::lock_guard<std::mutex> l(f->mu);
  uint64_t off = poff ? Rd64(poff) : f->pos;
  uint32_t st = kSuccess, got = 0;
  if (f->kind == FKind::Volume) {
    std::memset(GP(buf), 0, len);
    got = len;
  } else if (f->fd < 0) {
    st = kInvalidDeviceRequest;
  } else {
    ssize_t n = pread(f->fd, GP(buf), len, off_t(off));
    if (n < 0) st = kUnsuccessful;
    else if (n == 0 && len) st = kEndOfFile;
    else got = uint32_t(n);
    if (f->kind == FKind::RawDisk && st == kEndOfFile) {  // unwritten sectors read as zero
      std::memset(GP(buf), 0, len);
      got = len;
      st = kSuccess;
    }
  }
  f->pos = off + got;
  SetIosb(iosb, st, got);
  SignalCompletion(Arg(c, 1), Arg(c, 2));
  return st;
}

// NtWriteFile(Handle, Event, ApcRoutine, ApcContext, PIO_STATUS_BLOCK, Buffer,
//   Length, PLARGE_INTEGER ByteOffset)
KEXPORT(236, NtWriteFile, 8) {
  auto f = FileOf(Arg(c, 0));
  uint32_t iosb = Arg(c, 4), buf = Arg(c, 5), len = Arg(c, 6), poff = Arg(c, 7);
  if (!f) return kInvalidHandle;
  std::lock_guard<std::mutex> l(f->mu);
  uint64_t off = f->pos;
  if (poff) {
    uint64_t v = Rd64(poff);
    if (v == 0xFFFFFFFFFFFFFFFFull) {  // FILE_WRITE_TO_END_OF_FILE
      Stat s;
      off = StatOf(*f, &s) ? s.size : f->pos;
    } else if (v != 0xFFFFFFFFFFFFFFFEull) {  // FILE_USE_FILE_POINTER_POSITION
      off = v;
    }
  }
  uint32_t st = kSuccess, put = 0;
  if (f->kind == FKind::Volume) {
    if (f->readonly) st = kMediaWriteProtected;
    else put = len;  // XAPI formatting the cache partition: discarded
  } else if (f->readonly) {
    st = kMediaWriteProtected;
  } else if (f->fd < 0) {
    st = kInvalidDeviceRequest;
  } else {
    ssize_t n = pwrite(f->fd, GP(buf), len, off_t(off));
    if (n < 0) st = kUnsuccessful;
    else put = uint32_t(n);
  }
  f->pos = off + put;
  SetIosb(iosb, st, put);
  SignalCompletion(Arg(c, 1), Arg(c, 2));
  return st;
}

// NtFlushBuffersFile(Handle, PIO_STATUS_BLOCK)
KEXPORT(198, NtFlushBuffersFile, 2) {
  auto f = FileOf(Arg(c, 0));
  if (!f) return kInvalidHandle;
  if (f->fd >= 0 && !f->readonly) fsync(f->fd);
  SetIosb(Arg(c, 1), kSuccess, 0);
  return kSuccess;
}

// NtQueryInformationFile(Handle, PIO_STATUS_BLOCK, FileInformation, Length, Class)
KEXPORT(211, NtQueryInformationFile, 5) {
  auto f = FileOf(Arg(c, 0));
  uint32_t iosb = Arg(c, 1), out = Arg(c, 2), len = Arg(c, 3), cls = Arg(c, 4);
  if (!f) return kInvalidHandle;
  std::lock_guard<std::mutex> l(f->mu);
  Stat s;
  if (f->kind == FKind::Volume) s.dir = true;
  else if (!StatOf(*f, &s)) return kUnsuccessful;
  bool dir = f->kind == FKind::Dir || f->kind == FKind::Volume;
  uint32_t need = 0;
  auto times = [&](uint32_t at) {
    Wr64(at + 0, ToFileTime(s.ctime));
    Wr64(at + 8, ToFileTime(s.atime));
    Wr64(at + 16, ToFileTime(s.mtime));
    Wr64(at + 24, ToFileTime(s.mtime));
  };
  switch (cls) {
    case 4:  // FileBasicInformation
      need = 0x28;
      if (len < need) break;
      std::memset(GP(out), 0, need);
      times(out);
      Wr32(out + 0x20, Attributes(*f, dir));
      break;
    case 5:  // FileStandardInformation
      need = 0x18;
      if (len < need) break;
      std::memset(GP(out), 0, need);
      Wr64(out + 0, AllocSize(s.size));
      Wr64(out + 8, s.size);
      Wr32(out + 0x10, 1);
      Wr8(out + 0x14, f->delete_on_close);
      Wr8(out + 0x15, dir);
      break;
    case 14:  // FilePositionInformation
      need = 8;
      if (len < need) break;
      Wr64(out, f->pos);
      break;
    case 34:  // FileNetworkOpenInformation
      need = 0x38;
      if (len < need) break;
      std::memset(GP(out), 0, need);
      times(out);
      Wr64(out + 0x20, AllocSize(s.size));
      Wr64(out + 0x28, s.size);
      Wr32(out + 0x30, Attributes(*f, dir));
      break;
    case 6:  // FileInternalInformation
      need = 8;
      if (len < need) break;
      Wr64(out, std::hash<std::string>()(f->host));
      break;
    case 16:  // FileModeInformation
    case 17:  // FileAlignmentInformation
      need = 4;
      if (len < need) break;
      Wr32(out, 0);
      break;
    default:
      Warn("NtQueryInformationFile: class %u not supported (%s)", cls, f->host.c_str());
      SetIosb(iosb, kInvalidParameter, 0);
      return kInvalidParameter;
  }
  if (len < need) {
    SetIosb(iosb, kBufferTooSmall, 0);
    return kBufferTooSmall;
  }
  SetIosb(iosb, kSuccess, need);
  return kSuccess;
}

// NtSetInformationFile(Handle, PIO_STATUS_BLOCK, FileInformation, Length, Class)
KEXPORT(226, NtSetInformationFile, 5) {
  auto f = FileOf(Arg(c, 0));
  uint32_t iosb = Arg(c, 1), in = Arg(c, 2), cls = Arg(c, 4);
  if (!f) return kInvalidHandle;
  std::lock_guard<std::mutex> l(f->mu);
  uint32_t st = kSuccess;
  switch (cls) {
    case 4:  // FileBasicInformation: times/attributes; kept as the host has them
      break;
    case 14:  // FilePositionInformation
      f->pos = Rd64(in);
      break;
    case 19:  // FileAllocationInformation
    case 20:  // FileEndOfFileInformation
      if (f->readonly) st = kMediaWriteProtected;
      else if (f->fd < 0 || ftruncate(f->fd, off_t(Rd64(in))) != 0) st = kUnsuccessful;
      break;
    case 13:  // FileDispositionInformation {BOOLEAN DeleteFile}
      if (f->readonly) st = kCannotDelete;
      else f->delete_on_close = Rd8(in) != 0;
      break;
    case 10: {  // FileRenameInformation {ReplaceIfExists, RootDirectory, OBJECT_STRING FileName}
      if (f->readonly) {
        st = kMediaWriteProtected;
        break;
      }
      std::string name(reinterpret_cast<const char*>(GP(Rd32(in + 12))), Rd16(in + 8));
      uint32_t root = Rd32(in + 4);
      std::string path = name;
      if (root)
        if (auto rf = FileOf(root)) path = rf->where.nt + "\\" + name;
      vfs::Resolved r;
      std::string dst;
      if (!vfs::Resolve(path, &r) || (dst = vfs::PathForCreate(r)).empty()) {
        st = kObjectPathNotFound;
        break;
      }
      if (!Rd8(in) && access(dst.c_str(), F_OK) == 0) {
        st = kObjectNameCollision;
        break;
      }
      if (rename(f->host.c_str(), dst.c_str()) != 0) st = kAccessDenied;
      else {
        f->host = dst;
        f->where = r;
      }
      break;
    }
    default:
      Warn("NtSetInformationFile: class %u not supported (%s)", cls, f->host.c_str());
      st = kInvalidParameter;
  }
  SetIosb(iosb, st, 0);
  return st;
}

// NtQueryVolumeInformationFile(Handle, PIO_STATUS_BLOCK, FsInformation, Length, Class)
KEXPORT(218, NtQueryVolumeInformationFile, 5) {
  auto f = FileOf(Arg(c, 0));
  uint32_t iosb = Arg(c, 1), out = Arg(c, 2), len = Arg(c, 3), cls = Arg(c, 4);
  if (!f) return kInvalidHandle;
  const vfs::Resolved& r = f->where;
  uint32_t need = 0;
  switch (cls) {
    case 3: {  // FileFsSizeInformation
      need = 0x18;
      if (len < need) break;
      uint64_t cluster = kClusterSectors * kSectorSize;
      uint64_t total, avail;
      if (r.dev == vfs::Device::Disc) {
        total = 7ull << 30;
        avail = 0;
      } else if (r.dev == vfs::Device::Partition && r.partition >= 3) {
        total = 750ull << 20;  // the utility (cache) partition
        avail = g_zcache ? total / 2 : 0;  // 0 free: the game keeps its file cache off
      } else {
        total = 8ull << 30;
        avail = 4ull << 30;
      }
      Wr64(out + 0, total / cluster);
      Wr64(out + 8, avail / cluster);
      Wr32(out + 0x10, kClusterSectors);
      Wr32(out + 0x14, kSectorSize);
      break;
    }
    case 1: {  // FileFsVolumeInformation
      const char* label = r.dev == vfs::Device::Disc ? "HP2" : "";
      need = 0x12 + uint32_t(std::strlen(label));
      if (len < 0x12) {
        need = 0x12;
        break;
      }
      std::memset(GP(out), 0, std::min(len, need));
      Wr64(out, ToFileTime(1034856488));
      Wr32(out + 8, 0x4F500032);
      Wr32(out + 0xC, uint32_t(std::strlen(label)));
      std::memcpy(GP(out + 0x11), label, std::min<uint32_t>(len - 0x11, uint32_t(std::strlen(label))));
      break;
    }
    case 4:  // FileFsDeviceInformation {DeviceType, Characteristics}
      need = 8;
      if (len < need) break;
      Wr32(out, r.dev == vfs::Device::Disc ? 0x02 /*CD_ROM*/ : 0x07 /*DISK*/);
      Wr32(out + 4, r.dev == vfs::Device::Disc ? 0x1 /*REMOVABLE*/ : 0);
      break;
    case 5: {  // FileFsAttributeInformation {Attributes, MaxComponentNameLength, NameLength, Name}
      const char* fs = r.dev == vfs::Device::Disc ? "UDF" : "FATX";
      need = 12 + uint32_t(std::strlen(fs));
      if (len < need) break;
      Wr32(out, 0);
      Wr32(out + 4, 42);
      Wr32(out + 8, uint32_t(std::strlen(fs)));
      std::memcpy(GP(out + 12), fs, std::strlen(fs));
      break;
    }
    default:
      Warn("NtQueryVolumeInformationFile: class %u not supported", cls);
      SetIosb(iosb, kInvalidParameter, 0);
      return kInvalidParameter;
  }
  if (len < need) {
    SetIosb(iosb, kBufferTooSmall, 0);
    return kBufferTooSmall;
  }
  SetIosb(iosb, kSuccess, need);
  return kSuccess;
}

// NtQueryDirectoryFile(Handle, Event, ApcRoutine, ApcContext, PIO_STATUS_BLOCK,
//   FileInformation, Length, FileInformationClass, POBJECT_STRING FileMask,
//   BOOLEAN RestartScan): one FILE_DIRECTORY_INFORMATION per call (Xbox).
KEXPORT(207, NtQueryDirectoryFile, 10) {
  auto f = FileOf(Arg(c, 0));
  uint32_t iosb = Arg(c, 4), out = Arg(c, 5), len = Arg(c, 6), cls = Arg(c, 7),
           pmask = Arg(c, 8);
  bool restart = Arg(c, 9) & 0xFF;
  if (!f) return kInvalidHandle;
  if (f->kind != FKind::Dir && f->kind != FKind::Volume) return kInvalidParameter;
  if (cls != 1) Warn("NtQueryDirectoryFile: class %u treated as FileDirectoryInformation", cls);
  std::lock_guard<std::mutex> l(f->mu);
  bool first = !f->listed || restart;
  if (first) {
    f->entries = vfs::List(f->where);
    f->next = 0;
    f->listed = true;
    f->mask = pmask ? AnsiString(pmask) : std::string("*");
  }
  while (f->next < f->entries.size() && !vfs::MatchMask(f->mask, f->entries[f->next].name)) f->next++;
  if (f->next >= f->entries.size()) {
    uint32_t st = first ? kNoSuchFile : kNoMoreFiles;
    SetIosb(iosb, st, 0);
    return st;
  }
  const vfs::DirEntry& e = f->entries[f->next];
  uint32_t need = 0x40 + uint32_t(e.name.size());
  if (len < 0x40) {
    SetIosb(iosb, kBufferTooSmall, 0);
    return kBufferTooSmall;
  }
  std::memset(GP(out), 0, 0x40);
  Wr32(out + 0x04, uint32_t(f->next));
  for (int i = 0; i < 4; i++) Wr64(out + 0x08 + 8 * i, ToFileTime(e.mtime));
  Wr64(out + 0x28, e.size);
  Wr64(out + 0x30, AllocSize(e.size));
  Wr32(out + 0x38, e.dir ? kAttrDirectory : (f->readonly ? kAttrReadOnly : kAttrNormal));
  Wr32(out + 0x3C, uint32_t(e.name.size()));
  uint32_t copy = std::min<uint32_t>(uint32_t(e.name.size()), len - 0x40);
  std::memcpy(GP(out + 0x40), e.name.data(), copy);
  if (copy < e.name.size()) {
    SetIosb(iosb, kBufferOverflow, len);
    return kBufferOverflow;
  }
  f->next++;
  SetIosb(iosb, kSuccess, need);
  return kSuccess;
}

// NtDeviceIoControlFile(Handle, Event, ApcRoutine, ApcContext, PIO_STATUS_BLOCK,
//   IoControlCode, InputBuffer, InputBufferLength, OutputBuffer, OutputBufferLength)
KEXPORT(196, NtDeviceIoControlFile, 10) {
  auto f = FileOf(Arg(c, 0));
  uint32_t iosb = Arg(c, 4), code = Arg(c, 5), in = Arg(c, 6), inlen = Arg(c, 7), out = Arg(c, 8),
           outlen = Arg(c, 9);
  if (!f) return kInvalidHandle;
  uint64_t part_bytes = 750ull << 20;
  switch (code) {
    case 0x4D014: {  // IOCTL_SCSI_PASS_THROUGH_DIRECT (DVD drive)
      // SCSI_PASS_THROUGH_DIRECT: Length u16 @0, ScsiStatus @2, DataIn @8,
      // DataTransferLength @0xC, DataBuffer @0x14, Cdb[16] @0x1C.
      if (inlen < 0x2C || f->where.dev != vfs::Device::Disc) break;
      uint32_t len = Rd32(in + 0xC), buf = Rd32(in + 0x14);
      uint8_t op = Rd8(in + 0x1C), page = Rd8(in + 0x1E) & 0x3F;
      if (op == 0x5A && page == 0x3E && buf && len >= 0xD) {
        // MODE SENSE(10), Xbox DVD security page. XAPI (FUN_0011131C) boots
        // only if [0xB] == 1 (authenticated) and [0xA], [0xC] != 0 (an
        // Xbox game disc, game partition selected).
        std::memset(GP(buf), 0, len);
        Wr16(buf, 0x1A00);   // mode data length 26 (big-endian)
        Wr8(buf + 8, 0x3E);  // page code
        Wr8(buf + 9, 0x12);  // page length
        Wr8(buf + 0xA, 1);
        Wr8(buf + 0xB, 1);
        Wr8(buf + 0xC, 1);
        Wr8(in + 2, 0);      // SCSI GOOD
        SetIosb(iosb, kSuccess, len);
        return kSuccess;
      }
      Warn("SCSI pass-through: CDB %02X page %02X not supported", op, page);
      break;
    }
    case 0x70000:  // IOCTL_DISK_GET_DRIVE_GEOMETRY -> DISK_GEOMETRY
      if (outlen < 0x18) return kBufferTooSmall;
      Wr64(out, part_bytes / (255 * 63 * kSectorSize));  // Cylinders
      Wr32(out + 8, 12);                                  // FixedMedia
      Wr32(out + 0xC, 255);
      Wr32(out + 0x10, 63);
      Wr32(out + 0x14, kSectorSize);
      SetIosb(iosb, kSuccess, 0x18);
      return kSuccess;
    case 0x74004:  // IOCTL_DISK_GET_PARTITION_INFO -> PARTITION_INFORMATION
      if (outlen < 0x20) return kBufferTooSmall;
      std::memset(GP(out), 0, 0x20);
      Wr64(out + 0, 0x80000ull);   // StartingOffset
      Wr64(out + 8, part_bytes);   // PartitionLength
      Wr32(out + 0x14, uint32_t(f->where.partition));
      Wr8(out + 0x1A, 1);          // RecognizedPartition
      SetIosb(iosb, kSuccess, 0x20);
      return kSuccess;
    default:
      Warn("NtDeviceIoControlFile: IOCTL %08X on %s not supported", code, f->where.nt.c_str());
      break;
  }
  SetIosb(iosb, kInvalidDeviceRequest, 0);
  return kInvalidDeviceRequest;
}

// NtFsControlFile (same arguments): dismount/lock requests -> success.
KEXPORT(200, NtFsControlFile, 10) {
  auto f = FileOf(Arg(c, 0));
  if (!f) return kInvalidHandle;
  if (g_trace) Log("NtFsControlFile %08X on %s", Arg(c, 5), f->where.nt.c_str());
  SetIosb(Arg(c, 4), kSuccess, 0);
  return kSuccess;
}

// ---- symbolic links ----------------------------------------------------------------------------
// IoCreateSymbolicLink(POBJECT_STRING SymbolicLinkName, POBJECT_STRING DeviceName)
KEXPORT(67, IoCreateSymbolicLink, 2) {
  std::string link = AnsiString(Arg(c, 0)), dev = AnsiString(Arg(c, 1));
  // XAPI maps T:/U: to "\Device\Harddisk0\partition1\TDATA\<title id>" etc.
  if (g_trace) Log("IoCreateSymbolicLink %s -> %s", link.c_str(), dev.c_str());
  return vfs::CreateLink(link, dev) ? kSuccess : kObjectNameCollision;
}

// NtOpenSymbolicLinkObject(PHANDLE, POBJECT_ATTRIBUTES)
KEXPORT(203, NtOpenSymbolicLinkObject, 2) {
  std::string path;
  if (!ObjectPath(Arg(c, 1), &path)) return kObjectNameInvalid;
  auto o = std::make_shared<LinkObject>();
  if (!vfs::GetLink(path, &o->target)) return kObjectNameNotFound;
  Wr32(Arg(c, 0), NewHandle(o));
  return kSuccess;
}

// NtQuerySymbolicLinkObject(Handle, POBJECT_STRING LinkTarget, PULONG ReturnedLength)
KEXPORT(215, NtQuerySymbolicLinkObject, 3) {
  auto o = std::dynamic_pointer_cast<LinkObject>(GetHandle(Arg(c, 0)));
  if (!o) return kInvalidHandle;
  uint32_t s = Arg(c, 1);
  uint32_t n = uint32_t(o->target.size());
  if (Arg(c, 2)) Wr32(Arg(c, 2), n);
  if (Rd16(s + 2) < n) return kBufferTooSmall;
  std::memcpy(GP(Rd32(s + 4)), o->target.data(), n);
  Wr16(s, uint16_t(n));
  return kSuccess;
}

// FscSetCacheSize(NumberOfCachePages): the host has its own file cache.
KEXPORT(37, FscSetCacheSize, 1) { return kSuccess; }

}  // namespace hp2k
