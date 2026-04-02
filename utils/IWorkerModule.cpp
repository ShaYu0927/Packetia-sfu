#include "IWorkerModule.h"

void WorkerModuleRegistry::Add(ModulePtr module)
{
    if (module)
    {
        modules_.push_back(std::move(module));
    }
}

int WorkerModuleRegistry::RegisterAll()
{
    for (const auto& module : modules_)
    {
        if (!module)
            continue;
        
        int ret = module->Register();
        if (ret != 0)
        {
            for (auto it = registered_.rbegin(); it != registered_.rend(); ++it)
            {
                (*it)->Unregister(true);
            }
            registered_.clear();
            return ret;
        }

        registered_.push_back(module);
    }
    return 0;
}

void WorkerModuleRegistry::UnregisterAll(bool drain)
{
    for (auto it = registered_.rbegin(); it != registered_.rend(); ++it)
    {
        if (*it)
        {
            (*it)->Unregister(drain);
        }
    }
    registered_.clear();
}
