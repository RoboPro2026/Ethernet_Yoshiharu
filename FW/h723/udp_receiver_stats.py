#!/usr/bin/env python3
"""
STM32H723 Ethernet UDP パケット受信スクリプト（統計情報付き）
STM32から送信されるUDPパケットを受信して統計情報を表示します。
"""

import socket
import struct
import sys
import time
from datetime import datetime
from collections import deque

# 設定
LISTEN_IP = "192.168.1.1"     # 受信するIPアドレス（PCのIPアドレスに設定）
LISTEN_PORT = 55151              # 受信ポート
BUFFER_SIZE = 1024              # 受信バッファサイズ
STATS_INTERVAL = 1.0            # 統計表示間隔（秒）

class PacketStats:
    def __init__(self):
        self.total_packets = 0
        self.total_bytes = 0
        self.packet_loss_count = 0
        self.start_time = time.time()
        self.last_stats_time = self.start_time
        self.interval_packets = 0
        self.interval_bytes = 0
        self.last_counter = None
        self.packet_times = deque(maxlen=100)  # 最後の100パケットの受信時刻
        
    def update(self, data_size, counter=None):
        self.total_packets += 1
        self.total_bytes += data_size
        self.interval_packets += 1
        self.interval_bytes += data_size
        self.packet_times.append(time.time())
        
        # パケットロスチェック
        if self.last_counter is not None and counter is not None:
            expected = (self.last_counter + 1) & 0xFFFFFFFF
            if counter != expected:
                loss = counter - expected if counter > expected else counter + (0xFFFFFFFF - expected) + 1
                self.packet_loss_count += loss
        
        if counter is not None:
            self.last_counter = counter
    
    def get_interval_stats(self):
        current_time = time.time()
        elapsed = current_time - self.last_stats_time
        
        if elapsed > 0:
            pps = self.interval_packets / elapsed  # Packets per second
            bps = (self.interval_bytes * 8) / elapsed  # Bits per second
        else:
            pps = 0
            bps = 0
        
        # インターバルリセット
        self.interval_packets = 0
        self.interval_bytes = 0
        self.last_stats_time = current_time
        
        return pps, bps
    
    def get_total_stats(self):
        elapsed = time.time() - self.start_time
        if elapsed > 0:
            avg_pps = self.total_packets / elapsed
            avg_bps = (self.total_bytes * 8) / elapsed
        else:
            avg_pps = 0
            avg_bps = 0
        
        # パケット損失率
        if self.last_counter is not None:
            expected_total = self.last_counter + 1
            loss_rate = (self.packet_loss_count / expected_total * 100) if expected_total > 0 else 0
        else:
            loss_rate = 0
        
        return elapsed, avg_pps, avg_bps, loss_rate
    
    def get_jitter(self):
        """パケット間隔のジッター計算"""
        if len(self.packet_times) < 2:
            return 0
        
        intervals = []
        for i in range(1, len(self.packet_times)):
            intervals.append(self.packet_times[i] - self.packet_times[i-1])
        
        if not intervals:
            return 0
        
        avg_interval = sum(intervals) / len(intervals)
        variance = sum((x - avg_interval) ** 2 for x in intervals) / len(intervals)
        jitter = variance ** 0.5
        
        return jitter * 1000  # ms単位

def format_bytes(bps):
    """ビット/秒を読みやすい形式に変換"""
    units = ['bps', 'Kbps', 'Mbps', 'Gbps']
    unit_index = 0
    
    while bps >= 1000 and unit_index < len(units) - 1:
        bps /= 1000
        unit_index += 1
    
    return f"{bps:.2f} {units[unit_index]}"

def print_stats_header():
    print("\n" + "=" * 100)
    print(f"{'時刻':<12} | {'受信':<8} | {'速度':<12} | {'帯域幅':<15} | "
          f"{'累計':<10} | {'損失':<8} | {'ジッター':<10}")
    print("=" * 100)

def main():
    print("=" * 100)
    print("STM32H723 Ethernet UDP パケット受信プログラム（統計情報付き）")
    print("=" * 100)
    print(f"受信IP  : {LISTEN_IP}")
    print(f"受信Port: {LISTEN_PORT}")
    print("=" * 100)
    print("待機中... (Ctrl+C で終了)")
    
    # UDPソケット作成
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(STATS_INTERVAL)  # タイムアウト設定
    
    stats = PacketStats()
    
    try:
        sock.bind((LISTEN_IP, LISTEN_PORT))
        print_stats_header()
        
        while True:
            try:
                # データ受信
                data, addr = sock.recvfrom(BUFFER_SIZE)
                
                # データ解析
                counter = None
                if len(data) == 4:
                    counter = struct.unpack('<I', data)[0]
                
                # 統計更新
                stats.update(len(data), counter)
                
            except socket.timeout:
                # タイムアウト時は統計表示のみ
                pass
            
            # 統計表示
            current_time = time.time()
            if current_time - stats.last_stats_time >= STATS_INTERVAL:
                pps, bps = stats.get_interval_stats()
                elapsed, avg_pps, avg_bps, loss_rate = stats.get_total_stats()
                jitter = stats.get_jitter()
                
                timestamp = datetime.now().strftime("%H:%M:%S.%f")[:-3]
                
                print(f"{timestamp} | "
                      f"{pps:6.1f}pps | "
                      f"{avg_pps:8.1f}pps | "
                      f"{format_bytes(bps):>14} | "
                      f"{stats.total_packets:9d} | "
                      f"{stats.packet_loss_count:7d} | "
                      f"{jitter:7.2f}ms")
    
    except KeyboardInterrupt:
        print("\n" + "=" * 100)
        print("最終統計情報")
        print("=" * 100)
        
        elapsed, avg_pps, avg_bps, loss_rate = stats.get_total_stats()
        
        print(f"動作時間        : {elapsed:.2f} 秒")
        print(f"総受信パケット数: {stats.total_packets}")
        print(f"総受信データ量  : {stats.total_bytes} bytes ({stats.total_bytes / 1024:.2f} KB)")
        print(f"平均速度        : {avg_pps:.2f} pps")
        print(f"平均帯域幅      : {format_bytes(avg_bps)}")
        print(f"パケット損失    : {stats.packet_loss_count} ({loss_rate:.2f}%)")
        if stats.last_counter is not None:
            print(f"最終カウンター  : {stats.last_counter}")
        print(f"平均ジッター    : {stats.get_jitter():.2f} ms")
        print("=" * 100)
    
    except OSError as e:
        print(f"\n❌ エラー: {e}")
        print(f"\n💡 ヒント:")
        print(f"   - PCのIPアドレスが {LISTEN_IP} に設定されているか確認してください")
        print(f"   - ファイアウォールでポート {LISTEN_PORT} が開いているか確認してください")
        print(f"   - 他のプログラムがポート {LISTEN_PORT} を使用していないか確認してください")
        sys.exit(1)
    
    finally:
        sock.close()

if __name__ == "__main__":
    main()

