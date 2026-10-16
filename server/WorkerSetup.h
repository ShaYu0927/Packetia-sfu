#ifndef PACKETIA_SERVER_WORKERSETUP_H_
#define PACKETIA_SERVER_WORKERSETUP_H_

#include "AppConfig.h"

namespace server
{
class ServerLauncher;

// Configure the process-wide pools; the launcher owns their startup/shutdown.
// Recording owns a separate registry so it can drain and finalize its files
// before releasing the recording service's state.
void AddWorkerPools(ServerLauncher& launcher, const config::WorkerOptions& options = {});
}

#endif // PACKETIA_SERVER_WORKERSETUP_H_
