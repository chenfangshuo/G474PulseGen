#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# Copyright (c) 2026 Chen Fangshuo
# SPDX-License-Identifier: MIT
"""
Siglent SDS800X HD 系列 LAN SCPI 波形读取（标准库 socket + struct）。

只实现 DPT 扫描需要的子集：
  * 连接 / 文本查询 / 二进制块读取
  * :WAVeform:PREamble? 解析（346 字节 WAVEDESC）
  * WORD 模式 12-bit 数据换算（电压/电流 + 时间轴）
  * 单次采集、触发状态轮询、三通道读取

手册：D:\\Software\\Blog\\docs\\SDS800XHD_Series_ProgrammingGuide_EN11G.pdf
关键点：
  * 单次采集是 :TRIGger:MODE SINGle，不是 :SINGle
  * 12-bit 必须 :WAVeform:WIDTh WORD
  * PREamble 固定 #9 + 9 位长度；DATA? 是 #N + N 位长度，N 不固定
  * 长度字段在文档里有时按“点”写，实际按字节读；用 preamble 交叉校验
"""

from __future__ import annotations

import select
import socket
import struct
import time
from dataclasses import dataclass

import numpy as np


class ScopeError(Exception):
    """示波器通信/解析错误。"""


@dataclass
class WaveDesc:
    comm_type: int          # 0=byte, 1=word
    comm_order: int         # 0=LSB, 1=MSB
    data_bytes: int         # WAVE_ARRAY_1
    points: int             # WAVE_ARRAY_COUNT
    vdiv: float             # 垂直档位（不含探头衰减）
    voffset: float          # 垂直偏移（不含探头衰减）
    code_per_div: float     # 每格码值
    adc_bit: int
    interval: float         # s/point
    delay: float            # s
    probe: float            # 探头衰减/变比


@dataclass
class Waveform:
    source: str
    time: np.ndarray
    value: np.ndarray
    raw: np.ndarray
    unit: str
    scale: float
    offset: float
    desc: WaveDesc
    timebase: float

    @property
    def dt(self) -> float:
        return float(self.desc.interval)

    @property
    def fs(self) -> float:
        return 1.0 / self.desc.interval if self.desc.interval > 0 else 0.0


# ---------------------------------------------------------------------------
# 纯解析函数（可在无仪器的情况下单测）
# ---------------------------------------------------------------------------

def _s16(buf: bytes, off: int, endian: str) -> int:
    return struct.unpack_from(endian + "h", buf, off)[0]


def _s32(buf: bytes, off: int, endian: str) -> int:
    return struct.unpack_from(endian + "i", buf, off)[0]


def _f32(buf: bytes, off: int, endian: str) -> float:
    return struct.unpack_from(endian + "f", buf, off)[0]


def _f64(buf: bytes, off: int, endian: str) -> float:
    return struct.unpack_from(endian + "d", buf, off)[0]


def parse_preamble(payload: bytes) -> WaveDesc:
    """解析 :WAVeform:PREamble? 返回的 346 字节 WAVEDESC payload。"""
    if len(payload) < 346:
        raise ScopeError(f"preamble 太短：{len(payload)} 字节（需要 346）")
    if not payload.startswith(b"WAVEDESC"):
        # 有些固件会在前面带 \n；允许前导空白，但 payload 本身应以 WAVEDESC 开头。
        head = payload[:64].find(b"WAVEDESC")
        if head < 0:
            raise ScopeError("preamble 不是 WAVEDESC（检查 read_block 是否读对）")
        payload = payload[head:]
    comm_order = _s16(payload, 0x22, "<")
    endian = "<" if comm_order == 0 else ">"
    desc_len = _s32(payload, 0x24, endian)
    if desc_len != 346:
        raise ScopeError(f"WAVEDESC 长度字段是 {desc_len}，预期 346")
    comm_type = _s16(payload, 0x20, endian)
    if comm_type != 1:
        raise ScopeError(
            f"示波器返回的不是 WORD 模式（COMM_TYPE={comm_type}）；"
            "12-bit 波形请先 :WAVeform:WIDTh WORD"
        )
    return WaveDesc(
        comm_type=comm_type,
        comm_order=comm_order,
        data_bytes=_s32(payload, 0x3C, endian),
        points=_s32(payload, 0x74, endian),
        vdiv=_f32(payload, 0x9C, endian),
        voffset=_f32(payload, 0xA0, endian),
        code_per_div=_f32(payload, 0xA4, endian),
        adc_bit=_s16(payload, 0xAC, endian),
        interval=_f32(payload, 0xB0, endian),
        delay=_f64(payload, 0xB4, endian),
        probe=_f32(payload, 0x148, endian),
    )


def decode_waveform(
    payload: bytes,
    desc: WaveDesc,
    timebase: float,
    chunk_start: int = 0,
    id_scale: float = 1.0,
    grid: int = 10,
) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """
    把 WORD 原始码值解码成 (time, value, raw_code)。

    value = (code / code_per_div * vdiv - voffset) * probe * id_scale
    time  = delay - timebase*grid/2 + (chunk_start + index)*interval
    """
    if desc.comm_type != 1:
        raise ScopeError("decode_waveform 只支持 WORD 模式")
    if len(payload) % 2 != 0:
        raise ScopeError(f"WORD 数据字节数不是偶数：{len(payload)}")
    n = len(payload) // 2
    if desc.points and n != desc.points and n != desc.data_bytes // 2:
        # 只警告不失败：分片时 preamble 的 points 可能是总点数。
        pass
    dtype = ("<" if desc.comm_order == 0 else ">") + "i2"
    raw = np.frombuffer(payload, dtype=dtype)
    code = raw.astype(np.float64)
    if desc.code_per_div <= 0:
        raise ScopeError(f"code_per_div 非法：{desc.code_per_div}")
    value = (code / desc.code_per_div * desc.vdiv - desc.voffset) * desc.probe * id_scale
    idx = np.arange(len(raw), dtype=np.float64) + chunk_start
    t = desc.delay - timebase * grid / 2.0 + idx * desc.interval
    return t, value, raw


class SiglentScope:
    """LAN SCPI 客户端。默认 raw socket 5025，命令以 \\n 结尾。"""

    def __init__(
        self,
        host: str,
        port: int = 5025,
        timeout: float = 5.0,
        data_timeout: float = 30.0,
        verbose: bool = False,
    ):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.data_timeout = data_timeout
        self.verbose = verbose
        self._sock: socket.socket | None = None
        self._buf = b""

    # ---- 连接 ----
    def connect(self) -> "SiglentScope":
        self._sock = socket.create_connection((self.host, self.port), timeout=self.timeout)
        self._sock.settimeout(self.timeout)
        self._buf = b""
        if self.verbose:
            print(f"[scope] connected {self.host}:{self.port}")
        return self

    def close(self) -> None:
        if self._sock is not None:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None

    def _send(self, cmd: str) -> None:
        if self._sock is None:
            raise ScopeError("scope 未连接")
        if self.verbose:
            print(f"[scope] > {cmd}")
        self._sock.sendall(cmd.encode("ascii", "ignore") + b"\n")

    def _fill(self, n: int, timeout: float | None = None) -> None:
        if self._sock is None:
            raise ScopeError("scope 未连接")
        timeout = self.timeout if timeout is None else timeout
        deadline = time.time() + timeout
        while len(self._buf) < n:
            remain = deadline - time.time()
            if remain <= 0:
                raise ScopeError(f"scope 读取超时（需要 {n} 字节，已有 {len(self._buf)}）")
            ready, _, _ = select.select([self._sock], [], [], min(remain, 1.0))
            if not ready:
                continue
            chunk = self._sock.recv(max(4096, n - len(self._buf)))
            if not chunk:
                raise ScopeError("scope 连接被关闭")
            self._buf += chunk

    def _take(self, n: int, timeout: float | None = None) -> bytes:
        self._fill(n, timeout)
        out, self._buf = self._buf[:n], self._buf[n:]
        return out

    def _read_line(self, timeout: float | None = None) -> str:
        timeout = self.timeout if timeout is None else timeout
        deadline = time.time() + timeout
        while b"\n" not in self._buf:
            remain = deadline - time.time()
            if remain <= 0:
                raise ScopeError(f"scope 响应超时（已有 {self._buf!r}）")
            self._fill(len(self._buf) + 1, remain)
        line, self._buf = self._buf.split(b"\n", 1)
        return line.decode("ascii", "ignore").strip()

    # ---- 文本命令 ----
    def query(self, cmd: str, timeout: float | None = None) -> str:
        self._send(cmd)
        return self._read_line(timeout)

    def write(self, cmd: str) -> None:
        self._send(cmd)

    def idn(self) -> str:
        return self.query("*IDN?")

    # ---- 二进制块 ----
    def _skip_to_hash(self, timeout: float | None = None) -> None:
        """
        示波器若开着响应头（如 `C1:WF DESC,`），二进制块前面会带一段 ASCII。
        这里把第一个 '#' 之前的文本丢掉；'#' 之后就是标准 definite-length block。
        """
        timeout = self.data_timeout if timeout is None else timeout
        deadline = time.time() + timeout
        while b"#" not in self._buf:
            remain = deadline - time.time()
            if remain <= 0:
                raise ScopeError(f"没有等到二进制块头 '#'（缓冲 {self._buf[:80]!r}）")
            self._fill(len(self._buf) + 1, remain)
        idx = self._buf.find(b"#")
        prefix, self._buf = self._buf[:idx], self._buf[idx:]
        if prefix and self.verbose:
            print(f"[scope] dropped response prefix: {prefix[:80]!r}")

    def read_block(
        self,
        expected_bytes: int | None = None,
        timeout: float | None = None,
    ) -> bytes:
        """
        读取 SCPI definite-length binary block：`#<n><length><payload>`。
        expected_bytes 用于处理手册里“长度按点数写”的固件：
          * 若声明长度 < expected_bytes，则继续读到 expected_bytes；
          * 若声明长度 == expected_bytes，正常读；
          * 若声明长度 > expected_bytes，按声明长度读（以仪器为准）。
        """
        timeout = self.data_timeout if timeout is None else timeout
        self._skip_to_hash(timeout)
        head = self._take(2, timeout)
        if head[:1] != b"#":
            # 不是二进制块，可能是错误文本；把缓冲读成一行方便诊断
            self._buf = head + self._buf
            line = self._read_line(timeout)
            raise ScopeError(f"scope 返回的不是二进制块：{line!r}")
        nd = head[1:2]
        if not nd.isdigit():
            raise ScopeError(f"非法二进制块头：{head!r}")
        ndig = int(nd)
        if ndig == 0:
            # 不定长：读到 \n\n（不常见）
            while b"\n\n" not in self._buf:
                self._fill(len(self._buf) + 1, timeout)
            payload, self._buf = self._buf.split(b"\n\n", 1)
            return payload
        digits = self._take(ndig, timeout)
        if not digits.isdigit():
            raise ScopeError(f"非法二进制块长度：{digits!r}")
        declared = int(digits)
        to_read = declared
        if expected_bytes is not None:
            to_read = max(declared, expected_bytes)
        payload = self._take(to_read, timeout)
        # 有些固件会在块尾追加 0A 0A；只消费明确的尾部，不搜索 payload 内部。
        self._consume_trailer()
        return payload

    def _consume_trailer(self, timeout: float = 0.05) -> None:
        """
        消费二进制块后面的 CR/LF（可能多于 2 字节）。只消费空白，不吞下一条响应。
        """
        if self._sock is None:
            return
        # 先清掉缓冲区里已经有的 CR/LF
        while self._buf[:1] in (b"\n", b"\r"):
            self._buf = self._buf[1:]
        deadline = time.time() + timeout
        while True:
            remain = deadline - time.time()
            if remain <= 0:
                return
            ready, _, _ = select.select([self._sock], [], [], remain)
            if not ready:
                return
            chunk = self._sock.recv(4096)
            if not chunk:
                return
            self._buf += chunk
            while self._buf[:1] in (b"\n", b"\r"):
                self._buf = self._buf[1:]
            if self._buf:
                # 出现了非空白字节：那是下一条响应/数据，留给调用者
                return

    def query_block(self, cmd: str, expected_bytes: int | None = None) -> bytes:
        self._send(cmd)
        return self.read_block(expected_bytes=expected_bytes)

    # ---- 配置/状态查询 ----
    def _as_float(self, cmd: str) -> float:
        return float(self.query(cmd).strip().strip('"'))

    def channel_unit(self, ch: int) -> str:
        return self.query(f":CHANnel{ch}:UNIT?").strip().upper()

    def channel_scale(self, ch: int) -> float:
        return self._as_float(f":CHANnel{ch}:SCALe?")

    def channel_offset(self, ch: int) -> float:
        return self._as_float(f":CHANnel{ch}:OFFSet?")

    def channel_probe(self, ch: int) -> float:
        return self._as_float(f":CHANnel{ch}:PROBe?")

    def channel_switch(self, ch: int) -> str:
        return self.query(f":CHANnel{ch}:SWITch?").strip().upper()

    def channel_visible(self, ch: int) -> str:
        return self.query(f":CHANnel{ch}:VISible?").strip().upper()

    def timebase_scale(self) -> float:
        return self._as_float(":TIMebase:SCALe?")

    def acquire_points(self) -> int:
        return int(round(self._as_float(":ACQuire:POINts?")))

    def acquire_type(self) -> str:
        return self.query(":ACQuire:TYPE?").strip().upper()

    def acquire_mdepth(self) -> str:
        return self.query(":ACQuire:MDEPth?").strip().upper()

    def max_point(self) -> int:
        return int(round(self._as_float(":WAVeform:MAXPoint?")))

    def trigger_status(self) -> str:
        return self.query(":TRIGger:STATus?").strip().upper()

    def trigger_mode(self) -> str:
        return self.query(":TRIGger:MODE?").strip().upper()

    def num_acq(self) -> int:
        return int(round(self._as_float(":ACQuire:NUMACq?")))

    # ---- 采集 ----
    def configure_channels(self, channels=(1, 2, 3)) -> dict[int, dict]:
        info: dict[int, dict] = {}
        for ch in channels:
            if self.channel_switch(ch) != "ON":
                self.write(f":CHANnel{ch}:SWITch ON")
            if self.channel_visible(ch) != "ON":
                self.write(f":CHANnel{ch}:VISible ON")
            info[ch] = {
                "unit": self.channel_unit(ch),
                "scale": self.channel_scale(ch),
                "offset": self.channel_offset(ch),
                "probe": self.channel_probe(ch),
            }
        return info

    def arm_single(self, source: str = "C1", level: float = 5.0, slope: str = "RISing") -> None:
        self.write(":TRIGger:TYPE EDGE")
        self.write(f":TRIGger:EDGE:SOURce {source}")
        self.write(f":TRIGger:EDGE:SLOPe {slope}")
        self.write(f":TRIGger:EDGE:LEVel {level:.6E}")
        self.write(":TRIGger:EDGE:COUPling DC")
        self.write(":TRIGger:MODE SINGle")
        # 设置命令是异步的；立刻查询可能读到旧的 FTRIG/AUTO。轮询到 SINGle 为止。
        deadline = time.time() + 2.0
        mode = ""
        while time.time() < deadline:
            mode = self.trigger_mode()
            if "SING" in mode.upper():
                return
            time.sleep(0.05)
        raise ScopeError(f":TRIGger:MODE 没有切到 SINGle（读到 {mode!r}）")

    def wait_ready(self, timeout: float = 3.0, poll: float = 0.05) -> str:
        deadline = time.time() + timeout
        ran_once = False
        last = ""
        while time.time() < deadline:
            last = self.trigger_status()
            if last in ("READY", "ARM", "ARMED"):
                return last
            if last == "STOP" and not ran_once:
                # 有些固件在 MODE SINGle 后仍显示 Stop；给一次 RUN 兜底。
                self.write(":TRIGger:RUN")
                ran_once = True
            elif last in ("AUTO", "ROLL"):
                raise ScopeError(f"示波器处于 {last}，单次触发未生效；检查触发设置")
            time.sleep(poll)
        raise ScopeError(f"等待示波器 Ready 超时（最后状态 {last!r}）")

    def wait_stop(self, timeout: float = 5.0, poll: float = 0.05) -> str:
        deadline = time.time() + timeout
        stable = 0
        last = ""
        while time.time() < deadline:
            last = self.trigger_status()
            if last == "STOP":
                stable += 1
                if stable >= 2:
                    time.sleep(0.05)
                    return last
            elif last in ("AUTO", "ROLL"):
                raise ScopeError(f"示波器进入 {last}，采集不可信")
            else:
                stable = 0
            time.sleep(poll)
        raise ScopeError(f"等待示波器 Stop 超时（最后状态 {last!r}）")

    def read_channel(self, source: str, id_scale: float = 1.0) -> Waveform:
        """读取一个通道（同一帧内可依次调用 C1/C2/C3）。"""
        source = source.upper()
        ch = int(source.lstrip("C"))
        self.write(f":WAVeform:SOURce {source}")
        self.write(":WAVeform:WIDTh WORD")
        self.write(":WAVeform:BYTeorder LSB")
        total = self.acquire_points()
        maxp = self.max_point()
        if maxp <= 0 or maxp >= total:
            chunks = [(0, total)]
        else:
            chunks = [(s, min(maxp, total - s)) for s in range(0, total, maxp)]

        desc0: WaveDesc | None = None
        times: list[np.ndarray] = []
        values: list[np.ndarray] = []
        raws: list[np.ndarray] = []
        timebase = self.timebase_scale()

        for start, n in chunks:
            self.write(f":WAVeform:STARt {start}")
            self.write(f":WAVeform:POINt {n}")
            pre = self.query_block(":WAVeform:PREamble?", expected_bytes=346)
            desc = parse_preamble(pre)
            if desc0 is None:
                desc0 = desc
            expected = 2 * n
            data = self.query_block(":WAVeform:DATA?", expected_bytes=expected)
            if len(data) > expected:
                data = data[:expected]
            t, v, raw = decode_waveform(
                data, desc, timebase, chunk_start=start, id_scale=id_scale
            )
            times.append(t)
            values.append(v)
            raws.append(raw)

        if desc0 is None:
            raise ScopeError("没有读到 preamble")
        unit = self.channel_unit(ch)
        scale = self.channel_scale(ch)
        offset = self.channel_offset(ch)
        return Waveform(
            source=source,
            time=np.concatenate(times) if len(times) > 1 else times[0],
            value=np.concatenate(values) if len(values) > 1 else values[0],
            raw=np.concatenate(raws) if len(raws) > 1 else raws[0],
            unit=unit,
            scale=scale,
            offset=offset,
            desc=desc0,
            timebase=timebase,
        )

    def read_all_channels(
        self,
        sources=("C1", "C2", "C3"),
        id_scale: float = 1.0,
    ) -> dict[str, Waveform]:
        if self.trigger_status() != "STOP":
            raise ScopeError("读波形前示波器必须处于 Stop")
        out = {}
        for src in sources:
            out[src] = self.read_channel(src, id_scale=id_scale if src.upper() == "C2" else 1.0)
        if self.trigger_status() != "STOP":
            raise ScopeError("读三通道期间示波器状态发生变化，帧可能不一致")
        return out


# ---------------------------------------------------------------------------
# 离线自检：合成 preamble + WORD 数据，验证解析/换算/时间轴
# ---------------------------------------------------------------------------

def _synthetic_blocks() -> tuple[bytes, bytes, int, float]:
    points = 16
    desc = bytearray(346)
    desc[0:8] = b"WAVEDESC"
    struct.pack_into("<h", desc, 0x20, 1)      # WORD
    struct.pack_into("<h", desc, 0x22, 0)      # LSB
    struct.pack_into("<i", desc, 0x24, 346)
    struct.pack_into("<i", desc, 0x3C, points * 2)
    struct.pack_into("<i", desc, 0x74, points)
    struct.pack_into("<f", desc, 0x9C, 5.0)    # vdiv
    struct.pack_into("<f", desc, 0xA0, -5.0)   # voffset
    struct.pack_into("<f", desc, 0xA4, 25.0)   # code_per_div
    struct.pack_into("<h", desc, 0xAC, 12)     # adc_bit
    struct.pack_into("<f", desc, 0xB0, 2e-9)   # interval
    struct.pack_into("<d", desc, 0xB4, 20e-6)  # delay（触发点在 20us，记录从 -5us 开始）
    struct.pack_into("<f", desc, 0x148, 10.0)  # probe
    codes = np.arange(points, dtype=np.int16) * 100
    return bytes(desc), codes.tobytes(), points, 2e-9


def selftest() -> None:
    pre, data, points, interval = _synthetic_blocks()
    desc = parse_preamble(pre)
    assert desc.points == points and desc.comm_type == 1
    t, v, raw = decode_waveform(data, desc, timebase=5e-6)
    assert len(t) == points and len(v) == points
    # index 0: code 0 -> (0 - (-5))*10 = 50 V
    assert abs(v[0] - 50.0) < 1e-6, v[0]
    # index 1: code 100 -> (100/25*5 + 5)*10 = 250 V
    assert abs(v[1] - 250.0) < 1e-6, v[1]
    assert abs((t[1] - t[0]) - desc.interval) < 1e-20
    assert abs(t[0] - (-5e-6)) < 1e-12
    print("siglent_scope selftest OK")


if __name__ == "__main__":
    selftest()
