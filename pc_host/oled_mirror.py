#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
oled_mirror.py — STM32G474 脉冲发生器 PC 端上位机 (单文件)

功能:
  1. 1:1 OLED (128x128) 屏幕镜像: 读取 MCU 镜像帧 (0xAA 0x55 0xA5 协议), 像素级同步显示
  2. 虚拟方向键盘: 发送 BTN 帧 (等价物理摇杆按键)
  3. SCPI 命令行: 发送 CMD 帧, 显示 MCU 回 RSP/ACK
  4. 维持心跳: 每 500ms 发 PING, 未连接时 MCU 自动暂停镜像推流
  5. 串口/波特率选择按钮: 运行时枚举串口、切换波特率、断开/重连 (免命令行重启)
  6. 鼠标滚轮模拟板载编码器: 上滚=左/上, 下滚=右/下, 中键按下=确定, 长按=返回

用法:
  python oled_mirror.py [--port COM5] [--baud 460800] [--scale 4] [--demo]
  (不带 --port 时启动后从界面选择串口再连接)

依赖: pip install pygame pyserial
"""

import argparse
import os
import queue
import sys
import threading
import time

# pygame 惰性导入: 仅在 App 启动时进行, 以便协议工具函数可无 GUI 独立测试
pygame = None

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    serial = None
    list_ports = None  # --demo 模式可不依赖 pyserial


# ---------------------------------------------------------------- 协议常量
SYNC = (0xAA, 0x55, 0xA5)
T_BTN   = 0x01   # PC->MCU 虚拟按键  载荷=键码序列
T_CMD   = 0x02   # PC->MCU SCPI 命令 载荷=ASCII+'\n'
T_PING  = 0x03   # 双向心跳
T_FRAME = 0x10   # MCU->PC 屏幕镜像 2048B (原始)
T_RSP   = 0x11   # MCU->PC 响应 ASCII
T_ACK   = 0x12   # MCU->PC 连接确认
T_FRAME_RLE = 0x13  # MCU->PC 屏幕镜像 RLE 压缩

K_UP, K_DOWN, K_LEFT, K_RIGHT, K_ENTER, K_BACK = 0x01, 0x02, 0x03, 0x04, 0x05, 0x06
K_WHEEL_UP, K_WHEEL_DOWN = 0x08, 0x09  # 滚轮: 固件按当前页面类型智能分发 (菜单=上移, 数值=增大)

FRAME_LEN = 2048
WIDTH, HEIGHT = 128, 128

# 可循环切换的常用波特率 (CH9111L 高速模块可稳定 921600/2M; 保留低速兜底)
BAUDS = [2000000, 921600, 460800, 115200, 230400]

# 中键长按判定阈值 (s): 超过视为"返回", 否则"确定" (镜像板载编码器 KEY_LONG)
MID_LONG_PRESS_S = 0.6

# SCPI 命令列表 (TAB 自动补全)
SCPI_COMMANDS = ["OUTP", "TRIG", "12V", "MODE", "CHAN", "POL", "PULS", "DPULS",
                 "PWM", "LPWM", "COMP", "BURST", "PRESET", "STAT", "KEY", "HELP", "*IDN?"]

# TCP SCPI 服务端默认监听地址
TCP_HOST = "127.0.0.1"
TCP_PORT = 5025

# HELP 帮助面板内容 (英文, 避免 consolas 字体下中文乱码)
HELP_LINES = [
    "=== SCPI Commands ===",
    "OUTP:ON / OUTP:OFF    output enable",
    "TRIG                   single trigger",
    "12V:ON / 12V:OFF       12V output",
    "MODE:<m>               set mode",
    "  NPULSE DPULSE PWM NPULSELONG",
    "  PWMLONG COMPPWM COMPPWMLONG",
    "CHAN:<1-6>             select channel",
    "POL:0 / POL:1          polarity",
    "PULS:WIDTH:<us>        pulse width",
    "PULS:COUNT:<n>         pulse count",
    "PULS:INTV:<us>         interval",
    "DPULS:PW1:<us>         1st width (double)",
    "DPULS:INTV:<us>        interval (double)",
    "DPULS:PW2:<us>         2nd width (double)",
    "BURST:<hz>             PRF (0=single)",
    "PWM:PER:<us> / DUTY:<%>",
    "LPWM:PER:<us> / DUTY:<%>",
    "COMP:PER / DUTY / DTR / DTF",
    "PRESET:SAVE / LOAD     store",
    "STAT                   query status",
    "KEY:<1-6>              virtual key",
    "",
    "=== Mouse & Keys ===",
    "Wheel     encoder (up=next / down=prev)",
    "Middle    OK / hold = Back",
    "Arrows    UP DOWN LEFT RIGHT",
    "Enter=OK  Backspace=Back",
]


# ---------------------------------------------------------------- 协议工具
def crc16(data: bytes) -> int:
    """CRC16-CCITT 0x1021, init 0xFFFF, 无反射 —— 与 MCU uart_comm 完全一致"""
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def crc16_byte(crc: int, b: int) -> int:
    crc ^= b << 8
    for _ in range(8):
        if crc & 0x8000:
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF
        else:
            crc = (crc << 1) & 0xFFFF
    return crc


def build_frame(ftype: int, payload: bytes) -> bytes:
    """组一帧: <AA 55 A5> <TYPE> <LEN:LE> <Payload> <CRC16:LE>"""
    head = bytes(SYNC) + bytes([ftype, len(payload) & 0xFF, (len(payload) >> 8) & 0xFF])
    crc = crc16(head[3:] + payload)
    return head + payload + bytes([crc & 0xFF, (crc >> 8) & 0xFF])


def frame_btn(keys) -> bytes:
    return build_frame(T_BTN, bytes(keys))


def frame_cmd(cmd: str) -> bytes:
    return build_frame(T_CMD, cmd.encode('ascii'))


def frame_ping() -> bytes:
    return build_frame(T_PING, b'')


# ---------------------------------------------------------------- 读线程
class UartReader(threading.Thread):
    """阻塞读串口, 逐字节帧状态机解析, 结果经回调交付主线程"""

    def __init__(self, ser, on_frame):
        super().__init__(daemon=True)
        self.ser = ser
        self.on_frame = on_frame
        self._stop = threading.Event()
        self.bad_crc = 0   # CRC 失败帧计数 (诊断: 长帧丢字节会持续累加)
        # RX 状态机
        self._st = 'SYNC0'
        self._type = 0
        self._len = 0
        self._idx = 0
        self._crc = 0xFFFF
        self._payload = b''

    def stop(self):
        self._stop.set()

    def _reset(self):
        self._st = 'SYNC0'

    def _feed(self, b: int):
        st = self._st
        if st == 'SYNC0':
            if b == SYNC[0]:
                self._st = 'SYNC1'
        elif st == 'SYNC1':
            self._st = 'SYNC2' if b == SYNC[1] else 'SYNC0'
        elif st == 'SYNC2':
            if b == SYNC[2]:
                self._st = 'TYPE'
                self._crc = 0xFFFF
            else:
                self._st = 'SYNC0'
        elif st == 'TYPE':
            self._type = b
            self._crc = crc16_byte(self._crc, b)
            self._st = 'LEN0'
        elif st == 'LEN0':
            self._len = b
            self._crc = crc16_byte(self._crc, b)
            self._st = 'LEN1'
        elif st == 'LEN1':
            self._len |= b << 8
            self._crc = crc16_byte(self._crc, b)
            if self._len == 0:
                self._payload = b''      # 空载荷: 清残留, 避免误发上次帧的 payload
                self._st = 'CRC0'
            elif self._len > FRAME_LEN:
                self._reset()          # 载荷超限: 丢弃
            else:
                self._idx = 0
                self._payload = bytearray(self._len)
                self._st = 'PAYLOAD'
        elif st == 'PAYLOAD':
            self._payload[self._idx] = b
            self._crc = crc16_byte(self._crc, b)
            self._idx += 1
            if self._idx >= self._len:
                self._st = 'CRC0'
        elif st == 'CRC0':
            if self._crc & 0xFF == b:
                self._st = 'CRC1'
            else:
                self.bad_crc += 1
                self._reset()
        elif st == 'CRC1':
            if ((self._crc >> 8) & 0xFF) == b:
                self.on_frame(self._type, bytes(self._payload))
            else:
                self.bad_crc += 1
            self._reset()

    def run(self):
        while not self._stop.is_set():
            try:
                # 读尽当前缓冲立即返回, 避免 read(4096) 阻塞等满 4096 字节/超时
                # (timeout=0.5s) 导致帧被延迟最多 0.5s —— 动画结束时 MCU 停止推流,
                # 最后一帧会卡在这个等待里造成 ~1s 延迟。in_waiting>0 时一次读尽,
                # 无数据时 read(1) 阻塞等待 (高频推流时系统调用开销可接受)。
                n = self.ser.in_waiting
                data = self.ser.read(n) if n > 0 else self.ser.read(1)
            except Exception:
                self.on_frame('DISCONNECT', None)
                break
            if not data:
                continue
            for b in data:
                self._feed(b)


# ---------------------------------------------------------------- 帧渲染
def rle_decode(data: bytes) -> bytes:
    """RLE 解码: [count][byte], count=0 表示单字节字面量, 否则该字节重复 count 次。
    与 MCU 端 UcRleEncode 完全一致。"""
    out = bytearray()
    i = 0
    n = len(data)
    while i + 1 < n:
        cnt = data[i]
        b = data[i + 1]
        if cnt == 0:
            out.append(b)
        else:
            out.extend(bytes([b]) * cnt)
        i += 2
    return bytes(out)


def render_frame(frame: bytes) -> pygame.Surface:
    """SSD1315 页布局 [16][128] 点阵 -> RGB surface (白色点亮)"""
    # 用 bytes 直接构建以提速: 先构造 RGB 数组
    rgb = bytearray(WIDTH * HEIGHT * 3)
    for y in range(HEIGHT):
        page = y >> 3
        bit = y & 7
        base = y * WIDTH * 3
        row = frame[page * WIDTH:(page + 1) * WIDTH]
        for x in range(WIDTH):
            if row[x] & (1 << bit):
                i = base + x * 3
                rgb[i] = 255
                rgb[i + 1] = 255
                rgb[i + 2] = 255
    out = pygame.image.frombuffer(bytes(rgb), (WIDTH, HEIGHT), 'RGB')
    return out


def demo_frame(phase: int) -> bytes:
    """无串口测试图案: 棋盘 + 扫描线, 便于 UI 调试"""
    frame = bytearray(FRAME_LEN)
    for y in range(HEIGHT):
        page = y >> 3
        bit = y & 7
        base = page * WIDTH
        for x in range(WIDTH):
            v = ((x + phase) // 16 ^ (y + phase) // 16) & 1
            if (x + phase) % 64 < 32:
                v ^= 1
            if v:
                frame[base + x] |= (1 << bit)
    return bytes(frame)


def _enable_dpi_awareness():
    """Windows 高 DPI 缩放: 声明进程 DPI 感知, 避免 200% 缩放下文字边缘模糊。
    返回 DPI 缩放因子 (200% 缩放 = 2.0), 用于按比例放大窗口保持逻辑大小。"""
    if sys.platform != "win32":
        return 1.0
    try:
        import ctypes
        ctypes.windll.shcore.SetProcessDpiAwareness(2)   # PER_MONITOR_DPI_AWARE
    except Exception:
        try:
            import ctypes
            ctypes.windll.user32.SetProcessDPIAware()     # 旧版回退
        except Exception:
            pass
    try:
        import ctypes
        return ctypes.windll.user32.GetDpiForSystem() / 96.0
    except Exception:
        return 1.0


# ---------------------------------------------------------------- 主界面
class App:
    BG = (28, 30, 36)
    PANEL = (40, 42, 50)
    TEXT = (230, 232, 238)
    DIM = (150, 155, 165)
    GREEN = (80, 210, 110)
    RED = (235, 90, 90)
    BLUE = (90, 160, 230)

    def __init__(self, port=None, baud=2000000, scale=4, demo=False, dpi=1.0):
        global pygame
        import pygame  # 惰性: 仅在 GUI 启动时加载
        self.scale = scale
        self.demo = demo
        self.dpi = dpi              # 高 DPI 缩放因子 (200% = 2.0)

        # 串口状态
        self.port_name = port          # 当前选中串口名 (None=未选)
        self.baud = baud               # 当前选中波特率
        self.ser = None                # 已打开串口对象 (None=未连接)
        self.reader = None             # 读线程 (None=未连接)
        self.port_open = False         # 串口是否已打开
        self._tx_lock = threading.Lock()   # 串口写锁 (主线程 + TCP 线程并发写保护)

        self.conn_time = 0.0           # 最近收到 MCU 任一帧的时间
        self.last_frame = bytearray(FRAME_LEN)
        self.has_frame = False         # 是否收到过至少一帧镜像 (永久显示最后一帧用)
        self.frame_ts = 0.0
        self.frame_count = 0           # 累计镜像帧数
        self._fps_count = 0            # 帧率窗口内帧数
        self._fps_window_t = time.time()  # 帧率窗口起始
        self.mirror_fps = 0.0          # 实时镜像帧率 (fps)
        self.rsp_lines = []
        self.cmd_text = ""
        self.input_focused = False     # SCPI 输入框聚焦标记 (点击输入框才激活文本输入)
        self.out_enabled = None        # 输出使能状态 (None=未知, 由 STAT 响应的 OUT= 字段同步)
        self.msg_q = queue.Queue()
        self._last_ping = time.time()
        self._last_stat = 0.0        # 上次轮询 STAT 时刻 (同步板上输出开关等本地改动)
        self._poll_silent = 0        # 待静默处理的轮询 STAT 响应数 (不刷屏)

        # 串口下拉菜单状态
        self.port_menu_open = False        # 串口下拉菜单是否展开
        self._port_menu_rects = []         # [(rect, device_name), ...] 菜单项几何

        # 鼠标中键 (编码器按下) 状态
        self._mid_held = False
        self._mid_press_t = 0.0
        self._mid_long_fired = False

        # HELP 帮助面板状态
        self.help_open = False

        # SCPI 命令历史 (最近 10 条) + TAB 补全
        self.cmd_history = []
        self._hist_idx = None

        # 屏幕快照
        self._shot_idx = 0              # 会话内快照编号
        self.mode_name = None           # 当前模式名 (由 STAT 的 MODE= 同步)

        # TCP SCPI 服务端
        self._tcp_rsp_queue = queue.Queue(maxsize=64)  # RSP 响应队列 (供 TCP 客户端消费)
        self._tcp_server = None

        self.mirror_px = WIDTH * scale
        self.right_w = int(300 * dpi)
        WIN_W = self.mirror_px + self.right_w
        WIN_H = self.mirror_px + int(60 * dpi)
        pygame.init()
        pygame.display.set_caption("OLED Mirror / SCPI Console")
        self.screen = pygame.display.set_mode((WIN_W, WIN_H))
        self.font = pygame.font.SysFont("consolas", max(12, int(20 * dpi)))
        self.font_s = pygame.font.SysFont("consolas", max(10, int(16 * dpi)))
        self.clock = pygame.time.Clock()
        self.welcome_frame = self._build_welcome_frame()   # 未连接/断开后的静态欢迎画面

        mx = self.mirror_px
        W = self.right_w
        s = lambda v: int(round(v * dpi))   # 高 DPI 缩放辅助

        # 虚拟键几何 (右边板): 上下左右加宽, 间隙与 Enter/Back 行一致 (12px);
        # Enter/Back 下方新增 OUT 开关 + TRIG 触发快捷键
        self.btn_up    = pygame.Rect(mx + W // 2 - s(38), s(14), s(76), s(34))
        self.btn_left  = pygame.Rect(mx + s(24), s(54), s(76), s(34))
        self.btn_down  = pygame.Rect(mx + W // 2 - s(38), s(54), s(76), s(34))
        self.btn_right = pygame.Rect(mx + s(200), s(54), s(76), s(34))
        self.btn_enter = pygame.Rect(mx + s(24), s(94), s(120), s(34))
        self.btn_back  = pygame.Rect(mx + s(156), s(94), s(120), s(34))
        self.btn_out   = pygame.Rect(mx + s(24), s(134), s(120), s(34))
        self.btn_trig  = pygame.Rect(mx + s(156), s(134), s(120), s(34))

        # 串口/波特率选择行 (与按键区拉开距离)
        self.sel_port  = pygame.Rect(mx + s(10), s(184), s(100), s(28))
        self.sel_baud  = pygame.Rect(mx + s(112), s(184), s(76), s(28))
        self.btn_conn  = pygame.Rect(mx + s(190), s(184), s(100), s(28))

        # SCPI 输入框 + HELP 按钮 + RSP 消息区
        self.input_box = pygame.Rect(mx + s(10), s(220), W - s(70), s(30))
        self.btn_help  = pygame.Rect(mx + W - s(55), s(220), s(45), s(30))
        self.rsp_area  = pygame.Rect(mx + s(10), s(258), W - s(20), WIN_H - s(258) - s(10))

        # 屏幕快照按钮 (状态指示栏右边, 与状态栏垂直居中)
        self.btn_shot  = pygame.Rect(mx - s(90), self.mirror_px + s(15), s(80), s(30))

        # 可选串口列表 (每次点击端口按钮时刷新)
        self._ports = self._list_ports()

        # 启动后台 TCP SCPI 服务端 (默认 127.0.0.1:5025)
        if not self.demo:
            self._start_tcp_server()

        # 启动即连接 (带 --port 时); demo 或未指定端口则留待界面操作
        if not self.demo and self.port_name is not None:
            self._connect()

    # ------------------------------------------------------------ 串口管理
    def _list_ports(self):
        """枚举当前系统可用串口设备名列表"""
        if list_ports is None:
            return []
        try:
            return [p.device for p in list_ports.comports()]
        except Exception:
            return []

    def _connect(self):
        if self.ser is not None:
            return                       # 已连接
        if self.port_name is None:
            print("[串口] 未选择串口, 无法连接")
            return
        if serial is None:
            print("[串口] pyserial 未安装: pip install pyserial")
            return
        try:
            ser = serial.Serial(self.port_name, self.baud, timeout=0.5)
        except Exception as e:
            print(f"[串口] 打开 {self.port_name} @ {self.baud} 失败: {e}")
            self.ser = None
            self.port_open = False
            return
        self.ser = ser
        reader = UartReader(ser, self._on_frame)
        self.reader = reader
        # 用闭包绑定 reader 身份: 旧 reader 关闭时的 DISCONNECT 通知不污染新连接
        reader.on_frame = (lambda t, p, _r=reader: self._on_frame(t, p, _r))
        reader.start()
        self.port_open = True
        self.conn_time = time.time()
        self._last_ping = time.time()
        print(f"[串口] 已连接 {self.port_name} @ {self.baud}")
        # 连接后立即查询一次状态, 同步 OUT 开关指示 (避免按钮一开始显示灰色 ?)
        self._send(frame_cmd("STAT"))

    def _disconnect(self):
        if self.reader is not None:
            self.reader.stop()
            self.reader = None
        if self.ser is not None:
            try:
                self.ser.close()
            except Exception:
                pass
            self.ser = None
        self.port_open = False
        print("[串口] 已断开")

    def _cycle_port(self):
        """点击端口按钮: 展开/收起下拉列表 (点击具体项选中, 不再循环切换)"""
        if self.port_menu_open:
            self.port_menu_open = False
            self._port_menu_rects = []
            return
        self._ports = self._list_ports()
        if not self._ports:
            print("[串口] 未检测到串口设备")
            return
        self.port_menu_open = True
        # 构建下拉菜单项几何: 从端口按钮下方展开, 每项高 24px
        self._port_menu_rects = []
        y = self.sel_port.bottom + 2
        for dev in self._ports:
            self._port_menu_rects.append(
                (pygame.Rect(self.sel_port.x, y, self.sel_port.width, 24), dev))
            y += 26

    def _select_port(self, device):
        """下拉菜单选中某个串口 (连接中则先断开)"""
        self.port_menu_open = False
        if self.port_open and self.port_name != device:
            self._disconnect()
        self.port_name = device

    def _cycle_baud(self):
        """点击波特率按钮: 循环切换波特率 (连接中则先断开)"""
        if self.port_open:
            self._disconnect()
        if self.baud in BAUDS:
            i = BAUDS.index(self.baud)
            self.baud = BAUDS[(i + 1) % len(BAUDS)]
        else:
            self.baud = BAUDS[0]

    def _toggle_conn(self):
        if self.port_open:
            self._disconnect()
        else:
            self._connect()

    # ------------------------------------------------------------ 帧处理
    def _on_frame(self, ftype, payload, reader=None):
        if ftype == 'DISCONNECT':
            # 只有当前 reader 的断连才处理 (旧 reader 关闭时也会发 DISCONNECT)
            if reader is not None and reader is not self.reader:
                return
            self.port_open = False
            return
        self.msg_q.put((ftype, payload))

    def _count_frame(self):
        """镜像帧计数 + 实时帧率 (1s 滑动窗口)"""
        self.frame_count += 1
        self._fps_count += 1
        now = time.time()
        dt = now - self._fps_window_t
        if dt >= 1.0:
            self.mirror_fps = self._fps_count / dt
            self._fps_count = 0
            self._fps_window_t = now

    def _process_queue(self):
        try:
            while True:
                ftype, payload = self.msg_q.get_nowait()
                if ftype == 'DISCONNECT':
                    self.port_open = False
                    continue
                self.conn_time = time.time()
                if ftype == T_FRAME and payload is not None and len(payload) == FRAME_LEN:
                    self.last_frame = payload
                    self.has_frame = True
                    self.frame_ts = time.time()
                    self._count_frame()
                    if not getattr(self, '_frame_seen', False):
                        self._frame_seen = True
                        print("[镜像] 已收到第一帧 FRAME")
                elif ftype == T_FRAME_RLE and payload is not None:
                    # RLE 压缩帧: 解码回 2048B 点阵
                    dec = rle_decode(payload)
                    if len(dec) == FRAME_LEN:
                        self.last_frame = dec
                        self.has_frame = True
                        self.frame_ts = time.time()
                        self._count_frame()
                        if not getattr(self, '_frame_seen', False):
                            self._frame_seen = True
                            print("[镜像] 已收到第一帧 FRAME(RLE)")
                elif ftype in (T_RSP, T_ACK):
                    try:
                        txt = payload.decode('ascii', 'replace').strip()
                    except Exception:
                        txt = ''
                    if ftype == T_RSP:
                        # 从 STAT 响应同步输出使能/模式 (无论主动查询还是轮询)
                        is_stat = "OUT=" in txt
                        if is_stat:
                            for part in txt.split(";"):
                                if part.startswith("OUT="):
                                    self.out_enabled = (part[4:] == "ON")
                                elif part.startswith("MODE="):
                                    self.mode_name = part[5:]
                        # 轮询 STAT 响应静默 (只更新状态, 不打印/不刷屏); 主动查询才显示
                        is_poll = is_stat and self._poll_silent > 0
                        if is_poll:
                            self._poll_silent -= 1
                        else:
                            # 完整内容打印到终端 (GUI 窗口裁剪, 终端可见 FR/ST/OR/RX/LNK 全字段)
                            print("[RSP] " + txt)
                            self.rsp_lines.append("> " + txt)
                        # RSP 入队给 TCP 客户端消费 (轮询 STAT 不进入, 避免污染请求/响应对应)
                        if not is_poll:
                            try:
                                self._tcp_rsp_queue.put_nowait(txt)
                            except queue.Full:
                                pass
                    else:
                        # ACK 只更新连接指示, 不污染 rsp_lines (否则 500ms 刷屏淹没 RSP)
                        pass
                    self.rsp_lines = self.rsp_lines[-12:]
        except queue.Empty:
            pass

    def _send(self, data: bytes):
        if self.ser is None or not self.port_open:
            return False
        try:
            with self._tx_lock:
                self.ser.write(data)
            return True
        except Exception:
            self.port_open = False
            return False

    def _inject_key(self, code: int):
        self._send(frame_btn([code]))

    def _send_cmd(self):
        cmd = self.cmd_text.strip()
        if not cmd:
            return
        self.cmd_text = ""
        self.rsp_lines.append("< " + cmd)
        self.rsp_lines = self.rsp_lines[-12:]
        self._send(frame_cmd(cmd))
        # 记录历史 (去重, 最多 10 条)
        if cmd in self.cmd_history:
            self.cmd_history.remove(cmd)
        self.cmd_history.append(cmd)
        self.cmd_history = self.cmd_history[-10:]
        self._hist_idx = None

    def _tab_complete(self):
        """TAB 自动补全: 匹配命令前缀, 唯一则补全, 多个则补公共前缀"""
        prefix = self.cmd_text.upper()
        if not prefix:
            return
        matches = [c for c in SCPI_COMMANDS if c.startswith(prefix)]
        if not matches:
            return
        if len(matches) == 1:
            self.cmd_text = matches[0] + ":"
        else:
            common = os.path.commonprefix(matches)
            if len(common) > len(prefix):
                self.cmd_text = common
        self._hist_idx = None

    def _toggle_output(self):
        """OUT 按钮: 切换输出使能 (OUTP:ON / OUTP:OFF), 本地乐观翻转, STAT 回传校正"""
        if self.out_enabled:
            self._send(frame_cmd("OUTP:OFF"))
            self.out_enabled = False
        else:
            self._send(frame_cmd("OUTP:ON"))
            self.out_enabled = True

    def _fire_trig(self):
        """TRIG 按钮: 单次触发 (等价 TRIG 命令)"""
        self._send(frame_cmd("TRIG"))

    def _save_screenshot(self):
        """一键快照: 保存当前镜像为高质量 PNG 到 screenshots/ 文件夹"""
        if not self.has_frame:
            print("[快照] 无镜像帧可保存")
            self.rsp_lines.append("> no frame to save")
            self.rsp_lines = self.rsp_lines[-12:]
            return
        try:
            os.makedirs("screenshots", exist_ok=True)
            rgb = render_frame(self.last_frame)   # 128x128 原始点阵
            # 放大到 mirror_px 保持像素锐利 (transform.scale 为最近邻, 无平滑模糊)
            big = pygame.transform.scale(rgb, (self.mirror_px, self.mirror_px))
            ts = time.strftime("%Y%m%d_%H%M%S")
            mode = self.mode_name or "UNKNOWN"
            self._shot_idx += 1
            fname = os.path.join("screenshots", f"{ts}_{mode}_{self._shot_idx:03d}.png")
            pygame.image.save(big, fname)
            print(f"[快照] 已保存 {fname}")
            self.rsp_lines.append(f"> saved {fname}")
        except Exception as e:
            print(f"[快照] 保存失败: {e}")
            self.rsp_lines.append(f"> snapshot failed: {e}")
        self.rsp_lines = self.rsp_lines[-12:]

    # ------------------------------------------------------------ TCP SCPI 服务端
    def _start_tcp_server(self):
        """启动后台 TCP SCPI 服务端, 监听 127.0.0.1:5025"""
        import socket
        try:
            srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            srv.bind((TCP_HOST, TCP_PORT))
            srv.listen(5)
        except OSError as e:
            print(f"[TCP] 监听 {TCP_HOST}:{TCP_PORT} 失败: {e}")
            self.rsp_lines.append(f"> TCP listen fail: {e}")
            self.rsp_lines = self.rsp_lines[-12:]
            return
        self._tcp_server = srv
        msg = f"TCP SCPI server @ {TCP_HOST}:{TCP_PORT}"
        print(f"[TCP] {msg}")
        self.rsp_lines.append(f"> {msg}")
        self.rsp_lines = self.rsp_lines[-12:]
        threading.Thread(target=self._tcp_accept_loop, args=(srv,), daemon=True).start()

    def _tcp_accept_loop(self, srv):
        import socket
        while True:
            try:
                conn, addr = srv.accept()
            except OSError:
                break
            print(f"[TCP] 客户端连接 {addr}")
            threading.Thread(target=self._tcp_client_handler, args=(conn,), daemon=True).start()

    def _tcp_client_handler(self, conn):
        import socket
        conn.settimeout(1.0)
        buf = b""
        while True:
            try:
                data = conn.recv(4096)
            except socket.timeout:
                continue
            except (ConnectionError, OSError):
                break
            if not data:
                break
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                cmd = line.decode('ascii', 'ignore').strip()
                if not cmd:
                    continue
                # 清空积压旧 RSP, 保证本次请求-响应对应
                while not self._tcp_rsp_queue.empty():
                    try:
                        self._tcp_rsp_queue.get_nowait()
                    except queue.Empty:
                        break
                if not self._send(frame_cmd(cmd)):
                    try:
                        conn.sendall(b"ERR NOT CONNECTED\n")
                    except OSError:
                        return
                    continue
                # 期望查询 STAT 的命令才接受状态串; 其余命令过滤掉轮询/主动推送的 STAT 干扰
                want_stat = (cmd.upper() == "STAT")
                rsp = self._await_tcp_response(want_stat)
                if rsp is None and cmd.upper() != "TRIG":
                    # 首次命令偶发丢帧: 非 TRIG 可安全重发一次 (TRIG 重发会二次触发, 跳过)
                    self._send(frame_cmd(cmd))
                    rsp = self._await_tcp_response(want_stat)
                if rsp is not None:
                    try:
                        conn.sendall(rsp.encode('ascii', 'ignore') + b"\n")
                    except OSError:
                        return
                else:
                    try:
                        conn.sendall(b"ERR TIMEOUT\n")
                    except OSError:
                        return
        conn.close()

    def _await_tcp_response(self, want_stat, timeout=3.0):
        """等待一条与当前命令匹配的响应。

        非 STAT 查询时丢弃状态串 (来自 5s 轮询/固件状态变化主动推送的 STAT),
        避免请求/响应错位。want_stat=True 时接受任意响应 (STAT 查询只需最新状态)。
        """
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                rsp = self._tcp_rsp_queue.get(timeout=0.2)
            except queue.Empty:
                continue
            if want_stat:
                return rsp
            if "OUT=" in rsp:   # STAT 状态串 (含 OUT= 字段), 干扰项, 丢弃继续等
                continue
            return rsp
        return None

    # ------------------------------------------------------------ 事件
    def _handle_events(self):
        for ev in pygame.event.get():
            if ev.type == pygame.QUIT:
                self.quit()
            elif ev.type == pygame.KEYDOWN:
                if self.input_focused:
                    # 输入框聚焦: 键盘只当文本编辑
                    if ev.key == pygame.K_RETURN:
                        self._send_cmd()
                    elif ev.key == pygame.K_BACKSPACE:
                        self.cmd_text = self.cmd_text[:-1]
                        self._hist_idx = None
                    elif ev.key == pygame.K_ESCAPE:
                        self.input_focused = False   # 退出编辑, 恢复方向键遥控
                    elif ev.key == pygame.K_UP:
                        # 历史回溯: 上键 = 更早的命令
                        if self.cmd_history:
                            if self._hist_idx is None:
                                self._hist_idx = len(self.cmd_history) - 1
                            elif self._hist_idx > 0:
                                self._hist_idx -= 1
                            self.cmd_text = self.cmd_history[self._hist_idx]
                    elif ev.key == pygame.K_DOWN:
                        # 历史回溯: 下键 = 更新的命令
                        if self._hist_idx is not None:
                            if self._hist_idx < len(self.cmd_history) - 1:
                                self._hist_idx += 1
                                self.cmd_text = self.cmd_history[self._hist_idx]
                            else:
                                self._hist_idx = None
                                self.cmd_text = ""
                    elif ev.key == pygame.K_TAB:
                        self._tab_complete()
                    elif ev.unicode and ev.unicode.isprintable():
                        if len(self.cmd_text) < 90:
                            self.cmd_text += ev.unicode
                            self._hist_idx = None
                else:
                    # 非聚焦: 方向键 + 回车/退格/Esc 均为遥控虚拟按键
                    if ev.key == pygame.K_UP:
                        self._inject_key(K_UP)
                    elif ev.key == pygame.K_DOWN:
                        self._inject_key(K_DOWN)
                    elif ev.key == pygame.K_LEFT:
                        self._inject_key(K_LEFT)
                    elif ev.key == pygame.K_RIGHT:
                        self._inject_key(K_RIGHT)
                    elif ev.key == pygame.K_RETURN:
                        self._inject_key(K_ENTER)     # 非聚焦回车 = ENTER 虚拟键
                    elif ev.key == pygame.K_BACKSPACE:
                        self._inject_key(K_BACK)      # 非聚焦退格 = BACK 虚拟键
                    elif ev.key == pygame.K_ESCAPE:
                        self._inject_key(K_BACK)
            elif ev.type == pygame.MOUSEBUTTONDOWN:
                if ev.button == 4:
                    # 滚轮上滚: 固件按当前页面类型分发 (菜单上移 / 数值增大)
                    self._inject_key(K_WHEEL_UP)
                elif ev.button == 5:
                    # 滚轮下滚: 固件按当前页面类型分发 (菜单下移 / 数值减小)
                    self._inject_key(K_WHEEL_DOWN)
                elif ev.button == 2:
                    # 中键按下 = 编码器按键: 记录时刻, 长按判定在 run() 中做
                    self._mid_held = True
                    self._mid_press_t = time.time()
                    self._mid_long_fired = False
                elif ev.button == 1:
                    self._on_left_click(ev.pos)
            elif ev.type == pygame.MOUSEBUTTONUP and ev.button == 2:
                # 中键释放: 未触发过长按 -> 短按 = 确定 (msg_click)
                if self._mid_held and not self._mid_long_fired:
                    self._inject_key(K_ENTER)
                self._mid_held = False

    def _on_left_click(self, pos):
        # HELP 面板打开时: 点击任意处关闭面板, 并屏蔽其它控件
        if self.help_open:
            self.help_open = False
            return

        # 串口下拉菜单展开时: 优先处理菜单项点击 (点菜单项选中, 点其它地方收起)
        if self.port_menu_open:
            for rect, dev in self._port_menu_rects:
                if rect.collidepoint(pos):
                    self._select_port(dev)
                    return
            # 点击菜单外: 收起菜单 (若点中其它控件仍继续处理)
            if not self.sel_port.collidepoint(pos):
                self.port_menu_open = False
                # 不 return, 让下方控件判断生效 (点击键盘/输入框等)
        if self.btn_up.collidepoint(pos):
            self._inject_key(K_UP)
            self.input_focused = False
        elif self.btn_left.collidepoint(pos):
            self._inject_key(K_LEFT)
            self.input_focused = False
        elif self.btn_down.collidepoint(pos):
            self._inject_key(K_DOWN)
            self.input_focused = False
        elif self.btn_right.collidepoint(pos):
            self._inject_key(K_RIGHT)
            self.input_focused = False
        elif self.btn_enter.collidepoint(pos):
            self._inject_key(K_ENTER)
            self.input_focused = False
        elif self.btn_back.collidepoint(pos):
            self._inject_key(K_BACK)
            self.input_focused = False
        elif self.btn_out.collidepoint(pos):
            self._toggle_output()
            self.input_focused = False
        elif self.btn_trig.collidepoint(pos):
            self._fire_trig()
            self.input_focused = False
        elif self.sel_port.collidepoint(pos):
            self._cycle_port()
            self.input_focused = False
        elif self.sel_baud.collidepoint(pos):
            self._cycle_baud()
            self.input_focused = False
        elif self.btn_conn.collidepoint(pos):
            self._toggle_conn()
            self.input_focused = False
        elif self.btn_shot.collidepoint(pos):
            self._save_screenshot()
            self.input_focused = False
        elif self.btn_help.collidepoint(pos):
            self.help_open = True        # 打开帮助面板
            self.input_focused = False
        elif self.input_box.collidepoint(pos):
            self.input_focused = True    # 点击输入框 → 激活文本编辑
        else:
            # 点击其它任意位置: 取消输入框激活 (不再只能按 ESC 退出)
            self.input_focused = False

    def _wrap_text(self, text, font, max_width):
        """按像素宽度把文本折成多行 (消息区长文本完整显示)"""
        lines = []
        cur = ""
        for ch in text:
            if font.size(cur + ch)[0] <= max_width:
                cur += ch
            else:
                if cur:
                    lines.append(cur)
                cur = ch
        if cur:
            lines.append(cur)
        return lines or [""]

    def _build_welcome_frame(self):
        """生成静态欢迎画面点阵 (未连接/断开后显示, 仅居中 DISCONNECTED)"""
        surf = pygame.Surface((WIDTH, HEIGHT))
        surf.fill((0, 0, 0))
        font = pygame.font.SysFont("consolas", 13, bold=True)
        t = font.render("DISCONNECTED", True, (255, 255, 255))
        surf.blit(t, t.get_rect(center=(WIDTH // 2, HEIGHT // 2)))
        frame = bytearray(FRAME_LEN)
        for y in range(HEIGHT):
            page = y >> 3
            bit = y & 7
            base = page * WIDTH
            for x in range(WIDTH):
                if surf.get_at((x, y))[0] > 128:   # 白色点亮
                    frame[base + x] |= (1 << bit)
        return bytes(frame)

    # ------------------------------------------------------------ 绘制
    def _draw(self, now):
        scr = self.screen
        scr.fill(self.BG)

        # 镜像区: 未连接/断开后显示静态欢迎画面, 已连接且有帧才显示镜像
        if self.demo:
            frame = self.last_frame            # demo 模式: 移动棋盘格
        elif self.ser is None or not self.has_frame:
            frame = self.welcome_frame         # 未连接 / 已连接但还没收到帧: 欢迎画面
        else:
            frame = self.last_frame            # 已连接有帧: 镜像
        rgb = render_frame(frame)
        rgb = pygame.transform.scale(rgb, (self.mirror_px, self.mirror_px))
        scr.blit(rgb, (0, 0))

        # 底部状态
        connected = self.port_open and (now - self.conn_time < 1.5)
        col = self.GREEN if connected else self.RED
        d = self.dpi
        st = self.font.render("●" if connected else "○", True, col)
        scr.blit(st, (int(10 * d), self.mirror_px + int(12 * d)))
        info = self.font.render(
            "128x128 | {} px | {:.0f} fps{}".format(
                self.mirror_px, self.clock.get_fps(),
                " | DEMO" if self.demo else ""),
            True, self.DIM)
        scr.blit(info, (int(38 * d), self.mirror_px + int(12 * d)))

        # 诊断: CRC 失败帧计数 + 距上一帧时间 + 实时镜像帧率 + 累计帧数
        if self.reader is not None:
            diag = self.font_s.render(
                "bad_crc={} fr={:.0f} fps={:.1f} frames={}".format(
                    self.reader.bad_crc, time.time() - self.frame_ts,
                    self.mirror_fps, self.frame_count), True, self.RED)
            scr.blit(diag, (int(38 * d), self.mirror_px + int(34 * d)))

        # 屏幕快照按钮 (状态指示栏右边)
        sh = self.btn_shot.collidepoint(pygame.mouse.get_pos())
        pygame.draw.rect(scr, (60, 66, 80) if sh else (46, 50, 60), self.btn_shot, border_radius=5)
        pygame.draw.rect(scr, self.BLUE, self.btn_shot, 1, border_radius=5)
        sshot = self.font_s.render("SNAP", True, self.TEXT)
        scr.blit(sshot, sshot.get_rect(center=self.btn_shot.center))

        # 右板 (半透明遮罩)
        panel = pygame.Surface((self.right_w, self.mirror_px + int(60 * self.dpi)), pygame.SRCALPHA)
        panel.fill((255, 255, 255, 70))
        scr.blit(panel, (self.mirror_px, 0))

        lbl = self.font_s

        # 虚拟键盘 (统一用字母, 避免 consolas 字体下符号显示为乱码)
        for rect, txt in [
            (self.btn_up, "UP"), (self.btn_left, "LEFT"), (self.btn_down, "DOWN"),
            (self.btn_right, "RIGHT"), (self.btn_enter, "ENTER"), (self.btn_back, "BACK")]:
            hover = rect.collidepoint(pygame.mouse.get_pos())
            pygame.draw.rect(scr, self.BLUE if hover else (90, 100, 120), rect, border_radius=6)
            t = lbl.render(txt, True, self.TEXT)
            scr.blit(t, t.get_rect(center=rect.center))

        # OUT 开关 (EN=绿 / DIS=红 / 未知=灰) + TRIG 触发快捷键
        if self.out_enabled is None:
            out_txt, out_col = "OUT ?", self.DIM
        elif self.out_enabled:
            out_txt, out_col = "OUT EN", self.GREEN
        else:
            out_txt, out_col = "OUT DIS", self.RED
        for rect, txt, fg in [
            (self.btn_out, out_txt, out_col),
            (self.btn_trig, "TRIG", self.TEXT)]:
            hover = rect.collidepoint(pygame.mouse.get_pos())
            pygame.draw.rect(scr, self.BLUE if hover else (90, 100, 120), rect, border_radius=6)
            t = lbl.render(txt, True, fg)
            scr.blit(t, t.get_rect(center=rect.center))

        # 串口/波特率选择行 (英文, 避免 consolas 字体下中文乱码)
        port_txt = self.port_name if self.port_name else "Port"
        baud_txt = str(self.baud)
        conn_txt = "Disconnect" if self.port_open else "Connect"
        conn_col = self.RED if self.port_open else self.GREEN
        for rect, txt, fg in [
            (self.sel_port, port_txt, self.TEXT),
            (self.sel_baud, baud_txt, self.TEXT),
            (self.btn_conn, conn_txt, conn_col)]:
            hover = rect.collidepoint(pygame.mouse.get_pos())
            pygame.draw.rect(scr, (60, 66, 80) if hover else (46, 50, 60), rect, border_radius=5)
            pygame.draw.rect(scr, self.BLUE, rect, 1, border_radius=5)
            t = lbl.render(txt, True, fg)
            # 文本超出按钮宽度时裁剪显示
            if t.get_width() > rect.width - 8:
                t = lbl.render(txt[:max(1, rect.width // 9)], True, fg)
            scr.blit(t, t.get_rect(center=rect.center))

        # SCPI 输入 (聚焦时边框高亮绿色, 提示当前处于文本编辑模式)
        pygame.draw.rect(scr, (20, 22, 28), self.input_box, border_radius=4)
        pygame.draw.rect(scr, self.GREEN if self.input_focused else self.BLUE,
                         self.input_box, 1, border_radius=4)
        head = lbl.render("scpi> " + self.cmd_text + ("|" if self.input_focused else ""),
                          True, self.TEXT)
        scr.blit(head, self.input_box.move(6, 6))

        # HELP 按钮
        hh = self.btn_help.collidepoint(pygame.mouse.get_pos())
        pygame.draw.rect(scr, (60, 66, 80) if hh else (46, 50, 60), self.btn_help, border_radius=5)
        pygame.draw.rect(scr, self.BLUE, self.btn_help, 1, border_radius=5)
        ht = lbl.render("HELP", True, self.TEXT)
        scr.blit(ht, ht.get_rect(center=self.btn_help.center))

        # RSP 区 (支持折行, 从下往上画, 长文本自动换行完整显示)
        pygame.draw.rect(scr, (12, 14, 18), self.rsp_area)
        wrap_w = self.rsp_area.width - int(16 * d)
        all_lines = []
        for line in self.rsp_lines[-12:]:
            all_lines.extend(self._wrap_text(line, lbl, wrap_w))
        y = self.rsp_area.bottom - int(4 * d)
        for wline in reversed(all_lines):
            t = lbl.render(wline, True, self.DIM)
            yy = y - t.get_height()
            if yy < self.rsp_area.y:
                break
            scr.blit(t, (self.rsp_area.x + int(8 * d), yy))
            y = yy - int(2 * d)

        # 串口下拉菜单 (最后绘制, 确保覆盖在输入框/RSP 区之上)
        if self.port_menu_open:
            for rect, dev in self._port_menu_rects:
                hover = rect.collidepoint(pygame.mouse.get_pos())
                cur = (dev == self.port_name)
                pygame.draw.rect(scr, (70, 76, 90) if hover else (46, 50, 60), rect)
                pygame.draw.rect(scr, self.GREEN if cur else self.BLUE, rect, 1)
                t = lbl.render(dev, True, self.TEXT)
                scr.blit(t, (rect.x + int(8 * d), rect.centery - t.get_height() // 2))

        # HELP 帮助面板 (最后绘制, 覆盖全窗口)
        if self.help_open:
            w, h = scr.get_size()
            overlay = pygame.Surface((w, h), pygame.SRCALPHA)
            overlay.fill((0, 0, 0, 210))
            scr.blit(overlay, (0, 0))
            pad = int(16 * d)
            panel_rect = pygame.Rect(pad, pad, w - 2 * pad, h - 2 * pad)
            pygame.draw.rect(scr, (40, 42, 50), panel_rect, border_radius=int(8 * d))
            pygame.draw.rect(scr, self.BLUE, panel_rect, int(2 * d), border_radius=int(8 * d))
            y = panel_rect.y + int(12 * d)
            for line in HELP_LINES:
                t = lbl.render(line, True, self.TEXT)
                scr.blit(t, (panel_rect.x + int(16 * d), y))
                y += int(18 * d)
            tip = lbl.render("click anywhere to close", True, self.DIM)
            scr.blit(tip, (panel_rect.x + int(16 * d), panel_rect.bottom - int(22 * d)))

        pygame.display.flip()

    # ------------------------------------------------------------ 心跳
    def _heartbeat(self, now):
        if self.ser is not None and self.port_open:
            if now - self._last_ping >= 0.5:
                self._send(frame_ping())
                self._last_ping = now
            # 低频兜底轮询 STAT (5s): 固件已在状态变化时主动推送, 此处仅作断线重连/丢帧兜底。
            # 轮询响应由 _process_queue 静默处理, 不打印/不刷屏
            if now - self._last_stat >= 5.0:
                self._last_stat = now
                self._poll_silent += 1
                self._send(frame_cmd("STAT"))

    def quit(self):
        if self.reader is not None:
            self.reader.stop()
        if self.ser is not None:
            try:
                self.ser.close()
            except Exception:
                pass
        if self._tcp_server is not None:
            try:
                self._tcp_server.close()
            except Exception:
                pass
        pygame.quit()
        sys.exit(0)

    def run(self):
        demo_phase = 0
        while True:
            now = time.time()
            self._process_queue()
            self._handle_events()

            # 中键长按判定: 按住超阈值触发一次"返回" (msg_return)
            if self._mid_held and not self._mid_long_fired and \
                    now - self._mid_press_t > MID_LONG_PRESS_S:
                self._mid_long_fired = True
                self._inject_key(K_BACK)

            if self.ser is None and self.demo:
                # 演示模式: 定时换测试图案 (棋盘格移动)
                demo_phase = int(now * 4)
                self.last_frame = demo_frame(demo_phase)
                self.has_frame = True
                self.frame_ts = now
                self.conn_time = now
            self._heartbeat(now)
            self._draw(now)
            self.clock.tick(60)


def main():
    ap = argparse.ArgumentParser(description="STM32G474 OLED mirror + SCPI controller")
    ap.add_argument("--port", help="serial port e.g. COM5 (省略则从界面选择)")
    ap.add_argument("--baud", type=int, default=2000000)
    ap.add_argument("--scale", type=int, default=4)
    ap.add_argument("--demo", action="store_true", help="run without serial (test pattern)")
    args = ap.parse_args()

    if not args.demo and serial is None:
        print("pyserial 未安装: pip install pyserial, 或使用 --demo")
        sys.exit(1)

    # Windows 高 DPI: 声明感知避免文字模糊, 并按缩放因子放大窗口保持逻辑大小
    dpi_scale = _enable_dpi_awareness()
    scale = max(1, int(args.scale * dpi_scale))

    App(port=args.port, baud=args.baud, scale=scale, demo=args.demo, dpi=dpi_scale).run()


if __name__ == "__main__":
    main()
