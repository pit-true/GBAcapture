/* gbac_packet.c — ライン/音声パケットの解析 (FW v0.34, 全パケット 512 バイト = 中身 484 + 詰め物 28) */
#include "gbacap.h"

static uint16_t rd16(const uint8_t *p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* [0xA5][frameNo u8][lineNo u16LE][240 x u16LE] */
int gbac_line_parse(const uint8_t *pkt, size_t len, gbac_line_t *out) {
  if (pkt == 0 || out == 0) return 0;
  if (len < GBAC_PACKET_PAYLOAD) return 0;
  if (pkt[0] != GBAC_LINE_MAGIC) return 0;

  out->frame_no = pkt[1];
  out->line_no = rd16(pkt + 2);
  /* pkt は USB バッファ(4バイト境界)を指す前提。samples は pkt 内を指す。 */
  out->samples = (const uint16_t *)(const void *)(pkt + 4);
  return 1;
}

/* [0xA6][seq u8][pairCount u16LE][0] + pairCount x (u16 右, u16 左) + 0詰め */
int gbac_audio_parse(const uint8_t *pkt, size_t len, gbac_audio_t *out) {
  if (pkt == 0 || out == 0) return 0;
  if (len < GBAC_PACKET_PAYLOAD) return 0;
  if (pkt[0] != GBAC_AUDIO_MAGIC) return 0;

  uint16_t pairs = rd16(pkt + 2);
  if (pairs == 0) return 0;
  if ((size_t)pairs * 4u + 4u > GBAC_PACKET_PAYLOAD) return 0;

  out->seq = pkt[1];
  out->pairs = pairs;
  out->samples = (const uint16_t *)(const void *)(pkt + 4);
  return 1;
}
