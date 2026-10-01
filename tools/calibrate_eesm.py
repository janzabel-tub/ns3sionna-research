#!/usr/bin/env python3
"""Fit an HE EESM beta to independent packet-level link measurements.

The AWGN CSV must come from ns3_wifi_mimo_awgn_curve with the same PHY settings
as the measured JSON. No coefficient is installed in ns-3 automatically.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
from bisect import bisect_left
from pathlib import Path

import provenance


PHY_FIELDS = (
    "mcs", "coding", "packet_size_bytes", "nss", "num_rx_antennas",
    "channel_width_mhz", "guard_interval_ns",
)
MIN_TRIALS_PER_POINT = 1000


class CalibrationError(ValueError):
    """Invalid calibration input or an unidentifiable fit."""


def number(value, label):
    if isinstance(value, bool):
        raise CalibrationError(f"{label} must be a finite number")
    try:
        result = float(value)
    except (TypeError, ValueError) as exc:
        raise CalibrationError(f"{label} must be a finite number") from exc
    if not math.isfinite(result):
        raise CalibrationError(f"{label} must be a finite number")
    return result


def integer(value, label, minimum=0):
    result = number(value, label)
    if not result.is_integer() or result < minimum:
        raise CalibrationError(f"{label} must be an integer >= {minimum}")
    return int(result)


def probability(value, label):
    result = number(value, label)
    if not 0.0 <= result <= 1.0:
        raise CalibrationError(f"{label} must lie in [0, 1]")
    return result


def load_model(path: Path):
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        required = set(PHY_FIELDS) | {"snr_db", "per"}
        if not reader.fieldnames or not required.issubset(reader.fieldnames):
            raise CalibrationError(f"AWGN CSV requires columns: {sorted(required)}")
        rows = list(reader)
    if len(rows) < 3:
        raise CalibrationError("AWGN model needs at least three SNR points")
    metadata = {}
    for key in PHY_FIELDS:
        metadata[key] = rows[0][key] if key == "coding" else integer(rows[0][key], key)
    if metadata["coding"] not in ("BCC", "LDPC"):
        raise CalibrationError("coding must be BCC or LDPC")
    if (metadata["mcs"] > 11 or metadata["packet_size_bytes"] == 0 or
        not 1 <= metadata["nss"] <= 255 or
        not 1 <= metadata["num_rx_antennas"] <= 255 or
        metadata["channel_width_mhz"] not in (20, 40, 80, 160) or
        metadata["guard_interval_ns"] not in (800, 1600, 3200)):
        raise CalibrationError("invalid HE PHY configuration in AWGN CSV")
    x, y = [], []
    for index, row in enumerate(rows):
        for key in PHY_FIELDS:
            value = row[key] if key == "coding" else integer(row[key], key)
            if value != metadata[key]:
                raise CalibrationError(f"AWGN row {index + 2} changes {key}")
        snr = number(row["snr_db"], "snr_db")
        per = probability(row["per"], "per")
        if x and snr <= x[-1]:
            raise CalibrationError("AWGN SNR points must be strictly increasing")
        if y and per > y[-1] + 1e-12:
            raise CalibrationError("AWGN PER must be nonincreasing")
        x.append(snr)
        y.append(per)
    if not any(0.0 < per < 1.0 for per in y):
        raise CalibrationError("AWGN curve has no transition region")
    return metadata, x, y


def load_reference(path: Path, metadata):
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise CalibrationError("reference must be a JSON object")
    for field in ("source", "source_version"):
        if not isinstance(data.get(field), str) or not data[field].strip():
            raise CalibrationError(f"reference requires nonempty {field}")
    if data.get("source_kind") not in ("link_level", "synthetic"):
        raise CalibrationError("source_kind must be link_level or synthetic")
    config = data.get("phy")
    if not isinstance(config, dict):
        raise CalibrationError("reference requires phy settings")
    for key in PHY_FIELDS:
        value = config.get(key) if key == "coding" else integer(config.get(key), key)
        if value != metadata[key]:
            raise CalibrationError(f"reference phy.{key} differs from AWGN model")
    if not isinstance(data.get("awgn"), list) or len(data["awgn"]) < 3:
        raise CalibrationError("reference needs at least three measured AWGN points")
    if not isinstance(data.get("selective"), list) or not data["selective"]:
        raise CalibrationError("reference needs frequency selective measurements")
    for index, point in enumerate(data["awgn"]):
        if not isinstance(point, dict):
            raise CalibrationError(f"awgn[{index}] must be an object")
        point["snr_db"] = number(point.get("snr_db"), f"awgn[{index}].snr_db")
        if point.get("split") not in ("train", "validation"):
            raise CalibrationError(f"awgn[{index}] requires train or validation split")
        validate_counts(point, f"awgn[{index}]")
    if len({point["snr_db"] for point in data["awgn"]}) < 3:
        raise CalibrationError("reference needs three distinct AWGN SNR points")
    awgn_train = [p for p in data["awgn"] if p["split"] == "train"]
    awgn_validation = [p for p in data["awgn"] if p["split"] == "validation"]
    if len(awgn_train) < 2 or not awgn_validation:
        raise CalibrationError("need at least two AWGN training points and one held-out point")
    train_ids, validation_ids = set(), set()
    sample_count = None
    for index, point in enumerate(data["selective"]):
        label = f"selective[{index}]"
        if not isinstance(point, dict):
            raise CalibrationError(f"{label} must be an object")
        if not isinstance(point.get("case_id"), str) or not point["case_id"]:
            raise CalibrationError(f"{label} requires case_id")
        if point.get("split") not in ("train", "validation"):
            raise CalibrationError(f"{label} requires train or validation split")
        samples = point.get("sinr_linear")
        if not isinstance(samples, list) or len(samples) < 2:
            raise CalibrationError(f"{label} needs at least two SINR samples")
        samples = [number(value, f"{label}.sinr_linear") for value in samples]
        if any(value < 0.0 for value in samples) or max(samples) == min(samples):
            raise CalibrationError(f"{label} requires nonnegative, selective SINRs")
        if sample_count is None:
            sample_count = len(samples)
        elif len(samples) != sample_count:
            raise CalibrationError("all frequency selective rows need the same SINR count")
        point["sinr_linear"] = samples
        validate_counts(point, label)
        (train_ids if point["split"] == "train" else validation_ids).add(point["case_id"])
    if train_ids & validation_ids:
        raise CalibrationError("case_id may not occur in both train and validation")
    if len(train_ids) < 2 or len(validation_ids) < 1:
        raise CalibrationError("need at least two training cases and one held-out case")
    for label, points in (("AWGN training", awgn_train),
                          ("AWGN validation", awgn_validation),
                          ("training", [p for p in data["selective"] if p["split"] == "train"]),
                          ("validation", [p for p in data["selective"] if p["split"] == "validation"])):
        if not any(0 < point["errors"] < point["trials"] for point in points):
            raise CalibrationError(f"{label} measurements need a PER transition point")
    return data


def validate_counts(point, label):
    point["trials"] = integer(point.get("trials"), f"{label}.trials",
                              MIN_TRIALS_PER_POINT)
    point["errors"] = integer(point.get("errors"), f"{label}.errors")
    if point["errors"] > point["trials"]:
        raise CalibrationError(f"{label}.errors exceeds trials")


def model_per(snr_db, x, y):
    if snr_db < x[0] or snr_db > x[-1]:
        raise CalibrationError(f"effective SNR {snr_db:.6g} dB lies outside AWGN curve")
    upper = bisect_left(x, snr_db)
    if upper == 0:
        return y[0]
    if upper == len(x):
        return y[-1]
    weight = (snr_db - x[upper - 1]) / (x[upper] - x[upper - 1])
    return y[upper - 1] + weight * (y[upper] - y[upper - 1])


def eesm(samples, beta):
    minimum = min(samples)
    mean = math.fsum(math.exp(-(value - minimum) / beta) for value in samples) / len(samples)
    return minimum - beta * math.log(mean)


def predict(point, beta, x, y):
    effective = eesm(point["sinr_linear"], beta)
    return model_per(10.0 * math.log10(effective), x, y)


def neg_log_likelihood(points, beta, x, y):
    total = 0.0
    for point in points:
        try:
            per = predict(point, beta, x, y)
        except CalibrationError:
            return math.inf
        per = min(1.0 - 1e-12, max(1e-12, per))
        total -= point["errors"] * math.log(per)
        total -= (point["trials"] - point["errors"]) * math.log1p(-per)
    return total


def awgn_neg_log_likelihood(points, offset_db, x, y):
    total = 0.0
    for point in points:
        try:
            per = model_per(point["snr_db"] + offset_db, x, y)
        except CalibrationError:
            return math.inf
        per = min(1.0 - 1e-12, max(1e-12, per))
        total -= point["errors"] * math.log(per)
        total -= (point["trials"] - point["errors"]) * math.log1p(-per)
    return total


def fit_awgn_offset(points, x, y, minimum_db=-3.0, maximum_db=3.0):
    if (not math.isfinite(minimum_db) or not math.isfinite(maximum_db) or
        not minimum_db < maximum_db):
        raise CalibrationError("AWGN offset bounds must be finite and increasing")
    grid = [minimum_db + (maximum_db - minimum_db) * i / 600 for i in range(601)]
    scores = [awgn_neg_log_likelihood(points, value, x, y) for value in grid]
    best = min(range(len(scores)), key=scores.__getitem__)
    if not math.isfinite(scores[best]):
        raise CalibrationError("no AWGN offset candidate stays inside the model SNR range")
    if best in (0, 600):
        raise CalibrationError("AWGN offset optimum is at a search bound")
    left, right = grid[best - 1], grid[best + 1]
    ratio = (math.sqrt(5.0) - 1.0) / 2.0
    a = right - ratio * (right - left)
    b = left + ratio * (right - left)
    for _ in range(80):
        if awgn_neg_log_likelihood(points, a, x, y) < awgn_neg_log_likelihood(points, b, x, y):
            right, b = b, a
            a = right - ratio * (right - left)
        else:
            left, a = a, b
            b = left + ratio * (right - left)
    return (left + right) / 2.0


def fit_beta(points, x, y, beta_min, beta_max):
    if (not math.isfinite(beta_min) or not math.isfinite(beta_max) or
        not 0.0 < beta_min < beta_max):
        raise CalibrationError("beta bounds must satisfy 0 < min < max")
    low, high = math.log(beta_min), math.log(beta_max)
    grid = [low + (high - low) * i / 200 for i in range(201)]
    scores = [neg_log_likelihood(points, math.exp(value), x, y) for value in grid]
    best = min(range(len(scores)), key=scores.__getitem__)
    if not math.isfinite(scores[best]):
        raise CalibrationError("no beta candidate stays inside the AWGN SNR range")
    if best in (0, 200):
        raise CalibrationError("beta optimum is at a search bound; widen the bounds")
    left, right = grid[best - 1], grid[best + 1]
    ratio = (math.sqrt(5.0) - 1.0) / 2.0
    a = right - ratio * (right - left)
    b = left + ratio * (right - left)
    for _ in range(100):
        if neg_log_likelihood(points, math.exp(a), x, y) < neg_log_likelihood(points, math.exp(b), x, y):
            right, b = b, a
            a = right - ratio * (right - left)
        else:
            left, a = a, b
            b = left + ratio * (right - left)
    return math.exp((left + right) / 2.0)


def metrics(points, predictor):
    residuals = []
    for point in points:
        observed = point["errors"] / point["trials"]
        residuals.append(predictor(point) - observed)
    return {
        "points": len(points),
        "trials": sum(point["trials"] for point in points),
        "mae": math.fsum(abs(value) for value in residuals) / len(residuals),
        "rmse": math.sqrt(math.fsum(value * value for value in residuals) / len(residuals)),
        "max_abs_error": max(abs(value) for value in residuals),
    }


def calibrate(model_path, reference_path, beta_min=0.01, beta_max=1000.0,
              max_awgn_mae=0.05, max_validation_mae=0.10):
    metadata, x, y = load_model(model_path)
    reference = load_reference(reference_path, metadata)
    awgn_train = [p for p in reference["awgn"] if p["split"] == "train"]
    awgn_validation = [p for p in reference["awgn"] if p["split"] == "validation"]
    offset_db = fit_awgn_offset(awgn_train, x, y)
    awgn_train_metrics = metrics(awgn_train,
        lambda point: model_per(point["snr_db"] + offset_db, x, y))
    awgn_validation_metrics = metrics(awgn_validation,
        lambda point: model_per(point["snr_db"] + offset_db, x, y))
    shifted_x = [value - offset_db for value in x]
    train = [p for p in reference["selective"] if p["split"] == "train"]
    validation = [p for p in reference["selective"] if p["split"] == "validation"]
    beta = fit_beta(train, shifted_x, y, beta_min, beta_max)
    train_metrics = metrics(train, lambda point: predict(point, beta, shifted_x, y))
    validation_metrics = metrics(validation,
        lambda point: predict(point, beta, shifted_x, y))
    reasons = []
    if awgn_validation_metrics["mae"] > max_awgn_mae:
        reasons.append("held-out AWGN PER error exceeds threshold")
    if validation_metrics["mae"] > max_validation_mae:
        reasons.append("held-out frequency selective PER error exceeds threshold")
    if reference["source_kind"] == "synthetic":
        reasons.append("reference is synthetic")
    accepted = not reasons
    result = {
        "status": "validated" if accepted else "rejected",
        "reasons": reasons,
        "source": reference["source"],
        "source_version": reference["source_version"],
        "source_kind": reference["source_kind"],
        "phy": metadata,
        "awgn_model_sha256": hashlib.sha256(model_path.read_bytes()).hexdigest(),
        "reference_sha256": hashlib.sha256(reference_path.read_bytes()).hexdigest(),
        "candidate_beta_linear": beta,
        "candidate_beta_db": 10.0 * math.log10(beta),
        "candidate_awgn_snr_offset_db": offset_db,
        "awgn_train": awgn_train_metrics,
        "awgn_validation": awgn_validation_metrics,
        "train": train_metrics,
        "validation": validation_metrics,
        "thresholds": {"max_awgn_mae": max_awgn_mae,
                       "max_validation_mae": max_validation_mae},
    }
    if accepted:
        result["beta_linear"] = beta
        result["beta_db"] = 10.0 * math.log10(beta)
        result["awgn_snr_offset_db"] = offset_db
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model-awgn", required=True, type=Path)
    parser.add_argument("--reference", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--beta-min", type=float, default=0.01)
    parser.add_argument("--beta-max", type=float, default=1000.0)
    parser.add_argument("--max-awgn-mae", type=float, default=0.05)
    parser.add_argument("--max-validation-mae", type=float, default=0.10)
    args = parser.parse_args()
    try:
        if (not math.isfinite(args.max_awgn_mae) or
            not math.isfinite(args.max_validation_mae) or
            not 0.0 <= args.max_awgn_mae <= 1.0 or
            not 0.0 <= args.max_validation_mae <= 1.0):
            raise CalibrationError("MAE thresholds must lie in [0, 1]")
        if args.output.exists():
            raise CalibrationError("Existing calibration output must be preserved")
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with provenance.Run(args.output.with_suffix(args.output.suffix + ".run.json"),
                            {key: value for key, value in vars(args).items()
                             if key not in ("model_awgn", "reference", "output")},
                            inputs=(args.model_awgn, args.reference),
                            artifacts=(args.output,)) as run:
            provenance.backend_selected("offline-numpy-calibration")
            result = calibrate(args.model_awgn, args.reference, args.beta_min,
                               args.beta_max, args.max_awgn_mae,
                               args.max_validation_mae)
            args.output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n",
                                   encoding="utf-8")
            run.data["validation"] = {"calibration_status": result["status"]}
    except (CalibrationError, OSError, json.JSONDecodeError) as exc:
        parser.error(str(exc))
    print(f"status={result['status']} candidate_beta_linear={result['candidate_beta_linear']:.9g} "
          f"validation_mae={result['validation']['mae']:.6g}")
    return 0 if result["status"] == "validated" else 1


if __name__ == "__main__":
    raise SystemExit(main())
