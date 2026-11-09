#pragma once

#include "Strings.h"

#include <string>

// The choices and the game in progress, kept between sessions in ~/.config/billiards3d/billiards3d.ini.
struct Settings {
    int theme = 0;
    int level = 3;
    int mode = 0;
    int game = 0;   // eight-ball, nine-ball, Американка
    int aim = 0;    // full, short, off
    int camera = 0; // overview, behind, top
    int breaker = 0;
    bool sound = true;
    bool panel = true;
    Language language = Language::English;
    int windowWidth = 0, windowHeight = 0; // when it is neither maximized nor full screen; 0 before the first run
    bool maximized = false;
    std::string match;   // the balls and the score, as text
    std::string history; // the lines of the game's history, each after its kind, separated by '|'

    static Settings Load();
    void Save() const;
};
