#pragma once
#include "config/AppConfig.h"

namespace server {
// Compatibility alias; configuration now belongs to the central config module.
using ServerConfig = config::AppConfig;
}
