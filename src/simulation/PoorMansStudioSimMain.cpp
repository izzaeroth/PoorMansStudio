#include "simulation/SimulationRegressionSuite.h"
#include "app/VersionNumber.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    void printHelp()
    {
        std::cout
            << "PoorMansStudioSim " << mw::app::versionNumber << "\n"
            << "Deterministic hardware/plugin-free regression runner.\n\n"
            << "Usage:\n"
            << "  PoorMansStudioSim [--list] [--scenario ID ...] [--seed VALUE] [--output FOLDER] [--junit] [--timing]\n"
            << "  PoorMansStudioSim --help\n"
            << "  PoorMansStudioSim --version\n\n"
            << "With no --scenario arguments, every scenario runs.\n"
            << "Results are written as last_results.txt and last_results.json.\n"
            << "Deterministic PCM WAV fixtures are written under artifacts.\n"
            << "Use --junit to also write last_results.junit.xml.\n"
            << "Use --timing to also write last_timing.txt with per-scenario diagnostic timings.\n"
            << "Use --seed with decimal or 0x-prefixed hexadecimal VALUE to choose the H02 project-fixture seed.\n";
    }

    bool parseSeedValue(const std::string& text, std::uint32_t& value)
    {
        try
        {
            std::size_t consumed = 0;
            const auto parsed = std::stoull(text, &consumed, 0);
            if (consumed != text.size() || parsed > std::numeric_limits<std::uint32_t>::max())
                return false;
            value = static_cast<std::uint32_t>(parsed);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }


    bool writeRunStatus(const std::filesystem::path& path, const std::string& text, std::string& error)
    {
        try
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (!out)
                throw std::runtime_error("Could not open simulation run-status file.");
            out << text;
            if (!out)
                throw std::runtime_error("Could not write simulation run-status file.");
            error.clear();
            return true;
        }
        catch (const std::exception& e)
        {
            error = e.what();
            return false;
        }
    }

    std::filesystem::path defaultOutputFolder(const char* argv0)
    {
        std::error_code ec;
        auto executable = std::filesystem::absolute(argv0 != nullptr ? std::filesystem::path(argv0) : std::filesystem::path(), ec);
        auto executableFolder = executable.has_parent_path() ? executable.parent_path() : std::filesystem::current_path();
        if (executableFolder.filename() == "simulation" && executableFolder.parent_path().filename() == "workspace")
            return executableFolder;

        auto cwd = std::filesystem::current_path();
        if (std::filesystem::exists(cwd / "workspace"))
            return cwd / "workspace" / "simulation";
        if (cwd.filename() == "builds" && std::filesystem::exists(cwd.parent_path() / "workspace"))
            return cwd.parent_path() / "workspace" / "simulation";
        return executableFolder / "simulation_results";
    }
}

int main(int argc, char** argv)
{
    std::vector<std::string> requestedScenarios;
    auto outputFolder = defaultOutputFolder(argc > 0 ? argv[0] : nullptr);
    bool writeJunit = false;
    bool writeTiming = false;
    mw::simulation::SimulationRunOptions runOptions;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h")
        {
            printHelp();
            return 0;
        }
        if (arg == "--version")
        {
            std::cout << mw::app::versionNumber << "\n";
            return 0;
        }
        if (arg == "--list")
        {
            for (const auto& id : mw::simulation::availableSimulationScenarioIds())
                std::cout << id << '\n';
            return 0;
        }
        if (arg == "--scenario")
        {
            if (i + 1 >= argc)
            {
                std::cerr << "--scenario requires an ID.\n";
                return 2;
            }
            requestedScenarios.emplace_back(argv[++i]);
            continue;
        }
        if (arg == "--seed")
        {
            if (i + 1 >= argc)
            {
                std::cerr << "--seed requires a decimal or 0x-prefixed hexadecimal value.\n";
                return 2;
            }
            std::uint32_t parsedSeed = 0;
            const std::string seedText = argv[++i];
            if (!parseSeedValue(seedText, parsedSeed))
            {
                std::cerr << "Invalid --seed value: " << seedText << "\n";
                return 2;
            }
            runOptions.projectFixtureSeed = parsedSeed;
            continue;
        }
        if (arg == "--output")
        {
            if (i + 1 >= argc)
            {
                std::cerr << "--output requires a folder.\n";
                return 2;
            }
            outputFolder = argv[++i];
            continue;
        }
        if (arg == "--junit")
        {
            writeJunit = true;
            continue;
        }
        if (arg == "--timing")
        {
            writeTiming = true;
            continue;
        }

        std::cerr << "Unknown argument: " << arg << "\n";
        return 2;
    }

    // Create the requested output folder before any scenario executes. Full-suite
    // failures must never leave the caller with no folder and no diagnostic trail.
    std::error_code outputError;
    std::filesystem::create_directories(outputFolder, outputError);
    if (outputError)
    {
        std::cerr << "Could not create simulation output folder: " << outputError.message() << "\n";
        return 2;
    }

    const auto statusPath = outputFolder / "run_status.txt";
    std::string error;
    if (!writeRunStatus(statusPath,
            std::string("Poor Man's Studio simulation run\nVersion: ") + mw::app::versionNumber
                + "\nStatus: starting\n", error))
    {
        std::cerr << "Could not initialize simulation run status: " << error << "\n";
        return 2;
    }

    const auto allScenarioIds = mw::simulation::availableSimulationScenarioIds();
    std::size_t selectedScenarioCount = 0;
    if (requestedScenarios.empty())
    {
        selectedScenarioCount = allScenarioIds.size();
    }
    else
    {
        for (const auto& id : allScenarioIds)
        {
            if (std::find(requestedScenarios.begin(), requestedScenarios.end(), id) != requestedScenarios.end())
                ++selectedScenarioCount;
        }
    }

    bool checkpointWriteFailed = false;
    std::string checkpointError;
    const auto progress = [&](const mw::simulation::SimulationTimedRun& currentRun, const std::string& scenarioId, bool starting)
    {
        const auto ordinal = currentRun.report.scenarios.size() + (starting ? 1U : 0U);
        if (starting)
        {
            std::cout << "RUN  [" << scenarioId << "] " << ordinal << "/" << selectedScenarioCount << std::endl;
            std::ostringstream status;
            status << "Poor Man's Studio simulation run\n"
                   << "Version: " << mw::app::versionNumber << "\n"
                   << "Status: running\n"
                   << "Scenario: " << scenarioId << "\n"
                   << "Progress: " << currentRun.report.scenarios.size() << "/" << selectedScenarioCount << " completed\n";
            if (!writeRunStatus(statusPath, status.str(), checkpointError))
                checkpointWriteFailed = true;
            return;
        }

        const auto& result = currentRun.report.scenarios.back();
        std::cout << (result.passed ? "PASS " : "FAIL ") << "[" << result.id << "] "
                  << currentRun.report.scenarios.size() << "/" << selectedScenarioCount << std::endl;

        std::filesystem::path checkpointTextPath;
        std::filesystem::path checkpointJsonPath;
        if (!mw::simulation::writeSimulationReports(
                currentRun.report, outputFolder, checkpointTextPath, checkpointJsonPath, checkpointError))
        {
            checkpointWriteFailed = true;
        }

        if (writeJunit)
        {
            std::filesystem::path checkpointJunitPath;
            if (!mw::simulation::writeSimulationJUnitReport(
                    currentRun.report, outputFolder, checkpointJunitPath, checkpointError))
                checkpointWriteFailed = true;
        }

        if (writeTiming)
        {
            std::filesystem::path checkpointTimingPath;
            if (!mw::simulation::writeSimulationTimingReport(
                    currentRun, outputFolder, checkpointTimingPath, checkpointError))
                checkpointWriteFailed = true;
        }

        std::ostringstream status;
        status << "Poor Man's Studio simulation run\n"
               << "Version: " << mw::app::versionNumber << "\n"
               << "Status: running\n"
               << "Last completed: " << result.id << " (" << (result.passed ? "PASS" : "FAIL") << ")\n"
               << "Progress: " << currentRun.report.scenarios.size() << "/" << selectedScenarioCount << " completed\n";
        if (!writeRunStatus(statusPath, status.str(), checkpointError))
            checkpointWriteFailed = true;
    };

    // Always use the incremental timed runner internally. Timing is only emitted
    // when requested, but incremental execution guarantees visible progress and
    // checkpointed reports for both targeted and full-suite runs.
    auto timedRun = mw::simulation::runSimulationRegressionSuiteTimed(requestedScenarios, runOptions, progress);
    auto report = timedRun.report;

    if (report.scenarios.empty())
    {
        writeRunStatus(statusPath,
            std::string("Poor Man's Studio simulation run\nVersion: ") + mw::app::versionNumber
                + "\nStatus: error\nNo matching simulation scenarios were selected.\n", error);
        std::cerr << "No matching simulation scenarios were selected. Use --list to see IDs.\n";
        return 2;
    }

    std::cout << "\n" << mw::simulation::simulationReportToText(report);

    std::filesystem::path textPath;
    std::filesystem::path jsonPath;
    if (!mw::simulation::writeSimulationReports(report, outputFolder, textPath, jsonPath, error))
    {
        std::cerr << "Could not write simulation results: " << error << "\n";
        return 2;
    }

    std::vector<std::filesystem::path> artifactPaths;
    if (!mw::simulation::writeSimulationArtifacts(outputFolder, artifactPaths, error))
    {
        std::cerr << "Could not write simulation artifacts: " << error << "\n";
        return 2;
    }

    std::cout << "Text results: " << textPath.string() << "\n";
    std::cout << "JSON results: " << jsonPath.string() << "\n";
    for (const auto& artifactPath : artifactPaths)
        std::cout << "Artifact: " << artifactPath.string() << "\n";

    if (writeJunit)
    {
        std::filesystem::path junitPath;
        if (!mw::simulation::writeSimulationJUnitReport(report, outputFolder, junitPath, error))
        {
            std::cerr << "Could not write JUnit results: " << error << "\n";
            return 2;
        }
        std::cout << "JUnit results: " << junitPath.string() << "\n";
    }

    if (writeTiming)
    {
        std::filesystem::path timingPath;
        if (!mw::simulation::writeSimulationTimingReport(timedRun, outputFolder, timingPath, error))
        {
            std::cerr << "Could not write timing diagnostics: " << error << "\n";
            return 2;
        }
        std::cout << "Timing diagnostics: " << timingPath.string() << "\n";
    }

    std::ostringstream finalStatus;
    finalStatus << "Poor Man's Studio simulation run\n"
                << "Version: " << mw::app::versionNumber << "\n"
                << "Status: completed\n"
                << "Passed: " << report.passedCount() << "\n"
                << "Failed: " << report.failedCount() << "\n"
                << "Total: " << report.scenarios.size() << "\n";
    if (!writeRunStatus(statusPath, finalStatus.str(), error))
    {
        std::cerr << "Could not finalize simulation run status: " << error << "\n";
        return 2;
    }

    if (checkpointWriteFailed)
    {
        std::cerr << "One or more incremental simulation checkpoints could not be written: "
                  << checkpointError << "\n";
        return 2;
    }

    return report.allPassed() ? 0 : 1;
}
