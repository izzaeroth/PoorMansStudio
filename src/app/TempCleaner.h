#pragma once
#include <filesystem>
#include <string>

namespace mw::app
{
    struct TempCleanResult
    {
        bool success = false;
        int removedItems = 0;
        std::string message;
    };

    class TempCleaner
    {
    public:
        static TempCleanResult cleanTempFolder();
        static void cleanOldMxlExtractsOnStartup();
        static TempCleanResult cleanAppOwnedTempFiles();
    };
}
