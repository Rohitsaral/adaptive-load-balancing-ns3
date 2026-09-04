"""
analyze_results.py
Parses the summary CSVs (and FlowMonitor XML files, as a network-layer
cross-check) produced by adaptive-lb, and generates comparison plots.

Run after scripts/run_experiments.sh (or run standalone if results/
already has the CSV/XML files).
"""

import csv
import os
import re
import xml.etree.ElementTree as ET

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

RESULTS_DIR = os.path.join(os.path.dirname(__file__), "..", "results")
ALGOS = ["round-robin", "weighted", "adaptive", "qlearning"]
ALGO_LABELS = {"round-robin": "Round Robin",
               "weighted": "Weighted RR",
               "adaptive": "Adaptive (Heuristic)",
               "qlearning": "Intelligent (Q-Learning ML)"}
SCENARIOS = ["normal", "failure"]


def read_summary(algo, scenario):
    path = os.path.join(RESULTS_DIR, f"summary_{algo}_{scenario}.csv")
    if not os.path.exists(path):
        return None
    data = {}
    with open(path) as f:
        for row in csv.reader(f):
            if len(row) != 2 or row[0] == "metric":
                continue
            key, val = row
            try:
                data[key] = float(val)
            except ValueError:
                data[key] = val
    return data


def parse_ns_time(s):
    """FlowMonitor times look like '+123000000.0ns' or '+1.92e+09ns' -> seconds (float)."""
    if s is None:
        return 0.0
    m = re.match(r"\+?(-?[\d.]+(?:[eE][-+]?\d+)?)ns", s)
    return float(m.group(1)) / 1e9 if m else float(s)


def read_flowmonitor_avg_delay(algo, scenario):
    """Average end-to-end delay (seconds) across all flows, from FlowMonitor XML."""
    path = os.path.join(RESULTS_DIR, f"flow_{algo}_{scenario}.xml")
    if not os.path.exists(path):
        return 0.0
    tree = ET.parse(path)
    root = tree.getroot()
    delays, rx_totals = [], 0
    for flow in root.iter("Flow"):
        rx = int(flow.get("rxPackets", 0))
        if rx == 0:
            continue
        delay_sum = parse_ns_time(flow.get("delaySum"))
        delays.append(delay_sum / rx)
        rx_totals += rx
    return sum(delays) / len(delays) if delays else 0.0


def plot_scenario(scenario, results):
    algos = [ALGO_LABELS[a] for a in ALGOS if results[a] is not None]
    avg_rtt = [results[a]["avg_app_rtt_sec"] * 1000 for a in ALGOS if results[a]]
    net_delay = [results[a]["flowmon_avg_delay_ms"] for a in ALGOS if results[a]]
    loss = [results[a]["packet_loss_pct"] for a in ALGOS if results[a]]
    throughput = [results[a]["total_responses_received"] / results[a]["sim_time_sec"]
                  for a in ALGOS if results[a]]

    n_servers = int(results[ALGOS[0]]["num_servers"])
    dist = {a: [results[a].get(f"server{i}_requests_routed", 0)
                for i in range(n_servers)] for a in ALGOS if results[a]}

    colors = ["#4C72B0", "#DD8452", "#55A868", "#C44E52"][:len(algos)]

    fig, axes = plt.subplots(2, 2, figsize=(14, 11))
    axes = axes.flatten()

    axes[0].bar(algos, avg_rtt, color=colors)
    axes[0].set_title("Avg Application RTT / Delay (ms)")
    axes[0].tick_params(axis="x", rotation=20)

    axes[1].bar(algos, loss, color=colors)
    axes[1].set_title("Packet Loss (%)")
    axes[1].tick_params(axis="x", rotation=20)

    axes[2].bar(algos, throughput, color=colors)
    axes[2].set_title("Throughput (responses/sec)")
    axes[2].tick_params(axis="x", rotation=20)

    width = 0.8 / len(ALGOS)
    x = range(n_servers)
    for i, a in enumerate(ALGOS):
        if results[a] is None:
            continue
        offset = (i - (len(ALGOS) - 1) / 2) * width
        axes[3].bar([xi + offset for xi in x], dist[a], width=width,
                    label=ALGO_LABELS[a], color=colors[i])
    axes[3].set_title("Requests Routed per Server")
    axes[3].set_xticks(list(x))
    axes[3].set_xticklabels([f"Server {i}" for i in x])
    axes[3].legend(fontsize=8)

    fig.suptitle(f"Load Balancer Comparison -- {scenario.upper()} scenario "
                 f"(NS-3 simulation)", fontsize=13)
    fig.tight_layout()
    out_path = os.path.join(RESULTS_DIR, f"ns3_comparison_{scenario}.png")
    fig.savefig(out_path, dpi=130)
    plt.close(fig)
    print(f"Saved: {out_path}")


def main():
    for scenario in SCENARIOS:
        print(f"\n{'='*70}\nScenario: {scenario.upper()}\n{'='*70}")
        results = {}
        for algo in ALGOS:
            summary = read_summary(algo, scenario)
            if summary is None:
                print(f"  (missing results for {algo}/{scenario}, skipping)")
                results[algo] = None
                continue
            summary["flowmon_avg_delay_ms"] = 1000 * read_flowmonitor_avg_delay(algo, scenario)
            results[algo] = summary

            print(f"\n{ALGO_LABELS[algo]}")
            print(f"  Avg app-level RTT   : {summary['avg_app_rtt_sec']*1000:.2f} ms")
            print(f"  Avg network delay   : {summary['flowmon_avg_delay_ms']:.2f} ms (FlowMonitor)")
            print(f"  Packet loss         : {summary['packet_loss_pct']:.3f} %")
            n_servers = int(summary["num_servers"])
            dist = [int(summary.get(f"server{i}_requests_routed", 0)) for i in range(n_servers)]
            print(f"  Load distribution   : {dist}")

        if any(v is not None for v in results.values()):
            plot_scenario(scenario, results)


if __name__ == "__main__":
    main()
