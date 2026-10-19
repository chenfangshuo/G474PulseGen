#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# Copyright (c) 2026 Chen Fangshuo
# SPDX-License-Identifier: MIT
"""
DPT 自动扫描：固定 Vbus，扫第一脉冲宽度 tp1 -> 不同 ID。

链路：
    dpt_sweep.py
      ├─ TCP 192.168.1.3:5025  -> Siglent SDS804X HD (LAN SCPI)
      └─ TCP 127.0.0.1:5025    -> oled_mirror.py -> G474PulseGen -> 双脉冲

每个点采集 C1=VGS / C2=ID / C3=VDS，保存原始 CSV，调用
`dpt_analyzer.py` 算指标，最后输出 metrics.csv / summary.csv / summary_vs_id.png。

前置条件：
  * oled_mirror.py 已启动并连上 COM 口，TCP 5025 在监听。
  * 示波器 LAN SCPI 已打开（默认 192.168.1.3:5025）。
  * 12V 驱动电源已开，母线已充电；扫描期间只允许一个客户端连 oled_mirror。
  * Python 环境有 numpy / pandas / scipy / matplotlib（dpt_analyzer 的依赖）。
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import statistics
import sys
import time
import traceback
from dataclasses import asdict
from pathlib import Path

import numpy as np

from pulsegen_scpi import PulseGen, PulseGenError, PulseStatus
from siglent_scope import (
    ScopeError, SiglentScope, WaveDesc, Waveform, parse_preamble,
    selftest as scope_selftest,
)

HERE = Path(__file__).resolve().parent
DEFAULT_ANALYZER = Path(r"D:\Software\Blog\hugo-blog\content\posts\double-pulse-test-at-home")

DEFAULT_CONFIG = {
    "scope": {"host": "192.168.1.3", "port": 5025, "timeout": 5.0, "data_timeout": 30.0},
    "pulsegen": {"host": "127.0.0.1", "port": 5025, "timeout": 8.0, "channel": 1},
    "sweep": {"pw1_us": [2, 4, 6, 8, 10, 12, 14, 16, 18], "intv_us": 5, "pw2_us": 3},
    "repeat": 3,
    "settle_s": 0.5,
    "cool_down_s": 0.5,
    "arm_settle_s": 0.3,
    "limits": {
        "max_id_A": 35.0,
        "max_vpeak_V": 50.0,     # 母线固态电容 50V/470uF
        "warn_vpeak_V": 45.0,
        "max_ipeak_A": 60.0,
        "min_vbus_V": 19.0,
        "max_vbus_V": 21.0,
    },
    "trigger": {"source": "C1", "level_V": 2.0, "slope": "RISing"},
    "analysis": {
        "window_pre_ns": 80.0,
        "window_post_ns": 800.0,
        "id_scale_A_per_V": 1.0,
        "win_pulse2_early": [0.02, 0.05],
        "capture_check": True,
        "deskew_id_ns": 0.0,
    },
    "paths": {"analyzer_path": str(DEFAULT_ANALYZER), "output_root": "sweeps"},
    "per_point_plot": False,
    "manual_trigger": False,
    "stop_on_safety": True,
    "stop_on_analysis_error": False,
    "drift_check": False,
}


class SweepError(Exception):
    pass


class PointError(Exception):
    pass


def deep_merge(base: dict, override: dict) -> dict:
    out = dict(base)
    for k, v in override.items():
        if isinstance(v, dict) and isinstance(out.get(k), dict):
            out[k] = deep_merge(out[k], v)
        else:
            out[k] = v
    return out


def load_config(path: Path | None) -> dict:
    cfg = DEFAULT_CONFIG
    if path is not None:
        cfg = deep_merge(cfg, json.loads(path.read_text(encoding="utf-8")))
    return cfg


def write_json(path: Path, obj) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(obj, ensure_ascii=False, indent=2, default=str), encoding="utf-8")


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def import_analyzer(path: Path):
    if not (path / "dpt_analyzer.py").exists():
        raise SweepError(f"analyzer_path 下找不到 dpt_analyzer.py：{path}")
    sys.path.insert(0, str(path))
    import dpt_analyzer as dpt  # type: ignore
    return dpt


# ---------------------------------------------------------------------------
# 仪器初始化 / 检查
# ---------------------------------------------------------------------------

def connect_scope(cfg: dict, verbose: bool = False) -> SiglentScope:
    s = cfg["scope"]
    return SiglentScope(
        host=s["host"], port=int(s["port"]),
        timeout=float(s["timeout"]), data_timeout=float(s["data_timeout"]),
        verbose=verbose,
    ).connect()


def connect_pulsegen(cfg: dict, dry: bool = False, verbose: bool = False) -> PulseGen:
    p = cfg["pulsegen"]
    return PulseGen(
        host=p["host"], port=int(p["port"]), timeout=float(p["timeout"]),
        dry=dry, verbose=verbose,
    ).connect()


def check_scope(scope: SiglentScope, cfg: dict) -> dict:
    print(f"[scope] *IDN? -> {scope.idn()}")
    acq_type = scope.acquire_type()
    if "NORMAL" not in acq_type and "NORM" not in acq_type:
        raise SweepError(f"示波器采集类型是 {acq_type!r}，必须是 NORMAL（平均/ERES 会改变峰值）")
    points = scope.acquire_points()
    timebase = scope.timebase_scale()
    maxp = scope.max_point()
    print(f"[scope] points={points}, timebase={timebase*1e6:.3f} us/div, max_point={maxp}")
    print(f"[scope] acq_type={acq_type}, mdepth={scope.acquire_mdepth()}")
    # 触发源除了 C1，也允许 C4（Y7 SYNC）或 EX；对应的通道要打开/可见。
    trig_src = str(cfg["trigger"]["source"]).strip().upper()
    ch_list = [1, 2, 3]
    if trig_src.startswith("C") and trig_src[1:].isdigit():
        ch = int(trig_src[1:])
        if ch not in ch_list:
            ch_list.append(ch)
    chans = scope.configure_channels(tuple(ch_list))
    for ch, info in chans.items():
        print(f"[scope] CH{ch}: unit={info['unit']}, scale={info['scale']:.6g}, "
              f"offset={info['offset']:.6g}, probe={info['probe']:.6g}")
    if chans[1]["unit"] != "V":
        raise SweepError(f"CH1 单位是 {chans[1]['unit']}，应为 V")
    if chans[2]["unit"] != "A":
        raise SweepError(f"CH2 单位是 {chans[2]['unit']}，应为 A（电流探头）")
    if chans[3]["unit"] != "V":
        raise SweepError(f"CH3 单位是 {chans[3]['unit']}，应为 V")
    print(f"[scope] trigger mode={scope.trigger_mode()}, status={scope.trigger_status()}")
    scope.write(":WAVeform:SOURce C1")
    scope.write(":WAVeform:WIDTh WORD")
    scope.write(":WAVeform:BYTeorder LSB")
    pre = scope.query_block(":WAVeform:PREamble?", expected_bytes=346)
    desc = parse_preamble(pre)
    print(f"[scope] preamble: points={desc.points}, interval={desc.interval*1e9:.4f} ns, "
          f"delay={desc.delay*1e6:.4f} us, vdiv={desc.vdiv:.6g}, "
          f"offset={desc.voffset:.6g}, probe={desc.probe:.6g}, adc={desc.adc_bit} bit")
    if desc.points == 0:
        print("[scope] 当前没有波形数据（可能尚未采集）；扫描时会先触发再读")
    return {
        "points": points, "timebase_s_div": timebase, "max_point": maxp,
        "acq_type": acq_type, "mdepth": scope.acquire_mdepth(),
        "channels": chans, "preamble": asdict(desc),
        "trigger_mode": scope.trigger_mode(),
        "trigger_status": scope.trigger_status(),
    }


def check_pulsegen(pg: PulseGen) -> dict:
    print(f"[pulsegen] *IDN? -> {pg.idn()}")
    st = pg.status()
    print(f"[pulsegen] STAT: MODE={st.mode} OUT={st.out} 12V={st.v12} "
          f"CH={st.ch} FR={st.fr} ST={st.st} OR={st.ore} RX={st.rx} LNK={st.lnk}")
    return asdict(st)


def setup_pulsegen(pg: PulseGen, cfg: dict, first_pw1: int | None = None) -> None:
    ch = int(cfg["pulsegen"]["channel"])
    intv = int(cfg["sweep"]["intv_us"])
    pw2 = int(cfg["sweep"]["pw2_us"])
    pg.setup_double(channel=ch, intv_us=intv, pw2_us=pw2)
    # 先写好第一个点的 PW1，再开输出；避免 OUTP:ON 时用默认 PW1 先打一发。
    if first_pw1 is not None:
        pg.set_double_pulse(int(first_pw1), intv, pw2)
    st = pg.status()
    if st.out != "ON":
        pg.output(True)
        st = pg.status()
    if st.mode != "DPULSE" or st.out != "ON" or st.ch not in (ch, ch + 1):
        raise SweepError(f"发波板状态不对：{st}")
    print(f"[pulsegen] DPULSE ready: CH={ch}, INTV={intv} us, PW2={pw2} us, OUT=ON")


# ---------------------------------------------------------------------------
# 采集一个点
# ---------------------------------------------------------------------------

def capture_point(scope: SiglentScope, pg: PulseGen, cfg: dict, pw1: int, rep: int) -> dict:
    ch = int(cfg["pulsegen"]["channel"])
    pg.set_pw1(pw1)
    st_pre = pg.status()
    if st_pre.out != "ON" or st_pre.mode != "DPULSE" or st_pre.ch not in (ch, ch + 1):
        raise PointError(f"TRIG 前发波板状态不对：{st_pre}")

    trig_cfg = cfg["trigger"]
    scope.arm_single(
        source=str(trig_cfg["source"]),
        level=float(trig_cfg["level_V"]),
        slope=str(trig_cfg["slope"]),
    )
    scope.wait_ready()
    # 手动/控制台触发天然有人的延迟；脚本这里显式等一会儿，避开 arm 后的瞬态。
    time.sleep(float(cfg.get("arm_settle_s", 0.3)))
    num_pre = scope.num_acq()

    trig_ok = True
    trig_ambiguous = False
    if cfg.get("manual_trigger", False):
        print("  [manual] 请按发波板/OLED 的 TRIG（等待 60 s）...", flush=True)
        scope.wait_stop(timeout=60.0)
    else:
        try:
            pg.trigger()
        except PulseGenError as exc:
            trig_ok = False
            status = scope.trigger_status()
            if status != "STOP":
                raise PointError(f"TRIG 响应丢失且示波器未 Stop（{status}）：{exc}") from exc
            trig_ambiguous = True
            print(f"  [!] TRIG 响应丢失，但示波器已 Stop；保留该帧（ambiguous）")
        scope.wait_stop(timeout=5.0)
    time.sleep(0.08)
    num_post = scope.num_acq()
    if not trig_ambiguous and num_post == num_pre:
        raise PointError("示波器没有发生新采集（NUMACq 未变化）")

    waves = scope.read_all_channels(
        ("C1", "C2", "C3"),
        id_scale=float(cfg["analysis"]["id_scale_A_per_V"]),
    )
    status_post = scope.trigger_status()
    if status_post != "STOP":
        raise PointError(f"读波形后示波器状态是 {status_post}，不是 Stop")
    st_post = pg.status()
    return {
        "waves": waves, "st_pre": st_pre, "st_post": st_post,
        "trig_ok": trig_ok, "trig_ambiguous": trig_ambiguous,
        "num_pre": num_pre, "num_post": num_post, "status_post": status_post,
    }


def _timing_ok(timing, pw1_us: int, intv_us: int, pw2_us: int) -> bool:
    tol_tp1 = max(0.5e-6, 0.05 * pw1_us * 1e-6)
    return (
        abs(timing.tp1 - pw1_us * 1e-6) <= tol_tp1
        and abs(timing.t_dead - intv_us * 1e-6) <= 0.5e-6
        and abs(timing.tp2 - pw2_us * 1e-6) <= 0.5e-6
        and not timing.truncated
    )


def _median_or_none(a) -> float | None:
    try:
        arr = np.asarray(a, dtype=float)
        arr = arr[np.isfinite(arr)]
        return float(np.median(arr)) if len(arr) else None
    except Exception:
        return None


def save_raw_csv(path: Path, src: str, wave) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as f:
        f.write(f"Record Length,Analog:{len(wave.time)}\n")
        f.write(f"Sample Interval,{wave.dt:.12E}\n")
        f.write(f"Vertical Units,{wave.unit},\n")
        f.write(f"Vertical Scale,{wave.scale:.9E},\n")
        f.write(f"Vertical Offset,{wave.offset:.9E},\n")
        f.write("Horizontal Units,s\n")
        f.write(f"Source,{src}\n")
        f.write("Second,Value\n")
        for t, v in zip(wave.time, wave.value):
            f.write(f"{t:.12E},{v:.9E}\n")


def process_point(dpt, cfg: dict, cap: dict, pw1: int, rep: int, run_dir: Path,
                  run_id: str, ids: dict) -> tuple[dict, bool]:
    """保存原始数据，跑分析，返回 (metrics row, safety_fail)。"""
    waves = cap["waves"]
    intv = int(cfg["sweep"]["intv_us"])
    pw2 = int(cfg["sweep"]["pw2_us"])

    base = f"pw1_{pw1:02d}us_rep{rep}"
    raw_sub = run_dir / "raw" / base
    raw_sub.mkdir(parents=True, exist_ok=True)
    for src, wave in waves.items():
        save_raw_csv(raw_sub / f"{base}_{src}_data.csv", src, wave)

    # 各通道必须同一时间轴
    t = waves["C1"].time
    for src in ("C2", "C3"):
        if len(waves[src].time) != len(t):
            raise PointError(f"{src} 点数与 C1 不一致")

    warnings: list[str] = []
    timing = None
    levels = None
    metrics: dict = {}
    ctx = None
    analysis_ok = False
    error = ""
    capture_ratio = None
    try:
        data = dpt.DptData(
            t=t,
            vgs_raw=waves["C1"].value,
            id_raw=waves["C2"].value,
            vds_raw=waves["C3"].value,
            dt=float(np.median(np.diff(t))),
            fs=float(1.0 / np.median(np.diff(t))),
            warnings=[],
        )
        # PW2=3us 时把恢复基线窗口从默认 0.5%-2% 调到 2%-5%
        dpt.WIN_PULSE2_EARLY = tuple(cfg["analysis"]["win_pulse2_early"])
        acfg = cfg["analysis"]
        acfg_dpt = dpt.Config(
            window_mode="manual",
            window_pre_ns=float(acfg["window_pre_ns"]),
            window_post_ns=float(acfg["window_post_ns"]),
            plot=False, show=False, rogowski=False,
            deskew_id_ns=float(acfg["deskew_id_ns"]),
            out_dir=run_dir,
        )
        timing, w = dpt.detect_pulses(data.t, data.vgs_raw, acfg_dpt)
        metrics, ctx, levels, w2 = dpt.analyze(data, timing, acfg_dpt)
        warnings.extend(w)
        warnings.extend(w2)
        analysis_ok = True

        if cfg.get("per_point_plot", False):
            try:
                acfg_dpt.out_dir = raw_sub
                dpt.plot_analysis(data, timing, levels, metrics, ctx, acfg_dpt)
            except Exception as exc:
                warnings.append(f"per-point plot 失败：{exc}")

        if acfg.get("capture_check", True):
            try:
                dpt.WIN_PULSE2_EARLY = tuple(acfg["win_pulse2_early"])
                narrow = dpt.Config(
                    window_mode="manual",
                    window_pre_ns=float(acfg["window_pre_ns"]),
                    window_post_ns=200.0,
                    plot=False, show=False, rogowski=False,
                    deskew_id_ns=float(acfg["deskew_id_ns"]),
                    out_dir=run_dir,
                )
                m200, _, _, _ = dpt.analyze(data, timing, narrow)
                e_wide = metrics.get("eoff_uJ")
                e_narrow = m200.get("eoff_uJ")
                if e_wide and e_narrow and e_wide > 0:
                    capture_ratio = float(e_narrow / e_wide)
                    if capture_ratio < 0.99:
                        warnings.append(f"Eoff 200ns 窗口只捕获到 {capture_ratio*100:.1f}%")
            except Exception as exc:
                warnings.append(f"Eoff 窗口捕获率检查失败：{exc}")
    except Exception as exc:
        error = f"{type(exc).__name__}: {exc}"
        warnings.append(error)

    # 门禁
    vbus_pre = metrics.get("vbus_V")
    vbus_post = None
    id_tail = None
    if timing is not None:
        vds = waves["C3"].value
        idv = waves["C2"].value
        tail_m = t > (timing.t_off2 + 5e-6)
        if tail_m.any():
            vbus_post = float(np.median(vds[tail_m]))
            id_tail = float(np.median(idv[tail_m]))

    def _code_clipped(src: str) -> bool:
        raw = waves[src].raw
        return bool(len(raw) and np.max(np.abs(raw.astype(np.int64))) > 32000)

    clipped = {src: _code_clipped(src) for src in ("C1", "C2", "C3")}
    pre_ok = levels is not None and abs(levels.id_base_pre) < 0.5
    # 尾段电流应回到“采集前的基线”，而不是绝对 0（探头零点可能不是 0）。
    tail_ok = id_tail is None or (
        levels is not None and abs(id_tail - levels.id_base_pre) < 0.5
    )
    vbus_ok = (
        vbus_pre is not None
        and cfg["limits"]["min_vbus_V"] <= vbus_pre <= cfg["limits"]["max_vbus_V"]
        and (vbus_post is None or cfg["limits"]["min_vbus_V"] <= vbus_post <= cfg["limits"]["max_vbus_V"])
    )
    timing_ok = timing is not None and _timing_ok(timing, pw1, intv, pw2)
    qrr_trunc = bool(metrics.get("qrr_truncated", False))
    any_clip = any(clipped.values())

    i_off = metrics.get("i_off_A")
    vpeak = metrics.get("vpeak_V")
    ipeak = metrics.get("ipeak_A")
    safety_fail = False
    if i_off is not None and i_off > cfg["limits"]["max_id_A"]:
        safety_fail = True
        warnings.append(f"I_off={i_off:.2f} A 超过上限 {cfg['limits']['max_id_A']} A")
    if vpeak is not None and vpeak > cfg["limits"]["max_vpeak_V"]:
        safety_fail = True
        warnings.append(f"Vpeak={vpeak:.2f} V 超过 50V 电容硬限")
    if vpeak is not None and vpeak > cfg["limits"]["warn_vpeak_V"]:
        warnings.append(f"Vpeak={vpeak:.2f} V 接近 50V 电容上限")
    if ipeak is not None and ipeak > cfg["limits"]["max_ipeak_A"]:
        safety_fail = True
        warnings.append(f"Ipeak={ipeak:.2f} A 超过上限 {cfg['limits']['max_ipeak_A']} A")

    reasons = []
    if not analysis_ok:
        reasons.append("analysis_error")
    if not timing_ok:
        reasons.append("timing")
    if not pre_ok:
        reasons.append("pre_current")
    if not tail_ok:
        reasons.append("tail_current")
    if not vbus_ok:
        reasons.append("vbus")
    if any_clip:
        reasons.append("clipped")
    if cap["trig_ambiguous"]:
        reasons.append("trig_ambiguous")
    if safety_fail:
        reasons.append("safety")

    # Qrr 在 PW2=3us 时只作参考（可能 truncated），不因此丢掉 Eon/Eoff 有效的点。
    included = analysis_ok and timing_ok and pre_ok and tail_ok and vbus_ok \
        and not any_clip and not cap["trig_ambiguous"] and not safety_fail

    # 原始 meta
    meta = {
        "run_id": run_id,
        "pw1_cmd_us": pw1, "intv_us": intv, "pw2_us": pw2, "rep": rep,
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "scope_idn": ids.get("scope_idn"),
        "pulsegen_idn": ids.get("pulsegen_idn"),
        "trigger": cfg["trigger"],
        "analysis": cfg["analysis"],
        "id_scale_A_per_V": cfg["analysis"]["id_scale_A_per_V"],
        "preamble": {src: {
            "points": w.desc.points, "interval_s": w.desc.interval,
            "delay_s": w.desc.delay, "vdiv": w.desc.vdiv,
            "voffset": w.desc.voffset, "probe": w.desc.probe,
            "adc_bit": w.desc.adc_bit, "unit": w.unit,
            "scale": w.scale, "offset": w.offset,
        } for src, w in waves.items()},
        "trig_ok": cap["trig_ok"], "trig_ambiguous": cap["trig_ambiguous"],
        "num_pre": cap["num_pre"], "num_post": cap["num_post"],
        "st_pre": asdict(cap["st_pre"]), "st_post": asdict(cap["st_post"]),
        "warnings": warnings,
    }
    write_json(raw_sub / f"{base}_meta.json", meta)

    row = {
        "run_id": run_id,
        "timestamp": meta["timestamp"],
        "pw1_cmd_us": pw1,
        "pw1_meas_us": None if timing is None else timing.tp1 * 1e6,
        "tp2_meas_us": None if timing is None else timing.tp2 * 1e6,
        "t_dead_meas_us": None if timing is None else timing.t_dead * 1e6,
        "timing_ok": int(timing_ok),
        "rep": rep,
        "expected_id_A": 20.0 * (pw1 * 1e-6) / 10.7e-6,
        "vbus_pre_V": vbus_pre,
        "vbus_post_V": vbus_post,
        "i_off_A": i_off,
        "id_base_pre_A": None if levels is None else levels.id_base_pre,
        "id_base_dead_A": None if levels is None else levels.id_base_dead,
        "vgs_high_V": metrics.get("vgs_high_V"),
        "vds_on_V": metrics.get("vds_on_V"),
        "vds_off_V": metrics.get("vds_off_V"),
        "vpeak_V": vpeak,
        "vpeak_over_V": metrics.get("vpeak_over_V"),
        "ipeak_A": ipeak,
        "irm_A": metrics.get("irm_A"),
        "irm_plat_A": metrics.get("irm_plat_A"),
        "qrr_nC": metrics.get("qrr_nC"),
        "qrr_truncated": int(qrr_trunc),
        "tf_ns": metrics.get("tf_ns"),
        "tr_ns": metrics.get("tr_ns"),
        "tdoff_ns": metrics.get("tdoff_ns"),
        "tdon_ns": metrics.get("tdon_ns"),
        "tdvon_ns": metrics.get("tdvon_ns"),
        "trv_ns": metrics.get("trv_ns"),
        "tdvoff_ns": metrics.get("tdvoff_ns"),
        "tvf_ns": metrics.get("tvf_ns"),
        "didt_off_A_ns": metrics.get("didt_off_A_ns"),
        "dvdt_off_V_ns": metrics.get("dvdt_off_V_ns"),
        "didt_on_A_ns": metrics.get("didt_on_A_ns"),
        "dvdt_on_V_ns": metrics.get("dvdt_on_V_ns"),
        "eoff_uJ": metrics.get("eoff_uJ"),
        "eon_uJ": metrics.get("eon_uJ"),
        "etotal_uJ": metrics.get("etotal_uJ"),
        "eoff_capture_ratio": capture_ratio,
        "l_load_uH": metrics.get("l_load_uH"),
        "l_load_r2": metrics.get("l_load_r2"),
        "l_loop_nH": metrics.get("l_loop_nH"),
        "pre_ns": metrics.get("pre_ns"),
        "post_ns": metrics.get("post_ns"),
        "clipped_ch1": int(clipped["C1"]),
        "clipped_ch2": int(clipped["C2"]),
        "clipped_ch3": int(clipped["C3"]),
        "trig_ok": int(cap["trig_ok"]),
        "board_ore_delta": cap["st_post"].ore - cap["st_pre"].ore,
        "board_rx_delta": cap["st_post"].rx - cap["st_pre"].rx,
        "analysis_ok": int(analysis_ok),
        "included": int(included),
        "exclude_reason": ";".join(reasons),
        "warning_count": len(warnings),
        "warnings": "; ".join(warnings),
        "raw_dir": str(raw_sub),
        "error": error,
    }
    return row, safety_fail


# ---------------------------------------------------------------------------
# 汇总 / 出图
# ---------------------------------------------------------------------------

SUMMARY_KEYS = [
    "pw1_meas_us", "i_off_A", "vbus_pre_V", "vbus_post_V",
    "eon_uJ", "eoff_uJ", "etotal_uJ", "eoff_capture_ratio",
    "tr_ns", "tf_ns", "tdon_ns", "tdoff_ns", "tdvon_ns", "trv_ns", "tdvoff_ns", "tvf_ns",
    "didt_off_A_ns", "dvdt_off_V_ns", "didt_on_A_ns", "dvdt_on_V_ns",
    "vpeak_V", "vpeak_over_V", "ipeak_A", "irm_A", "irm_plat_A",
    "qrr_nC", "l_load_uH", "l_load_r2", "l_loop_nH", "vgs_high_V",
]

METRIC_FIELDS = [
    "run_id", "timestamp", "pw1_cmd_us", "pw1_meas_us", "tp2_meas_us",
    "t_dead_meas_us", "timing_ok", "rep", "expected_id_A",
    "vbus_pre_V", "vbus_post_V", "i_off_A", "id_base_pre_A", "id_base_dead_A",
    "vgs_high_V", "vds_on_V", "vds_off_V",
    "vpeak_V", "vpeak_over_V", "ipeak_A", "irm_A", "irm_plat_A",
    "qrr_nC", "qrr_truncated",
    "tf_ns", "tr_ns", "tdoff_ns", "tdon_ns", "tdvon_ns", "trv_ns", "tdvoff_ns", "tvf_ns",
    "didt_off_A_ns", "dvdt_off_V_ns", "didt_on_A_ns", "dvdt_on_V_ns",
    "eoff_uJ", "eon_uJ", "etotal_uJ", "eoff_capture_ratio",
    "l_load_uH", "l_load_r2", "l_loop_nH", "pre_ns", "post_ns",
    "clipped_ch1", "clipped_ch2", "clipped_ch3",
    "trig_ok", "board_ore_delta", "board_rx_delta",
    "analysis_ok", "included", "exclude_reason",
    "warning_count", "warnings", "raw_dir", "error",
]


def _num(v):
    return v is not None and isinstance(v, (int, float)) and np.isfinite(v)


def write_metrics(path: Path, rows: list[dict]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8-sig", newline="") as f:
        w = csv.DictWriter(f, fieldnames=METRIC_FIELDS, extrasaction="ignore", restval="")
        w.writeheader()
        for row in rows:
            w.writerow(row)


def summarize(rows: list[dict]) -> list[dict]:
    groups: dict[int, list[dict]] = {}
    for row in rows:
        groups.setdefault(int(row["pw1_cmd_us"]), []).append(row)
    out = []
    for pw1, group in sorted(groups.items()):
        inc = [r for r in group if r.get("included")]
        rec = {
            "pw1_cmd_us": pw1,
            "n_total": len(group),
            "n_included": len(inc),
        }
        for key in SUMMARY_KEYS:
            vals = [r[key] for r in inc if _num(r.get(key))]
            if not vals:
                rec[f"{key}_med"] = None
                rec[f"{key}_min"] = None
                rec[f"{key}_max"] = None
                rec[f"{key}_cv"] = None
                continue
            med = statistics.median(vals)
            rec[f"{key}_med"] = med
            rec[f"{key}_min"] = min(vals)
            rec[f"{key}_max"] = max(vals)
            mean = statistics.mean(vals)
            sd = statistics.pstdev(vals) if len(vals) > 1 else 0.0
            rec[f"{key}_cv"] = (sd / abs(mean)) if mean else None
        # 重复性标记
        flags = []
        for key in ("eoff_uJ", "eon_uJ", "i_off_A"):
            cv = rec.get(f"{key}_cv")
            if cv is not None and cv > 0.15:
                flags.append(f"{key} CV={cv*100:.1f}%")
        rec["repeatability_flag"] = "; ".join(flags)
        out.append(rec)
    return out


def write_summary_csv(path: Path, summary: list[dict]) -> None:
    if not summary:
        path.write_text("", encoding="utf-8")
        return
    fields = list(summary[0].keys())
    with path.open("w", encoding="utf-8-sig", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for row in summary:
            w.writerow(row)


def write_summary_md(path: Path, summary: list[dict]) -> None:
    lines = [
        "| I_off (A) | Eon (µJ) | Eoff (µJ) | tr (ns) | tf (ns) | Vpeak (V) | IRM (A) | Qrr (nC) |",
        "|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for rec in summary:
        def f(key, nd=2):
            v = rec.get(f"{key}_med")
            return "N/A" if v is None else f"{v:.{nd}f}"
        lines.append(
            f"| {f('i_off_A')} | {f('eon_uJ', 3)} | {f('eoff_uJ', 3)} | "
            f"{f('tr_ns')} | {f('tf_ns')} | {f('vpeak_V')} | {f('irm_A')} | {f('qrr_nC', 0)} |"
        )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    with path.open("a", encoding="utf-8") as f:
        f.write("\n> Qrr 在 PW2=3 µs 下可能未完全恢复，只作参考；"
                "要正式 Qrr/IRM 请补一个 PW2=10 µs 的单点。\n")


def make_plot(path: Path, summary: list[dict]) -> None:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from scipy.interpolate import PchipInterpolator

    recs = [r for r in summary if r.get("i_off_A_med") is not None]
    if not recs:
        print("[plot] 没有可画的 included 数据")
        return

    x = np.array([r["i_off_A_med"] for r in recs], dtype=float)

    def series(key: str):
        y = np.array([r.get(f"{key}_med") if r.get(f"{key}_med") is not None else np.nan for r in recs], dtype=float)
        lo = np.array([r.get(f"{key}_min") if r.get(f"{key}_min") is not None else np.nan for r in recs], dtype=float)
        hi = np.array([r.get(f"{key}_max") if r.get(f"{key}_max") is not None else np.nan for r in recs], dtype=float)
        yerr = np.vstack([np.nan_to_num(y - lo, nan=0.0), np.nan_to_num(hi - y, nan=0.0)])
        return y, yerr

    def draw(ax, xv, yv, err, color, label, marker="o", min_x=None, smooth=True):
        """测量点 + min/max 误差棒 + PCHIP 平滑线。"""
        mask = np.isfinite(xv) & np.isfinite(yv)
        if min_x is not None:
            mask &= xv >= min_x
        x2, y2 = xv[mask], yv[mask]
        e2 = err[:, mask] if err is not None else None
        ax.errorbar(
            x2, y2, yerr=e2, fmt=marker, linestyle="none",
            color=color, label=label, capsize=3, ms=5,
        )
        if smooth and len(x2) >= 2:
            order = np.argsort(x2)
            x3, y3 = x2[order], y2[order]
            keep = np.concatenate(([True], np.diff(x3) > 0))
            x3, y3 = x3[keep], y3[keep]
            if len(x3) >= 2:
                xs = np.linspace(x3.min(), x3.max(), 300)
                ys = PchipInterpolator(x3, y3)(xs)
                ax.plot(xs, ys, color=color, lw=2.2, alpha=0.9)

    fig, axs = plt.subplots(2, 2, figsize=(12, 8.5))
    axs = axs.ravel()

    # Eon / Eoff
    for key, color, label in (("eon_uJ", "#1f77b4", "Eon"), ("eoff_uJ", "#d62728", "Eoff")):
        y, err = series(key)
        draw(axs[0], x, y, err, color, label)
    axs[0].set_xlabel("I_off (A)")
    axs[0].set_ylabel("Energy (µJ)")
    axs[0].set_title("Eon / Eoff vs I_off")
    axs[0].grid(True, ls="--", alpha=0.35)
    axs[0].legend()

    # tr / tf / tdon / tdoff
    for key, color, label, marker in (
        ("tr_ns", "#1f77b4", "tr", "o"),
        ("tf_ns", "#d62728", "tf", "s"),
        ("tdon_ns", "#2ca02c", "tdon", "^"),
        ("tdoff_ns", "#9467bd", "tdoff", "D"),
    ):
        y, err = series(key)
        draw(axs[1], x, y, err, color, label, marker=marker)
    axs[1].set_xlabel("I_off (A)")
    axs[1].set_ylabel("Time (ns)")
    axs[1].set_title("tr / tf / tdon / tdoff vs I_off")
    axs[1].grid(True, ls="--", alpha=0.35)
    axs[1].legend(ncol=2)

    # Vpeak / IRM
    y, err = series("vpeak_V")
    draw(axs[2], x, y, err, "#1f77b4", "Vpeak")
    axs[2].set_xlabel("I_off (A)")
    axs[2].set_ylabel("Vpeak (V)", color="#1f77b4")
    axs[2].set_title("Vpeak / IRM vs I_off")
    axs[2].grid(True, ls="--", alpha=0.35)
    ax2b = axs[2].twinx()
    y, err = series("irm_A")
    draw(ax2b, x, y, err, "#d62728", "IRM", marker="s")
    ax2b.set_ylabel("IRM (A)", color="#d62728")
    h1, l1 = axs[2].get_legend_handles_labels()
    h2, l2 = ax2b.get_legend_handles_labels()
    axs[2].legend(h1 + h2, l1 + l2, loc="best")

    # Qrr / L_loop
    y, err = series("qrr_nC")
    # 低电流点的 Qrr 也容易被基线/恢复过程放大；只画 I_off >= 6 A。
    draw(axs[3], x, y, err, "#1f77b4", "Qrr", min_x=6.0)
    axs[3].set_xlabel("I_off (A)")
    axs[3].set_ylabel("Qrr (nC)", color="#1f77b4")
    axs[3].set_title("Qrr / L_loop vs I_off")
    axs[3].grid(True, ls="--", alpha=0.35)
    ax3b = axs[3].twinx()
    y, err = series("l_loop_nH")
    # 低电流点的过冲/dI/dt 太小，L_loop 估算不可靠；只画 I_off >= 10 A。
    draw(ax3b, x, y, err, "#2ca02c", "L_loop", marker="s", min_x=10.0)
    ax3b.set_ylabel("L_loop (nH)", color="#2ca02c")
    h1, l1 = axs[3].get_legend_handles_labels()
    h2, l2 = ax3b.get_legend_handles_labels()
    axs[3].legend(h1 + h2, l1 + l2, loc="best")
    axs[3].text(
        0.02, 0.03,
        "Qrr: I_off >= 6 A; L_loop: I_off >= 10 A\nlow-current estimates unreliable",
        transform=axs[3].transAxes, fontsize=8.5, color="#9aa8be",
    )

    fig.suptitle("DPT sweep: parameters vs I_off", fontsize=13, fontweight="bold")
    fig.tight_layout(rect=[0, 0, 1, 0.95])
    path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(path, dpi=200)
    plt.close(fig)
    print(f"[plot] {path}")


# ---------------------------------------------------------------------------
# 扫描主流程
# ---------------------------------------------------------------------------

def run_sweep(dpt, cfg: dict, run_dir: Path, verbose: bool = False) -> list[dict]:
    run_id = run_dir.name
    run_dir.mkdir(parents=True, exist_ok=True)
    (run_dir / "raw").mkdir(exist_ok=True)
    (run_dir / "processed").mkdir(exist_ok=True)
    (run_dir / "plots").mkdir(exist_ok=True)

    analyzer_path = Path(cfg["paths"]["analyzer_path"])
    analyzer_sha = ""
    try:
        analyzer_sha = sha256(analyzer_path / "dpt_analyzer.py")
    except Exception:
        pass
    config_snapshot = dict(cfg)
    config_snapshot["_analyzer_sha256"] = analyzer_sha
    config_snapshot["_run_id"] = run_id
    write_json(run_dir / "config.json", config_snapshot)

    scope = None
    pg = None
    ids = {}
    rows: list[dict] = []
    abort = False
    sweep_error: BaseException | None = None
    try:
        scope = connect_scope(cfg, verbose=verbose)
        pg = connect_pulsegen(cfg, verbose=verbose)
        check = check_scope(scope, cfg)
        ids["scope_idn"] = scope.idn()
        ids["pulsegen_idn"] = pg.idn()
        ids["scope_settings"] = check
        check_pulsegen(pg)
        pw1_list = [int(x) for x in cfg["sweep"]["pw1_us"]]
        setup_pulsegen(pg, cfg, first_pw1=pw1_list[0])

        repeat = int(cfg["repeat"])
        total = len(pw1_list) * repeat
        idx = 0
        for pw1 in pw1_list:
            for rep in range(1, repeat + 1):
                idx += 1
                print(f"\n[{idx}/{total}] pw1={pw1} us, rep={rep}")
                try:
                    cap = capture_point(scope, pg, cfg, pw1, rep)
                    row, safety_fail = process_point(
                        dpt, cfg, cap, pw1, rep, run_dir, run_id, ids
                    )
                    rows.append(row)
                    status = "OK" if row["included"] else f"EXCLUDED ({row['exclude_reason']})"
                    print(f"  I_off={row['i_off_A'] if row['i_off_A'] is not None else float('nan'):.2f} A, "
                          f"Eon={row['eon_uJ'] if row['eon_uJ'] is not None else float('nan'):.3f} uJ, "
                          f"Eoff={row['eoff_uJ'] if row['eoff_uJ'] is not None else float('nan'):.3f} uJ, "
                          f"Vpeak={row['vpeak_V'] if row['vpeak_V'] is not None else float('nan'):.2f} V -> {status}")
                    if row["warnings"]:
                        print(f"  warnings: {row['warnings']}")
                    if safety_fail and cfg["stop_on_safety"]:
                        print("  [ABORT] 安全上限触发，停止扫描")
                        abort = True
                        break
                except Exception as exc:
                    err = f"{type(exc).__name__}: {exc}"
                    print(f"  [FAIL] {err}")
                    if verbose:
                        traceback.print_exc()
                    rows.append({
                        "run_id": run_id,
                        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S"),
                        "pw1_cmd_us": pw1, "rep": rep,
                        "analysis_ok": 0, "included": 0,
                        "exclude_reason": "capture_error",
                        "error": err, "warnings": err, "warning_count": 1,
                        "raw_dir": "",
                    })
                    if cfg["stop_on_analysis_error"]:
                        abort = True
                        break
                time.sleep(float(cfg["settle_s"]))
            if abort:
                break
            time.sleep(float(cfg["cool_down_s"]))
    except BaseException as exc:
        sweep_error = exc
        if not isinstance(exc, KeyboardInterrupt):
            print(f"\n[ERROR] 扫描中止：{type(exc).__name__}: {exc}")
            if verbose:
                traceback.print_exc()
    finally:
        if pg is not None:
            try:
                pg.output(False)
            except Exception:
                pass
            try:
                pg.close()
            except Exception:
                pass
        if scope is not None:
            try:
                scope.close()
            except Exception:
                pass

    write_metrics(run_dir / "metrics.csv", rows)
    # 失败行字段不全时 summary 仍可用；只统计有 included 字段的
    rows_for_summary = [r for r in rows if "included" in r and "i_off_A" in r]
    summary = summarize(rows_for_summary)
    write_summary_csv(run_dir / "summary.csv", summary)
    write_summary_md(run_dir / "summary.md", summary)
    if not cfg.get("no_plot", False):
        make_plot(run_dir / "plots" / "summary_vs_id.png", summary)
    print(f"\n完成：{run_dir}")
    print(f"  metrics: {run_dir / 'metrics.csv'}")
    print(f"  summary: {run_dir / 'summary.csv'}")
    if sweep_error is not None:
        raise sweep_error
    return rows


def run_single(dpt, cfg: dict, pw1: int, verbose: bool = False) -> None:
    run_dir = HERE / "sweeps" / f"single_{time.strftime('%Y%m%d_%H%M%S')}_pw1_{pw1:02d}us"
    run_dir.mkdir(parents=True, exist_ok=True)
    scope = None
    pg = None
    ids = {}
    try:
        scope = connect_scope(cfg, verbose=verbose)
        pg = connect_pulsegen(cfg, verbose=verbose)
        check = check_scope(scope, cfg)
        ids["scope_idn"] = scope.idn()
        ids["pulsegen_idn"] = pg.idn()
        ids["scope_settings"] = check
        check_pulsegen(pg)
        setup_pulsegen(pg, cfg, first_pw1=pw1)
        cap = capture_point(scope, pg, cfg, pw1, 1)
        row, safety = process_point(dpt, cfg, cap, pw1, 1, run_dir, run_dir.name, ids)
        write_metrics(run_dir / "metrics.csv", [row])
        print(json.dumps({k: v for k, v in row.items() if k != "warnings"}, ensure_ascii=False, indent=2))
        print(f"warnings: {row['warnings']}")
        print(f"safety_fail: {safety}")
    finally:
        if pg is not None:
            try:
                pg.output(False)
            except Exception:
                pass
            try:
                pg.close()
            except Exception:
                pass
        if scope is not None:
            try:
                scope.close()
            except Exception:
                pass


def run_check(cfg: dict, verbose: bool = False) -> None:
    scope = None
    pg = None
    try:
        scope = connect_scope(cfg, verbose=verbose)
        pg = connect_pulsegen(cfg, verbose=verbose)
        check_scope(scope, cfg)
        check_pulsegen(pg)
        print("\n[check] 连接与设置检查通过（未发波）")
    finally:
        if pg is not None:
            pg.close()
        if scope is not None:
            scope.close()


def run_selftest(dpt, cfg: dict) -> None:
    print("== siglent_scope 合成块自检 ==")
    scope_selftest()
    print("\n== dpt_analyzer 复用自检（现有三份 CSV）==")
    bundle = Path(cfg["paths"]["analyzer_path"])
    paths = {
        "C1": bundle / "usr_wf_C1_data.csv",
        "C2": bundle / "usr_wf_C2_data.csv",
        "C3": bundle / "usr_wf_C3_data.csv",
    }
    for p in paths.values():
        if not p.exists():
            print(f"跳过：缺少 {p}")
            return
    cfg_dpt = dpt.Config(plot=False, show=False, rogowski=False)
    data = dpt.load_waveforms(paths, cfg_dpt)
    timing, w = dpt.detect_pulses(data.t, data.vgs_raw, cfg_dpt)
    m, ctx, levels, warnings = dpt.analyze(data, timing, cfg_dpt)
    print(f"I_off={m['i_off_A']:.2f} A, L_loop={m['l_loop_nH']:.1f} nH, "
          f"Qrr={m['qrr_nC']:.0f} nC, Eoff={m['eoff_uJ']:.3f} uJ, Eon={m['eon_uJ']:.3f} uJ")
    if not (20 <= m["i_off_A"] <= 30 and 8 <= m["l_loop_nH"] <= 15):
        raise SweepError("分析自检量级不对，检查 dpt_analyzer 版本/路径")
    print("dpt_analyzer 复用自检 OK")

    print("\n== 扫描/汇总/绘图 pipeline 自检（用现有 CSV 冒充一次采集）==")
    channels = {src: dpt.parse_siglent_csv(p) for src, p in paths.items()}
    t = channels["C1"].time
    dt = float(np.median(np.diff(t)))
    waves = {}
    fake_desc = WaveDesc(
        comm_type=1, comm_order=0, data_bytes=0, points=len(t),
        vdiv=1.0, voffset=0.0, code_per_div=1.0, adc_bit=16,
        interval=dt, delay=0.0, probe=1.0,
    )
    for src, ch in channels.items():
        waves[src] = Waveform(
            source=src, time=ch.time, value=ch.value,
            raw=np.zeros(len(ch.time), dtype=np.int16),
            unit=ch.unit or ("A" if src == "C2" else "V"),
            scale=0.0, offset=0.0, desc=fake_desc, timebase=5e-6,
        )
    cap = {
        "waves": waves,
        "st_pre": PulseStatus(mode="DPULSE", out="ON", v12="1", ch=1),
        "st_post": PulseStatus(mode="DPULSE", out="ON", v12="1", ch=1),
        "trig_ok": True, "trig_ambiguous": False,
        "num_pre": 0, "num_post": 1, "status_post": "STOP",
    }
    run_dir = HERE / "sweeps" / "selftest_synthetic"
    ids = {"scope_idn": "SELFTEST", "pulsegen_idn": "SELFTEST"}
    # 现有 CSV 的 PW2 是 10us，用一份临时配置让 timing 门禁通过
    cfg_pipe = deep_merge(cfg, {"sweep": {"pw2_us": 10}})
    row, safety = process_point(dpt, cfg_pipe, cap, 14, 1, run_dir, run_dir.name, ids)
    write_metrics(run_dir / "metrics.csv", [row])
    summary = summarize([row])
    write_summary_csv(run_dir / "summary.csv", summary)
    write_summary_md(run_dir / "summary.md", summary)
    make_plot(run_dir / "plots" / "summary_vs_id.png", summary)
    print(f"pipeline selftest: included={row['included']}, "
          f"I_off={row['i_off_A']:.2f} A, Eoff={row['eoff_uJ']:.3f} uJ, "
          f"Vpeak={row['vpeak_V']:.2f} V, safety={safety}")
    print(f"输出目录：{run_dir}")


def dry_run(cfg: dict) -> None:
    print("== DRY RUN ==")
    print(json.dumps(cfg, ensure_ascii=False, indent=2))
    print("\n命令序列：")
    print("  OUTP:OFF")
    print("  MODE:DPULSE")
    print(f"  CHAN:{cfg['pulsegen']['channel']}")
    print(f"  DPULS:INTV:{cfg['sweep']['intv_us']}")
    print(f"  DPULS:PW2:{cfg['sweep']['pw2_us']}")
    print(f"  DPULS:PW1:{int(cfg['sweep']['pw1_us'][0])}   # 先设第一点，再开输出")
    print("  OUTP:ON")
    for pw1 in cfg["sweep"]["pw1_us"]:
        print(f"  DPULS:PW1:{int(pw1)}  ->  :TRIGger:MODE SINGle -> TRIG -> wait STOP -> read C1/C2/C3")
    print("  OUTP:OFF")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="DPT SCPI 扫描：固定 Vbus，扫 tp1 -> ID，复用 dpt_analyzer.py",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "示例：\n"
            "  python dpt_sweep.py --check\n"
            "  python dpt_sweep.py --dry-run\n"
            "  python dpt_sweep.py --single --pw1 14\n"
            "  python dpt_sweep.py --pw1 2,4,6,8,10,12,14,16,18 --repeat 3\n"
            "  python dpt_sweep.py --config sweep_config.json\n"
        ),
    )
    p.add_argument("--config", type=Path, help="JSON 配置（与默认配置深合并）")
    p.add_argument("--scope-host", help="示波器 IP（默认 192.168.1.3）")
    p.add_argument("--scope-port", type=int, help="示波器 SCPI 端口（默认 5025）")
    p.add_argument("--pulsegen-host", help="oled_mirror TCP 地址（默认 127.0.0.1）")
    p.add_argument("--pulsegen-port", type=int, help="oled_mirror TCP 端口（默认 5025）")
    p.add_argument("--pw1", help="逗号分隔的 tp1 列表（us），如 2,4,6,8")
    p.add_argument("--repeat", type=int, help="每个点重复次数（默认 3）")
    p.add_argument("--settle", type=float, help="每 rep 后等待秒数（默认 0.5）")
    p.add_argument("--cool-down", type=float, help="每点后等待秒数（默认 0.5）")
    p.add_argument("--max-id", type=float, help="I_off 上限 A（默认 35）")
    p.add_argument("--max-vpeak", type=float, help="Vpeak 上限 V（默认 50，50V 电容硬限）")
    p.add_argument("--max-ipeak", type=float, help="Ipeak 上限 A（默认 60）")
    p.add_argument("--id-scale", type=float, help="CH2 A/V 额外比例（默认 1.0）")
    p.add_argument("--trig-level", type=float, help="示波器边沿触发电平 V（默认 2.0）")
    p.add_argument("--trig-source", help="示波器触发源（默认 C1；Y7 可接 C4/EXT）")
    p.add_argument("--analyzer-path", type=Path, help="dpt_analyzer.py 所在目录")
    p.add_argument("--output-root", type=Path, help="扫描数据根目录（默认 pc_host/sweeps）")
    p.add_argument("--check", action="store_true", help="只检查仪器连接和设置")
    p.add_argument("--single", action="store_true", help="只采一个点（配合 --pw1）")
    p.add_argument("--dry-run", action="store_true", help="只打印命令，不连接仪器")
    p.add_argument("--selftest", action="store_true", help="离线解析/分析自检")
    p.add_argument("--no-plot", action="store_true", help="不生成汇总图")
    p.add_argument("--per-point-plot", action="store_true", help="每个点额外生成 dpt 图")
    p.add_argument("--manual-trigger", action="store_true",
                   help="不发 SCPI TRIG，改为等待你手动按发波板/OLED 的 TRIG")
    p.add_argument("--verbose", action="store_true", help="打印 SCPI 命令/异常栈")
    return p


def apply_cli(cfg: dict, args) -> dict:
    if args.scope_host:
        cfg["scope"]["host"] = args.scope_host
    if args.scope_port:
        cfg["scope"]["port"] = args.scope_port
    if args.pulsegen_host:
        cfg["pulsegen"]["host"] = args.pulsegen_host
    if args.pulsegen_port:
        cfg["pulsegen"]["port"] = args.pulsegen_port
    if args.pw1:
        cfg["sweep"]["pw1_us"] = [int(float(x.strip())) for x in args.pw1.split(",") if x.strip()]
    if args.repeat is not None:
        cfg["repeat"] = args.repeat
    if args.settle is not None:
        cfg["settle_s"] = args.settle
    if args.cool_down is not None:
        cfg["cool_down_s"] = args.cool_down
    if args.max_id is not None:
        cfg["limits"]["max_id_A"] = args.max_id
    if args.max_vpeak is not None:
        cfg["limits"]["max_vpeak_V"] = args.max_vpeak
    if args.max_ipeak is not None:
        cfg["limits"]["max_ipeak_A"] = args.max_ipeak
    if args.id_scale is not None:
        cfg["analysis"]["id_scale_A_per_V"] = args.id_scale
    if args.trig_level is not None:
        cfg["trigger"]["level_V"] = args.trig_level
    if args.trig_source:
        cfg["trigger"]["source"] = args.trig_source
    if args.analyzer_path:
        cfg["paths"]["analyzer_path"] = str(args.analyzer_path)
    if args.output_root:
        cfg["paths"]["output_root"] = str(args.output_root)
    cfg["no_plot"] = bool(args.no_plot)
    cfg["per_point_plot"] = bool(args.per_point_plot)
    if args.manual_trigger:
        cfg["manual_trigger"] = True
    return cfg


def main(argv: list[str] | None = None) -> int:
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8")
        except Exception:
            pass
    args = build_parser().parse_args(argv)
    cfg = apply_cli(load_config(args.config), args)

    if args.dry_run:
        dry_run(cfg)
        return 0

    analyzer_path = Path(cfg["paths"]["analyzer_path"])
    try:
        dpt = import_analyzer(analyzer_path)
    except Exception as exc:
        print(f"[ERROR] 无法导入 dpt_analyzer：{exc}", file=sys.stderr)
        return 2

    if args.selftest:
        try:
            run_selftest(dpt, cfg)
            return 0
        except Exception as exc:
            print(f"[ERROR] selftest: {exc}", file=sys.stderr)
            return 2

    try:
        if args.check:
            run_check(cfg, verbose=args.verbose)
            return 0
        if args.single:
            pw1 = int(cfg["sweep"]["pw1_us"][0]) if not args.pw1 else int(float(args.pw1.split(",")[0]))
            run_single(dpt, cfg, pw1, verbose=args.verbose)
            return 0

        root = Path(cfg["paths"]["output_root"])
        if not root.is_absolute():
            root = HERE / root
        run_dir = root / time.strftime("%Y%m%d_%H%M%S")
        run_sweep(dpt, cfg, run_dir, verbose=args.verbose)
        return 0
    except KeyboardInterrupt:
        print("\n[中断] Ctrl-C")
        return 130
    except Exception as exc:
        print(f"[ERROR] {exc}", file=sys.stderr)
        if args.verbose:
            traceback.print_exc()
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
