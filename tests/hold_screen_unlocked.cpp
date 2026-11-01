// Holds the desktop's screen-saver inhibit as zd does, until its standard
// input closes. tests/audio_evidence.sh runs it so that no lock screen starts
// during the timed runs.

#include "../screen_lock_inhibitor.hpp"

#include <iostream>
#include <limits>

int main() {
    const zeta::ScreenLockInhibitor screen_lock{std::cerr};
    std::cin.ignore(std::numeric_limits<std::streamsize>::max());
}
