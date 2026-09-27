#ifndef __BL_PROTOCOL_H
#define __BL_PROTOCOL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* Wire protocol ---------------------------------------------------------------
 * Frame (host -> device and device -> host):
 *   [TYPE_OR_STATUS:1][LEN:4 LE][PAYLOAD:LEN][CRC32:4 LE]
 * CRC32 (zlib-compatible) covers TYPE_OR_STATUS + LEN + PAYLOAD.
 * USB CDC FS delivers at most 64 bytes per OUT packet, so frames are
 * reassembled through an internal ring buffer fed by the USB ISR.
 */

/* Host -> device command bytes */
#define BL_CMD_PING    0x01u
#define BL_CMD_ERASE   0x02u   /* payload: u32 LE image size                */
#define BL_CMD_WRITE   0x03u   /* payload: up to BL_MAX_PAYLOAD_SIZE bytes  */
#define BL_CMD_VERIFY  0x04u   /* payload: u32 LE crc32 of whole image      */
#define BL_CMD_JUMP    0x05u

/* Device -> host status bytes */
#define BL_STATUS_OK             0x00u
#define BL_STATUS_ERR_CRC        0x01u
#define BL_STATUS_ERR_BOUNDS     0x02u
#define BL_STATUS_ERR_FLASH      0x03u
#define BL_STATUS_ERR_VERIFY     0x04u
#define BL_STATUS_ERR_STATE      0x05u
#define BL_STATUS_ERR_UNKNOWN    0x06u
#define BL_STATUS_ERR_PARAM      0x07u

#define BL_MAX_PAYLOAD_SIZE   1024u

void    BL_ProtocolInit(void);
void    BL_OnUsbRx(const uint8_t *buf, uint32_t len);
void    BL_ProtocolPoll(void);

/* Vector table sanity check used before any jump into the application. */
uint8_t BL_VectorTableValid(void);

/* Validates the metadata record written after a successful VERIFY and
 * recomputes the CRC32 over the stored image. Returns non-zero when the
 * flashed application is intact. */
uint8_t BL_ImageIsValid(void);

/* CRC32 (zlib-compatible). Reset -> Update... -> Value. */
void     BL_Crc32Reset(void);
void     BL_Crc32Update(const uint8_t *data, uint32_t len);
uint32_t BL_Crc32Value(void);
uint32_t BL_Crc32Compute(const uint8_t *data, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* __BL_PROTOCOL_H */
