#pragma once
#include "obse/PluginAPI.h"

namespace OBMEFix
{
    bool Query(const OBSEInterface* obse, PluginInfo* info);
    bool Load(const OBSEInterface* obse);
}
