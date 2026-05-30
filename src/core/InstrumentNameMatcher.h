#pragma once
#include <string>
#include "core/InstrumentAssignment.h"

namespace mw::core
{
    class InstrumentNameMatcher
    {
    public:
        static InstrumentAssignment createDefaultAssignment(
            const std::string& importedName,
            int trackIndex
        );

        static std::string normalizeInstrumentName(const std::string& name);
        static int guessGeneralMidiProgram(const std::string& normalizedName);
    };
}
