// M0.5: USB-HS デバイス化 + 手放し書き込みループの完成
//
// - USB-HS バルクデバイスとして列挙 (VID 0x1209 / PID 0xC001 "GBAcapture")
// - EP5 (バルクOUT) でコマンド受信:
//     "GBAC" + 'B' → ISPブートローダへソフト突入（BOOTボタン不要で書き込み可能に）
//     "GBAC" + 'P' → EP4 (バルクIN) で 8B の PONG+バージョンを返す（疎通確認）
// - PA3 LED は 1Hz ハートビート（M0 の 5Hz と区別できる）
//
// ブートローダ突入方式: FLASH_STATR.MODE + ソフトリセット（docs/flashing.md 参照）
#include "ch32fun.h"
#include <string.h>
#include "hsusb_v30x.h"
#include "hsusb_v30x.c" // 例に倣い直接インクルード

#define FW_VERSION_MAJOR 0
#define FW_VERSION_MINOR 5

#define CMD_MAGIC_0 'G'
#define CMD_MAGIC_1 'B'
#define CMD_MAGIC_2 'A'
#define CMD_MAGIC_3 'C'
#define CMD_ENTER_BOOTLOADER 'B'
#define CMD_PING             'P'

static volatile int g_pending_cmd = 0;

// EP5 の受信バッファはスタック内蔵の HSUSBCTX.ENDPOINTS[5] (64B) をそのまま使う
void HandleGotEPComplete( struct _USBState * ctx, int ep )
{
	if( ep == 5 )
	{
		int len = USBHSD->RX_LEN;
		uint8_t * buf = ctx->ENDPOINTS[5];
		if( len >= 5 &&
			buf[0] == CMD_MAGIC_0 && buf[1] == CMD_MAGIC_1 &&
			buf[2] == CMD_MAGIC_2 && buf[3] == CMD_MAGIC_3 )
		{
			g_pending_cmd = buf[4];
		}

		// 受信再開（例の usbdevice_hs と同じ再アーム手順）
		ctx->USBHS_Endp_Busy[5] = 0;
		USBHSD->UEP5_RX_DMA = (uintptr_t)ctx->ENDPOINTS[5];
		USBHSD->UEP5_MAX_LEN = 64;
		USBHSD->UEP5_RX_CTRL ^= USBHS_UEP_R_TOG_DATA1;
		USBHSD->UEP5_RX_CTRL = ((USBHSD->UEP5_RX_CTRL) & ~USBHS_UEP_R_RES_MASK) | USBHS_UEP_R_RES_ACK;
	}
}

// WCH ISP ブートローダ (システムメモリ 0x1FFF8000) へソフト突入する。
// ブートローダは「SFTRSTF のみが立っている」ことを要求するため RMVF クリアが必須。
static void EnterIspBootloader( void )
{
	// USB をソフト切断してホスト側の再列挙を確実にする
	USBHSD->CONTROL &= ~USBHS_UC_DEV_PU_EN;
	Delay_Ms( 50 );

	__disable_irq();

	// FLASH_STATR.MODE(bit14)=1 → 次のリセットで BOOT 領域(システムメモリ)から起動
	FLASH->MODEKEYR = 0x45670123; // KEY1
	FLASH->MODEKEYR = 0xCDEF89AB; // KEY2
	FLASH->STATR = (1 << 14);

	// 全リセットフラグをクリアしてからソフトリセット
	RCC->RSTSCKR |= RCC_RMVF;
	NVIC_SystemReset();
	while(1);
}

int main()
{
	SystemInit();
	funGpioInitAll();
	funPinMode( PA3, GPIO_CFGLR_OUT_10Mhz_PP );

	HSUSBSetup();

	uint32_t last_toggle = 0;
	int led = 0;

	while(1)
	{
		if( g_pending_cmd == CMD_ENTER_BOOTLOADER )
		{
			EnterIspBootloader(); // 戻らない
		}
		if( g_pending_cmd == CMD_PING )
		{
			g_pending_cmd = 0;
			if( !( HSUSBCTX.USBHS_Endp_Busy[4] & 1 ) )
			{
				static const uint8_t pong[8] = {
					'G','B','A','C','!', FW_VERSION_MAJOR, FW_VERSION_MINOR, 0 };
				USBHS_SendEndpoint( 4, sizeof(pong), (uint8_t*)pong );
			}
		}

		// 1Hz ハートビート (SysTick は Delay_Ms 基準と同じクロック)
		uint32_t now = SysTick->CNT;
		if( (uint32_t)( now - last_toggle ) >= Ticks_from_Ms( 500 ) )
		{
			last_toggle = now;
			led = !led;
			funDigitalWrite( PA3, led ? FUN_HIGH : FUN_LOW );
		}
	}
}
