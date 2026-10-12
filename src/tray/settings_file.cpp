// SPDX-License-Identifier: GPL-2.0-or-later
#include "tray/settings_file.h"

#include <charconv>
#include <chrono>
#include <format>
#include <string>
#include <system_error>

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
  if (updates.checked) {
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(updates.checked->time_since_epoch());
    Line(text, "Checked", std::to_string(seconds.count()));
  }
  if (updates.latest) Line(text, "Latest", updates.latest->ToString());
  if (updates.tried) Line(text, "Tried", updates.tried->ToString());
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
  if (const auto checked = ini.Get("Updates", "Checked")) {
    long long seconds = 0;
    const auto [end, error] = std::from_chars(checked->data(), checked->data() + checked->size(), seconds);
    if (error == std::errc{} && end == checked->data() + checked->size() && seconds > 0) {
      updates.checked = std::chrono::system_clock::time_point(std::chrono::seconds(seconds));
    }
  }
  updates.latest = update::ParseVersion(ini.Get("Updates", "Latest").value_or(""));
  updates.tried = update::ParseVersion(ini.Get("Updates", "Tried").value_or(""));
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
