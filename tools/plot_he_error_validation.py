#!/usr/bin/env python3
"""Export a CSV and publication-ready PNG/PDF from the measured PER report."""
import argparse
import csv
import hashlib
import json
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    path = args.directory / "report.json"
    report = json.loads(path.read_text())
    assert report["execution_status"] == "completed"
    rows = report["measurements"]
    fields = ["id", "mcs", "ldpc", "nss", "psdu_bytes", "trials", "errors",
              "measured_per", "predicted_per", "wilson_lower", "wilson_upper",
              "mean_uncoded_ber", "effective_sinr", "sinr_min", "sinr_max",
              "table_prediction_inside_interval"]
    with (args.directory / "per_summary.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)
    plt.rcParams.update({"font.size": 10, "pdf.fonttype": 42})
    fig, axes = plt.subplots(1, 2, figsize=(11, 4.6), constrained_layout=True)
    for flat, color, label in ((True, "#2463a6", "Gleiche SINR"),
                               (False, "#cf563b", "Ungleiche SINR")):
        selected = [r for r in rows if (r["pair"] == 0) == flat]
        axes[0].errorbar([r["predicted_per"] for r in selected],
                        [r["measured_per"] for r in selected],
                        yerr=[[r["measured_per"] - r["wilson_lower"] for r in selected],
                              [r["wilson_upper"] - r["measured_per"] for r in selected]],
                        fmt="o", color=color, alpha=.75, markersize=4, capsize=2,
                        elinewidth=.7, label=label)
    axes[0].plot([0, 1], [0, 1], "--", color="0.4", linewidth=1)
    axes[0].set(xlim=(-.03, 1.03), ylim=(-.03, 1.03),
                xlabel="Vorhergesagte PER (ns-3)", ylabel="Gemessene PER (MATLAB)",
                title=f"{len(rows)} HE-Datenfälle, je {report['trials_per_case']} Pakete")
    axes[0].legend(loc="upper left")
    examples = [next(r for r in rows if r["id"] == name) for name in
                ("per_mcs0_ldpc0_nss2_flat", "per_mcs0_ldpc0_nss2_unequal_streams")]
    y = [r["measured_per"] for r in examples]
    axes[1].bar([0, 1], y, color=["#2463a6", "#cf563b"], width=.55)
    axes[1].errorbar([0, 1], y,
                    yerr=[[r["measured_per"] - r["wilson_lower"] for r in examples],
                          [r["wilson_upper"] - r["measured_per"] for r in examples]],
                    fmt="none", color="black", capsize=5)
    axes[1].axhline(examples[0]["predicted_per"], color="0.3", linestyle="--",
                   label=f"Vorhersage für beide: {examples[0]['predicted_per']:.3f}")
    axes[1].set(ylim=(0, .85), xticks=[0, 1],
                xticklabels=["Gleiche Streams", "Ungleiche Streams"], ylabel="Paketfehlerwahrscheinlichkeit",
                title=f"BPSK, BCC, 2 Streams\nGleiche mittlere BER: {examples[0]['mean_uncoded_ber']:.5f}")
    for x, r in enumerate(examples):
        axes[1].text(x, r["wilson_upper"] + .025, f"{r['errors']}/{r['trials']}", ha="center")
    axes[1].legend(loc="upper right")
    for ax in axes:
        ax.grid(axis="y", alpha=.2)
        ax.set_axisbelow(True)
    outputs = []
    for extension in ("png", "pdf"):
        target = args.directory / f"per_validation.{extension}"
        fig.savefig(target, dpi=180)
        outputs.append(target)
    plt.close(fig)
    outputs.append(args.directory / "per_summary.csv")
    tracked = [path, Path(__file__).resolve(), *outputs]
    manifest = {str(p.name): hashlib.sha256(p.read_bytes()).hexdigest() for p in tracked}
    (args.directory / "plot_provenance.json").write_text(json.dumps(manifest, indent=2) + "\n")


if __name__ == "__main__":
    main()
