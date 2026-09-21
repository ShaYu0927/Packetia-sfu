#include "ServerApp.h"

int main()
{
    server::ServerApp app(server::ServerConfig::FromEnvironment());
    if (!app.Start())
    {
        return -1;
    }
    app.Run();
    app.Stop();
    return 0;
}
