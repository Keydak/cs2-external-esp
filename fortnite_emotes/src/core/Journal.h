// Messages go to the CS2 developer console ("[emotes] ..."), warnings in the game's warning colour.
#pragma once
#include <string>

namespace emotes {

class Journal {
public:
    void info(const std::string& origin, const std::string& text);
    void warn(const std::string& origin, const std::string& text);
};

Journal& journal();

}  // namespace emotes
