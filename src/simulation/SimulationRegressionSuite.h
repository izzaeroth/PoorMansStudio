#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace mw::simulation
{
    inline constexpr std::uint32_t defaultSimulationProjectFixtureSeed = 0x06770077u;

    struct SimulationRunOptions
    {
        std::uint32_t projectFixtureSeed = defaultSimulationProjectFixtureSeed;
    };

    struct SimulationScenarioResult
    {
        std::string id;
        std::string stage;
        std::string name;
        bool passed = false;
        std::string details;
    };

    struct SimulationSuiteReport
    {
        std::string suiteVersion;
        std::string generatedAtUtc;
        std::vector<SimulationScenarioResult> scenarios;

        int passedCount() const noexcept;
        int failedCount() const noexcept;
        bool allPassed() const noexcept;
    };

    struct SimulationScenarioTiming
    {
        std::string id;
        double elapsedMilliseconds = 0.0;
    };

    struct SimulationTimedRun
    {
        SimulationSuiteReport report;
        std::vector<SimulationScenarioTiming> timings;
        double totalElapsedMilliseconds = 0.0;
    };

    std::vector<std::string> availableSimulationScenarioIds();
    SimulationSuiteReport runSimulationRegressionSuite(
        const std::vector<std::string>& requestedScenarioIds = {},
        const SimulationRunOptions& options = {});
    using SimulationProgressCallback = std::function<void(
        const SimulationTimedRun& currentRun, const std::string& scenarioId, bool starting)>;

    SimulationTimedRun runSimulationRegressionSuiteTimed(
        const std::vector<std::string>& requestedScenarioIds = {},
        const SimulationRunOptions& options = {},
        const SimulationProgressCallback& progressCallback = {});

    std::string simulationReportToText(const SimulationSuiteReport& report);
    std::string simulationReportToJson(const SimulationSuiteReport& report);
    std::string simulationReportToJUnitXml(const SimulationSuiteReport& report);
    std::string simulationTimingReportToText(const SimulationTimedRun& timedRun);

    bool writeSimulationReports(const SimulationSuiteReport& report,
                                const std::filesystem::path& outputFolder,
                                std::filesystem::path& textPath,
                                std::filesystem::path& jsonPath,
                                std::string& errorMessage);

    bool writeSimulationArtifacts(const std::filesystem::path& outputFolder,
                                  std::vector<std::filesystem::path>& artifactPaths,
                                  std::string& errorMessage);

    bool writeSimulationJUnitReport(const SimulationSuiteReport& report,
                                    const std::filesystem::path& outputFolder,
                                    std::filesystem::path& junitPath,
                                    std::string& errorMessage);

    bool writeSimulationTimingReport(const SimulationTimedRun& timedRun,
                                     const std::filesystem::path& outputFolder,
                                     std::filesystem::path& timingPath,
                                     std::string& errorMessage);
}
