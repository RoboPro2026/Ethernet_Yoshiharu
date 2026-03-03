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
    uint8_t rx_data[64];
} can_queue_entry_t;

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
} gw_ctx;

// Forward declarations
static void handleClientSocket(uint8_t client_idx);
static void sendPacketToAllClients(const can_gw_packet_t *packet);
static bool sendCANFrame(uint8_t channel, const can_frame_gw_t *frame);
static void printFDCANStatus(uint8_t channel);
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
    if (socket(0, Sn_MR_TCP, CAN_GW_PORT, 0x00) != 0)
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

    // Process queued CAN messages (send to Ethernet)
    while (gw_ctx.queue_tail != gw_ctx.queue_head)
    {
        // Get message from queue
        can_queue_entry_t *entry = &gw_ctx.queue[gw_ctx.queue_tail];

        // Convert to gateway packet
        can_gw_packet_t packet;
        memset(&packet, 0, sizeof(packet));
        packet.channel = entry->channel;
        convertFDCANToGateway(&entry->rx_header, entry->rx_data, &packet.frame);

        // Send to all connected clients
        sendPacketToAllClients(&packet);

        // Update tail (move to next message)
        gw_ctx.queue_tail = (gw_ctx.queue_tail + 1) & (CAN_GW_QUEUE_SIZE - 1);
    }

    // Process socket 0 (single client support)
    handleClientSocket(0);
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
    memcpy(entry->rx_data, rx_data, 64);

    // Update head (atomic on Cortex-M4)
    gw_ctx.queue_head = next_head;

    gw_ctx.stats.rx_frames[channel]++;
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

        // Check received data
        uint16_t size = getSn_RX_RSR(sn);
        if (size > 0)
        {
            if (size > sizeof(can_gw_packet_t))
            {
                size = sizeof(can_gw_packet_t);
            }

            ret = recv(sn, client->rx_buffer, size);

            if (ret == sizeof(can_gw_packet_t))
            {
                can_gw_packet_t *packet = (can_gw_packet_t *)client->rx_buffer;

                /* Ethernet おうむ返し（Python の recv が応答を待つため、切断を防ぐ） */
                if (getSn_TX_FSR(sn) >= sizeof(can_gw_packet_t))
                {
                    // send(sn, client->rx_buffer, sizeof(can_gw_packet_t));
                    gw_ctx.stats.eth_tx_bytes += sizeof(can_gw_packet_t);
                }

                /* CAN 送信 */
                if (packet->channel < CAN_GW_NUM_CHANNELS)
                {
                    if (sendCANFrame(packet->channel, &packet->frame))
                    {
                        gw_ctx.stats.tx_frames[packet->channel]++;
                    }
                    else
                    {
                        gw_ctx.stats.tx_errors[packet->channel]++;
                        // printFDCANStatus(packet->channel);
                    }
                }

                gw_ctx.stats.eth_rx_bytes += ret;
            }
            else
            {
                printf("[Socket %d] WARNING: Received %ld bytes, expected %d\r\n", sn, ret, sizeof(can_gw_packet_t));
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
        HAL_Delay(100); /* 再作成前に待機（DISCON の連鎖を防ぐ） */
        socket(sn, Sn_MR_TCP, CAN_GW_PORT, 0x00);
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

static void sendPacketToAllClients(const can_gw_packet_t *packet)
{
    // Only support socket 0 for now (single client)
    can_gw_client_t *client = &gw_ctx.clients[0];
    uint8_t sn = 0;

    // Always check socket status to determine if we should send
    uint8_t sock_status = getSn_SR(sn);

    // Only send if socket is in ESTABLISHED state
    if (sock_status == SOCK_ESTABLISHED)
    {
        // Check TX buffer free size before sending
        uint16_t free_size = getSn_TX_FSR(sn);
        uint16_t packet_size = sizeof(can_gw_packet_t);

        if (free_size < packet_size)
        {
            // TX buffer is full, skip this message
            printf("[Gateway] TX buffer full (free=%d, need=%d), skipping\r\n", free_size, packet_size);
            return;
        }

        int32_t ret = send(sn, (uint8_t *)packet, packet_size);

        if (ret > 0)
        {
            gw_ctx.stats.eth_tx_bytes += ret;
            // Success - no logging to reduce UART overhead
        }
        else if (ret < 0)
        {
            // Error occurred
            printf("[Gateway] Send ERROR: ret=%ld status=0x%02X free=%d\r\n",
                   ret, sock_status, free_size);

            // Check if socket is still valid
            uint8_t new_status = getSn_SR(sn);
            if (new_status != sock_status)
            {
                printf("[Gateway] Socket status changed after send: 0x%02X -> 0x%02X\r\n",
                       sock_status, new_status);
            }
        }
    }
    // else: Socket not ESTABLISHED, skip silently (normal when no client connected)
}

/* TX失敗時のFDCAN状態をUARTに出力（LEC, BO, EP, EW） */
static void printFDCANStatus(uint8_t channel)
{
    if (channel >= CAN_GW_NUM_CHANNELS)
        return;
    FDCAN_HandleTypeDef *h = gw_ctx.hfdcan[channel];
    if (!h || !h->Instance)
        return;
    uint32_t psr = h->Instance->PSR;
    uint32_t lec = (psr & FDCAN_PSR_LEC) >> FDCAN_PSR_LEC_Pos;
    const char *lec_str[] = {"NoErr", "Stuff", "Form", "ACK", "BitR", "BitD", "CRC", "NoChg"};
    printf("[CAN TX ERR] ch=%d PSR=0x%lX LEC=%lu(%s) BO=%lu EP=%lu EW=%lu\r\n",
           (int)channel, (unsigned long)psr, (unsigned long)lec,
           lec < 8 ? lec_str[lec] : "?",
           (unsigned long)((psr & FDCAN_PSR_BO) ? 1 : 0),
           (unsigned long)((psr & FDCAN_PSR_EP) ? 1 : 0),
           (unsigned long)((psr & FDCAN_PSR_EW) ? 1 : 0));
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

    // CANFD or standard CAN
    if (frame->flags != 0)
    {
        tx_header.FDFormat = FDCAN_FD_CAN;
        tx_header.BitRateSwitch = (frame->flags & 0x01) ? FDCAN_BRS_ON : FDCAN_BRS_OFF;
    }
    else
    {
        tx_header.FDFormat = FDCAN_CLASSIC_CAN;
        tx_header.BitRateSwitch = FDCAN_BRS_OFF;
    }

    // Data length - convert to FDCAN DLC
    switch (frame->len)
    {
    case 0:
        tx_header.DataLength = FDCAN_DLC_BYTES_0;
        break;
    case 1:
        tx_header.DataLength = FDCAN_DLC_BYTES_1;
        break;
    case 2:
        tx_header.DataLength = FDCAN_DLC_BYTES_2;
        break;
    case 3:
        tx_header.DataLength = FDCAN_DLC_BYTES_3;
        break;
    case 4:
        tx_header.DataLength = FDCAN_DLC_BYTES_4;
        break;
    case 5:
        tx_header.DataLength = FDCAN_DLC_BYTES_5;
        break;
    case 6:
        tx_header.DataLength = FDCAN_DLC_BYTES_6;
        break;
    case 7:
        tx_header.DataLength = FDCAN_DLC_BYTES_7;
        break;
    case 8:
        tx_header.DataLength = FDCAN_DLC_BYTES_8;
        break;
    default:
        // CANFD length encoding
        if (frame->len <= 12)
            tx_header.DataLength = FDCAN_DLC_BYTES_12;
        else if (frame->len <= 16)
            tx_header.DataLength = FDCAN_DLC_BYTES_16;
        else if (frame->len <= 20)
            tx_header.DataLength = FDCAN_DLC_BYTES_20;
        else if (frame->len <= 24)
            tx_header.DataLength = FDCAN_DLC_BYTES_24;
        else if (frame->len <= 32)
            tx_header.DataLength = FDCAN_DLC_BYTES_32;
        else if (frame->len <= 48)
            tx_header.DataLength = FDCAN_DLC_BYTES_48;
        else
            tx_header.DataLength = FDCAN_DLC_BYTES_64;
        break;
    }

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
    memset(frame, 0, sizeof(can_frame_gw_t));

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

    // Data length - convert FDCAN DLC to actual byte count
    switch (rx_header->DataLength)
    {
    case FDCAN_DLC_BYTES_0:
        frame->len = 0;
        break;
    case FDCAN_DLC_BYTES_1:
        frame->len = 1;
        break;
    case FDCAN_DLC_BYTES_2:
        frame->len = 2;
        break;
    case FDCAN_DLC_BYTES_3:
        frame->len = 3;
        break;
    case FDCAN_DLC_BYTES_4:
        frame->len = 4;
        break;
    case FDCAN_DLC_BYTES_5:
        frame->len = 5;
        break;
    case FDCAN_DLC_BYTES_6:
        frame->len = 6;
        break;
    case FDCAN_DLC_BYTES_7:
        frame->len = 7;
        break;
    case FDCAN_DLC_BYTES_8:
        frame->len = 8;
        break;
    case FDCAN_DLC_BYTES_12:
        frame->len = 12;
        break;
    case FDCAN_DLC_BYTES_16:
        frame->len = 16;
        break;
    case FDCAN_DLC_BYTES_20:
        frame->len = 20;
        break;
    case FDCAN_DLC_BYTES_24:
        frame->len = 24;
        break;
    case FDCAN_DLC_BYTES_32:
        frame->len = 32;
        break;
    case FDCAN_DLC_BYTES_48:
        frame->len = 48;
        break;
    case FDCAN_DLC_BYTES_64:
        frame->len = 64;
        break;
    default:
        frame->len = 0;
        break;
    }

    // Copy data
    if (frame->len > 0)
    {
        memcpy(frame->data, rx_data, frame->len);
    }

    // Flags
    frame->flags = 0;
    if (rx_header->FDFormat == FDCAN_FD_CAN)
    {
        if (rx_header->BitRateSwitch == FDCAN_BRS_ON)
        {
            frame->flags |= 0x01; // BRS
        }
        if (rx_header->ErrorStateIndicator == FDCAN_ESI_PASSIVE)
        {
            frame->flags |= 0x02; // ESI
        }
    }

    // Data
    memcpy(frame->data, rx_data, frame->len);
}
