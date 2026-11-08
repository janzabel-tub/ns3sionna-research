#!/usr/bin/env python3
"""Generate reproducible plots for the current ns3sionna SU-MIMO progress."""

import argparse
import csv
import re
import subprocess
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np


RESULT_RE = re.compile(
    r"scenario=(?P<scenario>\S+).*?"
    r"expected_throughput_Mbps=(?P<expected>[0-9.eE+-]+).*?"
    r"throughput_Mbps=(?P<throughput>[0-9.eE+-]+).*?"
    r"PER=(?P<per>[0-9.eE+-]+)"
)


def run_example(binary, mcs, snr_db, beta_linear):
    command = [
        str(binary),
        f"--mcs={mcs}",
        f"--referenceSnrDb={snr_db}",
        "--packetSize=1200",
        f"--eesmBetaLinear={beta_linear}",
    ]
    completed = subprocess.run(command, capture_output=True, text=True, check=False)
    output = completed.stdout + completed.stderr
    results = {
        match.group("scenario"): {
            "expected_mbps": float(match.group("expected")),
            "throughput_mbps": float(match.group("throughput")),
            "per": float(match.group("per")),
        }
        for match in RESULT_RE.finditer(output)
    }
    if "siso-1x1" not in results or "well-conditioned-2x2" not in results:
        raise RuntimeError(f"Could not parse example output:\n{output}")
    return results


def write_csv(path, fieldnames, rows):
    with path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def save_figure(figure, output_dir, stem):
    figure.savefig(output_dir / f"{stem}.png", dpi=200, bbox_inches="tight")
    figure.savefig(output_dir / f"{stem}.pdf", bbox_inches="tight")
    plt.close(figure)


def plot_throughput(output_dir, results):
    scenarios = ["1×1 SISO", "2×2 full rank"]
    values = [
        results["siso-1x1"]["throughput_mbps"],
        results["well-conditioned-2x2"]["throughput_mbps"],
    ]
    figure, axis = plt.subplots(figsize=(7.2, 4.8))
    bars = axis.bar(scenarios, values, color=["#3b82b5", "#e07a2d"], width=0.62)
    axis.bar_label(bars, labels=[f"{value:.2f} Mbit/s" for value in values], padding=5)
    axis.set_ylabel("Measured UDP throughput [Mbit/s]")
    axis.set_title("Current SU-MIMO throughput comparison", weight="bold", pad=28)
    axis.text(
        0.5,
        1.01,
        "HE MCS 5 · 20 dB reference SNR · 1200-byte packets",
        transform=axis.transAxes,
        ha="center",
        va="bottom",
        fontsize=9,
        color="#444444",
    )
    axis.set_ylim(0.0, max(values) * 1.22)
    axis.grid(axis="y", alpha=0.25)
    axis.spines[["top", "right"]].set_visible(False)
    figure.tight_layout()
    save_figure(figure, output_dir, "throughput_1x1_vs_full_rank_2x2")


def plot_per(output_dir, rows, title, channel_description, stem):
    figure, axis = plt.subplots(figsize=(7.4, 5.0))
    colors = {0: "#3b82b5", 3: "#e07a2d", 7: "#2f9e6f"}
    display_floor = 1e-6
    for mcs in (0, 3, 7):
        selected = [row for row in rows if row["mcs"] == mcs]
        snr = np.array([row["snr_db"] for row in selected])
        per = np.array([row["per"] for row in selected])
        axis.semilogy(
            snr,
            np.maximum(per, display_floor),
            marker="o",
            markersize=3.5,
            linewidth=2.0,
            color=colors[mcs],
            label=f"MCS {mcs}",
        )

    axis.set_xlabel("Reference SNR [dB]")
    axis.set_ylabel("Packet error rate (PER)")
    axis.set_title(title, weight="bold", pad=28)
    axis.text(
        0.5,
        1.01,
        f"{channel_description} · ns-3.40 TableBasedErrorRateModel · 1200-byte PSDU",
        transform=axis.transAxes,
        ha="center",
        va="bottom",
        fontsize=9,
        color="#444444",
    )
    axis.set_xlim(-10, 40)
    axis.set_ylim(display_floor, 1.05)
    axis.grid(which="both", alpha=0.25)
    axis.legend(frameon=False)
    axis.spines[["top", "right"]].set_visible(False)
    axis.text(
        0.01,
        0.02,
        "Exact zero values are displayed at the 10⁻⁶ plot floor.",
        transform=axis.transAxes,
        fontsize=8,
        color="#555555",
    )
    figure.tight_layout()
    save_figure(figure, output_dir, stem)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "plots",
    )
    parser.add_argument("--workers", type=int, default=4)
    args = parser.parse_args()

    ns3_root = Path(__file__).resolve().parents[3]
    binary = (
        ns3_root
        / "build/contrib/sionna/examples/ns3.40-ns3_wifi_sumimo_minimal-default"
    )
    if not binary.is_file():
        raise FileNotFoundError(f"Build the ns-3 examples first; missing {binary}")

    args.output_dir.mkdir(parents=True, exist_ok=True)

    throughput = run_example(binary, mcs=5, snr_db=20, beta_linear=2)
    throughput_rows = [
        {
            "scenario": scenario,
            "mcs": 5,
            "reference_snr_db": 20,
            "packet_size_bytes": 1200,
            **throughput[scenario],
        }
        for scenario in ("siso-1x1", "well-conditioned-2x2")
    ]
    write_csv(
        args.output_dir / "throughput_1x1_vs_full_rank_2x2.csv",
        [
            "scenario",
            "mcs",
            "reference_snr_db",
            "packet_size_bytes",
            "expected_mbps",
            "throughput_mbps",
            "per",
        ],
        throughput_rows,
    )
    plot_throughput(args.output_dir, throughput)

    jobs = [(mcs, snr) for mcs in (0, 3, 7) for snr in range(-10, 41)]

    def evaluate(job):
        mcs, snr = job
        result = run_example(binary, mcs=mcs, snr_db=snr, beta_linear=1)
        common = {"mcs": mcs, "snr_db": snr}
        return (
            {**common, "per": result["siso-1x1"]["per"]},
            {**common, "per": result["well-conditioned-2x2"]["per"]},
        )

    with ThreadPoolExecutor(max_workers=args.workers) as executor:
        evaluated_rows = list(executor.map(evaluate, jobs))
    siso_per_rows = [rows[0] for rows in evaluated_rows]
    mimo_per_rows = [rows[1] for rows in evaluated_rows]
    write_csv(
        args.output_dir / "per_vs_snr_mcs0_mcs3_mcs7.csv",
        ["mcs", "snr_db", "per"],
        siso_per_rows,
    )
    plot_per(
        args.output_dir,
        siso_per_rows,
        "1×1 HE PER versus SNR",
        "Flat 1×1 channel",
        "per_vs_snr_mcs0_mcs3_mcs7",
    )
    write_csv(
        args.output_dir / "per_vs_snr_full_rank_2x2_mcs0_mcs3_mcs7.csv",
        ["mcs", "snr_db", "per"],
        mimo_per_rows,
    )
    plot_per(
        args.output_dir,
        mimo_per_rows,
        "Full-rank 2×2 HE PER versus SNR",
        "Flat, well-conditioned 2×2 channel",
        "per_vs_snr_full_rank_2x2_mcs0_mcs3_mcs7",
    )


if __name__ == "__main__":
    main()
