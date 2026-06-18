#!/usr/bin/env python3

import argparse
import csv
import math
from collections import defaultdict
from pathlib import Path

import matplotlib.pyplot as plt


DEFAULT_INPUT_CSV = "example-sionna-tcp-sensing-low-l1.csv"
DEFAULT_SNR_PNG = "example-sionna-tcp-sensing-low-snr.png"
DEFAULT_POSITION_PNG = "example-sionna-tcp-sensing-low-position.png"

STATUS_STYLES = {
    "OK": ("tab:green", "o"),
    "ERROR": ("tab:orange", "x"),
    "DROP": ("tab:red", "v"),
}
DEFAULT_STYLE = ("tab:blue", "o")


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


def rx_status(row):
    return row.get("RxStatus") or "sample"


def ordered_statuses(groups):
    known = [name for name in ("OK", "ERROR", "DROP") if name in groups]
    extra = sorted(name for name in groups if name not in known)
    return known + extra


def scatter_by_status(samples, x_index, y_index, status_index, size):
    groups = defaultdict(list)
    for sample in samples:
        groups[sample[status_index]].append(sample)

    for name in ordered_statuses(groups):
        color, marker = STATUS_STYLES.get(name, DEFAULT_STYLE)
        status_samples = groups[name]
        plt.scatter(
            [sample[x_index] for sample in status_samples],
            [sample[y_index] for sample in status_samples],
            s=size,
            color=color,
            marker=marker,
            alpha=0.75,
            label=name,
        )


def snr_points(rows):
    samples = []
    for row in rows:
        time = finite_float(row["Time_s"])
        snr = finite_float(row["SNR_dB"])
        if time is not None and snr is not None:
            samples.append((time, snr, rx_status(row)))
    return sorted(samples)


def position_points(rows):
    samples = []
    for row in rows:
        time = finite_float(row["Time_s"])
        x_pos = finite_float(row["pos_x"])
        y_pos = finite_float(row["pos_y"])
        if time is not None and x_pos is not None and y_pos is not None:
            samples.append((time, x_pos, y_pos, rx_status(row)))
    return sorted(samples)


def save_snr_plot(rows, output):
    samples = snr_points(rows)
    if not samples:
        raise SystemExit("error: no finite SNR samples")

    plt.figure(figsize=(8, 4.5))
    plt.plot([sample[0] for sample in samples], [sample[1] for sample in samples], color="0.75")
    scatter_by_status(samples, 0, 1, 2, 18)
    plt.title("Lower-layer SNR")
    plt.xlabel("Time [s]")
    plt.ylabel("SNR [dB]")
    plt.grid(True, alpha=0.3)
    plt.legend(title="RxStatus")
    plt.tight_layout()
    plt.savefig(output, dpi=160)
    plt.close()


def save_position_plot(rows, output):
    samples = position_points(rows)
    if not samples:
        raise SystemExit("error: no finite position samples")

    plt.figure(figsize=(6, 6))
    plt.plot([sample[1] for sample in samples], [sample[2] for sample in samples], color="0.75")
    scatter_by_status(samples, 1, 2, 3, 22)
    plt.title("STA position samples")
    plt.xlabel("x position [m]")
    plt.ylabel("y position [m]")
    plt.grid(True, alpha=0.3)
    plt.axis("equal")
    plt.legend(title="RxStatus")
    plt.tight_layout()
    plt.savefig(output, dpi=160)
    plt.close()


def parse_args():
    parser = argparse.ArgumentParser(
        description="Plot the CSV log produced by example-sionna-tcp-sensing-low."
    )
    parser.add_argument("--input", default=DEFAULT_INPUT_CSV)
    parser.add_argument("--snr-output", default=DEFAULT_SNR_PNG)
    parser.add_argument("--position-output", default=DEFAULT_POSITION_PNG)
    return parser.parse_args()


def main():
    args = parse_args()
    rows = read_rows(args.input)
    save_snr_plot(rows, args.snr_output)
    save_position_plot(rows, args.position_output)


if __name__ == "__main__":
    main()
