#pragma once

#include <QString>

namespace rl {

// Application directories (XDG-compliant via QStandardPaths). Created on first use.
struct Paths {
    static QString dataDir();      // ~/.local/share/reolink-client
    static QString configDir();    // ~/.config/reolink-client
    static QString databaseFile(); // <dataDir>/reolink.db
    static QString recordingsDir(); // ~/Videos/Reolink (user-changeable later)
    static QString thumbnailsDir(); // <dataDir>/thumbnails (event snapshots)
    // Each camera's last-seen live frame, a small JPEG the Site tab drawer shows:
    // <dataDir>/last-frames/<hostId>_<channel>.jpg
    static QString lastFrameFile(qint64 hostId, int channel);
};

} // namespace rl
