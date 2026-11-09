#include "HttpAuth.h"

#include <mutex>

namespace rl::httpauth {
namespace {
std::mutex g_lock;
Provider g_provider;
Refused g_refused;
} // namespace

void install(Provider provider, Refused refused)
{
    std::lock_guard<std::mutex> g(g_lock);
    g_provider = std::move(provider);
    g_refused = std::move(refused);
}

QByteArray headersFor(const QUrl &url)
{
    Provider p;
    {
        std::lock_guard<std::mutex> g(g_lock);
        p = g_provider;
    }
    return p ? p(url) : QByteArray();
}

void forgetHeadersFor(const QUrl &url)
{
    Refused r;
    {
        std::lock_guard<std::mutex> g(g_lock);
        r = g_refused;
    }
    if (r)
        r(url);
}

} // namespace rl::httpauth
