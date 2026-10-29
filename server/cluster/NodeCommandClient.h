#pragma once
#include "RoomAdminService.h"
#include <map>
namespace server::cluster
{
service::meeting::RoomCommandSender MakeNodeCommandSender(std::map<std::string, std::string> credentials);
}
