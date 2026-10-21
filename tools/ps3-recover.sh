#!/bin/sh
# ps3-recover.sh [--check] -- bring the console back from a wedged RSX
# without anyone touching it.
#
# A faulted RSX survives the app being relaunched (rsxInit never returns in
# the next process), so the only cure is a hard reboot. webMAN runs in the XMB,
# not in the app, so it still answers when the app and the GPU are gone:
#   1. /reboot.ps3?hard
#   2. wait for webMAN to go away and come back
#   3. /play.ps3?<app title id> to start the app again
#   4. wait for the control port and a finished RSX init
#
# --check only reports: exit 0 when the app answers and the RSX is moving (or
# idle with nothing queued), 1 when it needs recovering.
. "$(dirname "$0")/local.env" 2>/dev/null || true
PS3=${PS3_IP:?set PS3_IP in tools/local.env}
APP=${PS3_APP:-MKWP00001}

ctl() { printf '%s' "$1" | nc -w "${2:-3}" "$PS3" 4001 2>/dev/null; }
webman_up() { curl -s -m 4 -o /dev/null -w '%{http_code}' "http://$PS3/cpursx.ps3" 2>/dev/null | grep -q 200; }

# RSX put/get from the control port's gfx report.
rsx_get() { ctl gfx 5 | sed -n 's/^rsx put=\([0-9a-f]*\) get=\([0-9a-f]*\).*/\1 \2/p'; }

healthy() {
    [ "$(ctl ping)" = "alive" ] || { echo "app not answering"; return 1; }
    a=$(rsx_get)
    [ -n "$a" ] || { echo "no RSX state (init: $(ctl gfx 5 | sed -n 's/^rsx init: //p'))"; return 1; }
    set -- $a
    [ "$1" = "$2" ] && return 0          # caught up
    get0=$2
    sleep 3
    b=$(rsx_get)
    set -- $b
    [ "$1" = "$2" ] && return 0
    [ "$2" != "$get0" ] && return 0      # GET moving (PUT moves even when the GPU is stuck)
    echo "RSX stuck at get=$2 (put=$1)"
    return 1
}

if [ "$1" = "--check" ]; then
    healthy
    exit $?
fi

echo "hard reboot"
curl -s -m 5 -o /dev/null "http://$PS3/reboot.ps3?hard"
i=0
while webman_up; do
    i=$((i + 1)); [ $i -gt 60 ] && { echo "console did not go down"; exit 1; }
    sleep 2
done
i=0
until webman_up; do
    i=$((i + 1)); [ $i -gt 150 ] && { echo "console did not come back"; exit 1; }
    sleep 2
done
# webMAN answers before the XMB is ready to start anything. It also mounts the
# last game it mounted at startup, and while a disc is mounted the app_home
# launch below runs the disc instead, so unmount first.
sleep 25
curl -s -m 10 -o /dev/null "http://$PS3/mount.ps3/unmount"
sleep 5
ours() { ctl gfx 5 | grep -q "^rsx init:"; }
n=0
until ours; do
    n=$((n + 1)); [ $n -gt 4 ] && { echo "could not start $APP"; exit 1; }
    echo "launching $APP"
    # play.ps3?<id> with /dev_hdd0/game/<id> present maps app_home there and
    # starts the app_home icon: the installed app, nothing mounted as a disc.
    curl -s -m 15 -o /dev/null "http://$PS3/play.ps3?$APP"
    i=0
    until [ "$(ctl ping 2)" = "alive" ] && ours; do
        i=$((i + 1)); [ $i -gt 20 ] && break
        sleep 3
    done
done
i=0
until ctl gfx 5 | grep -q "^rsx init: done"; do
    i=$((i + 1)); [ $i -gt 30 ] && { echo "RSX init did not finish: $(ctl gfx 5 | sed -n 's/^rsx init: //p')"; exit 1; }
    sleep 2
done
echo "app up"
