#pragma once

#include "ReolinkApi.h"

namespace rl {

// The seam between what the app asks a Reolink host over HTTP-CGI and the wire. The
// real adapter is ReolinkHttpClient (libcurl, login token, one request at a time);
// tests use a scripted adapter that answers from a table. Everything that only needs to
// send a batch of commands and learn why it failed depends on this, not on libcurl.
//
// Interface: call() is blocking (worker threads only), handles login itself, and never
// throws. lastFailKind() describes the most recent failed exchange.
class ReolinkTransport
{
public:
    virtual ~ReolinkTransport() = default;

    // Why the last exchange failed. The right reaction differs: Transport (device off,
    // wrong address) is safe to retry; Auth/Locked must NOT be auto-retried, because
    // every rejected login burns the firmware's 10-attempt counter toward a lockout.
    enum class FailKind { None, Transport, Auth, Locked, Protocol };

    virtual api::BatchResult call(const Json &commands) = 0;
    virtual FailKind lastFailKind() = 0;

    // A single command; returns the lone CommandResult.
    api::CommandResult callOne(const QString &cmd, Json param = Json::object(), int action = 0)
    {
        const api::BatchResult batch =
            call(Json::array({api::command(cmd, std::move(param), action)}));
        if (!batch.transportOk || batch.results.isEmpty()) {
            api::CommandResult r;
            r.cmd = cmd;
            r.detail = batch.error.isEmpty() ? QStringLiteral("no response") : batch.error;
            return r;
        }
        return batch.results.first();
    }
};

} // namespace rl
