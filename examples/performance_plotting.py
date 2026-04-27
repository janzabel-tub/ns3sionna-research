from __future__ import annotations

import math
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Mapping, Optional, Sequence, Tuple, Union

import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
import seaborn as sns


PathLike = Union[str, Path]
MetricName = str

FILE_RE = re.compile(
    r"^(?P<kind>throughput_results|snr_results|position_results|computationtime)_"
    r"(?P<coherence>[-+]?\d+(?:\.\d+)?)_(?P<seed>\d+)\.csv$"
)
TC_DIR_RE = re.compile(r"^tc(?P<coherence>[-+]?\d+(?:\.\d+)?)$")


@dataclass(frozen=True)
class DatasetSpec:
    label: str
    path: PathLike
    server_setting: Optional[str] = None

    @property
    def root(self) -> Path:
        return Path(self.path)

    @property
    def setting(self) -> str:
        return self.server_setting or self.label


@dataclass
class PerformanceData:
    throughput: pd.DataFrame
    snr: pd.DataFrame
    positions: pd.DataFrame
    computation: pd.DataFrame

    def summary(self) -> pd.DataFrame:
        frames = []
        for name, frame in [
            ("throughput", self.throughput),
            ("snr", self.snr),
            ("positions", self.positions),
            ("computation", self.computation),
        ]:
            if frame.empty:
                frames.append(
                    {
                        "table": name,
                        "rows": 0,
                        "datasets": 0,
                        "coherence_times_ms": "",
                        "seeds": "",
                    }
                )
                continue
            frames.append(
                {
                    "table": name,
                    "rows": len(frame),
                    "datasets": frame["dataset"].nunique(),
                    "coherence_times_ms": ", ".join(format_coherence(v) for v in sorted(frame["coherence_ms"].dropna().unique())),
                    "seeds": _range_label(frame["seed"].dropna().astype(int).unique()),
                }
            )
        return pd.DataFrame(frames)


def normalize_datasets(datasets: Union[Mapping[str, PathLike], Sequence[DatasetSpec], DatasetSpec, PathLike]) -> List[DatasetSpec]:
    if isinstance(datasets, DatasetSpec):
        return [datasets]
    if isinstance(datasets, (str, Path)):
        path = Path(datasets)
        return [DatasetSpec(label=path.name, path=path)]
    if isinstance(datasets, Mapping):
        return [DatasetSpec(label=str(label), path=path) for label, path in datasets.items()]
    return list(datasets)


def default_apartment_server_datasets(base_dir: PathLike = "results_Apartment") -> Dict[str, Path]:
    base = Path(base_dir)
    candidates = {
        "Full": base / "results_FullNew",
        "No diffraction": base / "results_noDiffracNew",
        "No diffuse scattering": base / "results_noDiffuseNew",
        "No reflection": base / "results_noReflecNew",
        "No refraction": base / "results_noRefracNew",
        "Three bounces": base / "results_threeBounce",
        "Four bounces": base / "results_fourBounce",
    }
    return {label: path for label, path in candidates.items() if path.exists()}


def use_legacy_plot_style() -> None:
    """Use the bright Matplotlib/Seaborn defaults used by the older notebook."""
    plt.style.use("default")
    sns.set_theme(context="notebook", style="white", palette="deep")


def load_performance_data(
    datasets: Union[Mapping[str, PathLike], Sequence[DatasetSpec], DatasetSpec, PathLike],
    coherence_times: Optional[Iterable[float]] = None,
    seeds: Optional[Iterable[int]] = None,
    recursive: bool = True,
    coherence_time_scale: float = 1.0,
) -> PerformanceData:
    specs = normalize_datasets(datasets)
    wanted_tc = _float_set(coherence_times)
    wanted_seeds = set(int(seed) for seed in seeds) if seeds is not None else None

    throughput_frames = []
    snr_frames = []
    position_frames = []
    computation_frames = []

    for spec in specs:
        root = spec.root
        if not root.exists():
            print(f"[warn] Dataset path does not exist: {root}")
            continue

        files = root.rglob("*.csv") if recursive else root.glob("tc*/*.csv")
        for path in sorted(files):
            meta = parse_result_file(path)
            if meta is None:
                continue

            kind, logged_coherence_value, seed = meta
            coherence_ms = logged_coherence_value * coherence_time_scale
            if wanted_tc is not None and not _contains_float(wanted_tc, coherence_ms):
                continue
            if wanted_seeds is not None and seed not in wanted_seeds:
                continue

            common = {
                "dataset": spec.label,
                "server_setting": spec.setting,
                "coherence_ms": coherence_ms,
                "coherence_label": format_coherence(coherence_ms),
                "seed": seed,
                "source_file": str(path),
            }

            if kind == "throughput_results":
                frame = _read_throughput(path, common)
                if not frame.empty:
                    throughput_frames.append(frame)
            elif kind == "snr_results":
                frame = _read_snr(path, common)
                if not frame.empty:
                    snr_frames.append(frame)
            elif kind == "position_results":
                frame = _read_positions(path, common)
                if not frame.empty:
                    position_frames.append(frame)
            elif kind == "computationtime":
                frame = _read_computation(path, common)
                if not frame.empty:
                    computation_frames.append(frame)

    return PerformanceData(
        throughput=_concat(throughput_frames),
        snr=_concat(snr_frames),
        positions=_concat(position_frames),
        computation=_concat(computation_frames),
    )


def parse_result_file(path: PathLike) -> Optional[Tuple[str, float, int]]:
    path = Path(path)
    match = FILE_RE.match(path.name)
    if not match:
        return None

    coherence_ms = float(match.group("coherence"))
    seed = int(match.group("seed"))

    parent_match = TC_DIR_RE.match(path.parent.name)
    if parent_match:
        parent_tc = float(parent_match.group("coherence"))
        if not math.isclose(parent_tc, coherence_ms, rel_tol=0.0, abs_tol=1e-9):
            print(f"[warn] Filename/parent coherence mismatch: {path}")

    return match.group("kind"), coherence_ms, seed


def format_coherence(value: float) -> str:
    value = float(value)
    if math.isclose(value, round(value), rel_tol=0.0, abs_tol=1e-9):
        return f"{int(round(value))} ms"
    return f"{value:g} ms"


def plot_metric_vs_coherence(
    data: PerformanceData,
    metric: MetricName,
    interval_ms: int = 100,
    datasets: Optional[Iterable[str]] = None,
    seeds: Optional[Iterable[int]] = None,
    sta_node_ids: Optional[Iterable[int]] = None,
    time_range: Optional[Tuple[float, float]] = None,
    drop_zero_throughput: bool = True,
    agg: str = "mean",
    error: str = "sem",
    save: bool = False,
    output_dir: PathLike = "plots",
    filename: Optional[str] = None,
    ax: Optional[plt.Axes] = None,
) -> Tuple[plt.Figure, plt.Axes, pd.DataFrame]:
    values = aggregate_metric_by_run(
        data,
        metric=metric,
        interval_ms=interval_ms,
        datasets=datasets,
        seeds=seeds,
        sta_node_ids=sta_node_ids,
        time_range=time_range,
        drop_zero_throughput=drop_zero_throughput,
        agg=agg,
    )
    if values.empty:
        raise ValueError(f"No data available for metric '{metric}'.")

    stats = _summary_stats(values, error=error)
    coherence_values = sorted(stats["coherence_ms"].unique())
    labels = [format_coherence(v) for v in coherence_values]
    x_positions = np.arange(len(coherence_values))

    if ax is None:
        fig, ax = plt.subplots(figsize=(9, 5))
    else:
        fig = ax.figure

    for dataset, group in stats.groupby("dataset", sort=False):
        group = group.set_index("coherence_ms").reindex(coherence_values)
        ax.errorbar(
            x_positions,
            group["center"],
            yerr=group["err"],
            marker="o",
            linewidth=2,
            capsize=4,
            label=dataset,
        )

    ax.set_xticks(x_positions)
    ax.set_xticklabels(labels)
    ax.set_xlabel("Coherence time")
    ax.set_ylabel(metric_label(metric, interval_ms))
    ax.set_title(f"{metric_title(metric)} over coherence time")
    ax.grid(True, alpha=0.3)
    ax.legend(title="Dataset")
    fig.tight_layout()
    save_figure(fig, filename or f"{metric}_vs_coherence.png", save=save, output_dir=output_dir)
    return fig, ax, values


def plot_metric_boxplot(
    data: PerformanceData,
    metric: MetricName,
    interval_ms: int = 100,
    datasets: Optional[Iterable[str]] = None,
    coherence_times: Optional[Iterable[float]] = None,
    seeds: Optional[Iterable[int]] = None,
    sta_node_ids: Optional[Iterable[int]] = None,
    time_range: Optional[Tuple[float, float]] = None,
    drop_zero_throughput: bool = True,
    by_run: bool = True,
    agg: str = "mean",
    legacy_style: bool = True,
    title: Optional[str] = None,
    save: bool = False,
    output_dir: PathLike = "plots",
    filename: Optional[str] = None,
    ax: Optional[plt.Axes] = None,
) -> Tuple[plt.Figure, plt.Axes, pd.DataFrame]:
    if metric == "computation_time":
        frame = select_computation(data, datasets=datasets, coherence_times=coherence_times, seeds=seeds)
        plot_df = frame.rename(columns={"computation_time_s": "value"}).copy()
    elif by_run:
        plot_df = aggregate_metric_by_run(
            data,
            metric=metric,
            interval_ms=interval_ms,
            datasets=datasets,
            coherence_times=coherence_times,
            seeds=seeds,
            sta_node_ids=sta_node_ids,
            time_range=time_range,
            drop_zero_throughput=drop_zero_throughput,
            agg=agg,
        )
    else:
        plot_df = metric_samples(
            data,
            metric=metric,
            interval_ms=interval_ms,
            datasets=datasets,
            coherence_times=coherence_times,
            seeds=seeds,
            sta_node_ids=sta_node_ids,
            time_range=time_range,
            drop_zero_throughput=drop_zero_throughput,
        )

    if plot_df.empty:
        raise ValueError(f"No data available for metric '{metric}'.")

    if legacy_style:
        use_legacy_plot_style()

    order = [format_coherence(v) for v in sorted(plot_df["coherence_ms"].dropna().unique())]
    plot_df["coherence_label"] = pd.Categorical(plot_df["coherence_label"], categories=order, ordered=True)

    if ax is None:
        fig, ax = plt.subplots(figsize=(10, 5))
    else:
        fig = ax.figure

    single_dataset = plot_df["dataset"].nunique() == 1
    hue = "coherence_label" if single_dataset and legacy_style else ("dataset" if not single_dataset else None)
    sns.boxplot(
        data=plot_df,
        x="coherence_label",
        y="value",
        hue=hue,
        palette="deep" if hue else None,
        width=0.5,
        ax=ax,
    )
    if hue == "coherence_label" and ax.get_legend() is not None:
        ax.get_legend().remove()

    medians = plot_df.groupby("coherence_label", observed=True)["value"].median()
    y_span = max(ax.get_ylim()[1] - ax.get_ylim()[0], 1e-9)
    for xtick, label in enumerate(order):
        if label in medians.index and pd.notna(medians.loc[label]):
            ax.text(xtick, medians.loc[label] + 0.01 * y_span, f"{medians.loc[label]:.2f}",
                    ha="center", size="x-small", color="black", weight="semibold")

    ax.set_xlabel("Coherence Time in ms" if legacy_style else "Coherence time")
    ax.set_ylabel("Mbps" if legacy_style and metric == "throughput" else metric_label(metric, interval_ms))
    ax.set_title(title or f"{metric_title(metric)} boxplot")
    if legacy_style:
        ax.grid(False)
    else:
        ax.grid(True, axis="y", alpha=0.3)
    if not single_dataset:
        ax.legend(title="Dataset")
    fig.tight_layout()
    save_figure(fig, filename or f"{metric}_boxplot.png", save=save, output_dir=output_dir)
    return fig, ax, plot_df


def plot_server_setting_comparison(
    data: PerformanceData,
    metric: MetricName,
    interval_ms: int = 100,
    coherence_times: Optional[Iterable[float]] = None,
    seeds: Optional[Iterable[int]] = None,
    sta_node_ids: Optional[Iterable[int]] = None,
    time_range: Optional[Tuple[float, float]] = None,
    drop_zero_throughput: bool = True,
    agg: str = "mean",
    save: bool = False,
    output_dir: PathLike = "plots",
    filename: Optional[str] = None,
    ax: Optional[plt.Axes] = None,
) -> Tuple[plt.Figure, plt.Axes, pd.DataFrame]:
    values = aggregate_metric_by_run(
        data,
        metric=metric,
        interval_ms=interval_ms,
        coherence_times=coherence_times,
        seeds=seeds,
        sta_node_ids=sta_node_ids,
        time_range=time_range,
        drop_zero_throughput=drop_zero_throughput,
        agg=agg,
    )
    if values.empty:
        raise ValueError(f"No data available for metric '{metric}'.")

    order = sorted(values["dataset"].unique())
    coherence_order = [format_coherence(v) for v in sorted(values["coherence_ms"].dropna().unique())]
    values["coherence_label"] = pd.Categorical(values["coherence_label"], categories=coherence_order, ordered=True)

    if ax is None:
        fig, ax = plt.subplots(figsize=(11, 5))
    else:
        fig = ax.figure

    sns.boxplot(
        data=values,
        x="dataset",
        y="value",
        hue="coherence_label" if values["coherence_ms"].nunique() > 1 else None,
        order=order,
        ax=ax,
    )
    ax.set_xlabel("Server/raytracer setting")
    ax.set_ylabel(metric_label(metric, interval_ms))
    ax.set_title(f"{metric_title(metric)} by server/raytracer setting")
    ax.tick_params(axis="x", rotation=25)
    ax.grid(True, axis="y", alpha=0.3)
    if values["coherence_ms"].nunique() > 1:
        ax.legend(title="Coherence time")
    fig.tight_layout()
    save_figure(fig, filename or f"{metric}_server_settings.png", save=save, output_dir=output_dir)
    return fig, ax, values


def plot_time_series(
    data: PerformanceData,
    metric: MetricName,
    dataset: Optional[str] = None,
    coherence_ms: Optional[float] = None,
    seed: Optional[int] = None,
    interval_ms: int = 100,
    sta_node_ids: Optional[Iterable[int]] = None,
    time_range: Optional[Tuple[float, float]] = None,
    drop_zero_throughput: bool = False,
    save: bool = False,
    output_dir: PathLike = "plots",
    filename: Optional[str] = None,
    ax: Optional[plt.Axes] = None,
) -> Tuple[plt.Figure, plt.Axes, pd.DataFrame]:
    samples = metric_samples(
        data,
        metric=metric,
        interval_ms=interval_ms,
        datasets=[dataset] if dataset is not None else None,
        coherence_times=[coherence_ms] if coherence_ms is not None else None,
        seeds=[seed] if seed is not None else None,
        sta_node_ids=sta_node_ids,
        time_range=time_range,
        drop_zero_throughput=drop_zero_throughput,
    )
    if samples.empty:
        raise ValueError(f"No data available for metric '{metric}'.")

    if ax is None:
        fig, ax = plt.subplots(figsize=(10, 5))
    else:
        fig = ax.figure

    hue_cols = ["dataset", "coherence_label", "seed"]
    if metric == "snr" and "sta_node_id" in samples.columns:
        hue_cols.append("sta_node_id")
    samples["_series"] = samples[hue_cols].astype(str).agg(" | ".join, axis=1)

    for label, group in samples.groupby("_series", sort=False):
        group = group.sort_values("time_s")
        ax.plot(group["time_s"], group["value"], linewidth=1.6, alpha=0.9, label=label)

    ax.set_xlabel("Time [s]")
    ax.set_ylabel(metric_label(metric, interval_ms))
    ax.set_title(f"{metric_title(metric)} over time")
    ax.grid(True, alpha=0.3)
    ax.legend(title="Run", fontsize="small")
    fig.tight_layout()
    save_figure(fig, filename or f"{metric}_time_series.png", save=save, output_dir=output_dir)
    samples = samples.drop(columns=["_series"])
    return fig, ax, samples


def plot_positions(
    data: PerformanceData,
    dataset: Optional[str] = None,
    coherence_times: Optional[Iterable[float]] = None,
    seed: Optional[int] = None,
    sta_node_ids: Optional[Iterable[int]] = None,
    ap_position: Optional[Tuple[float, float]] = (-1.0, 0.5),
    use_snr_if_missing: bool = True,
    save: bool = False,
    output_dir: PathLike = "plots",
    filename: Optional[str] = None,
) -> Tuple[plt.Figure, np.ndarray, pd.DataFrame]:
    frame = select_positions(
        data,
        dataset=dataset,
        coherence_times=coherence_times,
        seed=seed,
        sta_node_ids=sta_node_ids,
        use_snr_if_missing=use_snr_if_missing,
    )
    if frame.empty:
        raise ValueError("No position data available.")

    coherence_values = sorted(frame["coherence_ms"].dropna().unique())
    ncols = min(2, max(1, len(coherence_values)))
    nrows = int(math.ceil(len(coherence_values) / ncols))
    fig, axes = plt.subplots(nrows, ncols, figsize=(6 * ncols, 5 * nrows), squeeze=False, sharex=True, sharey=True)

    for ax in axes.ravel():
        ax.set_visible(False)

    for ax, coherence in zip(axes.ravel(), coherence_values):
        ax.set_visible(True)
        group = frame[frame["coherence_ms"] == coherence]
        if ap_position is not None:
            ax.scatter([ap_position[0]], [ap_position[1]], marker="^", s=110, color="tab:red", label="AP", zorder=5)

        for sta_id, sta_group in group.groupby("sta_node_id", sort=True):
            sta_group = sta_group.sort_values("time_s")
            ax.plot(sta_group["pos_x"], sta_group["pos_y"], linewidth=1.4, alpha=0.75, label=f"STA {int(sta_id)}")
            ax.scatter(sta_group["pos_x"].iloc[0], sta_group["pos_y"].iloc[0], color="tab:green", s=45, zorder=4)
            ax.scatter(sta_group["pos_x"].iloc[-1], sta_group["pos_y"].iloc[-1], color="tab:blue", marker="x", s=60, zorder=4)

        ax.set_title(f"{format_coherence(coherence)} ({len(group)} samples)")
        ax.set_xlabel("X position [m]")
        ax.set_ylabel("Y position [m]")
        ax.set_aspect("equal", adjustable="box")
        ax.grid(True, alpha=0.3)
        ax.legend(fontsize="small")

    fig.tight_layout()
    save_figure(fig, filename or "positions.png", save=save, output_dir=output_dir)
    return fig, axes, frame


def aggregate_metric_by_run(
    data: PerformanceData,
    metric: MetricName,
    interval_ms: int = 100,
    datasets: Optional[Iterable[str]] = None,
    coherence_times: Optional[Iterable[float]] = None,
    seeds: Optional[Iterable[int]] = None,
    sta_node_ids: Optional[Iterable[int]] = None,
    time_range: Optional[Tuple[float, float]] = None,
    drop_zero_throughput: bool = True,
    agg: str = "mean",
) -> pd.DataFrame:
    samples = metric_samples(
        data,
        metric=metric,
        interval_ms=interval_ms,
        datasets=datasets,
        coherence_times=coherence_times,
        seeds=seeds,
        sta_node_ids=sta_node_ids,
        time_range=time_range,
        drop_zero_throughput=drop_zero_throughput,
    )
    if samples.empty:
        return samples

    agg_func = _agg_func(agg)
    keys = ["dataset", "server_setting", "coherence_ms", "coherence_label", "seed", "source_file"]
    grouped = samples.groupby(keys, dropna=False)["value"].agg(agg_func).reset_index()
    grouped["metric"] = metric
    grouped["aggregation"] = agg
    return grouped


def metric_samples(
    data: PerformanceData,
    metric: MetricName,
    interval_ms: int = 100,
    datasets: Optional[Iterable[str]] = None,
    coherence_times: Optional[Iterable[float]] = None,
    seeds: Optional[Iterable[int]] = None,
    sta_node_ids: Optional[Iterable[int]] = None,
    time_range: Optional[Tuple[float, float]] = None,
    drop_zero_throughput: bool = False,
) -> pd.DataFrame:
    if metric == "throughput":
        frame = data.throughput.copy()
        if frame.empty:
            return frame
        frame = frame[frame["interval_ms"] == interval_ms]
        frame = frame.rename(columns={"throughput_mbps": "value"})
        if drop_zero_throughput:
            frame = frame[frame["value"] > 0.0]
    elif metric == "snr":
        frame = data.snr.copy()
        if frame.empty:
            return frame
        frame = frame.rename(columns={"snr_db": "value"})
        if sta_node_ids is not None:
            wanted_nodes = set(int(node_id) for node_id in sta_node_ids)
            frame = frame[frame["sta_node_id"].isin(wanted_nodes)]
    else:
        raise ValueError("metric must be 'throughput' or 'snr'")

    frame = _filter_common(frame, datasets=datasets, coherence_times=coherence_times, seeds=seeds, time_range=time_range)
    return frame.dropna(subset=["value"])


def select_positions(
    data: PerformanceData,
    dataset: Optional[str] = None,
    coherence_times: Optional[Iterable[float]] = None,
    seed: Optional[int] = None,
    sta_node_ids: Optional[Iterable[int]] = None,
    use_snr_if_missing: bool = True,
) -> pd.DataFrame:
    frame = data.positions.copy()
    if frame.empty and use_snr_if_missing:
        needed = ["time_s", "sta_node_id", "pos_x", "pos_y", "pos_z", "dataset", "server_setting", "coherence_ms", "coherence_label", "seed", "source_file"]
        required_position_cols = {"time_s", "sta_node_id", "pos_x", "pos_y", "pos_z"}
        if required_position_cols.issubset(data.snr.columns):
            available = [col for col in needed if col in data.snr.columns]
            frame = data.snr[available].copy()
        else:
            frame = pd.DataFrame()

    if frame.empty:
        return frame

    datasets = [dataset] if dataset is not None else None
    seeds = [seed] if seed is not None else None
    frame = _filter_common(frame, datasets=datasets, coherence_times=coherence_times, seeds=seeds)
    if sta_node_ids is not None and "sta_node_id" in frame.columns:
        wanted_nodes = set(int(node_id) for node_id in sta_node_ids)
        frame = frame[frame["sta_node_id"].isin(wanted_nodes)]
    return frame


def select_computation(
    data: PerformanceData,
    datasets: Optional[Iterable[str]] = None,
    coherence_times: Optional[Iterable[float]] = None,
    seeds: Optional[Iterable[int]] = None,
) -> pd.DataFrame:
    frame = data.computation.copy()
    if frame.empty:
        return frame
    return _filter_common(frame, datasets=datasets, coherence_times=coherence_times, seeds=seeds)


def save_figure(fig: plt.Figure, filename: str, save: bool = False, output_dir: PathLike = "plots") -> None:
    if not save:
        return
    output_path = Path(output_dir)
    output_path.mkdir(parents=True, exist_ok=True)
    fig.savefig(output_path / filename, dpi=200, bbox_inches="tight")


def metric_label(metric: MetricName, interval_ms: int = 100) -> str:
    if metric == "throughput":
        return f"Throughput [Mbps], {interval_ms} ms samples"
    if metric == "snr":
        return "SNR [dB]"
    if metric == "computation_time":
        return "Computation time [s]"
    return metric


def metric_title(metric: MetricName) -> str:
    if metric == "throughput":
        return "Throughput"
    if metric == "snr":
        return "SNR"
    if metric == "computation_time":
        return "Computation time"
    return metric.replace("_", " ").title()


def _read_throughput(path: Path, common: Dict[str, object]) -> pd.DataFrame:
    try:
        frame = pd.read_csv(path)
    except pd.errors.EmptyDataError:
        return pd.DataFrame()

    frame.columns = [str(col).strip() for col in frame.columns]
    time_col = _find_column(frame, ["Time", "Time_s"])
    throughput_col = _find_column(frame, ["Throughput_Mbps", "Throughput"])
    interval_col = _find_column(frame, ["measuring interval in ms", "interval_ms", "Interval_ms"])
    if not (time_col and throughput_col and interval_col):
        print(f"[warn] Skipping throughput file with unexpected columns: {path}")
        return pd.DataFrame()

    out = pd.DataFrame(common, index=frame.index)
    out["time_s"] = pd.to_numeric(frame[time_col], errors="coerce")
    out["throughput_mbps"] = pd.to_numeric(frame[throughput_col], errors="coerce")
    out["interval_ms"] = pd.to_numeric(frame[interval_col], errors="coerce").astype("Int64")
    return out.dropna(subset=["time_s", "throughput_mbps", "interval_ms"])


def _read_snr(path: Path, common: Dict[str, object]) -> pd.DataFrame:
    try:
        frame = pd.read_csv(path)
    except pd.errors.EmptyDataError:
        return pd.DataFrame()

    frame.columns = [str(col).strip() for col in frame.columns]
    time_col = _find_column(frame, ["Time_s", "Time"])
    node_col = _find_column(frame, ["STA_NodeId", "NodeId", "node_id"])
    snr_col = _find_column(frame, ["SNR_dB", "snr_db", "SNR"])
    if not (time_col and node_col and snr_col):
        print(f"[warn] Skipping SNR file with unexpected columns: {path}")
        return pd.DataFrame()

    out = pd.DataFrame(common, index=frame.index)
    out["time_s"] = pd.to_numeric(frame[time_col], errors="coerce")
    out["sta_node_id"] = pd.to_numeric(frame[node_col], errors="coerce").astype("Int64")
    out["snr_db"] = pd.to_numeric(frame[snr_col], errors="coerce")

    for source, target in [
        ("pos_x", "pos_x"),
        ("pos_y", "pos_y"),
        ("pos_z", "pos_z"),
        ("PathLoss_dB", "pathloss_db"),
        ("RxPower_dBm", "rx_power_dbm"),
        ("Decoded", "decoded"),
    ]:
        col = _find_column(frame, [source])
        if col:
            out[target] = pd.to_numeric(frame[col], errors="coerce")

    return out.dropna(subset=["time_s", "sta_node_id", "snr_db"])


def _read_positions(path: Path, common: Dict[str, object]) -> pd.DataFrame:
    try:
        frame = pd.read_csv(path)
    except pd.errors.EmptyDataError:
        return pd.DataFrame()

    frame.columns = [str(col).strip() for col in frame.columns]
    time_col = _find_column(frame, ["Time_s", "Time"])
    node_col = _find_column(frame, ["STA_NodeId", "NodeId", "node_id"])
    x_col = _find_column(frame, ["pos_x", "x"])
    y_col = _find_column(frame, ["pos_y", "y"])
    z_col = _find_column(frame, ["pos_z", "z"])
    if not (time_col and node_col and x_col and y_col and z_col):
        print(f"[warn] Skipping position file with unexpected columns: {path}")
        return pd.DataFrame()

    out = pd.DataFrame(common, index=frame.index)
    out["time_s"] = pd.to_numeric(frame[time_col], errors="coerce")
    out["sta_node_id"] = pd.to_numeric(frame[node_col], errors="coerce").astype("Int64")
    out["pos_x"] = pd.to_numeric(frame[x_col], errors="coerce")
    out["pos_y"] = pd.to_numeric(frame[y_col], errors="coerce")
    out["pos_z"] = pd.to_numeric(frame[z_col], errors="coerce")
    return out.dropna(subset=["time_s", "sta_node_id", "pos_x", "pos_y", "pos_z"])


def _read_computation(path: Path, common: Dict[str, object]) -> pd.DataFrame:
    try:
        frame = pd.read_csv(path, header=None)
    except pd.errors.EmptyDataError:
        return pd.DataFrame()

    if frame.empty:
        return pd.DataFrame()

    out = pd.DataFrame(common, index=frame.index)
    out["logged_coherence_ms"] = pd.to_numeric(frame.get(0), errors="coerce")
    out["computation_time_s"] = pd.to_numeric(frame.get(1), errors="coerce")
    out["mobile_speed_mps"] = pd.to_numeric(frame.get(2), errors="coerce")
    out["logged_seed"] = pd.to_numeric(frame.get(3), errors="coerce").astype("Int64")
    out["run_index"] = np.arange(len(out))
    return out.dropna(subset=["computation_time_s"])


def _find_column(frame: pd.DataFrame, candidates: Sequence[str]) -> Optional[str]:
    normalized = {str(col).strip().lower(): col for col in frame.columns}
    for candidate in candidates:
        key = candidate.strip().lower()
        if key in normalized:
            return normalized[key]
    return None


def _concat(frames: Sequence[pd.DataFrame]) -> pd.DataFrame:
    if not frames:
        return pd.DataFrame()
    return pd.concat(frames, ignore_index=True)


def _float_set(values: Optional[Iterable[float]]) -> Optional[List[float]]:
    if values is None:
        return None
    return [float(value) for value in values]


def _contains_float(values: Sequence[float], candidate: float) -> bool:
    return any(math.isclose(float(value), float(candidate), rel_tol=0.0, abs_tol=1e-9) for value in values)


def _filter_common(
    frame: pd.DataFrame,
    datasets: Optional[Iterable[str]] = None,
    coherence_times: Optional[Iterable[float]] = None,
    seeds: Optional[Iterable[int]] = None,
    time_range: Optional[Tuple[float, float]] = None,
) -> pd.DataFrame:
    if frame.empty:
        return frame

    out = frame
    if datasets is not None:
        wanted = set(str(dataset) for dataset in datasets if dataset is not None)
        out = out[out["dataset"].isin(wanted)]
    if coherence_times is not None:
        wanted_tc = _float_set(coherence_times)
        out = out[out["coherence_ms"].apply(lambda value: _contains_float(wanted_tc, value))]
    if seeds is not None:
        wanted_seeds = set(int(seed) for seed in seeds if seed is not None)
        out = out[out["seed"].isin(wanted_seeds)]
    if time_range is not None and "time_s" in out.columns:
        start, end = time_range
        out = out[(out["time_s"] >= start) & (out["time_s"] <= end)]
    return out


def _agg_func(name: str):
    if name == "mean":
        return "mean"
    if name == "median":
        return "median"
    if name == "max":
        return "max"
    if name == "min":
        return "min"
    raise ValueError("agg must be one of: mean, median, max, min")


def _summary_stats(values: pd.DataFrame, error: str = "sem") -> pd.DataFrame:
    rows = []
    for (dataset, coherence), group in values.groupby(["dataset", "coherence_ms"], sort=False):
        center = float(group["value"].mean())
        if error == "std":
            err = float(group["value"].std(ddof=1)) if len(group) > 1 else 0.0
        elif error == "none":
            err = 0.0
        else:
            err = float(group["value"].std(ddof=1) / math.sqrt(len(group))) if len(group) > 1 else 0.0
        rows.append({"dataset": dataset, "coherence_ms": coherence, "center": center, "err": err, "n": len(group)})
    return pd.DataFrame(rows)


def _range_label(values: Iterable[int]) -> str:
    values = sorted(set(int(value) for value in values))
    if not values:
        return ""
    if len(values) == 1:
        return str(values[0])
    if values == list(range(values[0], values[-1] + 1)):
        return f"{values[0]}-{values[-1]}"
    return ", ".join(str(value) for value in values[:12]) + ("..." if len(values) > 12 else "")
