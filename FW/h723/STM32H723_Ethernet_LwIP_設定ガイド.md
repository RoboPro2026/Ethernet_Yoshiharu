# STM32H723 Ethernet + LwIP 設定ガイド

## 1. MPU設定

**Cortex_M7 → MPU Settings**

### MPU Region 1: DMA Descriptors用（D2 SRAM）
| 設定項目 | 値 | 備考 |
|---------|-----|------|
| MPU Region | Enabled | |
| MPU Region Base Address | 0x30000000 | D2 SRAM先頭 |
| MPU Region Size | 32KB | D2 SRAM全体 |
| MPU SubRegion Disable | 0x0 | |
| MPU TEX field level | level 0 | |
| MPU Access Permission | ALL ACCESS PERMITTED | |
| MPU Instruction Access | DISABLE | |
| MPU Shareability Permission | DISABLE | ✅ |
| MPU Cacheable Permission | DISABLE | ✅ **重要** |
| MPU Bufferable Permission | **ENABLE** | ✅ **重要（現在DISABLE→要変更）** |

### MPU Region 2: AXI SRAM用（RX Pool配置用）
| 設定項目 | 値 | 備考 |
|---------|-----|------|
| MPU Region | Enabled | |
| MPU Region Base Address | 0x24000000 | AXI SRAM先頭 |
| MPU Region Size | 512KB | AXI SRAM全体 |
| MPU Access Permission | ALL ACCESS PERMITTED | |
| MPU Cacheable Permission | ENABLE | 通常メモリ |
| MPU Bufferable Permission | ENABLE | |

---

## 2. ETH設定

**Connectivity → ETH**

### Mode
- Mode: `RMII`
- Advanced Parameters:
  - PHY address: `0` または `1`（DP83848の設定による）

### Configuration
- Parameter Settings: デフォルトのままでOK

---

## 3. LwIP設定

**Middleware → LWIP**

### General Settings
| 設定項目 | 値 |
|---------|-----|
| LWIP_DHCP | Disabled（固定IPの場合） |
| IP_ADDRESS | 192.168.1.10 |
| NETMASK_ADDRESS | 255.255.255.0 |
| GATEWAY_ADDRESS | 192.168.1.1 |

### Key Settings（性能向上）
| パラメータ | デフォルト | 推奨値 | 説明 |
|-----------|----------|--------|------|
| MEMP_NUM_PBUF | 10 | 10 | |
| PBUF_POOL_SIZE | 10 | 12 | |
| TCP_MSS | 536 | **1460** | MTU最大値 |
| TCP_SND_BUF | 2144 | **5840** | 4 × TCP_MSS |
| TCP_SND_QUEUELEN | 9 | **16** | |
| TCP_WND | 2144 | **5840** | 4 × TCP_MSS |
| MEM_SIZE | 16000 | **16360** | D2 RAM制約考慮 |

---

## 4. RTOS設定

**Middleware → FREERTOS**

### Tasks and Queues
- `defaultTask` のStack Size: `128` → **512以上**に増やす
- 新しいタスク追加時もStack Sizeに注意

### Config parameters
- Timebase Source: `SysTick` → **TIM6などに変更**（推奨）

---

## 5. コード生成後の手動修正

### ✅ STM32H723XG_FLASH.ld

`.lwip_sec`セクションの後に以下を追加：

```ld
.lwip_sec (NOLOAD) :
{
  . = ABSOLUTE(0x30000000);
  *(.RxDecripSection) 
  
  . = ABSOLUTE(0x30000100);
  *(.TxDecripSection)
  
} >RAM_D2

/* RX Pool を AXI SRAM に配置（STM32H723は D2 RAM が 32KB しかないため） */
.lwip_rx_pool (NOLOAD) :
{
  . = ALIGN(32);
  *(.Rx_PoolSection)
} >RAM_D1
```

---

## チェックリスト

- [ ] MPU Region 1 の Bufferable を ENABLE に変更
- [ ] MPU Region 2 を AXI SRAM 用に設定
- [ ] LwIP の TCP_MSS を 1460 に変更
- [ ] LwIP の TCP_SND_BUF を 5840 に変更
- [ ] LwIP の TCP_SND_QUEUELEN を 16 に変更
- [ ] リンカスクリプトに .Rx_PoolSection を追加
- [ ] FreeRTOS タスクの Stack Size を確認

---

## メモリマップ（STM32H723）

| 領域 | アドレス | サイズ | 用途 |
|------|---------|--------|------|
| DMARxDscrTab | 0x30000000 | 96 bytes | RX DMAディスクリプタ |
| DMATxDscrTab | 0x30000100 | 96 bytes | TX DMAディスクリプタ |
| RX_POOL | AXI SRAM (0x24xxxxxx) | 約18KB | RXバッファプール（12個） |
| LwIP heap | 残りのD2/AXI SRAM | 16KB程度 | LwIPメモリプール |

**重要**: STM32H723はD2 RAMが32KBしかないため、RX Poolは必ずAXI SRAM（512KB）に配置すること。

---

## トラブルシューティング

### HardFault発生時
1. MPU設定を確認（特にCacheable/Bufferable）
2. リンカスクリプトでRx_PoolSectionが正しく配置されているか確認
3. スタックサイズを増やす（FreeRTOSタスク、ethernet_link_threadなど）

### リンクアップしない場合
1. PHYアドレスが正しいか確認（DP83848は通常1）
2. RMII 25MHz外部クロックが供給されているか確認
3. `DP83848_Init`の戻り値をチェック

### pbuf assertエラー
1. PBUF_POOL_SIZEを増やす
2. ETH_RX_BUFFER_CNTを増やす
3. メモリリークがないか確認（pbuf_free漏れ）

---

## 参考資料

- [ST公式: STM32H7 Ethernet + LwIP設定](https://community.st.com/t5/stm32-mcus/how-to-create-a-project-for-stm32h7-with-ethernet-and-lwip-stack/ta-p/49308)
- [STM32H7-LwIP-Examples (GitHub)](https://github.com/stm32-hotspot/STM32H7-LwIP-Examples)
- STM32H723 Reference Manual (RM0468)
- DP83848 Datasheet
- LwIP Documentation

---

## 注意事項

### ⚠️ DP83848 クロック供給方式

DP83848は以下のいずれかの方式でRMIIクロックを供給できます：

#### 推奨: 25MHz外部クリスタル
- DP83848のX1/X2ピンに25MHz水晶振動子を接続
- PHY内部でRMII用50MHzクロックを生成
- **STM32H7と完全に互換性あり** ✅
- 最も安定した動作が期待できる

#### 非推奨: REF_CLK_OUTモード
- PHYがRMIIクロックを出力してSTM32に供給
- タイミング制約により**動作が不安定になる可能性あり** ⚠️
- STの公式記事でも推奨されていない

**ハードウェア確認項目**:
- [ ] DP83848に25MHz水晶振動子が実装されているか
- [ ] STM32H7のETH_REF_CLK(PA1)がDP83848のクロック出力ピンに接続されているか
- [ ] PHYアドレス設定ピンが正しく設定されているか（通常は0または1）

---

最終更新: 2025-11-28

