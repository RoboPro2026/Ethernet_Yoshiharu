# かにはる V2 FW — Ethernet ↔ CAN ゲートウェイ

STM32G473 + W5500 による Classic CAN ↔ Ethernet (TCP) ゲートウェイです。  
FDCAN1 / FDCAN2 / FDCAN3 の 3 チャンネルを 1 本の TCP 接続で扱えます。

---

## プロトコル仕様

### パケットフォーマット

#### Gateway Packet（20 バイト固定）

```
+--------+----------+----------+
| Offset | Size     | Field    |
+--------+----------+----------+
| 0      | 1 byte   | Channel  |
| 1–3    | 3 bytes  | Reserved |
| 4–19   | 16 bytes | CAN Frame|
+--------+----------+----------+
```

#### CAN Frame（16 バイト）

```
+--------+----------+-----------+
| Offset | Size     | Field     |
+--------+----------+-----------+
| 0–3    | 4 bytes  | CAN ID    |
| 4      | 1 byte   | Length    |
| 5–7    | 3 bytes  | Reserved  |
| 8–15   | 8 bytes  | Data      |
+--------+----------+-----------+
```

> Classic CAN のみ対応（データは最大 8 バイト）。

### チャンネル番号

| Channel | ペリフェラル |
|---------|------------|
| `0`     | FDCAN1     |
| `1`     | FDCAN2     |
| `2`     | FDCAN3     |

### CAN ID フィールド（SocketCAN 互換）

```
Bit 31 (0x80000000): EFF — 拡張フレームフラグ（29 ビット ID）
Bit 30 (0x40000000): RTR — リモートフレームフラグ
Bit  0–28           : CAN ID（標準: 下位 11 ビット / 拡張: 29 ビット）
```

| フレームタイプ       | ID 範囲               | EFF | RTR |
|--------------------|-----------------------|-----|-----|
| 標準データフレーム   | `0x000`–`0x7FF`       | 0   | 0   |
| 標準リモートフレーム | `0x000`–`0x7FF`       | 0   | 1   |
| 拡張データフレーム   | `0x000`–`0x1FFFFFFF`  | 1   | 0   |
| 拡張リモートフレーム | `0x000`–`0x1FFFFFFF`  | 1   | 1   |

---

## ネットワーク設定

`Core/Src/main.c` の `gWIZNETINFO` で変更可能です。

| 項目    | デフォルト値        |
|---------|-------------------|
| MAC     | `00:08:DC:12:34:56` |
| IP      | `192.168.1.100`   |
| Subnet  | `255.255.255.0`   |
| Gateway | `192.168.1.1`     |
| Port    | `5000`            |

> [!CAUTION]
> DHCP には対応していません。接続後 `ping` や `nc` などで疎通を確認してください。  
> WiFi や VPN と同じセグメントの場合は IP を別セグメントに変更するか、WiFi を切断してください。

---

## ファームウェア

### ビルド

```bash
cd FW/V2_FW
make -j4
```

生成ファイル:

| ファイル             | 説明         |
|---------------------|------------|
| `build/V2_FW.elf`   | ELF        |
| `build/V2_FW.hex`   | HEX        |
| `build/V2_FW.bin`   | バイナリ    |

### 書き込み

STM32CubeProgrammer で `build/V2_FW.hex` または `build/V2_FW.bin` を書き込んでください。

### 主要ファイル

| ファイル                    | 説明                              |
|----------------------------|----------------------------------|
| `Core/Src/main.c`          | メインループ・W5500 初期化        |
| `Core/Src/can_gateway.c`   | ゲートウェイ実装                  |
| `Core/Inc/can_gateway.h`   | パケット構造体・API 定義           |
| `Core/Src/fdcan.c`         | FDCAN 初期化（1 Mbps Classic CAN）|
| `Eth/Src/w5500_port.c`     | W5500 SPI ラッパー                |

---

## アーキテクチャ

### CAN → Ethernet

```
FDCAN 割り込み (HAL_FDCAN_RxFifo0Callback)
  └→ RAM キュー（64 メッセージ循環バッファ）に追加（SPI なし）
       └→ メインループ: キューから取り出し → W5500 で TCP 送信
```

### Ethernet → CAN

```
W5500 TCP 受信
  └→ 20 バイトパケットをパース
       └→ FDCAN 送信
```

> 割り込みハンドラから直接 W5500 SPI にアクセスするとメインループと競合するため、  
> CAN 受信はキューイング方式を採用しています。

---

## Python クライアント

### `Core/Src/can_bridge.py` — 対話型送受信ツール

```bash
python3 can_bridge.py 192.168.1.100 5000
```

主なコマンド:

| コマンド | 説明 |
|---------|------|
| `send <ch> <id> [data...]` | CAN フレーム送信 |
| `burst <ch> <id> <count> <ms>` | バースト送信 |
| `stats` | 統計表示 |

### `Core/Src/bidir_test.py` — 双方向テスト + レイテンシ計測

Innomaker USB2CAN (macOS: slcan) と組み合わせて ETH↔CAN の双方向テストができます。

```bash
pip install python-can pyserial
python3 bidir_test.py 192.168.1.100 --iface /dev/cu.usbserial-XXXX
```

主なオプション:

| オプション | デフォルト | 説明 |
|-----------|-----------|------|
| `--channel` | `0` | FDCAN チャンネル（0–2） |
| `--iface` | 自動検出 | USB2CAN デバイスパス |
| `--bitrate` | `1000000` | CAN ビットレート |
| `--count` | `100` | 各方向の送信パケット数 |
| `--interval` | `0.005` | 送信間隔（秒） |
| `--eth-only` | — | ETH→CAN のみ |
| `--can-only` | — | CAN→ETH のみ |
