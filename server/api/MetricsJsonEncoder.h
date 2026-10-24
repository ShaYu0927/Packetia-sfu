#pragma once
#include <string>
namespace service { struct MonitorSnapshot; struct ServiceHealth; }
namespace server::api
{
std::string EncodeMetricsJson(const service::MonitorSnapshot& snapshot, const service::ServiceHealth& health);
}
