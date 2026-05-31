#include "vst/VstGraphicsProfile.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi.h>
#pragma comment(lib, "dxgi.lib")
#endif

namespace
{
    std::string nowLocalIsoLike()
    {
        const auto now = std::chrono::system_clock::now();
        const auto t = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        std::ostringstream out;
        out << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
        return out.str();
    }

#if defined(_WIN32)
    std::string wideToUtf8(const wchar_t* text)
    {
        if (text == nullptr || *text == L'\0')
            return {};

        const int needed = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
        if (needed <= 0)
            return {};

        std::string result(static_cast<std::size_t>(needed - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), needed, nullptr, nullptr);
        return result;
    }

    std::string vendorFromId(unsigned int vendorId)
    {
        switch (vendorId)
        {
            case 0x8086: return "Intel";
            case 0x10de: return "NVIDIA";
            case 0x1002:
            case 0x1022: return "AMD";
            case 0x1414: return "Microsoft";
            default:
            {
                std::ostringstream out;
                out << "Vendor 0x" << std::hex << vendorId;
                return out.str();
            }
        }
    }

    std::string adapterTypeFromDxgiFlags(unsigned int flags)
    {
        return (flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 ? "Software" : "Hardware";
    }
#endif
}

namespace mw::vst
{
    GraphicsProfile VstGraphicsProfileDetector::detect(bool firstLaunchAutoDetect)
    {
        GraphicsProfile profile;
        profile.detected = true;
        profile.source = firstLaunchAutoDetect ? "firstLaunchAutoDetect" : "manualRefresh";
        profile.lastDetectedLocal = nowLocalIsoLike();
        profile.preferredPluginGpuId = "auto";

#if defined(_WIN32)
        IDXGIFactory1* factory = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) && factory != nullptr)
        {
            for (UINT i = 0; ; ++i)
            {
                IDXGIAdapter1* adapter = nullptr;
                if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND)
                    break;

                if (adapter == nullptr)
                    continue;

                DXGI_ADAPTER_DESC1 desc{};
                if (SUCCEEDED(adapter->GetDesc1(&desc)))
                {
                    GraphicsAdapterInfo info;
                    info.name = wideToUtf8(desc.Description);
                    info.vendor = vendorFromId(desc.VendorId);
                    info.videoMemoryMb = static_cast<unsigned long long>(desc.DedicatedVideoMemory / (1024ull * 1024ull));
                    info.type = adapterTypeFromDxgiFlags(desc.Flags);

                    std::ostringstream id;
                    id << info.vendor << "-" << std::hex << desc.DeviceId << "-" << desc.SubSysId << "-" << desc.Revision;
                    info.id = id.str();

                    profile.adapters.push_back(std::move(info));
                }

                adapter->Release();
            }

            factory->Release();
        }

        profile.monitorCount = GetSystemMetrics(SM_CMONITORS);
        profile.mixedDpi = false; // Exact per-monitor DPI is left manual-safe for now; warnings still mention multi-monitor risk.
#else
        GraphicsAdapterInfo info;
        info.name = "Unknown graphics adapter";
        info.vendor = "Unknown";
        info.id = "unknown";
        info.type = "Hardware";
        profile.adapters.push_back(std::move(info));
        profile.monitorCount = 1;
#endif

        if (profile.adapters.empty())
        {
            GraphicsAdapterInfo info;
            info.name = "Unknown graphics adapter";
            info.vendor = "Unknown";
            info.id = "unknown";
            info.type = "Hardware";
            profile.adapters.push_back(std::move(info));
        }

        return profile;
    }
}
