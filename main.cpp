#include "RuntimeGuards.h"

// Keep the OBSE public ABI unchanged; implementation and state live in RuntimeGuards.
extern "C" bool OBSEPlugin_Query(const OBSEInterface* obse, PluginInfo* info)
{
    return OBMEFix::Query(obse, info);
}

extern "C" bool OBSEPlugin_Load(const OBSEInterface* obse)
{
    return OBMEFix::Load(obse);
}
