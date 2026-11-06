// [video] keys of ~/Library/Application Support/HP2/settings.toml (Render).
// The file is shared with other sections ([gamepad], [camera], ... of the
// XInput HLE); these functions read and rewrite only [video] keys and keep
// every other line as it is.
#pragma once

#include <string>

namespace hp2d3d {

// The settings file path ($HOME/Library/Application Support/HP2/settings.toml).
std::string VideoSettingsPath();

// [video] <key> as a boolean: true/false/1/0/yes/no/on/off; `def` when the
// file, the section or the key is missing or the value is not a boolean.
bool VideoSettingBool(const std::string& text, const char* key, bool def);

// `text` with [video] <key> = <value> set: replaces the key's line, or adds
// it at the end of the [video] section, or adds the section at the end.
std::string WithVideoSetting(const std::string& text, const char* key, const std::string& value);

// Whole-file helpers (missing file = empty text; the write creates the
// directory and replaces the file atomically). False on an I/O error.
std::string ReadTextFile(const std::string& path);
bool WriteTextFile(const std::string& path, const std::string& text);

}  // namespace hp2d3d
