#!/usr/bin/env python3

import argparse
import csv
import math
from pathlib import Path

import matplotlib.pyplot as plt


DEFAULT_TX_VECTOR_CSV = "example-sionna-tcp-minstrel-tx-vectors.csv"
DEFAULT_RATE_CHANGE_CSV = "example-sionna-tcp-minstrel-rate-changes.csv"
DEFAULT_TX_VECTOR_PNG = "example-sionna-tcp-minstrel-tx-vectors.png"
DEFAULT_RATE_CHANGE_PNG = "example-sionna-tcp-minstrel-rate-changes.png"


def finite_float(value):
    try:
        number = float(value)
    except (TypeError, ValueError):
        return None
    return number if math.isfinite(number) else None


def read_rows(path):
    if not Path(path).is_file():
        raise SystemExit(f"error: missing file: {path}")

    with open(path, newline="") as csv_file:
        return list(csv.DictReader(csv_file))


def points(rows, time_column, value_column):
    samples = []
    for row in rows:
        time = finite_float(row[time_column])
        value = finite_float(row[value_column])
        if time is not None and value is not None:
            samples.append((time, value))
    return sorted(samples)


def save_tx_vector_plot(rows, output):
    samples = points(rows, "Time_s", "Rate_Mbps")
    if not samples:
        raise SystemExit("error: no finite TX-vector rate samples")

    times, rates = zip(*samples)
    plt.figure(figsize=(8, 4.5))
    plt.scatter(times, rates, s=10, alpha=0.55)
    plt.title("Minstrel transmitted rate")
    plt.xlabel("Time [s]")
    plt.ylabel("TX vector rate [Mbit/s]")
    plt.grid(True, alpha=0.3)
    plt.tight_layout()
    plt.savefig(output, dpi=160)
    plt.close()


def save_rate_change_plot(rows, output):
    samples = points(rows, "Time_s", "NewRate_Mbps")
    if not samples:
        raise SystemExit("error: no finite Minstrel rate-change samples")

    times, rates = zip(*samples)
    plt.figure(figsize=(8, 4.5))
    if len(samples) > 1:
        plt.step(times, rates, where="post", linewidth=1.5)
    plt.scatter(times, rates, s=24, alpha=0.8)
    plt.title("Minstrel selected rate changes")
    plt.xlabel("Time [s]")
    plt.ylabel("Selected rate [Mbit/s]")
    plt.grid(True, alpha=0.3)
    plt.tight_layout()
    plt.savefig(output, dpi=160)
    plt.close()


def parse_args():
    parser = argparse.ArgumentParser(
        description="Plot the CSV logs produced by example-sionna-tcp-minstrel."
    )
    parser.add_argument("--tx-vectors", default=DEFAULT_TX_VECTOR_CSV)
    parser.add_argument("--rate-changes", default=DEFAULT_RATE_CHANGE_CSV)
    parser.add_argument("--tx-output", default=DEFAULT_TX_VECTOR_PNG)
    parser.add_argument("--rate-output", default=DEFAULT_RATE_CHANGE_PNG)
    return parser.parse_args()


def main():
    args = parse_args()
    save_tx_vector_plot(read_rows(args.tx_vectors), args.tx_output)
    save_rate_change_plot(read_rows(args.rate_changes), args.rate_output)


if __name__ == "__main__":
    main()
