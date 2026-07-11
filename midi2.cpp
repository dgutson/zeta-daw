#include "application.hpp"

#include <exception>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " /path/to/loop_soundfont.sf2  /path/to/live_soundfont.sf2\n";
        return 1;
    }

    try {
        zeta::Application app{argv[1], argv[2]};
        app.runInteractive();
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Fatal error: " << ex.what() << "\n";
        return 1;
    }
}
