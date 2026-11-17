"""Bounded native mechanics log ingestion and compact observational coverage."""

from __future__ import annotations

import json
import math
from pathlib import Path


def coasting_phase_estimate(trace: dict) -> dict | None:
    """262b40 stores torque before updating omega; conditional offline estimate."""
    keys = (
        "engine_angular_speed",
        "engine_torque_blend",
        "consumed_inertia",
        "physics_dt_candidate",
        "optimal_angular_speed_cache",
        "idle_angular_speed_cache",
        "engine_scale_candidate",
        "consumed_config_power",
        "max_angular_speed_cache",
    )
    values: list[float] = []
    for key in keys:
        value = trace.get(key)
        if (
            not isinstance(value, (int, float))
            or isinstance(value, bool)
            or not math.isfinite(value)
        ):
            return None
        values.append(float(value))
    omega, torque, inertia, dt, optimal, idle, scale, power, maximum = values
    if not (
        trace.get("source_verified")
        and trace.get("clocks_valid")
        and trace.get("shaft_available")
        and trace.get("shaft_gate_candidate") == 0
        and isinstance(trace.get("engine_flags_word"), int)
        and trace["engine_flags_word"] >> 16
        and trace.get("effective_throttle") == 0
        and (
            trace.get("launch_boost_timer") == 0
            or (
                trace.get("transmission_available") is True
                and isinstance(trace.get("transmission_gate_byte"), int)
                and trace["transmission_gate_byte"] != 0
            )
        )
        and inertia > 0
        and optimal > 0
        and 0 < dt <= 1
        and 0 < omega < maximum
        and abs(torque) > 1e-9
    ):
        return None
    previous = omega - torque / inertia * dt
    sign = 1 if idle > previous else (-1 if idle < previous else 0)
    predicted = sign * scale * power * 746 / optimal * previous / optimal
    if not math.isfinite(predicted):
        return None
    return {
        "omega_before_integration_estimate": previous,
        "coast_torque_estimate": predicted,
        "observed_torque": torque,
        "relative_error": abs(predicted - torque) / abs(torque),
        "scope": "conditional coasting phase estimate; not an observed function call",
    }


class MechanicsCollector:
    MAX_ROWS = 6000

    def __init__(self, source: Path, destination: Path):
        self.source, self.destination = source, destination
        self.offset, self.last_index = 0, 0
        self.previous_motion: tuple | None = None
        self.summary: dict = {
            "rows": 0,
            "invalid_rows": 0,
            "engine_rows": 0,
            "config_mismatch_rows": 0,
            "model_rows": 0,
            "motion_rows": 0,
            "source_masks": {},
            "dose_groups": {},
            "selected_vs_consumed_power_differs_rows": 0,
            "sampled_above_max_rows": 0,
            "shift_cut_timer_positive_rows": 0,
            "zero_gear_ratio_rows": 0,
            "coasting_phase_rows": 0,
            "sampled_kinematics": {
                "valid_intervals": 0,
                "units": "native position / simulation clock; SI calibration unverified",
            },
            "interpretation": "observational; unmatched inputs/routes do not establish causality",
            "dynamo": {
                "rows": 0,
                "geometry_rows": 0,
                "nitro_rows": 0,
                "eligible_rows": 0,
                "source_masks": {},
                "sensitivity_candidate_rows": {"1.25": 0, "2.5": 0, "5": 0},
                "grants": 0,
                "manual_grants": 0,
                "episodes": 0,
                "activations": 0,
                "spent": 0,
                "qualifying_seconds": 0,
                "progress_resets": 0,
                "scope": "borrowed-source drift candidate; grants mirror capped counter only, not guest pickup calls",
            },
        }
        destination.touch(exist_ok=False)

    @staticmethod
    def _range(target: dict, name: str, value) -> None:
        if type(value) not in (int, float) or not math.isfinite(value):
            return
        pair = target.setdefault(name, [value, value])
        pair[0], pair[1] = min(pair[0], value), max(pair[1], value)

    def _observe(self, row: dict) -> None:
        self._dynamo(row.get("dynamo"))
        trace = row["drivetrain"]
        self.summary["rows"] += 1
        mask = str(trace.get("source_body_mask", 0))
        masks = self.summary["source_masks"]
        masks[mask] = masks.get(mask, 0) + 1
        for flag, counter in (
            ("engine_available", "engine_rows"),
            ("model_available", "model_rows"),
            ("motion_valid", "motion_rows"),
        ):
            self.summary[counter] += int(trace.get(flag) is True)
        if (
            trace.get("engine_available")
            and trace.get("engine_config_matches") is False
        ):
            self.summary["config_mismatch_rows"] += 1
        if trace.get("engine_available"):
            if type(row.get("current")) in (int, float) and type(
                trace.get("consumed_config_power")
            ) in (int, float):
                self.summary["selected_vs_consumed_power_differs_rows"] += int(
                    not math.isclose(
                        row["current"], trace["consumed_config_power"], rel_tol=1e-5
                    )
                )
            self.summary["shift_cut_timer_positive_rows"] += int(
                trace.get("shift_cut_timer", 0) > 0
            )
        self.summary["sampled_above_max_rows"] += int(
            trace.get("model_available") is True
            and trace.get("above_max_angular_speed") is True
        )
        self.summary["zero_gear_ratio_rows"] += int(
            trace.get("transmission_available") is True and trace.get("gear_ratio") == 0
        )
        value = row.get("percent", 0)
        dose = (
            str(round(value, 1))
            if type(value) in (int, float) and math.isfinite(value)
            else "unknown"
        )
        groups = self.summary["dose_groups"]
        if dose not in groups and len(groups) >= 16:
            dose = "other"
        group = groups.setdefault(dose, {"rows": 0})
        group["rows"] += 1
        self._motion(row, trace, group)
        phase = coasting_phase_estimate(trace)
        if phase is not None:
            self.summary["coasting_phase_rows"] += 1
            self._range(group, "coasting_phase_relative_error", phase["relative_error"])
        self._range(group, "selected_power", row.get("current"))
        if trace.get("engine_available"):
            for name in (
                "consumed_config_power",
                "engine_torque_blend",
                "effective_throttle",
                "engine_angular_speed",
                "output_power_signal",
            ):
                self._range(group, name, trace.get(name))
            omega, maximum = (
                trace.get("engine_angular_speed"),
                trace.get("max_angular_speed_cache"),
            )
            if (
                type(omega) in (int, float)
                and type(maximum) in (int, float)
                and maximum > 0
            ):
                self._range(group, "engine_speed_to_limit_ratio", omega / maximum)
        if trace.get("motion_valid"):
            self._range(
                group,
                "speed_native_per_clock_unit",
                trace.get("speed_native_per_clock_unit"),
            )

    def _dynamo(self, trace) -> None:
        if not isinstance(trace, dict):
            return
        stats = self.summary["dynamo"]
        stats["rows"] += 1
        mask = str(trace.get("source_mask", 0))
        stats["source_masks"][mask] = stats["source_masks"].get(mask, 0) + 1
        for field, counter in (
            ("geometry_valid", "geometry_rows"),
            ("nitro_valid", "nitro_rows"),
            ("eligible", "eligible_rows"),
        ):
            stats[counter] += int(trace.get(field) is True)
        for field in (
            "grants",
            "manual_grants",
            "episodes",
            "activations",
            "spent",
            "qualifying_seconds",
            "progress_resets",
        ):
            value = trace.get(field)
            if type(value) in (int, float) and math.isfinite(value) and value >= 0:
                # Native counters include unsampled frames: never sum these values.
                stats[field] = max(stats[field], value)
        for field in (
            "forward",
            "lateral",
            "slip_degrees",
            "remaining",
            "fraction",
            "progress",
        ):
            self._range(stats, field, trace.get(field))
        forward, lateral = trace.get("forward"), trace.get("lateral")
        wheels, contacts = trace.get("wheels"), trace.get("contacts")
        if (
            trace.get("geometry_valid") is True
            and type(forward) in (int, float)
            and type(lateral) in (int, float)
            and math.isfinite(forward)
            and math.isfinite(lateral)
            and forward > 1
            and type(wheels) is int
            and type(contacts) is int
            and wheels > 0
            and contacts / wheels > 0.51
            and trace.get("input_suppressed") is False
            and trace.get("handbrake_suppressed") is False
        ):
            for threshold in stats["sensitivity_candidate_rows"]:
                if lateral > float(threshold):
                    stats["sensitivity_candidate_rows"][threshold] += 1

    def _motion(self, row: dict, trace: dict, group: dict) -> None:
        # Saved 6-vsync samples avoid single-vsync clock-phase aliasing.
        position, clock, car = (
            trace.get("physics_position"),
            trace.get("simulation_clock_candidate"),
            row.get("car"),
        )
        if not (
            trace.get("clock_source_verified")
            and trace.get("clocks_valid")
            and trace.get("physics_position_valid")
            and isinstance(car, str)
            and car
            and isinstance(position, list)
            and len(position) == 3
            and all(type(v) in (int, float) and math.isfinite(v) for v in position)
            and isinstance(clock, (int, float))
            and not isinstance(clock, bool)
            and math.isfinite(clock)
        ):
            self.previous_motion = None
            return
        speed = None
        if self.previous_motion is not None:
            prior_car, prior_clock, prior_position, prior_speed = self.previous_motion
            dt, distance = clock - prior_clock, math.dist(position, prior_position)
            if car == prior_car and 0 < dt <= 1 and distance <= 100:
                speed = distance / dt
                metrics = self.summary["sampled_kinematics"]
                metrics["valid_intervals"] += 1
                self._range(metrics, "speed_native_per_clock_unit", speed)
                self._range(group, "derived_speed_native_per_clock_unit", speed)
                if prior_speed is not None:
                    self._range(
                        metrics,
                        "acceleration_native_per_clock_squared",
                        (speed - prior_speed) / dt,
                    )
            elif car == prior_car and dt == 0 and distance == 0:
                return
        self.previous_motion = (car, clock, position, speed)

    def poll(self, *, start_ns: int) -> None:
        if self.summary["rows"] >= self.MAX_ROWS or not self.source.exists():
            return
        if self.source.stat().st_mtime_ns < start_ns:
            return
        with (
            self.source.open("rb") as source,
            self.destination.open("ab") as destination,
        ):
            source.seek(self.offset)
            # Native cap plus bounded per-poll work; incomplete final lines wait.
            for _ in range(256):
                line = source.readline(32769)
                if not line.endswith(b"\n"):
                    break
                self.offset = source.tell()
                try:
                    row = json.loads(line)
                    if (
                        not isinstance(row, dict)
                        or row.get("diagnostic") is not True
                        or not isinstance(row.get("drivetrain"), dict)
                    ):
                        raise ValueError("Unexpected native trace schema")
                    index = row.get("trace_rows")
                    if type(index) is not int or not 1 <= index <= self.MAX_ROWS:
                        raise ValueError("Invalid bounded trace index")
                    if index <= self.last_index:
                        continue
                    self._observe(row)
                except (ValueError, TypeError, UnicodeError):
                    self.summary["invalid_rows"] += 1
                    continue
                destination.write(line)
                self.last_index = index
                if self.summary["rows"] >= self.MAX_ROWS:
                    break
