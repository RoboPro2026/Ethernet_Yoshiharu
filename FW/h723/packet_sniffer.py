#!/usr/bin/env python3
"""
STM32H723 Ethernet パケットスニファー
指定したネットワークインターフェース上のすべてのパケットをキャプチャします。
"""

from scapy.all import sniff, IP, UDP, TCP, ICMP, ARP, Ether
import struct
from datetime import datetime
import sys

# 設定
INTERFACE = "en5"              # ネットワークインターフェース名
STM32_IP = "192.168.1.10"     # STM32のIPアドレス（フィルタ用、Noneで全て表示）
SHOW_ALL = False               # True: すべてのパケット表示, False: STM32関連のみ

# 統計情報
stats = {
    'total': 0,
    'udp': 0,
    'tcp': 0,
    'icmp': 0,
    'arp': 0,
    'other': 0,
    'stm32_packets': 0
}

def format_mac(mac):
    """MACアドレスを整形"""
    if mac:
        return ':'.join(f'{b:02x}' for b in bytes.fromhex(mac.replace(':', '')))
    return "不明"

def parse_udp_payload(payload):
    """UDPペイロードを解析（STM32のカウンター値）"""
    if len(payload) == 4:
        try:
            counter = struct.unpack('<I', payload)[0]
            return f"Counter: {counter} (0x{counter:08X})"
        except:
            return f"Raw: {payload.hex()}"
    elif len(payload) > 0:
        # 16バイトまで表示
        hex_data = payload[:16].hex()
        if len(payload) > 16:
            hex_data += "..."
        return f"Hex: {hex_data}"
    return "Empty"

def packet_handler(packet):
    """パケット処理ハンドラ"""
    global stats
    
    stats['total'] += 1
    timestamp = datetime.now().strftime("%H:%M:%S.%f")[:-3]
    
    # フィルタリング（STM32関連のみ表示する場合）
    if STM32_IP and not SHOW_ALL:
        if IP not in packet:
            return
        if packet[IP].src != STM32_IP and packet[IP].dst != STM32_IP:
            return
    
    # パケット情報を整形
    info_parts = []
    proto_name = "UNKNOWN"
    
    # Ethernetヘッダ
    if Ether in packet:
        src_mac = format_mac(packet[Ether].src)
        dst_mac = format_mac(packet[Ether].dst)
        info_parts.append(f"MAC: {src_mac} → {dst_mac}")
    
    # IPヘッダ
    if IP in packet:
        src_ip = packet[IP].src
        dst_ip = packet[IP].dst
        ttl = packet[IP].ttl
        info_parts.append(f"IP: {src_ip} → {dst_ip} (TTL:{ttl})")
        
        # STM32パケットカウント
        if src_ip == STM32_IP or dst_ip == STM32_IP:
            stats['stm32_packets'] += 1
        
        # UDP
        if UDP in packet:
            stats['udp'] += 1
            proto_name = "UDP"
            src_port = packet[UDP].sport
            dst_port = packet[UDP].dport
            length = packet[UDP].len
            info_parts.append(f"UDP: {src_port} → {dst_port} (Len:{length})")
            
            # ペイロード解析
            if packet[UDP].payload:
                payload_info = parse_udp_payload(bytes(packet[UDP].payload))
                info_parts.append(f"Data: {payload_info}")
        
        # TCP
        elif TCP in packet:
            stats['tcp'] += 1
            proto_name = "TCP"
            src_port = packet[TCP].sport
            dst_port = packet[TCP].dport
            flags = packet[TCP].flags
            seq = packet[TCP].seq
            info_parts.append(f"TCP: {src_port} → {dst_port} (Flags:{flags}, Seq:{seq})")
        
        # ICMP
        elif ICMP in packet:
            stats['icmp'] += 1
            proto_name = "ICMP"
            icmp_type = packet[ICMP].type
            icmp_code = packet[ICMP].code
            info_parts.append(f"ICMP: Type={icmp_type}, Code={icmp_code}")
        
        else:
            stats['other'] += 1
            proto_name = f"IP-Proto-{packet[IP].proto}"
    
    # ARP
    elif ARP in packet:
        stats['arp'] += 1
        proto_name = "ARP"
        op = "Request" if packet[ARP].op == 1 else "Reply"
        psrc = packet[ARP].psrc
        pdst = packet[ARP].pdst
        hwsrc = format_mac(packet[ARP].hwsrc)
        info_parts.append(f"ARP-{op}: {psrc} ({hwsrc}) → {pdst}")
    
    else:
        stats['other'] += 1
    
    # パケット情報表示
    print(f"[{timestamp}] {proto_name:8s} | {' | '.join(info_parts)}")

def print_header():
    """ヘッダー表示"""
    print("=" * 120)
    print("STM32H723 Ethernet パケットスニファー")
    print("=" * 120)
    print(f"インターフェース: {INTERFACE}")
    if STM32_IP and not SHOW_ALL:
        print(f"フィルタ        : STM32 IP = {STM32_IP}")
    else:
        print(f"フィルタ        : なし（全パケット表示）")
    print("=" * 120)
    print("キャプチャ開始... (Ctrl+C で終了)\n")

def print_stats():
    """統計情報表示"""
    print("\n" + "=" * 120)
    print("統計情報")
    print("=" * 120)
    print(f"総パケット数    : {stats['total']}")
    print(f"STM32関連       : {stats['stm32_packets']}")
    print(f"  - UDP         : {stats['udp']}")
    print(f"  - TCP         : {stats['tcp']}")
    print(f"  - ICMP        : {stats['icmp']}")
    print(f"  - ARP         : {stats['arp']}")
    print(f"  - その他      : {stats['other']}")
    print("=" * 120)

def main():
    try:
        print_header()
        
        # パケットキャプチャ開始
        sniff(iface=INTERFACE, prn=packet_handler, store=False)
    
    except KeyboardInterrupt:
        print_stats()
    
    except PermissionError:
        print("\n❌ エラー: パケットキャプチャには管理者権限が必要です")
        print("\n💡 以下のコマンドで実行してください:")
        print(f"   sudo python3 {sys.argv[0]}")
        sys.exit(1)
    
    except OSError as e:
        print(f"\n❌ エラー: {e}")
        print(f"\n💡 ヒント:")
        print(f"   - インターフェース名 '{INTERFACE}' が正しいか確認してください")
        print(f"   - 利用可能なインターフェースを確認: ifconfig または ip addr")
        sys.exit(1)
    
    except Exception as e:
        print(f"\n❌ 予期しないエラー: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)

if __name__ == "__main__":
    # コマンドライン引数チェック
    if len(sys.argv) > 1:
        INTERFACE = sys.argv[1]
    
    # Scapyのインストールチェック
    try:
        from scapy.all import sniff
    except ImportError:
        print("❌ エラー: scapyライブラリがインストールされていません")
        print("\n💡 以下のコマンドでインストールしてください:")
        print("   pip3 install scapy")
        sys.exit(1)
    
    main()

