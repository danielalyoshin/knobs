// SPDX-License-Identifier: GPL-2.0-or-later
#include "tray/settings_file.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <format>
#include <string>
#include <system_error>
#include <vector>

#include "import/obs_ini.h"
#include "util/text_file.h"
#include "util/win_strings.h"

namespace knobs::tray {
namespace {

std::string Escape(std::string_view value) {
  std::string escaped;
  for (const char c : value) {
    switch (c) {
      case '\\':
        escaped += "\\\\";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      default:
        escaped += c;
    }
  }
  return escaped;
}

void Line(std::string& text, std::string_view key, std::string_view value) {
  text += std::format("{}={}\n", key, Escape(value));
}

// As libobs's config_get_bool reads one: "true", or a non-zero number.
// Anything else that isn't "false" or "0" keeps the default.
bool ReadBool(const import::ObsIni& ini, std::string_view section, std::string_view key, bool fallback) {
  const auto value = ini.Get(section, key);
  if (!value) return fallback;
  const std::string lower = AsciiLower(*value);
  if (lower == "true" || lower == "1") return true;
  if (lower == "false" || lower == "0") return false;
  return fallback;
}

std::string_view DoorName(Door door) {
  switch (door) {
    case Door::kObsUser:
      return "obs";
    case Door::kNewToObs:
      return "new";
    case Door::kNone:
      break;
  }
  return "";
}

}  // namespace

std::string FormatSettings(const SavedSettings& saved) {
  const core::Settings& settings = saved.core;
  std::string text = "[OBS]\n";
  if (settings.obs_dir) Line(text, "Install", ToUtf8(*settings.obs_dir));
  if (settings.obs_config) Line(text, "Settings", ToUtf8(*settings.obs_config));
  Line(text, "PauseWhileOpen", settings.pause_for_obs ? "true" : "false");
  text += "\n[Audio]\n";
  if (!settings.mic.empty()) Line(text, "Mic", settings.mic);
  if (!settings.cable.empty()) {
    Line(text, "Cable", settings.cable);
    if (!settings.cable_name.empty()) Line(text, "CableName", settings.cable_name);
  }
  const FirstRunProgress& first_run = saved.first_run;
  text += "\n[Setup]\n";
  Line(text, "Done", first_run.done ? "true" : "false");
  if (!first_run.done && first_run.door != Door::kNone) {
    Line(text, "Door", DoorName(first_run.door));
    Line(text, "Page", PageName(first_run.reached));
  }
  const UpdateSettings& updates = saved.updates;
  text += "\n[Updates]\n";
  Line(text, "Check", updates.check ? "true" : "false");
  Line(text, "Install", updates.install ? "true" : "false");
  const auto time = [&text](std::string_view key, const std::optional<std::chrono::system_clock::time_point>& when) {
    if (!when) return;
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(when->time_since_epoch());
    Line(text, key, std::to_string(seconds.count()));
  };
  time("Checked", updates.checked);
  if (updates.latest) Line(text, "Latest", updates.latest->ToString());
  time("Seen", updates.seen);
  if (updates.reminded) Line(text, "Reminded", updates.reminded->ToString());
  if (updates.failures > 0) Line(text, "Failures", std::to_string(updates.failures));
  time("Failed", updates.failed);
  if (updates.tried) Line(text, "Tried", updates.tried->ToString());
  if (updates.installing) Line(text, "Installing", updates.installing->ToString());
  if (saved.warned) {
    std::string warned;
    for (const std::string& key : *saved.warned) warned += (warned.empty() ? "" : "\n") + key;
    text += "\n[Notices]\n";
    Line(text, "Warned", warned);
  }
  return text;
}

SavedSettings ParseSettings(std::string_view text) {
  const import::ObsIni ini = import::ObsIni::Parse(text);
  SavedSettings saved;
  core::Settings& settings = saved.core;
  const auto path = [&ini](std::string_view key) -> std::optional<std::filesystem::path> {
    const auto value = ini.Get("OBS", key);
    if (!value || value->empty()) return std::nullopt;
    return std::filesystem::path(FromUtf8(*value));
  };
  settings.obs_dir = path("Install");
  settings.obs_config = path("Settings");
  settings.pause_for_obs = ReadBool(ini, "OBS", "PauseWhileOpen", settings.pause_for_obs);
  settings.mic = ini.Get("Audio", "Mic").value_or("");
  settings.cable = ini.Get("Audio", "Cable").value_or("");
  if (!settings.cable.empty()) settings.cable_name = ini.Get("Audio", "CableName").value_or("");

  FirstRunProgress& first_run = saved.first_run;
  first_run.done = ReadBool(ini, "Setup", "Done", first_run.done);
  if (!first_run.done) {
    const std::string door = ini.Get("Setup", "Door").value_or("");
    for (const Door known : {Door::kObsUser, Door::kNewToObs}) {
      if (door == DoorName(known)) first_run.door = known;
    }
    // Only the first door's pages from the mic on are steps to resume at.
    const auto page = PageNamed(ini.Get("Setup", "Page").value_or(""));
    if (page && *page >= FirstRunPage::kMic) first_run.reached = *page;
  }

  UpdateSettings& updates = saved.updates;
  updates.check = ReadBool(ini, "Updates", "Check", updates.check);
  updates.install = ReadBool(ini, "Updates", "Install", updates.install);
  // A whole positive number, or nothing.
  const auto number = [&ini](std::string_view key) -> std::optional<long long> {
    const auto value = ini.Get("Updates", key);
    if (!value) return std::nullopt;
    long long n = 0;
    const auto [end, error] = std::from_chars(value->data(), value->data() + value->size(), n);
    if (error != std::errc{} || end != value->data() + value->size() || n <= 0) return std::nullopt;
    return n;
  };
  const auto time = [&number](std::string_view key) -> std::optional<std::chrono::system_clock::time_point> {
    if (const auto seconds = number(key)) return std::chrono::system_clock::time_point(std::chrono::seconds(*seconds));
    return std::nullopt;
  };
  updates.checked = time("Checked");
  updates.latest = update::ParseVersion(ini.Get("Updates", "Latest").value_or(""));
  // What's kept about the latest release means nothing without it.
  if (updates.latest) {
    updates.seen = time("Seen");
    updates.reminded = update::ParseVersion(ini.Get("Updates", "Reminded").value_or(""));
    updates.failures = static_cast<int>(std::min<long long>(number("Failures").value_or(0), 1000));
    updates.failed = time("Failed");
  }
  updates.tried = update::ParseVersion(ini.Get("Updates", "Tried").value_or(""));
  updates.installing = update::ParseVersion(ini.Get("Updates", "Installing").value_or(""));

  if (const auto warned = ini.Get("Notices", "Warned")) {
    std::vector<std::string>& keys = saved.warned.emplace();
    for (size_t start = 0; start <= warned->size();) {
      const size_t end = std::min(warned->find('\n', start), warned->size());
      if (end > start) keys.push_back(warned->substr(start, end - start));
      start = end + 1;
    }
  }
  return saved;
}

Result<SavedSettings> LoadSettings(const std::filesystem::path& file) {
  std::error_code ec;
  if (!std::filesystem::exists(file, ec) && !ec) return SavedSettings{};
  auto text = ReadText(file);
  if (!text) return Error{text.error()};
  return ParseSettings(*text);
}

Status SaveSettings(const std::filesystem::path& file, const SavedSettings& settings) {
  return WriteText(file, FormatSettings(settings));
}

}  // namespace knobs::tray
