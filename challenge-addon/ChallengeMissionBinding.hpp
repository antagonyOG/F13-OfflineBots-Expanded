#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace f13::challenges {
// Settings belong to a submitted challenge/map pair, not the entire game mode.
// A native replay may reuse them; a different challenge needs a fresh submit.
class MissionSettingsBinding {
public:
    bool Accept(std::uint64_t submission, std::string_view map, std::string_view challenge) {
        constexpr std::array<std::string_view, 10> classes{{
            "ChallengeC01_BrokenDown", "ChallengeC02_PackanackParty", "ChallengeC03_Stargazing",
            "ChallengeC04_PowerStruggle", "ChallengeC05_StripPoker", "ChallengeC06_PowerOut",
            "ChallengeC07_VacationParty", "ChallengeC08_Escaping", "ChallengeC09_JasonIsHere",
            "ChallengeC10_SnuggleByTheFire"
        }};
        if (!submission || map.find("/Game/Maps/Single_Player/") != 0) return false;
        bool stock = false;
        for (auto name : classes) {
            const std::string key = "/Game/Blueprints/MapRegistry/Challenges/" + std::string(name) + "." + std::string(name) + "_C";
            if (challenge == key) { stock = true; break; }
        }
        if (!stock) return false;
        if (submission != submission_) {
            submission_ = submission;
            map_ = map;
            challenge_ = challenge;
        }
        return map == map_ && challenge == challenge_;
    }
private:
    std::uint64_t submission_ = 0;
    std::string map_, challenge_;
};
}
