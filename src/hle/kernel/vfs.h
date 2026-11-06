#pragma once
// Xbox object-namespace paths -> host paths. No guest memory involved, so it
// is unit-testable on its own.
//
//   \??\D:\x            -> symbolic link \??\D: -> \Device\CdRom0\x
//   \Device\CdRom0\x    -> first of <mods dir>/x ..., <disc dir>/x (case-insensitive)
//   \Device\Harddisk0\Partition1\x -> <data dir>/hdd/partition1/x  (E:, T:, U:)
//   \Device\Harddisk0\PartitionN\x -> <data dir>/hdd/partitionN/x  (Z: cache, N>=3)
//   \Device\Harddisk0\Partition0   -> raw sectors in <data dir>/hdd/partition0.bin

#include <cstdint>
#include <string>
#include <vector>

namespace hp2k::vfs {

enum class Device { None, Disc, Partition, RawDisk };

struct Resolved {
  Device dev = Device::None;
  int partition = 0;        // Device::Partition
  std::string rel;          // path below the device root, '/'-separated, no leading '/'
  std::string nt;           // normalised NT path (\Device\...), for names/queries
  bool volume() const { return rel.empty(); }
};

struct Config {
  std::string disc_dir;
  std::vector<std::string> mods_dirs;  // first wins
  std::string data_dir;
};

void Init(const Config& cfg);
const Config& Cfg();

// Symbolic links (\??\X: -> \Device\...). Case-insensitive names.
bool CreateLink(const std::string& link, const std::string& target);  // false: exists
bool GetLink(const std::string& link, std::string* target);

// Resolves an absolute object path (after RootDirectory was applied).
// False if it names nothing we provide.
bool Resolve(const std::string& path, Resolved* out);

// Host path of an existing entry (case-insensitive per component). For the
// disc, mods dirs come first. Empty if it does not exist.
std::string FindExisting(const Resolved& r);
// Host path for creating `r` (existing components keep their on-disk case).
// Empty for read-only devices.
std::string PathForCreate(const Resolved& r);
// Host directory of the device root (disc dir for the disc).
std::string DeviceRoot(const Resolved& r);

struct DirEntry {
  std::string name;
  std::string host_path;
  bool dir = false;
  uint64_t size = 0;
  int64_t mtime = 0;  // unix seconds
};
// Directory listing, merged over mods dirs for the disc (first wins), sorted.
std::vector<DirEntry> List(const Resolved& r);

// Xbox/NT wildcard match (case-insensitive; '*', '?', "*.*" = everything).
bool MatchMask(const std::string& mask, const std::string& name);

std::string Lower(std::string s);

}  // namespace hp2k::vfs
