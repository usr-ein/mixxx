#!/usr/bin/env bash

# Build the Mixxx binary for the deck (arm64, in Docker) and swap it in on the
# Pi, which runs Mixxx from apt. Only the binary is replaced -- /usr/share/mixxx
# stays as apt left it, and the skin/config live in ~/.mixxx (see
# ../mixxx_config/upload.sh).

set -eux

# ssh alias for the deck's Pi. Override for a one-off: HOST=other ./upload.sh
HOST="${HOST:-trimixxx-pi}"
DIST="${DIST:-dist}"

cd "$(dirname "$0")"

# The binary is only swappable if it was built against the same Debian release
# the deck runs. That is trixie, and it is pinned here rather than derived, so
# a build does not need the deck switched on and cannot quietly become a
# different release. The deck is checked against it below instead.
#
# Building against the wrong release does not fail obviously: an older one
# links a libstdc++ missing symbols the Rust cxx bridge emits and dies at the
# link step with `undefined reference to __cxa_call_terminate`, a newer one
# links fine and then refuses to start on the Pi against an older glibc.
CODENAME="${CODENAME:-trixie}"

# Loud rather than silent when they disagree -- that is the whole reason this
# is pinned. Skipped if the deck is off, which is a supported way to build.
DECK_CODENAME="$(ssh -o ConnectTimeout=5 "$HOST" '. /etc/os-release && printf %s "$VERSION_CODENAME"' 2>/dev/null || true)"
if [ -n "$DECK_CODENAME" ] && [ "$DECK_CODENAME" != "$CODENAME" ]; then
	echo "==> $HOST runs $DECK_CODENAME, this builds for $CODENAME" >&2
	echo "==> re-run with CODENAME=$DECK_CODENAME, or fix the pin in this script" >&2
	exit 1
fi

docker buildx build --platform linux/arm64 --target export \
	--build-arg BASE="debian:${CODENAME}" \
	--output "type=local,dest=${DIST}" .

# Where apt put it, rather than assuming /usr/bin/mixxx.
MIXXX_BIN="$(ssh "$HOST" 'command -v mixxx')"

scp "${DIST}/mixxx" "$HOST":/tmp/mixxx

# Check the deck actually has every library the new binary asks for, before it
# replaces a working Mixxx. Cheaper to find out here than mid-set.
if ssh "$HOST" 'ldd /tmp/mixxx' | grep -F 'not found'; then
	ssh "$HOST" 'rm -f /tmp/mixxx'
	echo "==> libraries above are missing on $HOST, not installing" >&2
	exit 1
fi

# Keep apt's binary aside on first run, so a bad build can be undone with a copy
# instead of a reinstall: sudo cp -a /usr/bin/mixxx.apt /usr/bin/mixxx
ssh "$HOST" "test -e ${MIXXX_BIN}.apt || sudo cp -a ${MIXXX_BIN} ${MIXXX_BIN}.apt"
ssh "$HOST" "sudo install -m 0755 /tmp/mixxx ${MIXXX_BIN} && rm -f /tmp/mixxx"

# Same restart the config upload uses: Mixxx is started by the tty1 autologin.
ssh "$HOST" 'sudo systemctl restart getty@tty1.service'
echo "==> $HOST:$MIXXX_BIN"
