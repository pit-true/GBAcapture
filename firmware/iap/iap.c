// IAP (In-Application Programming) ブートローダ — 手放し書き込みループの心臓部
//
// フラッシュレイアウト (docs/flashing.md):
//   IAP: 0x08000000 (16KB, このFW自身。滅多に更新しない)
//   APP: 0x08004000 (112KB, 開発対象FW)
//
// 起動フロー:
//   1. BKP->DATAR1 == 0x424C ("BL") なら → マジックをクリアして IAP 更新モードに留まる
//   2. それ以外で APP 先頭が 0xFFFFFFFF でなければ → APP へジャンプ
//   3. APP が無ければ → IAP 更新モードに留まる
//
// 更新モードの USB コマンド (バルクOUT EP5、応答はバルクIN EP4):
//   'P' ping  → {G,B,A,C,'!', 0xAA(=IAPモード), IAP_VERSION, 0}
//   'E' 消去  → "GBAC"+'E'+pad3+u32サイズ → APP領域をページ(4KB)単位で消去 → {..'e',status,pages,0}
//   'W' 書込  → "GBAC"+'W'+pad1+u16長+u32オフセット+データ(≤256B,偶数長) → {..'w',status,0,0}
//   'V' 検証  → "GBAC"+'V'+pad3+u32サイズ → {..'v',0,0,0, u32 加算チェックサム} (12B応答)
//   'G' 起動  → ソフトリセット（マジック無し→APPへ）
//
// LED(PA3): 5Hz点滅 = IAP更新モード（APPは1Hz）
#include "ch32fun.h"
#include <string.h>
#include "hsusb_v30x.h"
#include "hsusb_v30x.c"

#define IAP_VERSION 1
#define IAP_MAGIC   0x424C  // "BL"
#define APP_BASE    0x08004000u
#define APP_MAX     ( 112 * 1024 )
#define PAGE_SIZE   4096u

static volatile int g_cmd_len = 0;
static uint8_t g_cmd[512];
static uint8_t g_rxbuf[512];

void HandleGotEPComplete( struct _USBState * ctx, int ep )
{
	if( ep == 5 )
	{
		int len = USBHSD->RX_LEN;
		if( len >= 5 && g_rxbuf[0]=='G' && g_rxbuf[1]=='B' && g_rxbuf[2]=='A' && g_rxbuf[3]=='C'
			&& g_cmd_len == 0 )
		{
			memcpy( g_cmd, g_rxbuf, len );
			g_cmd_len = len;
		}
		ctx->USBHS_Endp_Busy[5] = 0;
		USBHSD->UEP5_RX_DMA = (uintptr_t)g_rxbuf;
		USBHSD->UEP5_MAX_LEN = 512;
		USBHSD->UEP5_RX_CTRL = ((USBHSD->UEP5_RX_CTRL) & ~USBHS_UEP_R_RES_MASK) | USBHS_UEP_R_RES_ACK;
	}
}

static void ReplyN( const uint8_t * payload, int len )
{
	while( HSUSBCTX.USBHS_Endp_Busy[4] & 1 );
	uint8_t * t = HSUSBCTX.ENDPOINTS[4];
	memcpy( t, payload, len );
	USBHS_SendEndpoint( 4, len, t );
}

static void Reply8( uint8_t tag, uint8_t b5, uint8_t b6, uint8_t b7 )
{
	uint8_t p[8] = { 'G','B','A','C', tag, b5, b6, b7 };
	ReplyN( p, 8 );
}

// --- FLASH 自己書き換え (標準方式: 16bit書き込み / 4KBページ消去) ---

static void FlashUnlock( void )
{
	FLASH->KEYR = FLASH_KEY1;
	FLASH->KEYR = FLASH_KEY2;
}

static void FlashWait( void )
{
	while( FLASH->STATR & FLASH_STATR_BSY );
}

static void FlashErasePage( uint32_t addr )
{
	FlashWait();
	FLASH->CTLR |= FLASH_CTLR_PER;
	FLASH->ADDR = addr;
	FLASH->CTLR |= FLASH_CTLR_STRT;
	FlashWait();
	FLASH->CTLR &= ~FLASH_CTLR_PER;
}

static int FlashProgram( uint32_t addr, const uint8_t * data, int len )
{
	for( int i = 0; i < len; i += 2 )
	{
		uint16_t v = data[i] | ( data[i+1] << 8 );
		FLASH->CTLR |= FLASH_CTLR_PG;
		*(volatile uint16_t *)( addr + i ) = v;
		FlashWait();
		FLASH->CTLR &= ~FLASH_CTLR_PG;
		if( *(volatile uint16_t *)( addr + i ) != v ) return -1;
	}
	return 0;
}

static void ProcessCommand( void )
{
	switch( g_cmd[4] )
	{
	case 'P':
		Reply8( '!', 0xAA, IAP_VERSION, 0 );
		break;
	case 'E':
	{
		uint32_t size;
		memcpy( &size, g_cmd + 8, 4 );
		if( size == 0 || size > APP_MAX ) { Reply8( 'e', 1, 0, 0 ); break; }
		uint32_t pages = ( size + PAGE_SIZE - 1 ) / PAGE_SIZE;
		FlashUnlock();
		for( uint32_t p = 0; p < pages; p++ )
			FlashErasePage( APP_BASE + p * PAGE_SIZE );
		Reply8( 'e', 0, (uint8_t)pages, 0 );
		break;
	}
	case 'W':
	{
		uint16_t len;
		uint32_t offset;
		memcpy( &len, g_cmd + 6, 2 );
		memcpy( &offset, g_cmd + 8, 4 );
		if( len == 0 || ( len & 1 ) || len > 256 || offset + len > APP_MAX
			|| g_cmd_len < 12 + len )
		{
			Reply8( 'w', 1, 0, 0 );
			break;
		}
		FlashUnlock();
		int r = FlashProgram( APP_BASE + offset, g_cmd + 12, len );
		Reply8( 'w', r ? 2 : 0, 0, 0 );
		break;
	}
	case 'V':
	{
		uint32_t size, sum = 0;
		memcpy( &size, g_cmd + 8, 4 );
		if( size > APP_MAX ) size = APP_MAX;
		const uint8_t * p = (const uint8_t *)APP_BASE;
		for( uint32_t i = 0; i < size; i++ ) sum += p[i];
		uint8_t rep[12] = { 'G','B','A','C','v', 0, 0, 0 };
		memcpy( rep + 8, &sum, 4 );
		ReplyN( rep, 12 );
		break;
	}
	case 'G':
		Reply8( 'g', 0, 0, 0 );
		Delay_Ms( 20 );
		USBHSD->CONTROL &= ~USBHS_UC_DEV_PU_EN;
		Delay_Ms( 50 );
		NVIC_SystemReset(); // マジックは既にクリア済み → 再起動でAPPへ
		break;
	}
}

int main()
{
	// SystemInit 前（リセット直後の素の状態）で行き先を判定する。
	// こうすると APP はリセット直後同然の状態で起動できる
	RCC->APB1PCENR |= RCC_APB1Periph_PWR | RCC_APB1Periph_BKP;
	PWR->CTLR |= PWR_CTLR_DBP;
	uint16_t magic = BKP->DATAR1;
	uint32_t app_head = *(volatile uint32_t *)APP_BASE;

	if( magic != IAP_MAGIC && app_head != 0xFFFFFFFFu )
	{
		( (void (*)(void))APP_BASE )();
	}
	BKP->DATAR1 = 0; // マジック消費。以後は更新モードに留まる

	SystemInit();
	funGpioInitAll();
	funPinMode( PA3, GPIO_CFGLR_OUT_10Mhz_PP );

	HSUSBSetup();
	// EP5 受信バッファを 512B のものに差し替え（列挙後のバスリセットでも
	// HandleGotEPComplete 内で毎回再設定するため実用上問題ない）
	USBHSD->UEP5_RX_DMA = (uintptr_t)g_rxbuf;
	USBHSD->UEP5_MAX_LEN = 512;

	uint32_t last_toggle = 0;
	int led = 0;

	while(1)
	{
		if( g_cmd_len )
		{
			ProcessCommand();
			g_cmd_len = 0;
		}

		uint32_t now = SysTick->CNT;
		if( (uint32_t)( now - last_toggle ) >= Ticks_from_Ms( 100 ) ) // 5Hz点滅
		{
			last_toggle = now;
			led = !led;
			funDigitalWrite( PA3, led ? FUN_HIGH : FUN_LOW );
		}
	}
}
