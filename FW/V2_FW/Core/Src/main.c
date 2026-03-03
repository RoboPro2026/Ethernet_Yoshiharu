/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * @file           : main.c
 * @brief          : Main program body
 ******************************************************************************
 * @attention
 *
 * Copyright (c) 2026 STMicroelectronics.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "fdcan.h"
#include "spi.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "wizchip_conf.h"
#include "socket.h"
#include "w5500_port.h"
#include "can_gateway.h"
#include <string.h>
#include <stdio.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
extern volatile uint32_t systick_irq_count; /* stm32g4xx_it.c で定義 */
wiz_NetInfo gWIZNETINFO = {
    .mac = {0x00, 0x08, 0xDC, 0x12, 0x34, 0x56},
    .ip = {192, 168, 1, 100},
    .sn = {255, 255, 255, 0},
    .gw = {192, 168, 1, 1},
    .dns = {8, 8, 8, 8},
    .dhcp = NETINFO_STATIC};

#define SOCK_TCPS 0
#define PORT_TCPS 5000
#define DATA_BUF_SIZE 2048
uint8_t gDATABUF[DATA_BUF_SIZE];
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
void W5500_Init(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* -----------------------------------------------------------------------
 * DWT サイクルカウンタによる HAL_GetTick / HAL_Delay オーバーライド
 * SysTick 割り込み不要のため絶対にハングしない
 * ----------------------------------------------------------------------- */
// static void DWT_Init(void)
// {
//     CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
//     DWT->CYCCNT = 0;
//     DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
// }

// static volatile uint32_t dwt_ms_ovf = 0;
// static volatile uint32_t dwt_last = 0;

// uint32_t HAL_GetTick(void)
// {
//     uint32_t cyc = DWT->CYCCNT;
//     if (cyc < dwt_last)
//         dwt_ms_ovf++;
//     dwt_last = cyc;
//     uint64_t total = ((uint64_t)dwt_ms_ovf << 32) | cyc;
//     return (uint32_t)(total / (SystemCoreClock / 1000U));
// }

// void HAL_Delay(uint32_t Delay)
// {
//     uint32_t start = DWT->CYCCNT;
//     uint32_t cycles = Delay * (SystemCoreClock / 1000U);
//     while ((DWT->CYCCNT - start) < cycles)
//         ;
// }
/* ----------------------------------------------------------------------- */

/**
 * @brief  TCP Echo Server (Loopback)
 * @param  sn: Socket number
 * @param  buf: Data buffer
 * @param  port: Port number
 * @retval Received data size
 */
int32_t loopback_tcps(uint8_t sn, uint8_t *buf, uint16_t port)
{
    int32_t ret;
    uint16_t size = 0, sentsize = 0;
    static uint8_t last_status = 0xFF;
    uint8_t current_status = getSn_SR(sn);

    // Display status change
    if (current_status != last_status)
    {
        printf("Socket status changed: 0x%02X\r\n", current_status);
        last_status = current_status;
    }

    switch (current_status)
    {
    case SOCK_ESTABLISHED:
        if (getSn_IR(sn) & Sn_IR_CON)
        {
            printf("Client connected!\r\n");
            uint8_t destip[4];
            uint16_t destport;
            getSn_DIPR(sn, destip);
            destport = getSn_DPORT(sn);
            printf("Connected from: %d.%d.%d.%d:%d\r\n",
                   destip[0], destip[1], destip[2], destip[3], destport);
            setSn_IR(sn, Sn_IR_CON);
        }

        // Check received data size
        if ((size = getSn_RX_RSR(sn)) > 0)
        {
            if (size > DATA_BUF_SIZE)
                size = DATA_BUF_SIZE;
            ret = recv(sn, buf, size);

            if (ret <= 0)
            {
                printf("Receive error: %d\r\n", ret);
                return ret;
            }

            printf("Received %d bytes: ", ret);
            for (int i = 0; i < ret && i < 64; i++)
            {
                if (buf[i] >= 32 && buf[i] < 127)
                    printf("%c", buf[i]);
                else
                    printf(".");
            }
            printf("\r\n");

            // Echo back received data
            sentsize = 0;
            while (size != sentsize)
            {
                ret = send(sn, buf + sentsize, size - sentsize);
                if (ret < 0)
                {
                    printf("Send error: %d\r\n", ret);
                    close(sn);
                    return ret;
                }
                sentsize += ret;
            }
            printf("Sent %d bytes\r\n", sentsize);
        }
        break;

    case SOCK_CLOSE_WAIT:
        printf("Client disconnect request\r\n");
        if ((ret = disconnect(sn)) != SOCK_OK)
            return ret;
        printf("Disconnected\r\n");
        break;

    case SOCK_INIT:
        printf("Start listening (port:%d)\r\n", port);
        if ((ret = listen(sn)) != SOCK_OK)
        {
            printf("Listen failed: %d\r\n", ret);
            return ret;
        }
        printf("Waiting for connection...\r\n");
        break;

    case SOCK_CLOSED:
        printf("Create socket (port:%d)\r\n", port);
        if ((ret = socket(sn, Sn_MR_TCP, port, 0x00)) != sn)
        {
            printf("Socket creation failed: %d\r\n", ret);
            return ret;
        }
        break;

    default:
        break;
    }

    return 1;
}

void W5500_Init(void)
{
    uint8_t tmp;
    uint8_t memsize[2][8] = {{2, 2, 2, 2, 2, 2, 2, 2}, {2, 2, 2, 2, 2, 2, 2, 2}};

    printf("\r\n=== W5500 Initialization Start ===\r\n");

    // SPI Test
    printf("SPI communication test...\r\n");
    if (W5500_SPI_Test())
    {
        printf("SPI communication: OK\r\n");
    }
    else
    {
        printf("ERROR: SPI communication failed\r\n");
        while (1)
            ;
    }

    // W5500 Hardware Reset
    printf("W5500 hardware reset...\r\n");
    W5500_HW_Reset();
    printf("Reset complete\r\n");

    // Deselect CS
    W5500_Deselect();

    // Register callback functions
    printf("Registering callback functions...\r\n");
    reg_wizchip_cs_cbfunc(W5500_Select, W5500_Deselect);
    reg_wizchip_spi_cbfunc(W5500_ReadByte, W5500_WriteByte);
    reg_wizchip_spiburst_cbfunc(W5500_ReadBuf, W5500_WriteBuf);
    printf("Callback registration complete\r\n");

    // W5500 Chip initialization
    printf("W5500 chip initialization...\r\n");
    if (ctlwizchip(CW_INIT_WIZCHIP, (void *)memsize) == -1)
    {
        printf("ERROR: Chip initialization failed\r\n");
        while (1)
            ;
    }
    printf("Chip initialization complete\r\n");

    // PHY Link status check
    printf("Checking PHY link status...\r\n");
    int retry = 0;
    do
    {
        if (ctlwizchip(CW_GET_PHYLINK, (void *)&tmp) == -1)
        {
            printf("ERROR: Failed to get PHY link status\r\n");
            while (1)
                ;
        }
        if (tmp == PHY_LINK_OFF)
        {
            printf("No PHY link (retry %d)...\r\n", ++retry);
            HAL_Delay(500);
        }
        if (retry > 20)
        {
            printf("ERROR: PHY link timeout\r\n");
            printf("Please check if Ethernet cable is connected\r\n");
            while (1)
                ;
        }
    } while (tmp == PHY_LINK_OFF);
    printf("PHY link established!\r\n");

    // Network configuration
    printf("Configuring network...\r\n");
    ctlnetwork(CN_SET_NETINFO, (void *)&gWIZNETINFO);

    // Verify configuration
    wiz_NetInfo tmp_info;
    ctlnetwork(CN_GET_NETINFO, (void *)&tmp_info);

    printf("\r\n--- Network Configuration ---\r\n");
    printf("MAC: %02X:%02X:%02X:%02X:%02X:%02X\r\n",
           tmp_info.mac[0], tmp_info.mac[1], tmp_info.mac[2],
           tmp_info.mac[3], tmp_info.mac[4], tmp_info.mac[5]);
    printf("IP: %d.%d.%d.%d\r\n",
           tmp_info.ip[0], tmp_info.ip[1], tmp_info.ip[2], tmp_info.ip[3]);
    printf("SN: %d.%d.%d.%d\r\n",
           tmp_info.sn[0], tmp_info.sn[1], tmp_info.sn[2], tmp_info.sn[3]);
    printf("GW: %d.%d.%d.%d\r\n",
           tmp_info.gw[0], tmp_info.gw[1], tmp_info.gw[2], tmp_info.gw[3]);
    printf("=== W5500 Initialization Complete ===\r\n\r\n");

    // W5500 chip version check
    printf("--- Chip Information ---\r\n");
    uint8_t version = getVERSIONR();
    printf("W5500 Version: 0x%02X\r\n", version);

    // Register read/write test
    printf("\r\n--- Register Test ---\r\n");
    wiz_NetInfo read_info;
    ctlnetwork(CN_GET_NETINFO, (void *)&read_info);
    printf("Read test - IP: %d.%d.%d.%d\r\n",
           read_info.ip[0], read_info.ip[1], read_info.ip[2], read_info.ip[3]);

    // Socket 0 status check
    uint8_t sock_status = getSn_SR(0);
    printf("Socket 0 initial status: 0x%02X\r\n", sock_status);

    // PHY configuration check
    uint8_t phycfgr = getPHYCFGR();
    printf("\r\n--- PHY Configuration ---\r\n");
    printf("PHYCFGR: 0x%02X\r\n", phycfgr);
    if (phycfgr & 0x01)
    {
        printf("Link status: UP\r\n");
    }
    else
    {
        printf("Link status: DOWN\r\n");
    }

    printf("\r\n[IMPORTANT] W5500 does not support ICMP (ping)\r\n");
    printf("Please test with TCP connection instead of ping:\r\n");
    printf("  telnet %d.%d.%d.%d %d\r\n",
           tmp_info.ip[0], tmp_info.ip[1], tmp_info.ip[2], tmp_info.ip[3], PORT_TCPS);
    printf("or\r\n");
    printf("  nc %d.%d.%d.%d %d\r\n",
           tmp_info.ip[0], tmp_info.ip[1], tmp_info.ip[2], tmp_info.ip[3], PORT_TCPS);
    printf("-----------------------------\r\n\r\n");
}
/* USER CODE END 0 */

/**
 * @brief  The application entry point.
 * @retval int
 */
int main(void)
{

    /* USER CODE BEGIN 1 */

    /* USER CODE END 1 */

    /* MCU Configuration--------------------------------------------------------*/

    /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
    HAL_Init();

    /* USER CODE BEGIN Init */
    SCB->VTOR = FLASH_BASE; /* ベクタテーブルを Flash (0x08000000) に明示設定 */
    /* USER CODE END Init */

    /* Configure the system clock */
    SystemClock_Config();

    /* USER CODE BEGIN SysInit */

    /* USER CODE END SysInit */

    /* Initialize all configured peripherals */
    MX_GPIO_Init();
    MX_FDCAN1_Init();
    MX_FDCAN2_Init();
    MX_FDCAN3_Init();
    MX_SPI1_Init();
    MX_TIM1_Init();
    MX_TIM2_Init();
    MX_UART4_Init();
    MX_UART5_Init();
    MX_USART1_UART_Init();
    /* USER CODE BEGIN 2 */

    W5500_Init();
    printf("W5500 Initialization Complete\r\n");

    printf("CAN Gateway Initialization Start\r\n");
    CAN_Gateway_Init();

    // FDCAN開始
    printf("Starting FDCAN interfaces...\r\n");
    HAL_FDCAN_Start(&hfdcan1);
    HAL_FDCAN_Start(&hfdcan2);
    HAL_FDCAN_Start(&hfdcan3);

    // FDCAN RX割り込み有効化
    HAL_FDCAN_ActivateNotification(&hfdcan1, FDCAN_IT_RX_FIFO0_NEW_MESSAGE, 0);
    HAL_FDCAN_ActivateNotification(&hfdcan2, FDCAN_IT_RX_FIFO0_NEW_MESSAGE, 0);
    HAL_FDCAN_ActivateNotification(&hfdcan3, FDCAN_IT_RX_FIFO0_NEW_MESSAGE, 0);
    printf("FDCAN interfaces started\r\n");

    printf("\r\n========================================\r\n");
    printf("CAN-Ethernet Gateway Ready!\r\n");
    printf("IP: %d.%d.%d.%d\r\n",
           gWIZNETINFO.ip[0], gWIZNETINFO.ip[1],
           gWIZNETINFO.ip[2], gWIZNETINFO.ip[3]);
    printf("Port: 5000\r\n");
    printf("Channels: FDCAN1, FDCAN2, FDCAN3\r\n");
    printf("========================================\r\n\r\n");
    /* USER CODE END 2 */

    /* Infinite loop */
    /* USER CODE BEGIN WHILE */
    while (1)
    {
        /* USER CODE END WHILE */
        CAN_Gateway_Process();
        /* USER CODE BEGIN 3 */
    }
    /* USER CODE END 3 */
}

/**
 * @brief System Clock Configuration
 * @retval None
 */
void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    /** Configure the main internal regulator output voltage
     */
    HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1_BOOST);

    /** Initializes the RCC Oscillators according to the specified parameters
     * in the RCC_OscInitTypeDef structure.
     */
    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    RCC_OscInitStruct.HSEState = RCC_HSE_ON;
    RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    RCC_OscInitStruct.PLL.PLLM = RCC_PLLM_DIV1;
    RCC_OscInitStruct.PLL.PLLN = 20;
    RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
    RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV4;
    RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV2;
    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
    {
        Error_Handler();
    }

    /** Initializes the CPU, AHB and APB buses clocks
     */
    RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
    {
        Error_Handler();
    }
}

/* USER CODE BEGIN 4 */
void HAL_FDCAN_RxFifo0Callback(FDCAN_HandleTypeDef *hfdcan, uint32_t RxFifo0ITs)
{
    if ((RxFifo0ITs & FDCAN_IT_RX_FIFO0_NEW_MESSAGE) != 0)
    {
        FDCAN_RxHeaderTypeDef rx_header;
        uint8_t rx_data[64];

        memset(&rx_header, 0, sizeof(rx_header));

        if (HAL_FDCAN_GetRxMessage(hfdcan, FDCAN_RX_FIFO0, &rx_header, rx_data) == HAL_OK)
        {
            uint8_t gw_channel = 0xFF;
            if (hfdcan->Instance == FDCAN1)
                gw_channel = 0;
            else if (hfdcan->Instance == FDCAN2)
                gw_channel = 1;
            else if (hfdcan->Instance == FDCAN3)
                gw_channel = 2;

            if (gw_channel != 0xFF)
            {
                CAN_Gateway_OnCANReceived(gw_channel, &rx_header, rx_data);
            }
        }
    }
}

/* USER CODE END 4 */

/**
 * @brief  This function is executed in case of error occurrence.
 * @retval None
 */
void Error_Handler(void)
{
    /* USER CODE BEGIN Error_Handler_Debug */
    /* User can add his own implementation to report the HAL error return state */
    __disable_irq();
    while (1)
    {
    }
    /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
 * @brief  Reports the name of the source file and the source line number
 *         where the assert_param error has occurred.
 * @param  file: pointer to the source file name
 * @param  line: assert_param error line source number
 * @retval None
 */
void assert_failed(uint8_t *file, uint32_t line)
{
    /* USER CODE BEGIN 6 */
    /* User can add his own implementation to report the file name and line number,
       ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
    /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
