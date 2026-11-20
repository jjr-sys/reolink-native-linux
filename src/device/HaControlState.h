#pragma once

#include <QtGlobal>

namespace rl {

// One Home Assistant-backed control's order / read-back bookkeeping (the Bosch wiper and
// tracking): whether an order is out, whether a read is out, and a revision that orders the two.
//
// Invariant: an order bumps the revision when it is sent and again when it is answered; a read
// captures the revision when it is sent and is dropped if the revision changed by its answer.
// Anything else that makes a read out of date (a steer or tour start that ends tracking, a
// reset) calls invalidate().
struct HaControlState {
    bool pending = false;       // an order is out
    bool readPending = false;   // a read is out
    quint64 revision = 0;

    // A read or an order is out: the poll sends no read.
    bool busy() const { return pending || readPending; }

    // Returns the revision the read's answer is checked against.
    quint64 beginRead()
    {
        readPending = true;
        return revision;
    }
    // The read is answered: true when its answer still counts.
    bool endRead(quint64 captured)
    {
        readPending = false;
        return captured == revision;
    }

    // Returns the revision the order was sent at.
    quint64 beginOrder()
    {
        pending = true;
        return ++revision;
    }
    // The order is answered: true when something bumped the revision since it was sent (it
    // was overtaken). Either way a read sent while it was out is now older than this answer.
    bool endOrder(quint64 sent)
    {
        pending = false;
        const bool overtaken = revision != sent;
        ++revision;
        return overtaken;
    }

    void invalidate() { ++revision; }

    // Forget what is out: neither pending, and any answer still to come is out of date.
    void reset()
    {
        ++revision;
        pending = false;
        readPending = false;
    }
};

} // namespace rl
