#include "app/TempCleaner.h"
#include "app/AppPaths.h"

#include <system_error>

namespace mw::app
{
    TempCleanResult TempCleaner::cleanTempFolder()
    {
        TempCleanResult result;
        const auto temp = AppPaths::tempFolder();

        std::error_code ignored;
        std::filesystem::create_directories(temp, ignored);

        if (!std::filesystem::exists(temp))
        {
            result.success = true;
            result.message = "Temp folder did not exist; created it.";
            return result;
        }

        for (const auto& entry : std::filesystem::directory_iterator(temp, ignored))
        {
            std::filesystem::remove_all(entry.path(), ignored);
            ++result.removedItems;
        }

        result.success = true;
        result.message = "Temp folder cleaned. Removed " + std::to_string(result.removedItems) + " item(s).";
        return result;
    }


    TempCleanResult TempCleaner::cleanAppOwnedTempFiles()
    {
        TempCleanResult result;
        const auto temp = AppPaths::tempFolder();
        std::error_code ignored;
        std::filesystem::create_directories(temp, ignored);

        const auto previews = AppPaths::previewFolder();
        if (std::filesystem::exists(previews, ignored))
        {
            std::filesystem::remove_all(previews, ignored);
            ++result.removedItems;
        }

        const auto audioClipRender = AppPaths::tempFolder() / "audioclip_render";
        if (std::filesystem::exists(audioClipRender, ignored))
        {
            std::filesystem::remove_all(audioClipRender, ignored);
            ++result.removedItems;
        }

        const auto projects = AppPaths::projectsFolder();
        if (std::filesystem::exists(projects, ignored))
        {
            for (const auto& projectEntry : std::filesystem::directory_iterator(projects, ignored))
            {
                const auto audioTemp = projectEntry.path() / "input" / "audio" / "temp";
                if (std::filesystem::exists(audioTemp, ignored))
                {
                    std::filesystem::remove_all(audioTemp, ignored);
                    ++result.removedItems;
                }
            }
        }

        if (std::filesystem::exists(temp, ignored))
        {
            for (const auto& entry : std::filesystem::directory_iterator(temp, ignored))
            {
                const auto name = entry.path().filename().string();
                if (name.rfind("mxl_extract_", 0) == 0
                    || name.rfind("pms_preview_", 0) == 0
                    || name == "audioclip_render"
                    || name.rfind("preview_", 0) == 0)
                {
                    std::filesystem::remove_all(entry.path(), ignored);
                    ++result.removedItems;
                }
            }
        }

        result.success = true;
        result.message = "App-owned temp files cleaned. Removed " + std::to_string(result.removedItems) + " item(s).";
        return result;
    }

    void TempCleaner::cleanOldMxlExtractsOnStartup()
    {
        const auto temp = AppPaths::tempFolder();
        std::error_code ignored;
        std::filesystem::create_directories(temp, ignored);

        const auto previews = mw::app::AppPaths::previewFolder();
        std::filesystem::remove_all(previews, ignored);

        const auto audioClipRender = mw::app::AppPaths::tempFolder() / "audioclip_render";
        std::filesystem::remove_all(audioClipRender, ignored);

        if (!std::filesystem::exists(temp))
            return;

        for (const auto& entry : std::filesystem::directory_iterator(temp, ignored))
        {
            const auto name = entry.path().filename().string();
            if (name.rfind("mxl_extract_", 0) == 0)
                std::filesystem::remove_all(entry.path(), ignored);
        }
    }
}
