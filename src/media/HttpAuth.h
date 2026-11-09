#pragma once
// Request headers the media player adds when it opens an http(s) URL, for servers that
// sign in with a session cookie rather than a login in the URL (Frigate on port 8971).
// The device layer installs the provider; the media layer only asks it. Thread-safe.
#include <QByteArray>
#include <QUrl>
#include <functional>

namespace rl::httpauth {

// Header lines for `url`, each ending "\r\n"; empty for none. May block (it can sign in),
// so it is called on the player's worker thread, never the GUI thread.
using Provider = std::function<QByteArray(const QUrl &url)>;
// The server refused the headers given for `url` (401): forget them, so the next open signs in.
using Refused = std::function<void(const QUrl &url)>;

void install(Provider provider, Refused refused);
QByteArray headersFor(const QUrl &url);
void refused(const QUrl &url);

} // namespace rl::httpauth
