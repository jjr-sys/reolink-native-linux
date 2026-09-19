#pragma once

#include "protocol/ReolinkHttpClient.h"

#include <QString>

#include <memory>

namespace rl {

// Everything a download needs to know about one camera, captured when it is queued (so
// removing the device from the sidebar later does not break a running or retried item).
struct DownloadSource {
    qint64 hostId = -1;     // the NVR/host; downloads to one host run one at a time
    QString site;           // host name, e.g. "Woorabinda"
    QString camera;         // camera name, e.g. "Kitchen"
    int channel = 0;
    QString host;
    int port = 443;
    bool https = true;
    std::shared_ptr<ReolinkHttpClient> client;
    bool valid() const { return client != nullptr; }
};

} // namespace rl
