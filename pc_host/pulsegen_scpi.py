#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# Copyright (c) 2026 Chen Fangshuo
# SPDX-License-Identifier: MIT
"""
G474PulseGen 双脉冲 SCPI 高层封装。

链路：本脚本 -> TCP 127.0.0.1:5025 -> oled_mirror.py -> 串口 -> STM32G474。
复用 demo_scripts.py 的 ScpiClient（标准库 socket，不依赖 PyVISA）。

注意：
  * DPULS:PW1/INTV/PW2 固件用 atoi，只接受整数 us（1..200）。
  * 越界参数固件可能仍回 OK；这里先做范围校验。
  * MODE/CHAN 会把 PW1 重置为默认值；正确顺序是 MODE -> CHAN -> INTV -> PW2 -> PW1。
  * TRIG 不自动重试；服务端对 TRIG 也不做超时重发。
"""

from __future__ import annotations

from dataclasses import dataclass

from demo_scripts import ScpiClient, ScpiError


class PulseGenError(Exception):
    """发波板命令错误。"""


@dataclass
class PulseStatus:
    mode: str = ""
    out: str = ""
    v12: str = ""
    ch: int = 0          # 原始 STAT 字段；固件打印的是 g_pulse_ctrl.channel + 1
    fr: int = 0
    st: int = 0
    ore: int = 0
    rx: int = 0
    lnk: int = 0

    @classmethod
    def parse(cls, text: str) -> "PulseStatus":
        fields: dict[str, str] = {}
        for part in text.strip().split(";"):
            if "=" in part:
                k, _, v = part.partition("=")
                fields[k.strip().upper()] = v.strip()
        def _int(key: str) -> int:
            try:
                return int(float(fields.get(key, "0")))
            except ValueError:
                return 0
        return cls(
            mode=fields.get("MODE", ""),
            out=fields.get("OUT", ""),
            v12=fields.get("12V", ""),
            ch=_int("CH"),
            fr=_int("FR"),
            st=_int("ST"),
            ore=_int("OR"),
            rx=_int("RX"),
            lnk=_int("LNK"),
        )


class PulseGen:
    def __init__(
        self,
        host: str = "127.0.0.1",
        port: int = 5025,
        timeout: float = 8.0,
        dry: bool = False,
        verbose: bool = False,
    ):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.dry = dry
        self.verbose = verbose
        self._client: ScpiClient | None = None

    def connect(self) -> "PulseGen":
        if self.dry:
            return self
        try:
            self._client = ScpiClient(
                host=self.host, port=self.port, timeout=self.timeout, dry=False
            ).connect()
        except ScpiError as exc:
            raise PulseGenError(str(exc)) from exc
        return self

    def close(self) -> None:
        if self._client is not None:
            try:
                self._client.close()
            except Exception:
                pass
            self._client = None

    def _query(self, cmd: str, expect: str | None = "OK") -> str:
        if self.dry:
            if self.verbose:
                print(f"[pulsegen] > {cmd} (dry)")
            if cmd == "*IDN?":
                return "PulseGen,G474PulseGen,0001,1.0"
            if cmd == "STAT":
                return "MODE=DPULSE;OUT=ON;12V=1;CH=1;FR=0;ST=0;OR=0;RX=0;LNK=0"
            return "OK"
        if self._client is None:
            raise PulseGenError("发波板未连接")
        try:
            rsp = self._client.query(cmd)
        except ScpiError as exc:
            raise PulseGenError(f"{cmd}: {exc}") from exc
        if self.verbose:
            print(f"[pulsegen] {cmd} -> {rsp}")
        if expect is not None and rsp.strip().upper() != expect.upper():
            raise PulseGenError(f"{cmd} 返回 {rsp!r}，期望 {expect!r}")
        return rsp

    def idn(self) -> str:
        return self._query("*IDN?", expect=None)

    def status(self) -> PulseStatus:
        return PulseStatus.parse(self._query("STAT", expect=None))

    def setup_double(self, channel: int = 1, intv_us: int = 5, pw2_us: int = 3) -> None:
        self._validate_us("INTV", intv_us)
        self._validate_us("PW2", pw2_us)
        self._query("MODE:DPULSE")
        self._query(f"CHAN:{int(channel)}")
        self._query(f"DPULS:INTV:{int(intv_us)}")
        self._query(f"DPULS:PW2:{int(pw2_us)}")

    def set_pw1(self, pw1_us: int) -> None:
        """只改第一脉冲宽度（demo_scripts 的 double 流程就是这样做的）。"""
        pw1_us = int(round(pw1_us))
        self._validate_us("PW1", pw1_us)
        self._query(f"DPULS:PW1:{pw1_us}")

    def set_double_pulse(self, pw1_us: int, intv_us: int = 5, pw2_us: int = 3) -> None:
        """每个点都重发三个参数，避免某个字段曾经非法导致 SetPW 整体不生效。"""
        pw1_us = int(round(pw1_us))
        intv_us = int(round(intv_us))
        pw2_us = int(round(pw2_us))
        for name, val in (("PW1", pw1_us), ("INTV", intv_us), ("PW2", pw2_us)):
            self._validate_us(name, val)
        self._query(f"DPULS:INTV:{intv_us}")
        self._query(f"DPULS:PW2:{pw2_us}")
        self._query(f"DPULS:PW1:{pw1_us}")

    def output(self, on: bool) -> None:
        self._query("OUTP:ON" if on else "OUTP:OFF")

    def trigger(self) -> None:
        """单次双脉冲。绝不自动重试。"""
        self._query("TRIG")

    @staticmethod
    def _validate_us(name: str, value: int | float) -> None:
        if not float(value).is_integer():
            raise PulseGenError(f"DPULS:{name} 只接受整数 us，收到 {value!r}")
        v = int(value)
        if not (1 <= v <= 200):
            raise PulseGenError(f"DPULS:{name} 超出 1..200 us：{v}")
