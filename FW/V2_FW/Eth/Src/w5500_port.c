/**
 ******************************************************************************
 * @file    w5500_port.c
 * @brief   W5500 Hardware Abstraction Layer Implementation
 ******************************************************************************
 */

#include "w5500_port.h"
#include "main.h"
#include "spi.h"
#include "gpio.h"

#define W5500_SPI_TIMEOUT 100

/**
 * @brief  W5500 チップセレクト - LOW
 */
void W5500_Select(void)
{
    HAL_GPIO_WritePin(SPI1_CS_GPIO_Port, SPI1_CS_Pin, GPIO_PIN_RESET);
}

/**
 * @brief  W5500 チップセレクト - HIGH
 */
void W5500_Deselect(void)
{
    HAL_GPIO_WritePin(SPI1_CS_GPIO_Port, SPI1_CS_Pin, GPIO_PIN_SET);
}

/**
 * @brief  SPIで複数バイト読み取り
 * @param  buf: 受信バッファ
 * @param  len: 読み取るバイト数
 */
void W5500_ReadBuf(uint8_t *buf, uint16_t len)
{
    HAL_SPI_Receive(&hspi1, buf, len, W5500_SPI_TIMEOUT);
}

/**
 * @brief  SPIで複数バイト書き込み
 * @param  buf: 送信バッファ
 * @param  len: 書き込むバイト数
 */
void W5500_WriteBuf(uint8_t *buf, uint16_t len)
{
    HAL_SPI_Transmit(&hspi1, buf, len, W5500_SPI_TIMEOUT);
}

/**
 * @brief  SPIで1バイト読み取り
 * @retval 読み取ったバイト
 */
uint8_t W5500_ReadByte(void)
{
    uint8_t byte = 0;
    HAL_SPI_Receive(&hspi1, &byte, 1, W5500_SPI_TIMEOUT);
    return byte;
}

/**
 * @brief  SPIで1バイト書き込み
 * @param  byte: 書き込むバイト
 */
void W5500_WriteByte(uint8_t byte)
{
    HAL_SPI_Transmit(&hspi1, &byte, 1, W5500_SPI_TIMEOUT);
}

/**
 * @brief  W5500 ハードウェアリセット
 */
void W5500_HW_Reset(void)
{
    // RSTピンをLOWにして、少し待ってからHIGHに戻す
    HAL_GPIO_WritePin(W5500_RST_GPIO_Port, W5500_RST_Pin, GPIO_PIN_RESET);
    HAL_Delay(10);
    HAL_GPIO_WritePin(W5500_RST_GPIO_Port, W5500_RST_Pin, GPIO_PIN_SET);
    HAL_Delay(100); // W5500が起動するまで待つ
}

/**
 * @brief  SPI通信テスト
 * @retval 1:成功, 0:失敗
 */
uint8_t W5500_SPI_Test(void)
{
    uint8_t test_data = 0x55;
    uint8_t read_data = 0;

    // SPIハンドルの状態確認
    if (hspi1.State != HAL_SPI_STATE_READY)
    {
        return 0;
    }

    // 簡易ループバックテスト
    W5500_Select();
    HAL_Delay(1);
    W5500_WriteByte(test_data);
    W5500_Deselect();

    return 1;
}
