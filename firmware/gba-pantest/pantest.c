// 偽ころキャプチャ L/R判定用テストROM
// 左chのみ: 低い音(262Hz) + 画面が赤 → 無音1秒 → 右chのみ: 高い音(880Hz) + 画面が青 → 繰り返し
// ビューアで「赤いとき左スピーカーから低音」が鳴れば L/R は正しい
#include <stdint.h>

#define REG16(a) ( *(volatile uint16_t *)(a) )
#define REG_DISPCNT     REG16(0x04000000)
#define REG_VCOUNT      REG16(0x04000006)
#define REG_SOUNDCNT_L  REG16(0x04000080)
#define REG_SOUNDCNT_H  REG16(0x04000082)
#define REG_SOUNDCNT_X  REG16(0x04000084)
#define REG_SOUND1CNT_L REG16(0x04000060)
#define REG_SOUND1CNT_H REG16(0x04000062)
#define REG_SOUND1CNT_X REG16(0x04000064)

static void fill_screen(uint16_t color)
{
	volatile uint16_t *vram = (volatile uint16_t *)0x06000000;
	for (int i = 0; i < 240 * 160; i++)
		vram[i] = color;
}

static void wait_frames(int n)
{
	while (n--) {
		while (REG_VCOUNT >= 160) {}
		while (REG_VCOUNT < 160) {}
	}
}

static void tone(int freq_hz, uint16_t pan_bits)
{
	// pan_bits: bit12=ch1左, bit8=ch1右
	REG_SOUNDCNT_L = pan_bits | 0x77;             // 左右マスタ音量最大 + ch1パン
	REG_SOUND1CNT_L = 0;                          // スイープなし
	REG_SOUND1CNT_H = (0xF << 12) | (2 << 6);     // 音量15, デューティ50%
	REG_SOUND1CNT_X = 0x8000 | (2048 - 131072 / freq_hz);
}

static void silence(void)
{
	REG_SOUNDCNT_L = 0x77; // パン全オフ
}

int main(void)
{
	REG_SOUNDCNT_X = 0x80;  // サウンドマスタ有効
	REG_SOUNDCNT_H = 0x02;  // PSG音量100%
	REG_DISPCNT = 0x0403;   // Mode3 + BG2

	for (;;) {
		fill_screen(0x001F);          // 赤 (BGR555)
		tone(262, 1 << 12);           // 左のみ・低い音
		wait_frames(180);
		silence();
		fill_screen(0x0000);
		wait_frames(60);

		fill_screen(0x7C00);          // 青
		tone(880, 1 << 8);            // 右のみ・高い音
		wait_frames(180);
		silence();
		fill_screen(0x0000);
		wait_frames(60);
	}
}
