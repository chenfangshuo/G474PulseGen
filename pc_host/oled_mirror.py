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

FRAME_LEN = 2048
WIDTH, HEIGHT = 128, 128

# 可循环切换的常用波特率 (CH9111L 高速模块可稳定 921600/2M; 保留低速兜底)
BAUDS = [460800, 921600, 2000000, 115200, 230400]

# 中键长按判定阈值 (s): 超过视为"返回", 否则"确定" (镜像板载编码器 KEY_LONG)
MID_LONG_PRESS_S = 0.6


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
                # 块读取: 逐字节 read(1) 在 921600 下每秒 9.2 万次系统调用, 读线程跟不上
                # 导致 CH340 缓冲溢出丢字节、CRC 全错 (上位机纯色), 改为一次读尽缓冲
                data = self.ser.read(4096)
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


# ---------------------------------------------------------------- 主界面
class App:
    BG = (28, 30, 36)
    PANEL = (40, 42, 50)
    TEXT = (230, 232, 238)
    DIM = (150, 155, 165)
    GREEN = (80, 210, 110)
    RED = (235, 90, 90)
    BLUE = (90, 160, 230)

    def __init__(self, port=None, baud=460800, scale=4, demo=False):
        global pygame
        import pygame  # 惰性: 仅在 GUI 启动时加载
        self.scale = scale
        self.demo = demo

        # 串口状态
        self.port_name = port          # 当前选中串口名 (None=未选)
        self.baud = baud               # 当前选中波特率
        self.ser = None                # 已打开串口对象 (None=未连接)
        self.reader = None             # 读线程 (None=未连接)
        self.port_open = False         # 串口是否已打开

        self.conn_time = 0.0           # 最近收到 MCU 任一帧的时间
        self.last_frame = bytearray(FRAME_LEN)
        self.has_frame = False         # 是否收到过至少一帧镜像 (永久显示最后一帧用)
        self.frame_ts = 0.0
        self.rsp_lines = []
        self.cmd_text = ""
        self.input_focused = False     # SCPI 输入框聚焦标记 (点击输入框才激活文本输入)
        self.out_enabled = None        # 输出使能状态 (None=未知, 由 STAT 响应的 OUT= 字段同步)
        self.msg_q = queue.Queue()
        self._last_ping = time.time()

        # 串口下拉菜单状态
        self.port_menu_open = False        # 串口下拉菜单是否展开
        self._port_menu_rects = []         # [(rect, device_name), ...] 菜单项几何

        # 鼠标中键 (编码器按下) 状态
        self._mid_held = False
        self._mid_press_t = 0.0
        self._mid_long_fired = False

        self.mirror_px = WIDTH * scale
        self.right_w = 300
        WIN_W = self.mirror_px + self.right_w
        WIN_H = self.mirror_px + 60
        pygame.init()
        pygame.display.set_caption("OLED Mirror / SCPI Console")
        self.screen = pygame.display.set_mode((WIN_W, WIN_H))
        self.font = pygame.font.SysFont("consolas", 20)
        self.font_s = pygame.font.SysFont("consolas", 16)
        self.clock = pygame.time.Clock()

        mx = self.mirror_px
        W = self.right_w

        # 虚拟键几何 (右边板): 上下左右加宽, 间隙与 Enter/Back 行一致 (12px);
        # Enter/Back 下方新增 OUT 开关 + TRIG 触发快捷键
        self.btn_up    = pygame.Rect(mx + W // 2 - 38, 14, 76, 34)
        self.btn_left  = pygame.Rect(mx + 24, 54, 76, 34)
        self.btn_down  = pygame.Rect(mx + W // 2 - 38, 54, 76, 34)
        self.btn_right = pygame.Rect(mx + 200, 54, 76, 34)
        self.btn_enter = pygame.Rect(mx + 24, 94, 120, 34)
        self.btn_back  = pygame.Rect(mx + 156, 94, 120, 34)
        self.btn_out   = pygame.Rect(mx + 24, 134, 120, 34)
        self.btn_trig  = pygame.Rect(mx + 156, 134, 120, 34)

        # 串口/波特率选择行 (与按键区拉开距离)
        self.sel_port  = pygame.Rect(mx + 10, 184, 100, 28)
        self.sel_baud  = pygame.Rect(mx + 112, 184, 76, 28)
        self.btn_conn  = pygame.Rect(mx + 190, 184, 100, 28)

        # SCPI 输入框 + RSP 消息区
        self.input_box = pygame.Rect(mx + 10, 220, W - 20, 30)
        self.rsp_area  = pygame.Rect(mx + 10, 258, W - 20, WIN_H - 258 - 10)

        # 可选串口列表 (每次点击端口按钮时刷新)
        self._ports = self._list_ports()

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
                        if not getattr(self, '_frame_seen', False):
                            self._frame_seen = True
                            print("[镜像] 已收到第一帧 FRAME(RLE)")
                elif ftype in (T_RSP, T_ACK):
                    try:
                        txt = payload.decode('ascii', 'replace').strip()
                    except Exception:
                        txt = ''
                    # 完整内容打印到终端 (GUI 窗口裁剪, 终端可见 FR/ST/OR/RX/LNK 全字段)
                    if ftype == T_RSP:
                        print("[RSP] " + txt)
                        self.rsp_lines.append("> " + txt)
                        # 从 STAT 响应同步输出使能状态 (OUT=ON/OFF)
                        if "OUT=" in txt:
                            for part in txt.split(";"):
                                if part.startswith("OUT="):
                                    self.out_enabled = (part[4:] == "ON")
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
                    elif ev.key == pygame.K_ESCAPE:
                        self.input_focused = False   # 退出编辑, 恢复方向键遥控
                    elif ev.unicode and ev.unicode.isprintable():
                        if len(self.cmd_text) < 90:
                            self.cmd_text += ev.unicode
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
                    # 滚轮上滚 = 板载编码器正转 (main.c: diff>=2 -> msg_left)
                    self._inject_key(K_LEFT)
                elif ev.button == 5:
                    # 滚轮下滚 = 板载编码器反转 (main.c: diff<=-2 -> msg_right)
                    self._inject_key(K_RIGHT)
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
        elif self.input_box.collidepoint(pos):
            self.input_focused = True    # 点击输入框 → 激活文本编辑

    # ------------------------------------------------------------ 绘制
    def _draw(self, now):
        scr = self.screen
        scr.fill(self.BG)

        # 镜像区: 只要收到过帧就永久显示最后一帧 (OLED 是保持型显示, 画面静止时
        # MCU 按脏帧策略不再推流, 若仍按超时清屏会退回纯色背景)
        if self.has_frame:
            rgb = render_frame(self.last_frame)
            rgb = pygame.transform.scale(rgb, (self.mirror_px, self.mirror_px))
        else:
            rgb = pygame.Surface((self.mirror_px, self.mirror_px))
            rgb.fill(self.PANEL)
        scr.blit(rgb, (0, 0))

        # 底部状态
        connected = self.port_open and (now - self.conn_time < 1.5)
        col = self.GREEN if connected else self.RED
        st = self.font.render("●" if connected else "○", True, col)
        scr.blit(st, (10, self.mirror_px + 14))
        info = self.font.render(
            "128x128 | {} px | {:.0f} fps{}".format(
                self.mirror_px, self.clock.get_fps(),
                " | DEMO" if self.demo else ""),
            True, self.DIM)
        scr.blit(info, (38, self.mirror_px + 14))

        # 诊断: CRC 失败帧计数 (长帧丢字节会持续累加, 正常应接近 0)
        if self.reader is not None:
            diag = self.font_s.render("bad_crc={} fr={:.0f}".format(
                self.reader.bad_crc, time.time() - self.frame_ts), True, self.RED)
            scr.blit(diag, (38, self.mirror_px + 36))

        # 右板 (半透明遮罩)
        panel = pygame.Surface((self.right_w, self.mirror_px + 60), pygame.SRCALPHA)
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

        # RSP 区
        pygame.draw.rect(scr, (12, 14, 18), self.rsp_area)
        y = self.rsp_area.bottom - 6
        for line in reversed(self.rsp_lines[-8:]):
            t = lbl.render(line, True, self.DIM)
            scr.blit(t, (self.rsp_area.x + 8, y - t.get_height()))
            y -= t.get_height() + 2

        # 串口下拉菜单 (最后绘制, 确保覆盖在输入框/RSP 区之上)
        if self.port_menu_open:
            for rect, dev in self._port_menu_rects:
                hover = rect.collidepoint(pygame.mouse.get_pos())
                cur = (dev == self.port_name)
                pygame.draw.rect(scr, (70, 76, 90) if hover else (46, 50, 60), rect)
                pygame.draw.rect(scr, self.GREEN if cur else self.BLUE, rect, 1)
                t = lbl.render(dev, True, self.TEXT)
                scr.blit(t, (rect.x + 8, rect.centery - t.get_height() // 2))

        pygame.display.flip()

    # ------------------------------------------------------------ 心跳
    def _heartbeat(self, now):
        if self.ser is not None and self.port_open and now - self._last_ping >= 0.5:
            self._send(frame_ping())
            self._last_ping = now

    def quit(self):
        if self.reader is not None:
            self.reader.stop()
        if self.ser is not None:
            try:
                self.ser.close()
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

            if self.ser is None:
                # 演示模式: 定时换测试图案
                demo_phase = int(now * 4)
                self.last_frame = demo_frame(demo_phase)
                self.frame_ts = now
                self.conn_time = now
            self._heartbeat(now)
            self._draw(now)
            self.clock.tick(30)


def main():
    ap = argparse.ArgumentParser(description="STM32G474 OLED mirror + SCPI controller")
    ap.add_argument("--port", help="serial port e.g. COM5 (省略则从界面选择)")
    ap.add_argument("--baud", type=int, default=460800)
    ap.add_argument("--scale", type=int, default=4)
    ap.add_argument("--demo", action="store_true", help="run without serial (test pattern)")
    args = ap.parse_args()

    if not args.demo and serial is None:
        print("pyserial 未安装: pip install pyserial, 或使用 --demo")
        sys.exit(1)

    App(port=args.port, baud=args.baud, scale=args.scale, demo=args.demo).run()


if __name__ == "__main__":
    main()
