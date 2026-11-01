#pragma once

#include <iosfwd>
#include <memory>

namespace zeta {

// Keeps the desktop from locking the screen while zd runs: the performer
// plays only the MIDI controller, which the desktop does not count as
// activity. Holds an org.freedesktop.ScreenSaver inhibit on the user's
// session bus, which the desktop also releases when the connection closes,
// so a crash leaves nothing held. Without a session bus or that service, as
// for the Raspberry Pi's system service, it holds nothing. Either way it
// prints one line; a failure to inhibit is printed, never thrown.
class ScreenLockInhibitor final {
public:
    explicit ScreenLockInhibitor(std::ostream& log);
    ~ScreenLockInhibitor();

    ScreenLockInhibitor(const ScreenLockInhibitor&) = delete;
    ScreenLockInhibitor& operator=(const ScreenLockInhibitor&) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace zeta
