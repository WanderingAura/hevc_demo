#!/usr/bin/env bash
# Two network namespaces joined by a veth pair: tc netem shapes the traffic,
# while both sides still share one clock, so the logged ages are exact.
#
#   sudo tools/netem/ns.sh up
#   sudo tools/netem/ns.sh run-server [key=value ...]   # namespace h265-srv, 10.77.0.1
#   sudo tools/netem/ns.sh run-client [key=value ...]   # namespace h265-cli, headless
#   sudo tools/netem/ns.sh profile clean|delay|bwcollapse|loss|ctrlstall
#   sudo tools/netem/ns.sh down
#
# BUILD=build/debug overrides the binaries' directory (default build/release).
set -euo pipefail

SRV=h265-srv
CLI=h265-cli
DEV_S=veth-srv
DEV_C=veth-cli
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${BUILD:-$ROOT/build/release}"

# Shapes the server -> client direction, which carries the video.
shape() { tc -n "$SRV" qdisc replace dev "$DEV_S" root netem "$@"; }
unshape() { tc -n "$SRV" qdisc del dev "$DEV_S" root 2>/dev/null || true; }

case "${1:-}" in
up)
    ip netns add "$SRV"
    ip netns add "$CLI"
    ip link add "$DEV_S" type veth peer name "$DEV_C"
    ip link set "$DEV_S" netns "$SRV"
    ip link set "$DEV_C" netns "$CLI"
    ip -n "$SRV" addr add 10.77.0.1/24 dev "$DEV_S"
    ip -n "$CLI" addr add 10.77.0.2/24 dev "$DEV_C"
    for ns in "$SRV" "$CLI"; do ip -n "$ns" link set lo up; done
    ip -n "$SRV" link set "$DEV_S" up
    ip -n "$CLI" link set "$DEV_C" up
    ;;
down)
    ip netns del "$SRV" 2>/dev/null || true
    ip netns del "$CLI" 2>/dev/null || true
    ;;
run-server)
    shift
    exec ip netns exec "$SRV" "$BUILD/h265sim-server" "$ROOT/config/h265sim.ini" "$@"
    ;;
run-client)
    shift
    exec ip netns exec "$CLI" "$BUILD/h265sim-client" "$ROOT/config/h265sim.ini" transport.host=10.77.0.1 --headless "$@"
    ;;
profile)
    case "${2:-}" in
    clean) unshape ;;
    delay) shape delay 20ms 5ms ;;  # doc 10: age rises by the delay, no false CATCHUP
    bwcollapse)                     # below the 4 Mbit/s bitrate for 5 s, then back
        shape rate 1mbit
        sleep 5
        unshape
        ;;
    loss) shape loss 1% ;;  # over TCP, loss becomes retransmission delay
    ctrlstall)              # control channel blocked for 5 s; periodic IDRs must keep recovering
        ip netns exec "$CLI" iptables -A OUTPUT -p tcp --dport 5601 -j DROP
        sleep 5
        ip netns exec "$CLI" iptables -D OUTPUT -p tcp --dport 5601 -j DROP
        ;;
    *)
        echo "profiles: clean delay bwcollapse loss ctrlstall" >&2
        exit 1
        ;;
    esac
    ;;
*)
    sed -n '2,11p' "$0"
    exit 1
    ;;
esac
