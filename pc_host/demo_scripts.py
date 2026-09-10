#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
发波板 (STM32G474) SCPI 自动化演示脚本
=====================================

通过标准 socket 连接上位机 `oled_mirror.py` 内嵌的 TCP SCPI 服务端 (默认 127.0.0.1:5025),
驱动真实发波板执行常见测试流程。仅依赖标准库 (socket/argparse/time/sys), 无需 pyserial/pygame。

用法示例:
    python demo_scripts.py idn
    python demo_scripts.py single --end 25 --dwell 0.3
    python demo_scripts.py double --pw1-start 2 --pw1-end 14
    python demo_scripts.py pwm-sweep --fstart 20000 --fend 200000
    python demo_scripts.py --dry-run single --end 5      # 只打印命令, 不连接/发送

前置条件:
    先启动上位机 `python oled_mirror.py --port COMx` (非 --demo), 确保串口已连接、
    TCP SCPI 服务端已监听 127.0.0.1:5025。

SCPI 命令语法说明 (与固件 `uart_comm.c::UcScpiExec` 100% 一致):
    仅冒号 `:` 分隔, 无空格参数、无问号查询语义 (特例: `*IDN?` 带 ?、裸 `STAT` 不带 ?)。
    所有命令形如 `HEAD[:SUB]:VALUE`。

注意: 控制台输出采用 ASCII + 简体中文, 避免 GBK/UTF-8 控制台编码不兼容 (不输出 micro/箭头等特殊符号)。
"""

import argparse
import socket
import sys
import time

# ---- 默认连接参数 (与 oled_mirror.py 保持一致) ----
DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 5025
DEFAULT_TIMEOUT = 8.0

# 触发输出的演示子命令 (用于中断时兜底关闭输出)
POWER_DEMOS = ("single", "double", "pwm-sweep", "comp", "burst")


# ---- 完整 SCPI 命令参考 (与固件 Core/Src/uart_comm.c::UcScpiExec 一致) ----
# 本地可查: python demo_scripts.py scpi  (无需连接/无需上位机)
SCPI_REFERENCE = """\
================================================================
发波板 SCPI 命令参考 (与固件 Core/Src/uart_comm.c::UcScpiExec 一致)
================================================================

[语法规则]
  - 命令大小写不敏感, 仅冒号 ':' 分隔, 形如 HEAD[:SUB]:VALUE (最多三段)。
  - 不支持空格分隔参数, 也不支持标准 SCPI 的 '?' 查询语义。
    特例: *IDN? 带 '?'; STAT 为裸命令不带 '?' (STAT? 会被判 ERR CMD)。

[系统/查询]
  *IDN?               查询设备标识 -> PulseGen,G474PulseGen,0001,1.0
  STAT                查询状态 (裸写, 不带 '?'), 返回字段见下方 [STAT 字段]
  HELP                查询命令清单 (精简版)

[输出/模式/通道/极性]
  OUTP:ON|OFF         输出使能/关闭
  MODE:<m>            切模式: NPULSE | DPULSE | PWM | NPULSELONG | PWMLONG | COMPPWM | COMPPWMLONG
  CHAN:<n>            选通道 1~6
  POL:<0|1>           极性: 0=高有效, 1=低有效
  12V:ON|OFF          12V_OUT 手动开关
  PRESET:SAVE|LOAD    保存/调出 Flash -> OK SAVED / OK LOADED (失败 ERR FLASH / ERR NOPRESET)
  TRIG                单次触发
  KEY:<n>             虚拟按键: 1=上 2=下 3=左 4=右 5=确定 6=返回 8=滚轮上 9=滚轮下

[波形参数] (三段式 HEAD:SUB:VALUE)
  PULS:WIDTH:<us>     N 脉冲脉宽, 0.01~1500 us (0.01us 分辨率, 支持小数)
  PULS:COUNT:<n>      N 脉冲个数, 1~100 (整数)
  PULS:INTV:<us>      N 脉冲间歇, 1~1500 us
  DPULS:PW1:<us>      双脉冲第 1 脉宽, 整数 1~200 us (固件 atoi 截断小数)
  DPULS:INTV:<us>     双脉冲间隔, 整数 1~200 us
  DPULS:PW2:<us>      双脉冲第 2 脉宽, 整数 1~200 us
  PWM:PER:<us>        PWM 周期, 整数 1~1500 us
  PWM:DUTY:<%>        PWM 占空比, 0~100 %
  LPWM:PER:<s>        长 PWM 周期, 0.001~1000 秒 (注意单位是【秒】不是 us)
  LPWM:DUTY:<%>       长 PWM 占空比, 0.01~100 %
  COMP:PER:<us>       互补 PWM 周期, 1~1500 us (仅 COMPPWM 模式有效)
  COMP:DUTY:<%>       互补 PWM 占空比, 0~100 %
  COMP:DTR:<ns>       上升沿死区, 0~12000 ns (防直通)
  COMP:DTF:<ns>       下降沿死区, 0~12000 ns (防直通)
  BURST:<Hz>          猝发 PRF, 0=单次, 1~100000 Hz

[STAT 字段含义] (返回 MODE=...;OUT=...;12V=...;CH=...;FR=...;ST=...;OR=...;RX=...;LNK=...)
  MODE  模式名: NONE/NPULSE/DPULSE/PWM/NPULSELONG/PWMLONG/COMPPWM/COMPPWMLONG
  OUT   输出使能 (ON/OFF)
  12V   12V 开关 (0/1)
  CH    通道 (1~6)
  FR    已发镜像帧数
  ST    RXNE 风暴触发次数 (诊断: 接收风暴保护触发)
  OR    ORE 超载次数 (诊断: 接收超载丢字节, 2Mbps 下应保持 0)
  RX    累计接收字节
  LNK   连接状态 (1=已连接, 0=空闲)

[错误码]
  ERR EMPTY  空命令
  ERR ARG    参数错误
  ERR VAL    缺少数值
  ERR SUB    未知子命令
  ERR MODE   模式错误 (如 COMP 命令在非 COMPPWM 模式)
  ERR CH     通道越界
  ERR POL    极性错误
  ERR CMD    未知命令

[注意事项]
  - DPULS:PW1/INTV/PW2 固件用 atoi 解析, 仅整数 us (小数被截断)。
  - PWM:PER 为整数 us, 高频端周期量化 (如 5us 对应 200kHz)。
  - LPWM:PER 单位是秒不是 us (固件 v*1000 后 /1000)。
  - COMP 系列仅在 MODE:COMPPWM 下有效, 否则返回 ERR MODE。
  - PULS:WIDTH 支持小数 (0.01us 分辨率), 其余参数多为整数。
================================================================
"""


class ScpiError(Exception):
    """SCPI 通信/命令错误, 携带中文可读信息。"""
    pass


class ScpiClient:
    """极简 SCPI TCP 客户端。

    处理 TCP 连接、带换行符的命令发送、单行查询与异常捕获。
    服务端协议: 每条命令以 '\\n' 结尾, 返回单行响应 (亦以 '\\n' 结尾)。
    """

    def __init__(self, host=DEFAULT_HOST, port=DEFAULT_PORT,
                 timeout=DEFAULT_TIMEOUT, dry=False):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.dry = dry
        self._sock = None

    # ---- 连接管理 ----
    def connect(self):
        """建立 TCP 连接; dry 模式跳过。"""
        if self.dry:
            print(f"[DRY-RUN] 跳过连接 {self.host}:{self.port} (演练模式, 不发送)")
            return self
        try:
            self._sock = socket.create_connection((self.host, self.port),
                                                  timeout=self.timeout)
            self._sock.settimeout(self.timeout)
            print(f"已连接 SCPI 服务端 {self.host}:{self.port}")
        except (ConnectionRefusedError, ConnectionResetError,
                socket.timeout, OSError) as e:
            raise ScpiError(
                f"无法连接 SCPI 服务端 {self.host}:{self.port}: {e}\n"
                f"请确认已启动 oled_mirror.py (非 --demo) 且串口已连接。"
            ) from e
        return self

    def close(self):
        """优雅关闭连接。"""
        if self._sock is not None:
            try:
                self._sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            self._sock.close()
            self._sock = None

    # ---- 核心收发 ----
    def query(self, cmd):
        """发送一条命令 (自动补 '\\n'), 返回去首尾空白后的单行响应。

        dry 模式不真正发送, 返回 "OK" 供流程走通 (仅演练用)。
        """
        cmd = cmd.strip()
        if self.dry:
            return "OK"
        if self._sock is None:
            raise ScpiError("尚未建立连接, 请先调用 connect()")
        try:
            self._sock.sendall(cmd.encode("ascii") + b"\n")
        except (ConnectionResetError, BrokenPipeError, OSError) as e:
            raise ScpiError(f"发送命令失败: {e}") from e

        # 循环 recv 直到收满一个完整行 (以 '\\n' 结尾)
        buf = b""
        while b"\n" not in buf:
            try:
                chunk = self._sock.recv(4096)
            except socket.timeout as e:
                raise ScpiError(f"等待响应超时 ({self.timeout}s): {cmd}") from e
            except (ConnectionResetError, OSError) as e:
                raise ScpiError(f"接收响应失败: {e}") from e
            if not chunk:
                raise ScpiError(f"连接被服务端关闭, 未收到响应: {cmd}")
            buf += chunk
        line, _, _ = buf.partition(b"\n")
        return line.decode("ascii", "ignore").strip()


# ---- 通用辅助 ----

def send(client, cmd, expect="OK"):
    """发送命令并打印结果; dry 模式只打印命令。返回响应字符串。

    expect: 期望的响应 (默认 "OK"); 传 None 表示不校验 (用于 *IDN?/STAT/HELP)。
    命令返回非预期 (如 ERR TIMEOUT) 时, 自动抓一次 STAT 诊断, 便于定位丢帧方向。
    """
    rsp = client.query(cmd)
    if client.dry:
        print(f"[DRY-RUN] {cmd}")
    elif expect is not None and rsp != expect:
        print(f"  [!]  {cmd:24s} -> {rsp}  (期望 {expect})")
        dump_status(client)
    else:
        print(f"  OK   {cmd:24s} -> {rsp}")
    return rsp


def dump_status(client):
    """异常/超时后自动抓固件 STAT 诊断, 区分丢帧方向。"""
    if client.dry:
        return
    try:
        rsp = client.query("STAT")
    except ScpiError as e:
        print(f"  [诊断] STAT 查询失败: {e}  (若 MCU RX 被风暴保护关闭, STAT 也会丢)")
        return
    print(f"  [诊断] {rsp}")
    fields = {}
    for part in rsp.split(";"):
        if "=" in part:
            k, _, v = part.partition("=")
            fields[k.strip()] = v.strip()
    st = int(fields.get("ST") or 0)
    ore = int(fields.get("OR") or 0)
    if st or ore:
        print(f"  [诊断] ST={st} OR={ore} >0 -> 丢帧在 PC->MCU 接收侧 (TRIG 命令可能未被固件处理)")
    else:
        print(f"  [诊断] ST/OR=0 -> 命令已达固件, 疑似 MCU->PC 的 OK 响应丢失 (请留意上位机 bad_crc)")


def _fmt(v):
    """数值转字符串: 整数去 .0, 小数保留有效位。"""
    if isinstance(v, int):
        return str(v)
    s = ("%.4f" % v).rstrip("0").rstrip(".")
    return s if s else "0"


def _frange(start, end, step):
    """浮点等差序列 (规避累积误差), 含终点 (误差 1e-9 内)。"""
    i = 0
    while True:
        v = round(start + i * step, 9)
        if v > end + 1e-9:
            break
        yield v
        i += 1


def _sleep(client, secs):
    """驻留延时; dry 模式跳过 (演练无需等待)。"""
    if not client.dry:
        time.sleep(secs)


# ---- 演示 1: 单脉冲脉宽递增 (电感饱和摸底) ----

def demo_single(client, args):
    print(f"\n=== 演示1 单脉冲脉宽递增 (电感饱和摸底) ===")
    print(f"脉宽 {_fmt(args.start)} -> {_fmt(args.end)} us, "
          f"步进 {_fmt(args.step)} us, 驻留 {args.dwell} s, 通道 CH{args.channel}")
    send(client, "*IDN?", expect=None)
    send(client, "MODE:NPULSE")
    send(client, f"CHAN:{args.channel}")
    if args.polarity is not None:
        send(client, f"POL:{args.polarity}")
    send(client, "PULS:COUNT:1")
    send(client, "BURST:0")
    send(client, "OUTP:ON")

    pts = list(_frange(args.start, args.end, args.step))
    total = len(pts)
    for idx, w in enumerate(pts, 1):
        print(f"[{idx}/{total}] 脉宽 {_fmt(w)} us")
        send(client, f"PULS:WIDTH:{_fmt(w)}")
        send(client, "TRIG")
        _sleep(client, args.dwell)
    send(client, "OUTP:OFF")


# ---- 演示 2: 双脉冲第一脉冲递增 (功率 MOS 双脉冲电流阶梯) ----

def demo_double(client, args):
    print(f"\n=== 演示2 双脉冲第一脉冲递增 (电流阶梯测试) ===")
    print(f"PW1 {_fmt(args.pw1_start)} -> {_fmt(args.pw1_end)} us, "
          f"步进 {_fmt(args.step)} us, 固定 INTV={args.intv} us / PW2={args.pw2} us, "
          f"驻留 {args.dwell} s, 通道 CH{args.channel}")
    send(client, "MODE:DPULSE")
    send(client, f"CHAN:{args.channel}")
    send(client, f"DPULS:INTV:{args.intv}")   # 固定死区间隔
    send(client, f"DPULS:PW2:{args.pw2}")     # 固定第二脉冲
    send(client, "OUTP:ON")

    pts = list(_frange(args.pw1_start, args.pw1_end, args.step))
    total = len(pts)
    for idx, pw1 in enumerate(pts, 1):
        v = int(round(pw1))                   # DPULS:* 固件用 atoi, 仅整数 us
        print(f"[{idx}/{total}] 第一脉冲 {v} us")
        send(client, f"DPULS:PW1:{v}")
        send(client, "TRIG")
        _sleep(client, args.dwell)
    send(client, "OUTP:OFF")


# ---- 演示 3: PWM 自动扫频 (占空比固定) ----

def demo_pwm_sweep(client, args):
    print(f"\n=== 演示3 PWM 自动扫频 ===")
    print(f"频率 {_fmt(args.fstart)} -> {_fmt(args.fend)} Hz, "
          f"步进 {_fmt(args.fstep)} Hz, 占空比 {_fmt(args.duty)} %, "
          f"驻留 {args.dwell} s, 通道 CH{args.channel}")
    send(client, "MODE:PWM")
    send(client, f"CHAN:{args.channel}")
    send(client, f"PWM:DUTY:{_fmt(args.duty)}")
    send(client, "OUTP:ON")

    pts = list(_frange(args.fstart, args.fend, args.fstep))
    total = len(pts)
    last_per = None
    applied = 0
    for idx, f in enumerate(pts, 1):
        per = int(round(1e6 / f))             # PWM:PER 为整数 us
        if per == last_per:                   # 高频端周期量化, 去重避免重复写同一周期
            print(f"[{idx}/{total}] 请求 {_fmt(f)} Hz -> 周期量化重复 ({per} us), 跳过")
            continue
        last_per = per
        applied += 1
        actual_f = 1e6 / per
        print(f"[{idx}/{total}] 请求 {_fmt(f)} Hz -> 周期 {per} us (实际 {_fmt(actual_f)} Hz)")
        send(client, f"PWM:PER:{per}")
        _sleep(client, args.dwell)
    print(f"共施加 {applied} 个有效频点")
    send(client, "OUTP:OFF")


# ---- 演示 4: 互补 PWM 死区 ----

def demo_comp(client, args):
    print(f"\n=== 演示4 互补 PWM 死区 ===")
    print(f"周期 {_fmt(args.period)} us, 占空比 {_fmt(args.duty)} %, "
          f"DTR={args.dtr} ns, DTF={args.dtf} ns, 驻留 {args.dwell} s, 通道 CH{args.channel}")
    send(client, "MODE:COMPPWM")
    send(client, f"CHAN:{args.channel}")
    send(client, f"COMP:PER:{_fmt(args.period)}")
    send(client, f"COMP:DUTY:{_fmt(args.duty)}")
    send(client, f"COMP:DTR:{args.dtr}")      # 上升沿死区, 防直通
    send(client, f"COMP:DTF:{args.dtf}")      # 下降沿死区, 防直通
    send(client, "OUTP:ON")
    print(f"互补 PWM 输出中, 驻留 {args.dwell} s (可观察死区波形)...")
    _sleep(client, args.dwell)
    send(client, "OUTP:OFF")


# ---- 演示 5: N 脉冲猝发 ----

def demo_burst(client, args):
    print(f"\n=== 演示5 N 脉冲猝发 ===")
    print(f"脉宽 {_fmt(args.width)} us, 个数 {args.count}, 间歇 {_fmt(args.interval)} us, "
          f"PRF {args.prf} Hz, 驻留 {args.dwell} s, 通道 CH{args.channel}")
    send(client, "MODE:NPULSE")
    send(client, f"CHAN:{args.channel}")
    send(client, f"PULS:WIDTH:{_fmt(args.width)}")
    send(client, f"PULS:COUNT:{args.count}")
    send(client, f"PULS:INTV:{_fmt(args.interval)}")
    send(client, f"BURST:{args.prf}")
    send(client, "OUTP:ON")
    _sleep(client, args.dwell)
    send(client, "OUTP:OFF")


# ---- 只读查询工具 ----

def demo_idn(client, args):
    send(client, "*IDN?", expect=None)

def demo_stat(client, args):
    send(client, "STAT", expect=None)

def demo_help(client, args):
    send(client, "HELP", expect=None)

def demo_scpi(client, args):
    print(SCPI_REFERENCE)


# ---- 参数解析 ----

def build_parser():
    p = argparse.ArgumentParser(
        prog="demo_scripts.py",
        description="发波板 SCPI 自动化演示脚本 (标准 socket, 连接 oled_mirror.py TCP 服务端)",
    )
    p.add_argument("--host", default=DEFAULT_HOST,
                   help=f"SCPI 服务端地址 (默认 {DEFAULT_HOST})")
    p.add_argument("--port", type=int, default=DEFAULT_PORT,
                   help=f"SCPI 服务端端口 (默认 {DEFAULT_PORT})")
    p.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT,
                   help="单命令响应超时秒数 (默认 8.0)")
    p.add_argument("--dry-run", action="store_true", dest="dry",
                   help="演练模式: 只打印命令, 不连接/发送")
    sub = p.add_subparsers(dest="command", required=True)

    sp = sub.add_parser("single", help="单脉冲脉宽递增 (电感饱和摸底)")
    sp.add_argument("--start", type=float, default=1.0, help="起始脉宽 us (默认 1)")
    sp.add_argument("--end", type=float, default=25.0, help="上限脉宽 us (默认 25)")
    sp.add_argument("--step", type=float, default=1.0, help="步进 us (默认 1)")
    sp.add_argument("--dwell", type=float, default=0.3, help="每步驻留秒 (默认 0.3)")
    sp.add_argument("--channel", type=int, default=1, help="通道 1-6 (默认 1)")
    sp.add_argument("--polarity", type=int, choices=[0, 1], default=None,
                    help="极性 0=高有效 1=低有效 (默认不改)")
    sp.set_defaults(func=demo_single)

    dp = sub.add_parser("double", help="双脉冲第一脉冲递增 (电流阶梯)")
    dp.add_argument("--pw1-start", type=float, default=2.0, help="PW1 起始 us (默认 2)")
    dp.add_argument("--pw1-end", type=float, default=14.0, help="PW1 上限 us (默认 14)")
    dp.add_argument("--step", type=float, default=2.0, help="PW1 步进 us (默认 2)")
    dp.add_argument("--intv", type=int, default=20, help="死区间隔 us (默认 20)")
    dp.add_argument("--pw2", type=int, default=10, help="第二脉冲 us (默认 10)")
    dp.add_argument("--dwell", type=float, default=0.3, help="每步驻留秒 (默认 0.3)")
    dp.add_argument("--channel", type=int, default=1, help="通道 1-6 (默认 1)")
    dp.set_defaults(func=demo_double)

    pp = sub.add_parser("pwm-sweep", help="PWM 自动扫频 (默认 20kHz 到 200kHz)")
    pp.add_argument("--fstart", type=float, default=20000.0, help="起始频率 Hz (默认 20000)")
    pp.add_argument("--fend", type=float, default=200000.0, help="终止频率 Hz (默认 200000)")
    pp.add_argument("--fstep", type=float, default=5000.0, help="频率步进 Hz (默认 5000)")
    pp.add_argument("--duty", type=float, default=50.0, help="占空比 %% (默认 50)")
    pp.add_argument("--dwell", type=float, default=0.2, help="每频点驻留秒 (默认 0.2)")
    pp.add_argument("--channel", type=int, default=1, help="通道 1-6 (默认 1)")
    pp.set_defaults(func=demo_pwm_sweep)

    cp = sub.add_parser("comp", help="互补 PWM 死区演示")
    cp.add_argument("--period", type=float, default=50.0, help="周期 us (默认 50)")
    cp.add_argument("--duty", type=float, default=50.0, help="占空比 %% (默认 50)")
    cp.add_argument("--dtr", type=int, default=100, help="上升沿死区 ns (默认 100)")
    cp.add_argument("--dtf", type=int, default=80, help="下降沿死区 ns (默认 80)")
    cp.add_argument("--dwell", type=float, default=3.0, help="驻留秒 (默认 3)")
    cp.add_argument("--channel", type=int, default=1, help="通道 1-6 (默认 1)")
    cp.set_defaults(func=demo_comp)

    bp = sub.add_parser("burst", help="N 脉冲猝发演示")
    bp.add_argument("--width", type=float, default=5.0, help="脉宽 us (默认 5)")
    bp.add_argument("--count", type=int, default=10, help="脉冲个数 (默认 10)")
    bp.add_argument("--interval", type=float, default=2.0, help="间歇 us (默认 2)")
    bp.add_argument("--prf", type=int, default=1000, help="猝发 PRF Hz (默认 1000)")
    bp.add_argument("--dwell", type=float, default=3.0, help="驻留秒 (默认 3)")
    bp.add_argument("--channel", type=int, default=1, help="通道 1-6 (默认 1)")
    bp.set_defaults(func=demo_burst)

    ip = sub.add_parser("idn", help="查询设备标识 *IDN?")
    ip.set_defaults(func=demo_idn)
    st = sub.add_parser("stat", help="查询状态 STAT")
    st.set_defaults(func=demo_stat)
    hp = sub.add_parser("help", help="查询命令清单 HELP")
    hp.set_defaults(func=demo_help)
    sc = sub.add_parser("scpi", help="打印完整 SCPI 命令参考 (本地, 无需连接)")
    sc.set_defaults(func=demo_scpi)

    return p


def main(argv=None):
    args = build_parser().parse_args(argv)
    if args.command == "scpi":
        print(SCPI_REFERENCE)   # 本地参考, 无需连接
        return 0
    client = ScpiClient(args.host, args.port, args.timeout, dry=args.dry)
    try:
        client.connect()
    except ScpiError as e:
        print(f"错误: {e}", file=sys.stderr)
        return 1

    rc = 0
    is_power = args.command in POWER_DEMOS
    try:
        args.func(client, args)
    except ScpiError as e:
        print(f"错误: {e}", file=sys.stderr)
        rc = 1
    except KeyboardInterrupt:
        print("\n用户中断。")
        rc = 130
    finally:
        if is_power and rc != 0:
            # 中断/异常时兜底关闭输出, 避免功率残留
            try:
                client.query("OUTP:OFF")
                print("安全兜底: 输出已关闭。")
            except Exception:
                pass
        client.close()
    return rc


if __name__ == "__main__":
    sys.exit(main())
