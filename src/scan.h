#pragma once
#include <functional>
#include <string>
#include <vector>
#include "config.h"

namespace ob {
// Looks through the plugin folders. Each file new or changed since the last
// scan is opened in a child process ("--describe"), so a plugin that crashes
// while loading only leaves itself out. Results are kept in the config.
std::vector<PluginInfo> scanPlugins(Config& cfg, bool full, std::function<void(const std::string&)> progress);

// The child's side: prints a JSON array of what the file holds.
int describeMain(const std::string& format, const std::string& path);

std::string selfPath();
}
