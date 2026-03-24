/**
 * @file can_gateway.h
 * @brief CAN-Ethernet Gateway for STM32G474 + W5500
 * @details 3-channel FDCAN to Ethernet bridge with SocketCAN compatibility
 */

#ifndef CAN_GATEWAY_H
#define CAN_GATEWAY_H

#ifdef __cplusplus
extern "C"
{
#endif

#include "main.h"
#include "fdcan.h"
#include <stdint.h>
#include <stdbool.h>

// Gateway configuration
#define CAN_GW_MAX_CLIENTS 1       // Maximum concurrent clients (single socket)
#define CAN_GW_PORT 5000           // TCP port
#define CAN_GW_BUFFER_SIZE 16384   // RX buffer per socket (matches W5500 16KB allocation)
#define CAN_GW_NUM_CHANNELS 3      // FDCAN1, FDCAN2, FDCAN3
#define CAN_GW_QUEUE_SIZE 512      // CAN RX queue size (must be power of 2)
#define CAN_GW_BATCH_SIZE 16       // Max frames to batch into one TCP send()
#define CAN_ETH_TX_QUEUE_SIZE 1024 // ETH→CAN software TX queue per channel (must be power of 2)

    // CAN frame structure (Classic CAN, 8 bytes max)
    typedef struct
    {
        uint32_t can_id; // CAN ID + flags (bit31=EFF, bit30=RTR)
        uint8_t len;     // Data length (0-8)
        uint8_t __res0;  // Reserved
        uint8_t __res1;  // Reserved
        uint8_t __res2;  // Reserved
        uint8_t data[8]; // Data (Classic CAN max 8 bytes)
    } __attribute__((packed)) can_frame_gw_t;

    // Gateway packet structure  (total 20 bytes)
    typedef struct
    {
        uint8_t channel;      // CAN channel (0=FDCAN1, 1=FDCAN2, 2=FDCAN3)
        uint8_t reserved[3];  // Alignment
        can_frame_gw_t frame; // CAN frame (16 bytes)
    } __attribute__((packed)) can_gw_packet_t;

    // Gateway statistics
    typedef struct
    {
        uint32_t tx_frames[CAN_GW_NUM_CHANNELS];
        uint32_t rx_frames[CAN_GW_NUM_CHANNELS];
        uint32_t tx_errors[CAN_GW_NUM_CHANNELS];
        uint32_t rx_errors[CAN_GW_NUM_CHANNELS];
        uint32_t eth_tx_bytes;
        uint32_t eth_rx_bytes;
        uint32_t active_clients;
    } can_gw_stats_t;

    /**
     * @brief Initialize CAN Gateway
     * @return true if successful
     */
    bool CAN_Gateway_Init(void);

    /**
     * @brief Gateway main loop (call periodically)
     */
    void CAN_Gateway_Process(void);

    /**
     * @brief Queue CAN received message (call from interrupt handler)
     * @param channel CAN channel (0-2)
     * @param rx_header RX header
     * @param rx_data RX data
     * @note This function is interrupt-safe and does not access W5500
     */
    void CAN_Gateway_OnCANReceived(uint8_t channel, const FDCAN_RxHeaderTypeDef *rx_header, const uint8_t *rx_data);

    /**
     * @brief Re-create TCP sockets after link recovery (does not touch FDCAN)
     */
    void CAN_Gateway_ResetSockets(void);

    /**
     * @brief Get gateway statistics
     * @return Statistics structure
     */
    can_gw_stats_t CAN_Gateway_GetStats(void);

    /**
     * @brief Print statistics to UART
     */
    void CAN_Gateway_PrintStats(void);

#ifdef __cplusplus
}
#endif

#endif /* CAN_GATEWAY_H */
