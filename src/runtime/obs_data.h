// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <memory>
#include <string>

#include "runtime/obs_api.h"

// Owning pointers to libobs's settings objects (obs_data), and the reads that
// pre-flight and the loader's callers share.
namespace knobs::runtime {

struct DataReleaser {
  const ObsApi* api;
  void operator()(obs_data_t* data) const { api->obs_data_release(data); }
};
using DataPtr = std::unique_ptr<obs_data_t, DataReleaser>;

struct ArrayReleaser {
  const ObsApi* api;
  void operator()(obs_data_array_t* array) const { api->obs_data_array_release(array); }
};
using ArrayPtr = std::unique_ptr<obs_data_array_t, ArrayReleaser>;

inline DataPtr Own(const ObsApi& api, obs_data_t* data) { return DataPtr(data, DataReleaser{&api}); }
inline ArrayPtr Own(const ObsApi& api, obs_data_array_t* array) { return ArrayPtr(array, ArrayReleaser{&api}); }

// A string `data` has, or "" if it has none.
inline std::string String(const ObsApi& api, obs_data_t* data, const char* name) {
  const char* value = api.obs_data_get_string(data, name);
  return value ? value : "";
}

// libobs creates a source by its versioned type, falling back to its type
// (obs_load_source_type).
inline std::string VersionedId(const ObsApi& api, obs_data_t* data) {
  std::string id = String(api, data, "versioned_id");
  return id.empty() ? String(api, data, "id") : id;
}

}  // namespace knobs::runtime
