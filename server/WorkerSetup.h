#ifndef PACKETIA_SERVER_WORKERSETUP_H_
#define PACKETIA_SERVER_WORKERSETUP_H_

#include "AppConfig.h"

namespace server
{
class ServerLauncher;

void AddWorkerPools(ServerLauncher& launcher, const config::WorkerOptions& options = {});
}

#endif // PACKETIA_SERVER_WORKERSETUP_H_
