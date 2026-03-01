/**
 ******************************************************************************
 * @file    w5500_port.h
 * @brief   W5500 Hardware Abstraction Layer
 ******************************************************************************
 */

#ifndef W5500_PORT_H
#define W5500_PORT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* SPI読み書き関数 */
void W5500_Select(void);
void W5500_Deselect(void);
void W5500_ReadBuf(uint8_t* buf, uint16_t len);
void W5500_WriteBuf(uint8_t* buf, uint16_t len);
uint8_t W5500_ReadByte(void);
void W5500_WriteByte(uint8_t byte);

/* W5500ハードウェア初期化 */
void W5500_HW_Reset(void);

/* SPI通信テスト */
uint8_t W5500_SPI_Test(void);

#ifdef __cplusplus
}
#endif

#endif /* W5500_PORT_H */

