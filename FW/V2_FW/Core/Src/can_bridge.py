#!/usr/bin/env python3
"""
CAN-Ethernet Gateway Bridge (Python)
STM32G474 + W5500 CAN Gateway通信用プログラム

使い方:
    python3 can_bridge.py <IP> <PORT>
    例: python3 can_bridge.py 192.168.1.100 5000
"""

import socket
import struct
import threading
import time
import sys
import signal

class CANFrame:
    """SocketCAN互換のCANフレーム"""
    def __init__(self, can_id=0, data=b'', flags=0):
        self.can_id = can_id
        self.data = data[:64]  # 最大64バイト
        self.len = len(self.data)
        self.flags = flags
    
    def pack(self):
        """バイナリパケットに変換"""
        # can_id (4), len (1), flags (1), res0 (1), res1 (1), data (64)
        packed_data = self.data + b'\x00' * (64 - len(self.data))
        return struct.pack('<I B B B B 64s', 
                          self.can_id, 
                          self.len, 
                          self.flags, 
                          0, 0,  # reserved
                          packed_data)
    
    @staticmethod
    def unpack(data):
        """バイナリパケットからCANフレームを復元"""
        can_id, length, flags, _, _, packed_data = struct.unpack('<I B B B B 64s', data)
        frame = CANFrame(can_id, packed_data[:length], flags)
        return frame
    
    def __str__(self):
        data_hex = ' '.join(f'{b:02X}' for b in self.data)
        # Check if extended ID
        is_extended = bool(self.can_id & 0x80000000)
        is_rtr = bool(self.can_id & 0x40000000)
        actual_id = self.can_id & 0x1FFFFFFF
        
        id_str = f"0x{actual_id:08X}" if is_extended else f"0x{actual_id:03X}"
        flags = ""
        if is_extended:
            flags += " [EXT]"
        if is_rtr:
            flags += " [RTR]"
        
        return f"ID={id_str}{flags} DLC={self.len} Data=[{data_hex}]"


class CANGatewayPacket:
    """CAN Gateway パケット (channel情報を含む)"""
    HEADER_SIZE = 4  # channel (1) + reserved (3)
    FRAME_SIZE = 72  # CANフレーム
    TOTAL_SIZE = HEADER_SIZE + FRAME_SIZE  # 76バイト
    
    def __init__(self, channel, frame):
        self.channel = channel  # 0=CAN1, 1=CAN2, 2=CAN3
        self.frame = frame
    
    def pack(self):
        """バイナリパケットに変換"""
        header = struct.pack('<B 3x', self.channel)  # channel + 3 reserved
        return header + self.frame.pack()
    
    @staticmethod
    def unpack(data):
        """バイナリパケットから復元"""
        if len(data) < CANGatewayPacket.TOTAL_SIZE:
            raise ValueError(f"Packet too short: {len(data)} bytes")
        
        channel = struct.unpack('<B', data[0:1])[0]
        frame_data = data[4:76]  # Skip header (4 bytes)
        frame = CANFrame.unpack(frame_data)
        return CANGatewayPacket(channel, frame)
    
    def __str__(self):
        return f"CAN{self.channel} {self.frame}"


class CANEthernetBridge:
    """CAN-Ethernet Gateway ブリッジクライアント"""
    
    def __init__(self, host, port):
        self.host = host
        self.port = port
        self.sock = None
        self.running = False
        self.rx_thread = None
        self.rx_callback = None
        self.tx_seq = 0  # 送信シーケンス番号
        self.rx_seq = 0  # 受信シーケンス番号
    
    def connect(self):
        """ゲートウェイに接続"""
        try:
            print(f"Creating socket...")
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.sock.settimeout(10)  # 10秒タイムアウト
            print(f"Connecting to {self.host}:{self.port}...")
            self.sock.connect((self.host, self.port))
            self.sock.settimeout(None)  # タイムアウト解除
            print(f"Connected to {self.host}:{self.port}")
            return True
        except socket.timeout:
            print(f"Connection timeout to {self.host}:{self.port}")
            print(f"   Server may be busy or not responding")
            return False
        except ConnectionRefusedError:
            print(f"Connection refused to {self.host}:{self.port}")
            print(f"   Server may not be running")
            return False
        except OSError as e:
            if e.errno == 65:  # No route to host
                print(f"No route to host: {self.host}:{self.port}")
                print(f"   Check network configuration and routing")
            else:
                print(f"Connection failed: {e}")
            return False
        except Exception as e:
            print(f"Connection failed: {e}")
            import traceback
            traceback.print_exc()
            return False
    
    def disconnect(self):
        """接続を切断"""
        self.running = False
        if self.rx_thread:
            self.rx_thread.join(timeout=2)
        if self.sock:
            self.sock.close()
            print("Disconnected")
    
    def send_can_frame(self, channel, can_id, data, silent=False, extended=False, rtr=False):
        """
        CANフレームを送信
        
        Args:
            channel: CANチャンネル (0-2)
            can_id: CAN ID (標準: 0-0x7FF, 拡張: 0-0x1FFFFFFF)
            data: データバイト列
            silent: ログ出力を抑制するか
            extended: 拡張ID (29ビット) を使用するか
            rtr: リモートフレームとして送信するか
        """
        try:
            # Apply flags to CAN ID
            full_can_id = can_id & 0x1FFFFFFF  # Mask to 29 bits
            if extended:
                full_can_id |= 0x80000000  # Set EFF (Extended Frame Format) flag
            if rtr:
                full_can_id |= 0x40000000  # Set RTR (Remote Transmission Request) flag
            
            frame = CANFrame(full_can_id, data)
            packet = CANGatewayPacket(channel, frame)
            self.sock.sendall(packet.pack())
            self.tx_seq += 1
            if not silent:
                print(f"TX[{self.tx_seq:05d}]: {packet}")
            return True
        except Exception as e:
            print(f"Send error: {e}")
            return False
    
    def send_burst(self, channel, start_id, count, interval_ms, auto_increment=True, extended=False):
        """
        バースト送信（IDとデータを自動インクリメント）
        
        Args:
            channel: CANチャンネル (0-2)
            start_id: 開始ID
            count: 送信パケット数
            interval_ms: 送信間隔（ミリ秒）
            auto_increment: IDとデータを自動インクリメントするか
            extended: 拡張ID (29ビット) を使用するか
        """
        print(f"\nBurst send: {count} packets on CAN{channel}")
        id_format = "0x{:08X}" if extended else "0x{:03X}"
        print(f"  Start ID: " + id_format.format(start_id))
        print(f"  ID type: {'Extended (29-bit)' if extended else 'Standard (11-bit)'}")
        print(f"  Interval: {interval_ms} ms")
        print(f"  Auto increment: {auto_increment}")
        print("-" * 50)
        
        interval_sec = interval_ms / 1000.0
        success_count = 0
        
        for i in range(count):
            # IDを計算
            can_id = (start_id + i) if auto_increment else start_id
            
            # データを生成（8バイト）
            if auto_increment:
                # カウンタを含む8バイトのデータ
                data = bytes([
                    (i >> 24) & 0xFF,  # カウンタ上位
                    (i >> 16) & 0xFF,
                    (i >> 8) & 0xFF,
                    i & 0xFF,          # カウンタ下位
                    0xAA, 0xBB, 0xCC, 0xDD
                ])
            else:
                # 固定データ
                data = bytes([0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88])
            
            # 送信（10パケットごとにログ表示）
            silent = (i % 10 != 0) and (i != count - 1)
            if self.send_can_frame(channel, can_id, data, silent=silent, extended=extended):
                success_count += 1
            
            # 最後のパケット以外は待機
            if i < count - 1:
                time.sleep(interval_sec)
        
        print("-" * 50)
        print(f"Burst complete: {success_count}/{count} packets sent\n")
        return success_count
    
    def start_receive(self, callback=None):
        """受信スレッドを開始"""
        self.rx_callback = callback
        self.running = True
        self.rx_thread = threading.Thread(target=self._receive_loop, daemon=True)
        self.rx_thread.start()
        print("Receive thread started")
    
    def _receive_loop(self):
        """受信ループ（別スレッド）"""
        buffer = b''
        while self.running:
            try:
                # データ受信
                data = self.sock.recv(4096)
                if not data:
                    print("Connection closed by server")
                    break
                
                buffer += data
                
                # パケットを解析
                while len(buffer) >= CANGatewayPacket.TOTAL_SIZE:
                    packet_data = buffer[:CANGatewayPacket.TOTAL_SIZE]
                    buffer = buffer[CANGatewayPacket.TOTAL_SIZE:]
                    
                    try:
                        packet = CANGatewayPacket.unpack(packet_data)
                        self.rx_seq += 1
                        print(f"RX[{self.rx_seq:05d}]: {packet}")
                        
                        # コールバック呼び出し
                        if self.rx_callback:
                            self.rx_callback(packet.channel, packet.frame)
                    
                    except Exception as e:
                        print(f"Parse error: {e}")
            
            except socket.timeout:
                continue
            except Exception as e:
                if self.running:
                    print(f"Receive error: {e}")
                break


def signal_handler(sig, frame):
    """Ctrl+C ハンドラ"""
    print("\n\nStopping...")
    sys.exit(0)


def main():
    """メイン関数"""
    
    # 引数チェック
    if len(sys.argv) < 3:
        print("Usage: python3 can_bridge.py <IP> <PORT>")
        print("Example: python3 can_bridge.py 192.168.1.100 5000")
        sys.exit(1)
    
    host = sys.argv[1]
    port = int(sys.argv[2])
    
    # Ctrl+C ハンドラ
    signal.signal(signal.SIGINT, signal_handler)
    
    # ブリッジ接続
    bridge = CANEthernetBridge(host, port)
    
    if not bridge.connect():
        sys.exit(1)
    
    # 受信コールバック（オプション）
    def on_receive(channel, frame):
        # 受信時の追加処理をここに書く
        pass
    
    # 受信開始
    bridge.start_receive(callback=on_receive)
    
    print("\n" + "="*50)
    print("CAN-Ethernet Bridge Started!")
    print("="*50)
    print("Commands:")
    print("  send <ch> <id> <data> [ext] [rtr]       - Send CAN frame")
    print("    Example: send 0 0x123 11 22 33 44     - Standard ID data frame")
    print("    Example: send 0 0x123 rtr              - Standard ID remote frame")
    print("    Example: send 0 0x12345678 AA BB ext  - Extended ID data frame")
    print("    Example: send 0 0x12345678 ext rtr    - Extended ID remote frame")
    print("  burst <ch> <id> <count> <ms> [ext]      - Burst send")
    print("    Example: burst 0 0x100 100 10         - Standard ID burst")
    print("    Example: burst 0 0x10000000 50 5 ext  - Extended ID burst")
    print("  test                                     - Send test frames")
    print("  stats                                    - Show statistics")
    print("  quit                                     - Exit program")
    print("="*50 + "\n")
    
    # 接続安定化待ち（自動送信は一旦無効化して切断原因を切り分け）
    # print("Sending test frames...")
    # time.sleep(0.5)
    # bridge.send_can_frame(0, 0x123, bytes([0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88]))
    # time.sleep(0.1)
    # bridge.send_can_frame(2, 0x456, bytes([0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11]))
    # print("\nTest frames sent\n")
    
    # インタラクティブモード
    try:
        while True:
            cmd = input(">>> ").strip()
            
            if not cmd:
                continue
            
            if cmd == "quit" or cmd == "exit":
                break
            
            elif cmd == "test":
                print("Sending test frames...")
                bridge.send_can_frame(0, 0x100, bytes(range(8)))
                bridge.send_can_frame(2, 0x300, bytes([0xFF - i for i in range(8)]))
                print("Test frames sent")
            
            elif cmd.startswith("burst"):
                parts = cmd.split()
                if len(parts) < 5:
                    print("Usage: burst <channel> <start_id> <count> <interval_ms> [ext]")
                    print("Example: burst 0 0x100 100 10")
                    print("Example: burst 0 0x10000000 100 10 ext")
                    continue
                
                try:
                    channel = int(parts[1])
                    start_id = int(parts[2], 0)  # 0x100 or 256
                    count = int(parts[3])
                    interval_ms = int(parts[4])
                    extended = len(parts) > 5 and parts[5].lower() == 'ext'
                    
                    if channel < 0 or channel > 2:
                        print("Error: Channel must be 0-2")
                        continue
                    
                    if count <= 0 or count > 10000:
                        print("Error: Count must be 1-10000")
                        continue
                    
                    if interval_ms < 0:
                        print("Error: Interval must be >= 0")
                        continue
                    
                    bridge.send_burst(channel, start_id, count, interval_ms, extended=extended)
                
                except ValueError as e:
                    print(f"Error: {e}")
                except KeyboardInterrupt:
                    print("\n\nBurst interrupted by user")
            
            elif cmd == "stats":
                print("\n" + "="*50)
                print("📊 Statistics")
                print("="*50)
                print(f"  TX packets: {bridge.tx_seq}")
                print(f"  RX packets: {bridge.rx_seq}")
                print(f"  Connection: {'Active' if bridge.running else 'Closed'}")
                print("="*50 + "\n")
            
            elif cmd.startswith("send"):
                parts = cmd.split()
                if len(parts) < 3:
                    print("Usage: send <channel> <id> [data bytes...] [ext] [rtr]")
                    print("  ext: Use extended ID (29-bit)")
                    print("  rtr: Send as remote frame (no data)")
                    print("Examples:")
                    print("  send 0 0x123 11 22 33       - Standard ID, data frame")
                    print("  send 0 0x123 rtr            - Standard ID, remote frame")
                    print("  send 0 0x12345678 ext       - Extended ID, data frame (no data)")
                    print("  send 0 0x12345678 ext rtr   - Extended ID, remote frame")
                    continue
                
                try:
                    channel = int(parts[1])
                    can_id = int(parts[2], 0)  # 0x123 or 123
                    
                    # Check for flags
                    flags_lower = [p.lower() for p in parts[3:]]
                    extended = 'ext' in flags_lower
                    rtr = 'rtr' in flags_lower
                    
                    # Extract data bytes (exclude 'ext' and 'rtr' keywords)
                    data_parts = [p for p in parts[3:] if p.lower() not in ['ext', 'rtr']]
                    data = bytes([int(x, 0) for x in data_parts]) if data_parts else b''
                    
                    # Remote frames should have no data (or DLC only)
                    if rtr and len(data) > 0:
                        print("Warning: Remote frame with data. Data will be ignored.")
                        data = b''
                    
                    bridge.send_can_frame(channel, can_id, data, extended=extended, rtr=rtr)
                
                except ValueError as e:
                    print(f"Error: {e}")
            
            else:
                print(f"Unknown command: {cmd}")
    
    except EOFError:
        pass
    
    # 統計情報表示
    print("\n" + "="*50)
    print("Final Statistics")
    print("="*50)
    print(f"  TX packets: {bridge.tx_seq}")
    print(f"  RX packets: {bridge.rx_seq}")
    print("="*50)
    
    # 切断
    bridge.disconnect()
    print("\nBye!")


if __name__ == "__main__":
    main()

