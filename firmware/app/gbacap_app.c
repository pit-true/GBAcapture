// GBAcapture APP ファームウェア (0x08004000 に配置、IAP からジャンプされる)
//
// バルクOUT(EP5) コマンド "GBAC"+1B → バルクIN(EP4) で応答:
//   'P' ping → {G,B,A,C,'!', 0x00(=APPモード), ver, rst_flags} (8B)
//   'F' 計測 → {G,B,A,C,'F',0,0,0, dck/s u32, lp/s u32, sps/s u32} (20B)
//   'U' 更新 → BKPマジックを書いてソフトリセット → IAP更新モードへ（手放し書き込み）
//
// 既知のスタックバグ回避: USBHS_SendEndpoint は data ポインタを UEP2_TX_DMA に
// 書いてしまう([2-1]ハードコード)ため、EP4 の既定バッファ ENDPOINTS[4] に
// memcpy してから送る（UEP4_TX_DMA は初期化時から ENDPOINTS[4] を指している）。
#include "ch32fun.h"
#include <string.h>
#include "hsusb_v30x.h"
#include "hsusb_v30x.c"

#define FW_VERSION 10     // v0.10: 'R' 生ピン診断を追加
#define IAP_MAGIC  0x424C // "BL" — firmware/iap/iap.c と一致させること

// --- M1: GBA信号計測 ---
// DCK(PA0): 4.19MHz級バースト → TIM2 外部クロックモード(TI1FP1)でハードウェアカウント
//           16bitタイマなので更新割り込みで上位ワードを延長
// LP(PA1):  ~13.6kHz → EXTI1 割り込みカウント
// SPS(PA2): ~59.7Hz  → EXTI2 割り込みカウント
static volatile uint32_t g_tim2_ovf;   // TIM2 オーバーフロー回数
static volatile uint32_t g_lp_count;
static volatile uint32_t g_sps_count;

// 1秒窓の確定値（'F' コマンドで返す）
static uint32_t g_dck_per_s, g_lp_per_s, g_sps_per_s;

void TIM2_IRQHandler( void ) __attribute__((interrupt));
void TIM2_IRQHandler( void )
{
	if( TIM2->INTFR & TIM_UIF )
	{
		TIM2->INTFR = (uint16_t)~TIM_UIF;
		g_tim2_ovf++;
	}
}

void EXTI1_IRQHandler( void ) __attribute__((interrupt));
void EXTI1_IRQHandler( void )
{
	EXTI->INTFR = 1 << 1;
	g_lp_count++;
}

void EXTI2_IRQHandler( void ) __attribute__((interrupt));
void EXTI2_IRQHandler( void )
{
	EXTI->INTFR = 1 << 2;
	g_sps_count++;
}

static void SignalCountersInit( void )
{
	RCC->APB1PCENR |= RCC_APB1Periph_TIM2;
	RCC->APB2PCENR |= RCC_APB2Periph_GPIOA | RCC_APB2Periph_AFIO;

	// PA0/PA1/PA2 は入力フローティング（GBA側が駆動する）
	funPinMode( PA0, GPIO_CFGLR_IN_FLOAT );
	funPinMode( PA1, GPIO_CFGLR_IN_FLOAT );
	funPinMode( PA2, GPIO_CFGLR_IN_FLOAT );

	// TIM2: TI1(PA0) を外部クロックとしてカウント
	TIM2->CHCTLR1 = 0x01;              // CC1S=01: TI1 を IC1 に
	TIM2->SMCFGR = TIM_TS_2 | TIM_TS_0 // TS=101: TI1FP1
	             | TIM_SMS;            // SMS=111: 外部クロックモード1
	TIM2->ATRLR = 0xFFFF;
	TIM2->DMAINTENR = TIM_UIE;
	TIM2->CTLR1 = TIM_CEN;
	NVIC_EnableIRQ( TIM2_IRQn );

	// EXTI1(PA1) / EXTI2(PA2): 立ち上がりエッジ
	AFIO->EXTICR[0] &= ~( (0xF << 4) | (0xF << 8) ); // EXTI1/2 = PortA
	EXTI->RTENR |= (1 << 1) | (1 << 2);
	EXTI->INTENR |= (1 << 1) | (1 << 2);
	NVIC_EnableIRQ( EXTI1_IRQn );
	NVIC_EnableIRQ( EXTI2_IRQn );
}

static inline uint32_t Dck32( void )
{
	// オーバーフローカウンタと CNT を整合させて読む
	uint32_t ovf1, cnt;
	do {
		ovf1 = g_tim2_ovf;
		cnt = TIM2->CNT;
	} while( ovf1 != g_tim2_ovf );
	return ( ovf1 << 16 ) | cnt;
}

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

// 'R': 生ピン診断。PA0/PA1/PA2 と GPIOB をタイトループで大量サンプリングし、
// エッジ数・Hデューティ・GPIOBの活動ビットを返す。
// 「信号が本当に来ているか」「浮いてノイズを拾っているだけか」を切り分ける。
static void ReplyRawDiag( void )
{
	if( HSUSBCTX.USBHS_Endp_Busy[4] & 1 ) return;

	const uint32_t LOOPS = 1000000;
	uint32_t trans[3] = {0,0,0}, high[3] = {0,0,0};
	uint32_t b_trans = 0;
	uint16_t b_or = 0, b_and = 0xFFFF;
	uint32_t prev_a = GPIOA->INDR & 7;
	uint16_t prev_b = (uint16_t)GPIOB->INDR;

	for( uint32_t i = 0; i < LOOPS; i++ )
	{
		uint32_t a = GPIOA->INDR & 7;
		uint16_t b = (uint16_t)GPIOB->INDR;
		uint32_t chg = a ^ prev_a;
		if( chg & 1 ) trans[0]++;
		if( chg & 2 ) trans[1]++;
		if( chg & 4 ) trans[2]++;
		if( a & 1 ) high[0]++;
		if( a & 2 ) high[1]++;
		if( a & 4 ) high[2]++;
		if( b != prev_b ) b_trans++;
		b_or |= b;
		b_and &= b;
		prev_a = a;
		prev_b = b;
	}

	uint8_t * t = HSUSBCTX.ENDPOINTS[4];
	t[0]='G'; t[1]='B'; t[2]='A'; t[3]='C'; t[4]='R'; t[5]=0; t[6]=0; t[7]=0;
	memcpy( t + 8,  &trans[0], 4 );  // A0(DCK) エッジ数
	memcpy( t + 12, &trans[1], 4 );  // A1(LP)
	memcpy( t + 16, &trans[2], 4 );  // A2(SPS)
	memcpy( t + 20, &high[0], 4 );   // A0 Hだったサンプル数
	memcpy( t + 24, &high[1], 4 );
	memcpy( t + 28, &high[2], 4 );
	memcpy( t + 32, &b_trans, 4 );   // GPIOB が変化した回数
	memcpy( t + 36, &b_or, 2 );      // 一度でもHになったビット
	memcpy( t + 38, &b_and, 2 );     // 常にHだったビット
	memcpy( t + 40, &LOOPS, 4 );
	USBHS_SendEndpoint( 4, 44, t );
}

static void ReplyFreq( void )
{
	if( HSUSBCTX.USBHS_Endp_Busy[4] & 1 ) return;
	uint8_t * t = HSUSBCTX.ENDPOINTS[4];
	t[0]='G'; t[1]='B'; t[2]='A'; t[3]='C'; t[4]='F'; t[5]=0; t[6]=0; t[7]=0;
	memcpy( t + 8,  &g_dck_per_s, 4 );
	memcpy( t + 12, &g_lp_per_s, 4 );
	memcpy( t + 16, &g_sps_per_s, 4 );
	USBHS_SendEndpoint( 4, 20, t );
}

// IAP 更新モードへ移行する（手放し書き込みループ。docs/flashing.md）
// BKP レジスタにマジックを書いてソフトリセット → IAP がマジックを見て更新モードに留まる
__attribute__((noreturn)) static void EnterIapUpdater( void )
{
	RCC->APB1PCENR |= RCC_APB1Periph_PWR | RCC_APB1Periph_BKP;
	PWR->CTLR |= PWR_CTLR_DBP;
	BKP->DATAR1 = IAP_MAGIC;

	USBHSD->CONTROL &= ~USBHS_UC_DEV_PU_EN; // USBソフト切断で再列挙を確実に
	Delay_Ms( 50 );
	NVIC_SystemReset();
	while(1);
}

int main()
{
	SystemInit();
	g_boot_rstsckr = RCC->RSTSCKR;

	funGpioInitAll();
	funPinMode( PA3, GPIO_CFGLR_OUT_10Mhz_PP );

	SignalCountersInit();
	HSUSBSetup();

	uint32_t last_toggle = 0;
	uint32_t last_meas = 0;
	uint32_t prev_dck = 0, prev_lp = 0, prev_sps = 0;
	int led = 0;

	while(1)
	{
		int cmd = g_pending_cmd;
		if( cmd )
		{
			g_pending_cmd = 0;
			switch( cmd )
			{
			case 'P':
				Reply8( '!', 0x00 /* APPモード */, FW_VERSION, (uint8_t)( g_boot_rstsckr >> 24 ) );
				break;
			case 'F':
				ReplyFreq();
				break;
			case 'R':
				ReplyRawDiag();
				break;
			case 'U':
				EnterIapUpdater(); // 戻らない
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

		// 1秒窓で各信号のカウントを確定
		if( (uint32_t)( now - last_meas ) >= Ticks_from_Ms( 1000 ) )
		{
			last_meas = now;
			uint32_t dck = Dck32(), lp = g_lp_count, sps = g_sps_count;
			g_dck_per_s = dck - prev_dck;
			g_lp_per_s  = lp - prev_lp;
			g_sps_per_s = sps - prev_sps;
			prev_dck = dck; prev_lp = lp; prev_sps = sps;
		}
	}
}
