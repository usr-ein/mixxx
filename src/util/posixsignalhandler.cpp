#include "util/posixsignalhandler.h"

#ifndef __WINDOWS__

#include <sys/socket.h>
#include <unistd.h>

#include <QCoreApplication>
#include <QSocketNotifier>
#include <csignal>

#include "moc_posixsignalhandler.cpp"
#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("SignalHandler");

/// The pair the handler writes to and the notifier reads from.
int s_socketPair[2] = {-1, -1};
} // namespace

namespace mixxx {

void PosixSignalHandler::handle(int signalNumber) {
    // Async-signal-safe only. One byte, no formatting, no allocation, and the
    // return value is deliberately ignored -- there is nothing useful to do if
    // this fails, and calling anything that could report it would be unsafe.
    const char byte = static_cast<char>(signalNumber);
    const ssize_t ignored = ::write(s_socketPair[0], &byte, sizeof(byte));
    Q_UNUSED(ignored);
}

bool PosixSignalHandler::install(QObject* pParent) {
    if (s_socketPair[0] != -1) {
        return true;
    }
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, s_socketPair) != 0) {
        kLogger.warning() << "could not create the signal socket pair;"
                          << "termination signals keep their default behaviour";
        return false;
    }

    new PosixSignalHandler(pParent);

    struct sigaction action {};
    action.sa_handler = &PosixSignalHandler::handle;
    sigemptyset(&action.sa_mask);
    // SA_RESTART so a signal arriving mid-syscall does not turn into a spurious
    // EINTR somewhere in the audio or network path.
    action.sa_flags = SA_RESTART;
    // SA_RESETHAND makes the *second* signal fall back to the default action, so
    // a graceful quit that hangs can still be killed -- with systemd that is the
    // difference between a stuck stop and one that escalates to SIGKILL as
    // intended.
    action.sa_flags |= SA_RESETHAND;

    for (const int signalNumber : {SIGTERM, SIGINT, SIGHUP}) {
        if (::sigaction(signalNumber, &action, nullptr) != 0) {
            kLogger.warning() << "could not install a handler for signal" << signalNumber;
        }
    }
    return true;
}

PosixSignalHandler::PosixSignalHandler(QObject* pParent)
        : QObject(pParent) {
    m_pNotifier = new QSocketNotifier(s_socketPair[1], QSocketNotifier::Read, this);
    connect(m_pNotifier,
            &QSocketNotifier::activated,
            this,
            &PosixSignalHandler::onActivated);
}

void PosixSignalHandler::onActivated() {
    m_pNotifier->setEnabled(false);
    char byte = 0;
    const ssize_t read = ::read(s_socketPair[1], &byte, sizeof(byte));
    Q_UNUSED(read);

    kLogger.info() << "caught signal" << static_cast<int>(byte) << "- shutting down";

    // The ordinary quit path: this unwinds the event loop, runs the destructors,
    // saves settings and joins the threads, exactly as closing the window does.
    QCoreApplication::quit();
}

} // namespace mixxx

#endif // !__WINDOWS__
