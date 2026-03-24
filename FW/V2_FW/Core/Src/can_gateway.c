/**
 * @file can_gateway.c
 * @brief CAN-Ethernet Gateway implementation
 */

#include "can_gateway.h"
#include "socket.h"
#include <string.h>
#include <stdio.h>

// Client connection structure
typedef struct
{
    uint8_t socket_num;
    bool connected;
    uint8_t rx_buffer[CAN_GW_BUFFER_SIZE];
    uint16_t rx_len;
} can_gw_client_t;

// CAN message queue entry
typedef struct
{
    uint8_t channel;
    FDCAN_RxHeaderTypeDef rx_header;
    uint8_t rx_data[8]; // Classic CAN: max 8 bytes
} can_queue_entry_t;

// ETH→CAN software TX queue (per channel, main loop only)
typedef struct
{
    can_frame_gw_t frames[CAN_ETH_TX_QUEUE_SIZE];
    uint16_t head; // write index
    uint16_t tail; // read index
} can_eth_tx_queue_t;

// Gateway context
static struct
{
    can_gw_client_t clients[CAN_GW_MAX_CLIENTS];
    can_gw_stats_t stats;
    FDCAN_HandleTypeDef *hfdcan[CAN_GW_NUM_CHANNELS];
    bool initialized;

    // CAN RX queue (circular buffer, interrupt-safe)
    can_queue_entry_t queue[CAN_GW_QUEUE_SIZE];
    volatile uint32_t queue_head; // Write index (used by interrupt)
    volatile uint32_t queue_tail; // Read index (used by main loop)

    // ETH→CAN TX queue per channel (absorbs FDCAN TX FIFO full)
    can_eth_tx_queue_t eth_tx_queue[CAN_GW_NUM_CHANNELS];
} gw_ctx;

// Forward declarations
static void handleClientSocket(uint8_t client_idx);
static bool sendCANFrame(uint8_t channel, const can_frame_gw_t *frame);
static void convertFDCANToGateway(const FDCAN_RxHeaderTypeDef *rx_header,
                                  const uint8_t *rx_data,
                                  can_frame_gw_t *frame);

bool CAN_Gateway_Init(void)
{
    memset(&gw_ctx, 0, sizeof(gw_ctx));

    // Store FDCAN handles
    gw_ctx.hfdcan[0] = &hfdcan1;
    gw_ctx.hfdcan[1] = &hfdcan2;
    gw_ctx.hfdcan[2] = &hfdcan3;

    // Configure FDCAN filters to accept all messages (standard and extended IDs)
    FDCAN_FilterTypeDef sFilterConfig;

    // Filter for FDCAN1 - Standard IDs (0x000 - 0x7FF)
    sFilterConfig.IdType = FDCAN_STANDARD_ID;
    sFilterConfig.FilterIndex = 0;
    sFilterConfig.FilterType = FDCAN_FILTER_RANGE;
    sFilterConfig.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
    sFilterConfig.FilterID1 = 0x000;
    sFilterConfig.FilterID2 = 0x7FF;
    if (HAL_FDCAN_ConfigFilter(&hfdcan1, &sFilterConfig) != HAL_OK)
    {
        printf("FDCAN1 standard filter config failed\r\n");
        return false;
    }

    // Filter for FDCAN1 - Extended IDs (0x00000000 - 0x1FFFFFFF)
    sFilterConfig.IdType = FDCAN_EXTENDED_ID;
    sFilterConfig.FilterIndex = 0;
    sFilterConfig.FilterType = FDCAN_FILTER_RANGE;
    sFilterConfig.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
    sFilterConfig.FilterID1 = 0x00000000;
    sFilterConfig.FilterID2 = 0x1FFFFFFF;
    if (HAL_FDCAN_ConfigFilter(&hfdcan1, &sFilterConfig) != HAL_OK)
    {
        printf("FDCAN1 extended filter config failed\r\n");
        return false;
    }

    // Filter for FDCAN2 - Standard IDs
    sFilterConfig.IdType = FDCAN_STANDARD_ID;
    sFilterConfig.FilterIndex = 0;
    sFilterConfig.FilterID1 = 0x000;
    sFilterConfig.FilterID2 = 0x7FF;
    if (HAL_FDCAN_ConfigFilter(&hfdcan2, &sFilterConfig) != HAL_OK)
    {
        printf("FDCAN2 standard filter config failed\r\n");
        return false;
    }

    // Filter for FDCAN2 - Extended IDs
    sFilterConfig.IdType = FDCAN_EXTENDED_ID;
    sFilterConfig.FilterIndex = 0;
    sFilterConfig.FilterID1 = 0x00000000;
    sFilterConfig.FilterID2 = 0x1FFFFFFF;
    if (HAL_FDCAN_ConfigFilter(&hfdcan2, &sFilterConfig) != HAL_OK)
    {
        printf("FDCAN2 extended filter config failed\r\n");
        return false;
    }

    // Filter for FDCAN3 - Standard IDs
    sFilterConfig.IdType = FDCAN_STANDARD_ID;
    sFilterConfig.FilterIndex = 0;
    sFilterConfig.FilterID1 = 0x000;
    sFilterConfig.FilterID2 = 0x7FF;
    if (HAL_FDCAN_ConfigFilter(&hfdcan3, &sFilterConfig) != HAL_OK)
    {
        printf("FDCAN3 standard filter config failed\r\n");
        return false;
    }

    // Filter for FDCAN3 - Extended IDs
    sFilterConfig.IdType = FDCAN_EXTENDED_ID;
    sFilterConfig.FilterIndex = 0;
    sFilterConfig.FilterID1 = 0x00000000;
    sFilterConfig.FilterID2 = 0x1FFFFFFF;
    if (HAL_FDCAN_ConfigFilter(&hfdcan3, &sFilterConfig) != HAL_OK)
    {
        printf("FDCAN3 extended filter config failed\r\n");
        return false;
    }

    printf("FDCAN filters configured:\r\n");
    printf("  Standard ID: 0x000-0x7FF\r\n");
    printf("  Extended ID: 0x00000000-0x1FFFFFFF\r\n");

    // Initialize client socket (only socket 0 for now, single client support)
    gw_ctx.clients[0].socket_num = 0;
    gw_ctx.clients[0].connected = false;

    // Close and setup socket 0
    close(0);
    printf("[Socket 0] Creating socket on port %d\r\n", CAN_GW_PORT);
    if (socket(0, Sn_MR_TCP, CAN_GW_PORT, Sn_MR_ND) != 0)
    {
        printf("ERROR: Failed to create socket 0\r\n");
        return false;
    }
    printf("[Socket 0] Status: 0x%02X\r\n", getSn_SR(0));

    printf("[Socket 0] Listening on port %d\r\n", CAN_GW_PORT);
    if (listen(0) != SOCK_OK)
    {
        printf("ERROR: Failed to listen on socket 0\r\n");
        return false;
    }
    printf("[Socket 0] Status: 0x%02X\r\n", getSn_SR(0));

    printf("CAN Gateway initialized\r\n");
    printf("Channels: %d (FDCAN1, FDCAN2, FDCAN3)\r\n", CAN_GW_NUM_CHANNELS);
    printf("Max clients: 1 (single client support)\r\n");
    printf("Port: %d\r\n", CAN_GW_PORT);

    gw_ctx.initialized = true;
    return true;
}

void CAN_Gateway_Process(void)
{
    if (!gw_ctx.initialized)
    {
        return;
    }

    // ETH→CAN 優先: ETH ポーリングを先に行う（パケットはソフトウェアキューに積まれる）
    handleClientSocket(0);

    // ETH→CAN TX キュードレイン: FDCAN TX FIFO に空きがある限り送信
    for (uint8_t ch = 0; ch < CAN_GW_NUM_CHANNELS; ch++)
    {
        can_eth_tx_queue_t *q = &gw_ctx.eth_tx_queue[ch];
        while (q->tail != q->head)
        {
            if (HAL_FDCAN_GetTxFifoFreeLevel(gw_ctx.hfdcan[ch]) == 0)
            {
                break; // TX FIFO 満杯 → 次ループで再試行
            }
            if (sendCANFrame(ch, &q->frames[q->tail]))
                gw_ctx.stats.tx_frames[ch]++;
            else
                gw_ctx.stats.tx_errors[ch]++;
            q->tail = (q->tail + 1) & (CAN_ETH_TX_QUEUE_SIZE - 1);
        }
    }

    // CAN→ETH: 最大 CAN_GW_BATCH_SIZE フレームをまとめて 1 回の send() で送信
    static uint8_t batch[CAN_GW_BATCH_SIZE * sizeof(can_gw_packet_t)];
    uint16_t batch_len   = 0;
    uint8_t  batch_count = 0;

    while (gw_ctx.queue_tail != gw_ctx.queue_head && batch_count < CAN_GW_BATCH_SIZE)
    {
        can_queue_entry_t *entry  = &gw_ctx.queue[gw_ctx.queue_tail];
        can_gw_packet_t   *packet = (can_gw_packet_t *)(batch + batch_len);

        memset(packet, 0, sizeof(can_gw_packet_t));
        packet->channel = entry->channel;
        convertFDCANToGateway(&entry->rx_header, entry->rx_data, &packet->frame);

        batch_len += sizeof(can_gw_packet_t);
        batch_count++;
        gw_ctx.stats.rx_frames[entry->channel]++;
        gw_ctx.queue_tail = (gw_ctx.queue_tail + 1) & (CAN_GW_QUEUE_SIZE - 1);
    }

    if (batch_count > 0)
    {
        uint8_t sn = 0;
        if (getSn_SR(sn) == SOCK_ESTABLISHED)
        {
            // TX バッファは 8KB、バッチ最大 320 バイトなので空き確認不要
            send(sn, batch, batch_len);
            gw_ctx.stats.eth_tx_bytes += batch_len;
        }
    }
}

void CAN_Gateway_OnCANReceived(uint8_t channel, const FDCAN_RxHeaderTypeDef *rx_header, const uint8_t *rx_data)
{
    if (!gw_ctx.initialized || channel >= CAN_GW_NUM_CHANNELS)
    {
        return;
    }

    // Add to queue (interrupt-safe, no SPI communication)
    uint32_t next_head = (gw_ctx.queue_head + 1) & (CAN_GW_QUEUE_SIZE - 1);

    // Check if queue is full
    if (next_head == gw_ctx.queue_tail)
    {
        // Queue full, drop packet (could increment error counter here)
        return;
    }

    // Copy message to queue
    can_queue_entry_t *entry = &gw_ctx.queue[gw_ctx.queue_head];
    entry->channel = channel;
    entry->rx_header = *rx_header;
    memcpy(entry->rx_data, rx_data, 8);

    // Update head (atomic on Cortex-M4)
    gw_ctx.queue_head = next_head;
    // rx_frames はメインループ側 (CAN_Gateway_Process) でカウント
}

void CAN_Gateway_ResetSockets(void)
{
    close(0);
    gw_ctx.clients[0].connected  = false;
    gw_ctx.stats.active_clients  = 0;

    if (socket(0, Sn_MR_TCP, CAN_GW_PORT, Sn_MR_ND) == 0)
    {
        listen(0);
        printf("[Socket 0] Listening on port %d (after link recovery)\r\n", CAN_GW_PORT);
    }
    else
    {
        printf("[Socket 0] ERROR: Failed to recreate socket\r\n");
    }
}

can_gw_stats_t CAN_Gateway_GetStats(void)
{
    return gw_ctx.stats;
}

void CAN_Gateway_PrintStats(void)
{
    printf("\r\n=== CAN Gateway Statistics ===\r\n");

    for (int i = 0; i < CAN_GW_NUM_CHANNELS; i++)
    {
        printf("CAN%d: TX=%lu RX=%lu TX_ERR=%lu RX_ERR=%lu\r\n",
               i,
               gw_ctx.stats.tx_frames[i],
               gw_ctx.stats.rx_frames[i],
               gw_ctx.stats.tx_errors[i],
               gw_ctx.stats.rx_errors[i]);
    }

    printf("Ethernet: TX=%lu bytes, RX=%lu bytes\r\n",
           gw_ctx.stats.eth_tx_bytes,
           gw_ctx.stats.eth_rx_bytes);
    printf("Active clients: %lu\r\n", gw_ctx.stats.active_clients);
    printf("==============================\r\n\r\n");
}

// Private functions

static void handleClientSocket(uint8_t client_idx)
{
    can_gw_client_t *client = &gw_ctx.clients[client_idx];
    uint8_t sn = client->socket_num;
    int32_t ret;
    uint8_t status = getSn_SR(sn);
    static uint8_t last_status = 0xFF;

    // Only print critical status changes (connected/disconnected)
    // SOCK_CLOSED / SOCK_ESTABLISHED の場合は IR を残す（各 case で使用するため）
    if (status != last_status)
    {
        uint8_t ir = getSn_IR(sn);
        if (ir != 0 && status != SOCK_CLOSED && status != SOCK_ESTABLISHED)
        {
            setSn_IR(sn, ir);
        }
        last_status = status;
    }

    switch (status)
    {
    case SOCK_ESTABLISHED:
        if (getSn_IR(sn) & Sn_IR_CON)
        {
            setSn_IR(sn, Sn_IR_CON);
            client->connected = true;
            gw_ctx.stats.active_clients++;
        }

        // W5500 RXバッファを一括読み込みしてまとめて処理
        {
            uint16_t size = getSn_RX_RSR(sn);
            if (size >= sizeof(can_gw_packet_t))
            {
                // パケット境界に揃えて最大限一括読み込み
                uint16_t max_read = (CAN_GW_BUFFER_SIZE / sizeof(can_gw_packet_t))
                                    * sizeof(can_gw_packet_t);
                if (size > max_read) size = max_read;
                size = (size / sizeof(can_gw_packet_t)) * sizeof(can_gw_packet_t);

                ret = recv(sn, client->rx_buffer, size);
                if (ret > 0 && ((uint16_t)ret % sizeof(can_gw_packet_t)) == 0)
                {
                    int num_packets = ret / sizeof(can_gw_packet_t);
                    for (int p = 0; p < num_packets; p++)
                    {
                        can_gw_packet_t *packet = (can_gw_packet_t *)(client->rx_buffer
                                                  + p * sizeof(can_gw_packet_t));
                        uint8_t ch = packet->channel;
                        if (ch >= CAN_GW_NUM_CHANNELS) continue;

                        // FDCAN TX FIFO が満杯でもロスしないようソフトウェアキューに積む
                        can_eth_tx_queue_t *q = &gw_ctx.eth_tx_queue[ch];
                        uint16_t next_head    = (q->head + 1) & (CAN_ETH_TX_QUEUE_SIZE - 1);
                        if (next_head != q->tail)
                        {
                            q->frames[q->head] = packet->frame;
                            q->head            = next_head;
                        }
                        else
                        {
                            gw_ctx.stats.tx_errors[ch]++; // ソフトウェアキュー満杯
                        }
                    }
                    gw_ctx.stats.eth_rx_bytes += ret;
                }
            }
        }
        break;

    case SOCK_LISTEN:
        // Just wait for connection - W5500 will auto-transition to ESTABLISHED
        // Periodically check for errors
        {
            static uint32_t last_check = 0;
            uint32_t now = HAL_GetTick();
            if (now - last_check > 1000) // Check every second
            {
                uint8_t ir = getSn_IR(sn);
                if (ir != 0)
                {
                    printf("[Socket %d] LISTEN IR=0x%02X", sn, ir);
                    if (ir & 0x08)
                        printf(" TIMEOUT");
                    if (ir & 0x04)
                        printf(" RECV");
                    if (ir & 0x02)
                        printf(" DISCON");
                    if (ir & 0x01)
                        printf(" CON");
                    printf("\r\n");
                    setSn_IR(sn, ir);
                }

                // Also check W5500 version to ensure hardware is OK
                static uint8_t check_count = 0;
                if (++check_count >= 10) // Every 10 seconds
                {
                    uint8_t ver = getVERSIONR();
                    if (ver != 0x04)
                    {
                        printf("[W5500] ERROR: Version mismatch! Got 0x%02X, expected 0x04\r\n", ver);
                    }
                    check_count = 0;
                }

                last_check = now;
            }
        }
        break;

    case SOCK_SYNSENT: // 0x15 - Connection in progress
    case SOCK_SYNRECV: // 0x16 - Connection in progress
    case 0x11:         // W5500 中間状態（接続確立前の遷移）、そのまま待機
        // Connection establishment in progress, just wait
        break;

    case SOCK_CLOSE_WAIT:
        disconnect(sn);
        if (client->connected)
        {
            gw_ctx.stats.active_clients--;
            client->connected = false;
            printf("[Client disconnected] Socket %d\r\n", sn);
        }
        break;

    case SOCK_INIT:
        printf("[Socket %d] Listening on port %d\r\n", sn, CAN_GW_PORT);
        listen(sn);
        break;

    case SOCK_CLOSED:
    {
        uint8_t ir = getSn_IR(sn);
        printf("[Socket %d] Closed IR=0x%02X (", sn, ir);
        if (ir & 0x08)
            printf("TIMEOUT ");
        if (ir & 0x04)
            printf("RECV ");
        if (ir & 0x02)
            printf("DISCON ");
        if (ir & 0x01)
            printf("CON ");
        printf(") -> recreate\r\n");
        setSn_IR(sn, 0xFF);
    }
        socket(sn, Sn_MR_TCP, CAN_GW_PORT, Sn_MR_ND);
        break;

    case SOCK_FIN_WAIT:  // 0x18 - Closing
    case SOCK_CLOSING:   // 0x1A - Closing
    case SOCK_TIME_WAIT: // 0x1B - Closing
    case SOCK_LAST_ACK:  // 0x1D - Closing
        // Connection closing, just wait for CLOSED state
        break;

    default:
        // Unknown status - log but don't close immediately
        // Only close after multiple consecutive unknown states
        static uint8_t unknown_count = 0;
        static uint8_t last_unknown_status = 0;

        if (status == last_unknown_status)
        {
            unknown_count++;
        }
        else
        {
            unknown_count = 1;
            last_unknown_status = status;
        }

        printf("[Socket %d] Unknown status 0x%02X (count=%d)\r\n", sn, status, unknown_count);

        // Only reset after 5 consecutive unknown states
        if (unknown_count >= 5)
        {
            printf("[Socket %d] Too many unknown states, resetting socket\r\n", sn);
            close(sn);
            unknown_count = 0;
            last_unknown_status = 0;
        }
        break;
    }
}

static bool sendCANFrame(uint8_t channel, const can_frame_gw_t *frame)
{
    if (channel >= CAN_GW_NUM_CHANNELS)
    {
        return false;
    }

    // Debug: Print frame being sent to CAN (disabled for performance)
    // printf("[CAN TX] ch=%d frame->can_id=0x%lX len=%d\r\n", channel, frame->can_id, frame->len);

    FDCAN_HandleTypeDef *hfdcan = gw_ctx.hfdcan[channel];
    FDCAN_TxHeaderTypeDef tx_header;

    // Configure TX header
    tx_header.Identifier = frame->can_id & 0x1FFFFFFF;

    // printf("[CAN TX] tx_header.Identifier=0x%lX\r\n", tx_header.Identifier);

    if (frame->can_id & 0x80000000)
    {
        tx_header.IdType = FDCAN_EXTENDED_ID;
    }
    else
    {
        tx_header.IdType = FDCAN_STANDARD_ID;
    }

    if (frame->can_id & 0x40000000)
    {
        tx_header.TxFrameType = FDCAN_REMOTE_FRAME;
    }
    else
    {
        tx_header.TxFrameType = FDCAN_DATA_FRAME;
    }

    tx_header.FDFormat = FDCAN_CLASSIC_CAN;
    tx_header.BitRateSwitch = FDCAN_BRS_OFF;

    // Data length (Classic CAN: 0-8 bytes)
    static const uint32_t dlc_table[9] = {
        FDCAN_DLC_BYTES_0,
        FDCAN_DLC_BYTES_1,
        FDCAN_DLC_BYTES_2,
        FDCAN_DLC_BYTES_3,
        FDCAN_DLC_BYTES_4,
        FDCAN_DLC_BYTES_5,
        FDCAN_DLC_BYTES_6,
        FDCAN_DLC_BYTES_7,
        FDCAN_DLC_BYTES_8,
    };
    tx_header.DataLength = dlc_table[(frame->len <= 8) ? frame->len : 8];

    tx_header.ErrorStateIndicator = FDCAN_ESI_ACTIVE;
    tx_header.TxEventFifoControl = FDCAN_NO_TX_EVENTS;
    tx_header.MessageMarker = 0;

    // Send message
    if (HAL_FDCAN_AddMessageToTxFifoQ(hfdcan, &tx_header, (uint8_t *)frame->data) != HAL_OK)
    {
        return false;
    }

    return true;
}

static void convertFDCANToGateway(const FDCAN_RxHeaderTypeDef *rx_header,
                                  const uint8_t *rx_data,
                                  can_frame_gw_t *frame)
{
    // frame は呼び出し元の memset(packet, 0) で既にゼロクリア済み

    // Debug: Print raw header (disabled for performance)
    // printf("[Convert] RX: Identifier=0x%lX IdType=%lu DLC=0x%lX\r\n",
    //        rx_header->Identifier, rx_header->IdType, rx_header->DataLength);

    // CAN ID
    frame->can_id = rx_header->Identifier;

    if (rx_header->IdType == FDCAN_EXTENDED_ID)
    {
        frame->can_id |= 0x80000000; // EFF flag
    }

    if (rx_header->RxFrameType == FDCAN_REMOTE_FRAME)
    {
        frame->can_id |= 0x40000000; // RTR flag
    }

    // Data length: FDCAN_DLC_BYTES_N = N (0x00000000〜0x00000008)
    frame->len = (rx_header->DataLength <= 8) ? (uint8_t)rx_header->DataLength : 8;

    // Copy data
    if (frame->len > 0)
    {
        memcpy(frame->data, rx_data, frame->len);
    }
}
