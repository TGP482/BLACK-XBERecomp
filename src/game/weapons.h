#pragma once
// The weapons page of the F10 panel: any of the game's player weapons in any level (weapons.cpp)
#include <string>
#include <vector>

namespace weapons {

struct Choice {
    std::string label;  // display name and the game's code, "AK-47 (AK1)"
    int type;           // index in the level's weapon table
    bool held;          // in one of the player's two slots now
};

struct View {
    bool in_level = false;  // a level is being played: the choices can be given
    std::vector<Choice> choices;
    std::string status;  // the last thing done, or why it could not be
};

View Current();      // any thread
void Give(int type);  // any thread: done on the game thread's next tick
void FillAmmo();      // any thread: both slots' ammunition to full

}  // namespace weapons
