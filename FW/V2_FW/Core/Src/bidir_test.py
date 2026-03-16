#!/usr/bin/env python3
"""
双方向 CAN テスト + レイテンシ計測
  Innomaker USB2CAN (macOS: slcan)  <-->  STM32 CAN-Ethernet Gateway (TCP)

Usage:
    python3 bidir_test.py <gateway_ip> --iface /dev/cu.usbmodemXXXX
    python3 bidir_test.py 192.168.1.100 --iface /dev/cu.usbmodem1234 --count 200

Requirements:
    pip install python-can

macOS での USB2CAN デバイス確認:
    ls /dev/cu.usbmodem*
"""

import argparse
import socket
import struct
import threading
import time
import queue
import statistics
import can
import signal
import sys

# ─── パケット定義 (20 bytes, Classic CAN) ────────────────────────────────────
# channel(1) + reserved(3) + can_id(4) + len(1) + res(3) + data(8)
PACKET_SIZE = 20
GW_PORT     = 5000


def pack_packet(channel: int, can_id: int, data: bytes, extended=False) -> bytes:
    full_id = can_id & 0x1FFFFFFF
    if extended:
        full_id |= 0x80000000
    payload = data[:8].ljust(8, b'\x00')
    frame  = struct.pack('<I B B B B 8s', full_id, len(data[:8]), 0, 0, 0, payload)
    header = struct.pack('<B 3x', channel)
    return header + frame


def unpack_packet(raw: bytes):
    """(channel, can_id, data) を返す。失敗時 None"""
    if len(raw) < PACKET_SIZE:
        return None
    channel = struct.unpack('<B', raw[0:1])[0]
    can_id, length, _, _, _, data = struct.unpack('<I B B B B 8s', raw[4:20])
    return channel, can_id, bytes(data[:length])


# ─── TCP 受信スレッド ─────────────────────────────────────────────────────────
def tcp_recv_loop(sock, rx_queue: queue.Queue, stop: threading.Event):
    buf = b''
    while not stop.is_set():
        try:
            sock.settimeout(0.5)
            chunk = sock.recv(PACKET_SIZE * 32)
            if not chunk:
                break
            buf += chunk
            while len(buf) >= PACKET_SIZE:
                pkt = unpack_packet(buf[:PACKET_SIZE])
                if pkt:
                    rx_queue.put(('tcp', time.monotonic(), pkt))
                buf = buf[PACKET_SIZE:]
        except socket.timeout:
            continue
        except Exception as e:
            if not stop.is_set():
                print(f'[TCP RX] {e}')
            break


# ─── USB2CAN 受信スレッド ──────────────────────────────────────────────────────
def can_recv_loop(bus: can.BusABC, rx_queue: queue.Queue, stop: threading.Event):
    while not stop.is_set():
        try:
            msg = bus.recv(timeout=0.5)
            if msg:
                rx_queue.put(('can', time.monotonic(), msg))
        except Exception as e:
            if not stop.is_set():
                print(f'[CAN RX] {e}')
            break


# ─── 結果表示ヘルパー ──────────────────────────────────────────────────────────
def print_latency(label: str, latencies_ms: list):
    if not latencies_ms:
        print(f'  {label}: 受信データなし')
        return
    print(f'  {label}:')
    print(f'    サンプル数 : {len(latencies_ms)}')
    print(f'    最小       : {min(latencies_ms):.2f} ms')
    print(f'    最大       : {max(latencies_ms):.2f} ms')
    print(f'    平均       : {statistics.mean(latencies_ms):.2f} ms')
    if len(latencies_ms) >= 2:
        print(f'    中央値     : {statistics.median(latencies_ms):.2f} ms')
        print(f'    標準偏差   : {statistics.stdev(latencies_ms):.2f} ms')


# ─── メイン ───────────────────────────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser(description='CAN-Gateway 双方向テスト＋レイテンシ計測')
    parser.add_argument('ip',           help='Gateway IP (例: 192.168.1.100)')
    parser.add_argument('--port',       type=int,   default=GW_PORT)
    parser.add_argument('--channel',    type=int,   default=0,     help='FDCAN チャンネル 0-2')
    parser.add_argument('--iface',      default='',                help='USB2CAN デバイス (例: /dev/cu.usbmodem1234)')
    parser.add_argument('--bitrate',    type=int,   default=1000000)
    parser.add_argument('--count',      type=int,   default=100,   help='各方向の送信パケット数')
    parser.add_argument('--interval',   type=float, default=0.005, help='送信間隔[s] (0=最速)')
    parser.add_argument('--timeout',    type=float, default=5.0,   help='受信タイムアウト[s]')
    parser.add_argument('--eth-only',   action='store_true',       help='ETH→CAN テストのみ')
    parser.add_argument('--can-only',   action='store_true',       help='CAN→ETH テストのみ')
    args = parser.parse_args()

    # デバイス自動検出
    iface = args.iface
    if not iface:
        import glob
        candidates = (
            glob.glob('/dev/cu.usbmodem*') +
            glob.glob('/dev/cu.usbserial*') +
            glob.glob('/dev/tty.usbmodem*') +
            glob.glob('/dev/tty.usbserial*')
        )
        if not candidates:
            print('[!] USB2CAN デバイスが見つかりません。--iface で指定してください')
            print('    確認: ls /dev/cu.usb*')
            sys.exit(1)
        iface = candidates[0]
        print(f'[CAN] デバイス自動検出: {iface}')

    stop = threading.Event()
    rx_queue: queue.Queue = queue.Queue()

    def on_sigint(sig, frame):
        print('\n[!] 中断')
        stop.set()
    signal.signal(signal.SIGINT, on_sigint)

    # ── TCP 接続 ──────────────────────────────────────────────────────────────
    print(f'[TCP] {args.ip}:{args.port} に接続中...')
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((args.ip, args.port))
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)  # Nagle 無効
    print('[TCP] 接続完了')

    # ── USB2CAN 接続 (macOS: slcan) ───────────────────────────────────────────
    print(f'[CAN] {iface} を開く...')
    bus = can.interface.Bus(
        channel=iface,
        interface='socketcan',
        bitrate=args.bitrate,
    )
    print('[CAN] 接続完了')

    # ── 受信スレッド開始 ──────────────────────────────────────────────────────
    t_tcp = threading.Thread(target=tcp_recv_loop, args=(sock, rx_queue, stop), daemon=True)
    t_can = threading.Thread(target=can_recv_loop, args=(bus, rx_queue, stop), daemon=True)
    t_tcp.start()
    t_can.start()
    time.sleep(0.3)

    # ─────────────────────────────────────────────────────────────────────────
    # Test 1: ETH → CAN
    #   シーケンス番号を data[0:4] に埋め込み、USB2CAN 受信時に照合
    # ─────────────────────────────────────────────────────────────────────────
    eth2can_sent   = {}  # seq -> (can_id, send_time)
    eth2can_latency = []

    if not args.can_only:
        print(f'\n{"="*55}')
        print(f' Test 1: ETH → CAN  (ch={args.channel}, {args.count} pkts)')
        print(f'{"="*55}')
        BASE_ID = 0x100

        for seq in range(args.count):
            if stop.is_set():
                break
            can_id = BASE_ID + (seq % 0x600)
            data   = struct.pack('>I', seq) + b'\xAA\xBB\xCC\xDD'
            pkt    = pack_packet(args.channel, can_id, data)
            t_send = time.monotonic()
            sock.sendall(pkt)
            eth2can_sent[seq] = (can_id & 0x1FFFFFFF, t_send)
            if args.interval > 0:
                time.sleep(args.interval)

        print(f'  送信完了 ({len(eth2can_sent)} pkts)、{args.timeout}s 待機中...')
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline and not stop.is_set():
            try:
                src, t_recv, payload = rx_queue.get(timeout=0.1)
                if src != 'can':
                    rx_queue.put((src, t_recv, payload))  # CAN→ETH テスト用に戻す
                    continue
                msg: can.Message = payload
                # data[0:4] でシーケンス番号照合
                if len(msg.data) >= 4:
                    seq = struct.unpack('>I', bytes(msg.data[:4]))[0]
                    if seq in eth2can_sent:
                        arb_id, t_send = eth2can_sent.pop(seq)
                        if msg.arbitration_id == arb_id:
                            eth2can_latency.append((t_recv - t_send) * 1000)
            except queue.Empty:
                pass

        recv_count = args.count - len(eth2can_sent)
        loss = len(eth2can_sent)
        print(f'  受信: {recv_count} / {args.count}  損失: {loss}')

    # ─────────────────────────────────────────────────────────────────────────
    # Test 2: CAN → ETH
    #   シーケンス番号を data[0:4] に埋め込み、TCP 受信時に照合
    # ─────────────────────────────────────────────────────────────────────────
    can2eth_sent    = {}  # seq -> send_time
    can2eth_latency = []

    if not args.eth_only:
        print(f'\n{"="*55}')
        print(f' Test 2: CAN → ETH  ({args.count} pkts)')
        print(f'{"="*55}')
        BASE_ID2 = 0x200

        for seq in range(args.count):
            if stop.is_set():
                break
            arb_id = BASE_ID2 + (seq % 0x600)
            data   = struct.pack('>I', seq) + b'\x11\x22\x33\x44'
            msg    = can.Message(arbitration_id=arb_id, data=data, is_extended_id=False)
            t_send = time.monotonic()
            bus.send(msg)
            can2eth_sent[seq] = t_send
            if args.interval > 0:
                time.sleep(args.interval)

        print(f'  送信完了 ({len(can2eth_sent)} pkts)、{args.timeout}s 待機中...')
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline and not stop.is_set():
            try:
                src, t_recv, payload = rx_queue.get(timeout=0.1)
                if src != 'tcp':
                    continue
                ch, can_id, dat = payload
                if len(dat) >= 4:
                    seq = struct.unpack('>I', dat[:4])[0]
                    if seq in can2eth_sent:
                        t_send = can2eth_sent.pop(seq)
                        can2eth_latency.append((t_recv - t_send) * 1000)
            except queue.Empty:
                pass

        recv_count2 = args.count - len(can2eth_sent)
        loss2 = len(can2eth_sent)
        print(f'  受信: {recv_count2} / {args.count}  損失: {loss2}')

    # ─────────────────────────────────────────────────────────────────────────
    # 結果サマリ
    # ─────────────────────────────────────────────────────────────────────────
    print(f'\n{"="*55}')
    print(' レイテンシ結果')
    print(f'{"="*55}')
    if not args.can_only:
        print_latency('ETH → CAN (TCP送信 → USB2CAN受信)', eth2can_latency)
    if not args.eth_only:
        print_latency('CAN → ETH (USB2CAN送信 → TCP受信)', can2eth_latency)
    print(f'{"="*55}\n')

    stop.set()
    try:
        sock.close()
    except Exception:
        pass
    try:
        bus.shutdown()
    except Exception:
        pass


if __name__ == '__main__':
    main()
