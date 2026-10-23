#!/usr/bin/env python3
"""Per-stage latency breakdown from the two per-frame CSVs, joined by seq.

usage: .venv/bin/python tools/analysis/breakdown.py server_frames.csv client_frames.csv [--plot out.png]

Stages (all CLOCK_REALTIME ns; exact when both sides share a host):
  isp+render   capture -> encoder input      (simulated ISP delay, synthetic render)
  encode       encoder in -> out
  server queue encoder out -> first write    (send queue, waiting for a writable socket)
  network      first write -> last byte received
  client queue last byte -> decode start     (decode queue)
  decode       decode start -> end
  display      decode end -> presented
"""
import argparse

import pandas as pd

MS = 1e6


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("server_csv")
    ap.add_argument("client_csv")
    ap.add_argument("--plot", help="write a PNG with the stages and ages over time")
    args = ap.parse_args()

    s = pd.read_csv(args.server_csv)
    c = pd.read_csv(args.client_csv)
    m = s[s.dropped == 0].merge(c, on="seq", suffixes=("_srv", "_cli"))
    shown = m[m.fate == "shown"]

    stages = pd.DataFrame({
        "isp+render": shown.enc_in_ts - shown.capture_ts_srv,
        "encode": shown.enc_out_ts - shown.enc_in_ts,
        "server queue": shown.send_start_ts - shown.enc_out_ts,
        "network": shown.recv_last_ts - shown.send_start_ts,
        "client queue": shown.decode_start - shown.recv_last_ts,
        "decode": shown.decode_end - shown.decode_start,
        "display": shown.display_ts - shown.decode_end,
        "= age on screen": shown.age_show,
    }) / MS

    print(f"frames: server {len(s)} rows ({int(s.dropped.sum())} dropped), client {len(c)} rows, "
          f"{len(shown)} shown\n")
    print("per-stage latency (ms) for shown frames")
    table = stages.describe(percentiles=[0.5, 0.95, 0.99]).T[["mean", "50%", "95%", "99%", "max"]]
    print(table.round(2).to_string())

    drops = s[s.dropped == 1].drop_reason.value_counts()
    if len(drops):
        print("\nserver drops:", ", ".join(f"{k} {v}" for k, v in drops.items()))
    print("client fates:", ", ".join(f"{k} {v}" for k, v in c.fate.value_counts().items()))
    t_stale_hits = c[(c.fate == "shown")].age_show.gt(400 * MS).sum()
    print(f"shown with age > 400 ms (default T_stale): {t_stale_hits}")
    anomalies = int(c.anomaly.sum())
    if anomalies:
        print(f"clock anomalies: {anomalies}")

    if args.plot:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt

        t = (shown.capture_ts_srv - s.capture_ts.min()) / 1e9
        fig, (a1, a2) = plt.subplots(2, 1, figsize=(12, 7), sharex=True)
        parts = stages.drop(columns="= age on screen")
        a1.stackplot(t, *[parts[k] for k in parts.columns], labels=parts.columns)
        a1.set_ylabel("ms")
        a1.set_title("latency by stage (shown frames)")
        a1.legend(loc="upper left", fontsize=8, ncol=4)
        tc = (c.recv_last_ts - s.capture_ts.min()) / 1e9
        a2.plot(tc, c.age_recv / MS, ".", ms=2, label="age at receive (all frames)")
        a2.plot(t, shown.age_show / MS, ".", ms=2, label="age on screen")
        a2.set_xlabel("time (s)")
        a2.set_ylabel("ms")
        a2.legend(loc="upper left", fontsize=8)
        fig.tight_layout()
        fig.savefig(args.plot, dpi=110)
        print(f"\nplot: {args.plot}")


if __name__ == "__main__":
    main()
