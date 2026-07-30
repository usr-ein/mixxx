#pragma once

#ifndef __WINDOWS__

#include <QObject>

class QSocketNotifier;

namespace mixxx {

/// Turns SIGTERM, SIGINT and SIGHUP into a normal application quit.
///
/// Without this, `systemctl stop getty@tty1`, a `kill`, or Ctrl-C terminates
/// Mixxx instantly: no destructors, no `deleting Library`, no settings written,
/// no threads joined. On a deck that is the ordinary way the session ends, so
/// "shut down cleanly" and "shut down at all" were the same thing only by luck.
///
/// **A signal handler may not touch Qt.** It runs on whatever stack the signal
/// interrupted, and almost nothing is async-signal-safe -- allocating, locking,
/// or posting an event from one is undefined behaviour that usually manifests
/// as a deadlock on the allocator. So the handler does the one thing it may:
/// `write()` a byte to a socket pair. A QSocketNotifier picks that up on the
/// GUI thread's event loop, where quitting is safe.
///
/// The second signal is deliberately *not* handled: if a graceful quit hangs,
/// a second Ctrl-C or a systemd escalation to SIGKILL should still work rather
/// than being swallowed.
class PosixSignalHandler : public QObject {
    Q_OBJECT

  public:
    /// Install handlers. Returns false if the socket pair could not be created,
    /// in which case signals keep their default behaviour.
    static bool install(QObject* pParent = nullptr);

  private:
    explicit PosixSignalHandler(QObject* pParent);

    /// Async-signal-safe: writes one byte and returns.
    static void handle(int signalNumber);

    void onActivated();

    QSocketNotifier* m_pNotifier = nullptr;
};

} // namespace mixxx

#endif // !__WINDOWS__
