#!/bin/bash
#
# apt-get for CI, bounded and retried. Takes exactly the arguments apt-get does.
#
# Usage: scripts/ci_apt_get.sh update -qq
#        scripts/ci_apt_get.sh install -y -qq --no-install-recommends <packages>
#
# The runner's jobs share one apt-cacher-ng proxy, and several jobs run at once. When two of them
# fetch the same index at the same moment, apt can be left waiting on a connection the proxy has
# already closed -- and Acquire::http::Timeout does not fire for that, so it waits for as long as the
# runner lets the job live (#79). A stalled attempt is killed from outside and tried again; the
# retry almost always gets through, because by then the proxy holds the file.
#
# A clean `apt-get update` takes seconds, so a minute is generous for one and short enough
# that a stalled attempt costs little. Installing the toolchain can legitimately take longer on a
# cold cache, hence the separate, larger bound for `install`.
set -u

attempts=5
if [ "${1:-}" = install ]; then
	attempt_seconds=300
else
	attempt_seconds=60
fi

for attempt in $(seq "$attempts"); do
	timeout "$attempt_seconds" apt-get "$@" && exit 0
	echo "ci_apt_get: apt-get $1 failed or stalled (attempt $attempt of $attempts)" >&2
	sleep $((attempt * 5))
done
exit 1
