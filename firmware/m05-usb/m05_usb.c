// M0.5 (FW v0.6): USB-HS デバイス + ブートローダ突入診断
//
// バルクOUT(EP5) コマンド "GBAC"+1B → バルクIN(EP4) で 8B 応答:
//   'P' ping     → {G,B,A,C,'!', ver_major, ver_minor, rst_flags}
//                  rst_flags = RCC->RSTSCKR >> 24（前回リセット要因。SFT=bit4相当）
//   'S' diag     → MODEKEYR のみで FLASH_STATR.MODE セットを試行（リセットせず）
//                  {G,B,A,C,'$', statr_hi(セット後), statr_hi(クリア後), 0}
//                  bit6 (=STATRのbit14) が立てばフラグ書き込み成功
//   'T' diag     → KEYR で FLASH 全体をアンロックしてから同上
//                  {G,B,A,C,'%', statr_hi(セット後), statr_hi(クリア後), 0}
//   'B' 突入     → MODEKEYRのみ方式でブートローダへ（リセットする）
//   'C' 突入     → KEYRアンロック込み方式でブートローダへ（リセットする）
//
// 既知のスタックバグ回避: USBHS_SendEndpoint は data ポインタを UEP2_TX_DMA に
// 書いてしまう([2-1]ハードコード)ため、EP4 の既定バッファ ENDPOINTS[4] に
// memcpy してから送る（UEP4_TX_DMA は初期化時から ENDPOINTS[4] を指している）。
#include "ch32fun.h"
#include <string.h>
#include "hsusb_v30x.h"
#include "hsusb_v30x.c"

#define FW_VERSION_MAJOR 0
#define FW_VERSION_MINOR 6

// FLASH_KEY1/KEY2/FLASH_CTLR_LOCK は ch32v30xhw.h で定義済み
#define FLASH_STATR_MODE (1 << 14)

static volatile int g_pending_cmd = 0;
static uint32_t g_boot_rstsckr; // 起動直後の RSTSCKR（リセット要因の記録）

void HandleGotEPComplete( struct _USBState * ctx, int ep )
{
	if( ep == 5 )
	{
		int len = USBHSD->RX_LEN;
		uint8_t * buf = ctx->ENDPOINTS[5];
		if( len >= 5 && buf[0]=='G' && buf[1]=='B' && buf[2]=='A' && buf[3]=='C' )
		{
			g_pending_cmd = buf[4];
		}
		// 受信再開（TOG_AUTO が立っているのでトグルは触らない）
		ctx->USBHS_Endp_Busy[5] = 0;
		USBHSD->UEP5_RX_DMA = (uintptr_t)ctx->ENDPOINTS[5];
		USBHSD->UEP5_MAX_LEN = 64;
		USBHSD->UEP5_RX_CTRL = ((USBHSD->UEP5_RX_CTRL) & ~USBHS_UEP_R_RES_MASK) | USBHS_UEP_R_RES_ACK;
	}
}

static void Reply8( uint8_t tag, uint8_t b5, uint8_t b6, uint8_t b7 )
{
	if( HSUSBCTX.USBHS_Endp_Busy[4] & 1 ) return;
	uint8_t * txbuf = HSUSBCTX.ENDPOINTS[4];
	txbuf[0]='G'; txbuf[1]='B'; txbuf[2]='A'; txbuf[3]='C';
	txbuf[4]=tag; txbuf[5]=b5; txbuf[6]=b6; txbuf[7]=b7;
	USBHS_SendEndpoint( 4, 8, txbuf ); // data は UEP2_TX_DMA に書かれるが無害。EP4 は既定 DMA(=txbuf) から送信
}

// MODE フラグセットを試行し (STATRセット後, クリア後) の上位バイトを返す
static void TryModeFlag( int unlock_flash, uint8_t * hi_after_set, uint8_t * hi_after_clear )
{
	if( unlock_flash )
	{
		FLASH->KEYR = FLASH_KEY1;
		FLASH->KEYR = FLASH_KEY2;
	}
	FLASH->MODEKEYR = FLASH_KEY1;
	FLASH->MODEKEYR = FLASH_KEY2;
	FLASH->STATR = FLASH_STATR_MODE;
	*hi_after_set = (uint8_t)( FLASH->STATR >> 8 );

	// 元に戻す（診断でフラグを立てっぱなしにしない）
	FLASH->MODEKEYR = FLASH_KEY1;
	FLASH->MODEKEYR = FLASH_KEY2;
	FLASH->STATR = 0;
	*hi_after_clear = (uint8_t)( FLASH->STATR >> 8 );

	if( unlock_flash )
		FLASH->CTLR |= FLASH_CTLR_LOCK;
}

static void EnterIspBootloader( int unlock_flash )
{
	USBHSD->CONTROL &= ~USBHS_UC_DEV_PU_EN; // USBソフト切断
	Delay_Ms( 50 );
	__disable_irq();

	if( unlock_flash )
	{
		FLASH->KEYR = FLASH_KEY1;
		FLASH->KEYR = FLASH_KEY2;
	}
	FLASH->MODEKEYR = FLASH_KEY1;
	FLASH->MODEKEYR = FLASH_KEY2;
	FLASH->STATR = FLASH_STATR_MODE;

	RCC->RSTSCKR |= RCC_RMVF; // 全リセットフラグをクリア（ブートローダはSFTRSTFのみを要求）
	NVIC_SystemReset();
	while(1);
}

int main()
{
	SystemInit();
	g_boot_rstsckr = RCC->RSTSCKR;

	funGpioInitAll();
	funPinMode( PA3, GPIO_CFGLR_OUT_10Mhz_PP );

	HSUSBSetup();

	uint32_t last_toggle = 0;
	int led = 0;

	while(1)
	{
		int cmd = g_pending_cmd;
		if( cmd )
		{
			g_pending_cmd = 0;
			uint8_t a, b;
			switch( cmd )
			{
			case 'P':
				Reply8( '!', FW_VERSION_MAJOR, FW_VERSION_MINOR, (uint8_t)( g_boot_rstsckr >> 24 ) );
				break;
			case 'S':
				TryModeFlag( 0, &a, &b );
				Reply8( '$', a, b, 0 );
				break;
			case 'T':
				TryModeFlag( 1, &a, &b );
				Reply8( '%', a, b, 0 );
				break;
			case 'B':
				EnterIspBootloader( 0 ); // 戻らない
				break;
			case 'C':
				EnterIspBootloader( 1 ); // 戻らない
				break;
			}
		}

		uint32_t now = SysTick->CNT;
		if( (uint32_t)( now - last_toggle ) >= Ticks_from_Ms( 500 ) )
		{
			last_toggle = now;
			led = !led;
			funDigitalWrite( PA3, led ? FUN_HIGH : FUN_LOW );
		}
	}
}
