#!/usr/bin/env python3
"""
STM32H723 Ethernet UDP パケット受信スクリプト
STM32から送信されるUDPパケットを受信して表示します。
"""

import socket
import struct
import sys
from datetime import datetime

# 設定
LISTEN_IP = "192.168.1.1"     # 受信するIPアドレス（PCのIPアドレスに設定）
LISTEN_PORT = 55151              # 受信ポート
BUFFER_SIZE = 1024              # 受信バッファサイズ

def main():
    print("=" * 60)
    print("STM32H723 Ethernet UDP パケット受信プログラム")
    print("=" * 60)
    print(f"受信IP  : {LISTEN_IP}")
    print(f"受信Port: {LISTEN_PORT}")
    print("=" * 60)
    print("待機中... (Ctrl+C で終了)\n")
    
    # UDPソケット作成
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    
    try:
        # ソケットをバインド
        sock.bind((LISTEN_IP, LISTEN_PORT))
        
        packet_count = 0
        last_counter = None
        
        while True:
            # データ受信
            data, addr = sock.recvfrom(BUFFER_SIZE)
            packet_count += 1
            
            # 受信時刻
            timestamp = datetime.now().strftime("%H:%M:%S.%f")[:-3]
            
            # データ解析
            if len(data) == 4:
                # uint32_tのカウンター値を解析（リトルエンディアン）
                counter = struct.unpack('<I', data)[0]
                
                # パケットロスチェック
                if last_counter is not None:
                    expected = (last_counter + 1) & 0xFFFFFFFF
                    if counter != expected:
                        loss = counter - expected if counter > expected else counter + (0xFFFFFFFF - expected) + 1
                        print(f"⚠️  パケットロス検出: {loss}個のパケットが失われました")
                
                last_counter = counter
                
                # 受信情報表示
                print(f"[{timestamp}] パケット#{packet_count:6d} | "
                      f"送信元: {addr[0]}:{addr[1]} | "
                      f"カウンター: {counter:10d} (0x{counter:08X})")
            else:
                # 想定外のデータサイズ
                print(f"[{timestamp}] パケット#{packet_count:6d} | "
                      f"送信元: {addr[0]}:{addr[1]} | "
                      f"サイズ: {len(data)} bytes | "
                      f"データ(hex): {data.hex()}")
    
    except KeyboardInterrupt:
        print("\n" + "=" * 60)
        print("プログラム終了")
        print(f"総受信パケット数: {packet_count}")
        if last_counter is not None:
            print(f"最終カウンター値: {last_counter}")
        print("=" * 60)
    
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

