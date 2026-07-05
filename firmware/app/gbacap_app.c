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

#define FW_VERSION 20     // v0.20: サーキュラDMAストリーミング + EP1/3/4分散送信
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

// M2 ラインキャプチャの状態（実体・詳細は下方の M2 セクション）
#define CAP_MAX_LINES 32
static volatile uint8_t  g_cap_state;   // 0=idle 1=SPS待ち 2=キャプチャ中 3=完了
static volatile uint16_t g_cap_start, g_cap_count;
static volatile uint16_t g_cap_line;    // 現在のGBAライン番号（SPSで0）
static volatile uint16_t g_cap_filled;
static void CapArmDma( int slot );

// M3 ストリーミング (v0.20 サーキュラDMA方式):
// - 'S' で DMA をサーキュラモード(7680サンプル=32ライン)で1回だけアーム
// - DCK が可視区間しか動かないので、サンプル総数はそのままピクセル位置
// - VBLANK(約5ms)のDMA停止をメインループで検出 → その位置=ライン0先頭(算術で確定)
// - 割り込みタイミングがデータ経路に入らないため水平ジッタが原理的に出ない
static volatile uint8_t g_stream;       // 1=ストリーミング中
#define STR_RING_SAMPLES ( CAP_MAX_LINES * 240 ) // 7680
static uint32_t g_st_total;      // 取り込んだサンプル総数
static uint32_t g_st_lastcur;    // 前回のDMA位置
static uint32_t g_st_sent;       // 送信済みサンプル数
static uint32_t g_st_anchor;     // ライン0先頭のサンプル総数(フレームアンカー)
static uint32_t g_st_lastchange; // 最後にDMAが進んだ時刻
static uint8_t  g_st_anchored;   // アンカー有効
static uint8_t  g_st_install;    // 今の停止期間でアンカー済み
static uint8_t  g_st_frame;      // フレーム番号

void EXTI1_IRQHandler( void ) __attribute__((interrupt));
void EXTI1_IRQHandler( void )
{
	EXTI->INTFR = 1 << 1;
	g_lp_count++; // LP は計測のみ（キャプチャは SPL 駆動）
}

// PA2 の実信号は SPL（可視ライン開始パルス、160回/フレーム。VBLANK中は約5ms休止）。
// 「前回パルスからのギャップが長い → 次はライン0」でフレーム同期する。
static uint32_t g_spl_last;
static volatile uint16_t g_spl_line; // 現在の可視ライン番号 (0-159)

void EXTI2_IRQHandler( void ) __attribute__((interrupt));
void EXTI2_IRQHandler( void )
{
	EXTI->INTFR = 1 << 2;
	g_sps_count++; // 名前は歴史的経緯。実体は SPL のパルス数

	uint32_t now = SysTick->CNT;
	uint32_t gap = now - g_spl_last;
	g_spl_last = now;
	// ライン周期 ≈73µs。150µs 超のギャップは VBLANK 明け = ライン0
	int frame_start = ( gap > ( Ticks_from_Ms( 1 ) * 150 ) / 1000 );

	if( frame_start )
		g_spl_line = 0;
	else if( g_spl_line < 227 )
		g_spl_line++;

	// ストリーミング中はSPL割り込みをデータ経路に使わない
	// （サーキュラDMA + VBLANK休止検出で全て賄う。ジッタ源を排除）
	if( g_stream )
		return;

	if( g_cap_state == 1 && frame_start )
	{
		g_cap_filled = 0;
		g_cap_state = 2;
	}
	if( g_cap_state == 2 )
	{
		uint16_t line = g_spl_line;
		// 直前のラインが範囲内なら、そのDMA転送(240サンプル)は完了している
		// (frame_start の場合の直前ライン = 前フレームの159)
		uint16_t prev = frame_start ? 159 : ( line == 0 ? 0xFFFF : (uint16_t)( line - 1 ) );
		if( prev != 0xFFFF
			&& prev >= g_cap_start && prev < (uint16_t)( g_cap_start + g_cap_count ) )
			g_cap_filled++;
		if( g_cap_filled >= g_cap_count )
		{
			DMA1_Channel5->CFGR &= ~DMA_CFGR1_EN;
			DMA1_Channel7->CFGR &= ~DMA_CFGR1_EN;
			g_cap_state = 3;
		}
		else if( line >= g_cap_start && line < (uint16_t)( g_cap_start + g_cap_count ) )
		{
			CapArmDma( line - g_cap_start ); // このラインの 240 DCK を取り込む
		}
	}
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

// --- M2: DMAラインキャプチャ ---
// DCK(PA0=TI1) の負エッジごとに:
//   CC1イベント → DMA1_CH5 が GPIOB->INDR(16bit) をラインバッファへ
//   CC2イベント(同じTI1をIC2に写像) → DMA1_CH7 が GPIOC->INDR下位バイトへ
//   (PC4/PC5 = 赤の下位2bit)
static uint16_t g_capbuf[CAP_MAX_LINES][240];
static uint8_t  g_capbuf_c[CAP_MAX_LINES][240];
static uint16_t g_dbg_last_cntr; // 診断: 直近キャプチャの最終スロットDMA残量

static void CapArmDma( int slot )
{
	DMA1_Channel5->CFGR &= ~DMA_CFGR1_EN;
	DMA1_Channel5->PADDR = (uintptr_t)&GPIOB->INDR;
	DMA1_Channel5->MADDR = (uintptr_t)g_capbuf[slot];
	DMA1_Channel5->CNTR = 240;
	DMA1_Channel5->CFGR = DMA_Priority_VeryHigh | DMA_MemoryDataSize_HalfWord
	                    | DMA_PeripheralDataSize_HalfWord | DMA_MemoryInc_Enable
	                    | DMA_CFGR1_EN;

	DMA1_Channel7->CFGR &= ~DMA_CFGR1_EN;
	DMA1_Channel7->PADDR = (uintptr_t)&GPIOC->INDR;
	DMA1_Channel7->MADDR = (uintptr_t)g_capbuf_c[slot];
	DMA1_Channel7->CNTR = 240;
	DMA1_Channel7->CFGR = DMA_Priority_VeryHigh | DMA_MemoryDataSize_Byte
	                    | DMA_PeripheralDataSize_HalfWord | DMA_MemoryInc_Enable
	                    | DMA_CFGR1_EN;
}

// TIM2 を「DCK負エッジで入力キャプチャ→DMA要求」モードにする（キャプチャ用）
static void Tim2ConfigCapture( void )
{
	TIM2->CTLR1 = 0;
	TIM2->SMCFGR = 0;                        // 内部クロック
	TIM2->CHCTLR1 = 0x01 | (0x02 << 8);      // CC1S=01(TI1), CC2S=10(TI1をIC2へ)
	TIM2->CCER = TIM_CC1E | TIM_CC1P | TIM_CC2E | TIM_CC2P; // 両ch負エッジ
	TIM2->DMAINTENR = TIM_CC1DE | TIM_CC2DE; // CC1→DMA1_CH5, CC2→DMA1_CH7
	TIM2->ATRLR = 0xFFFF;
	TIM2->CTLR1 = TIM_CEN;
}

// TIM2 を周波数計測用（DCK外部クロックカウント）に戻す
static void Tim2ConfigCount( void )
{
	TIM2->CTLR1 = 0;
	TIM2->CCER = 0;
	TIM2->CHCTLR1 = 0x01;
	TIM2->SMCFGR = TIM_TS_2 | TIM_TS_0 | TIM_SMS;
	TIM2->ATRLR = 0xFFFF;
	TIM2->DMAINTENR = TIM_UIE;
	TIM2->CTLR1 = TIM_CEN;
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

// EP4(バルクIN)送信。専用512Bバッファ経由（内蔵ENDPOINTS[4]は64Bしかないため）。
// USBHS_SendEndpoint の dataポインタは常にUEP2_TX_DMAへ書かれるスタックバグがあるので
// UEP4_TX_DMA を毎回自分で設定する。
static uint8_t g_txpkt[512];
static int SendEp4( const uint8_t * data, int len )
{
	uint32_t t0 = SysTick->CNT;
	while( HSUSBCTX.USBHS_Endp_Busy[4] & 1 )
		if( (uint32_t)( SysTick->CNT - t0 ) > Ticks_from_Ms( 500 ) ) return -1;
	if( data != g_txpkt )
		memcpy( g_txpkt, data, len );
	USBHSD->UEP4_TX_DMA = (uintptr_t)g_txpkt;
	USBHS_SendEndpoint( 4, len, 0 );
	return 0;
}

static void Reply8( uint8_t tag, uint8_t b5, uint8_t b6, uint8_t b7 )
{
	uint8_t p[8] = { 'G','B','A','C', tag, b5, b6, b7 };
	SendEp4( p, 8 );
}

// 'R': 生ピン診断。PA0/PA1/PA2 と GPIOB をタイトループで大量サンプリングし、
// エッジ数・Hデューティ・GPIOBの活動ビットを返す。
// 「信号が本当に来ているか」「浮いてノイズを拾っているだけか」を切り分ける。
static void ReplyRawDiag( void )
{
	if( HSUSBCTX.USBHS_Endp_Busy[4] & 1 ) return;

	const uint32_t LOOPS = 1000000;
	uint32_t trans[3] = {0,0,0}, high[3] = {0,0,0};
	uint32_t b_trans = 0, c_trans = 0;
	uint16_t b_bit_trans[16] = {0};
	uint16_t b_or = 0, b_and = 0xFFFF;
	uint8_t c_or = 0, c_and = 0xFF;
	uint32_t prev_a = GPIOA->INDR & 7;
	uint16_t prev_b = (uint16_t)GPIOB->INDR;
	uint8_t prev_c = (uint8_t)( GPIOC->INDR >> 4 ) & 3; // PC4/PC5 → bit0/1

	for( uint32_t i = 0; i < LOOPS; i++ )
	{
		uint32_t a = GPIOA->INDR & 7;
		uint16_t b = (uint16_t)GPIOB->INDR;
		uint8_t c = (uint8_t)( GPIOC->INDR >> 4 ) & 3;
		uint32_t chg = a ^ prev_a;
		if( chg & 1 ) trans[0]++;
		if( chg & 2 ) trans[1]++;
		if( chg & 4 ) trans[2]++;
		if( a & 1 ) high[0]++;
		if( a & 2 ) high[1]++;
		if( a & 4 ) high[2]++;
		uint16_t bchg = b ^ prev_b;
		if( bchg )
		{
			b_trans++;
			for( int k = 0; k < 16; k++ )
				if( ( bchg >> k ) & 1 && b_bit_trans[k] != 0xFFFF )
					b_bit_trans[k]++;
		}
		if( c != prev_c ) c_trans++;
		b_or |= b; b_and &= b;
		c_or |= c; c_and &= c;
		prev_a = a;
		prev_b = b;
		prev_c = c;
	}

	// --- プル切り分けテスト ---
	// 内部プル(約30-50kΩ)で引っ張って読み値が変わる = 外部が駆動していない(浮き)
	// 変わらない = 外部が強く駆動している(または強い外部プル)
	uint8_t a_pd, a_pu;   // PA0-2: プルダウン時/プルアップ時の読み値(bit0-2)
	uint16_t b_pd, b_pu;  // GPIOB 全体

	// 入力プルモードへ (CNF=10, ODR=0でPD/1でPU)
	funPinMode( PA0, GPIO_CFGLR_IN_PUPD );
	funPinMode( PA1, GPIO_CFGLR_IN_PUPD );
	funPinMode( PA2, GPIO_CFGLR_IN_PUPD );
	for( int p = 0; p < 16; p++ )
		if( 0xFF3B & (1 << p) )
			funPinMode( (PB0 + p), GPIO_CFGLR_IN_PUPD );

	GPIOA->OUTDR &= ~7;      // プルダウン
	GPIOB->OUTDR &= ~0xFF3B;
	Delay_Ms( 2 );
	a_pd = GPIOA->INDR & 7;
	b_pd = (uint16_t)GPIOB->INDR;

	GPIOA->OUTDR |= 7;       // プルアップ
	GPIOB->OUTDR |= 0xFF3B;
	Delay_Ms( 2 );
	a_pu = GPIOA->INDR & 7;
	b_pu = (uint16_t)GPIOB->INDR;

	// フローティングに戻す
	funPinMode( PA0, GPIO_CFGLR_IN_FLOAT );
	funPinMode( PA1, GPIO_CFGLR_IN_FLOAT );
	funPinMode( PA2, GPIO_CFGLR_IN_FLOAT );
	for( int p = 0; p < 16; p++ )
		if( 0xFF3B & (1 << p) )
			funPinMode( (PB0 + p), GPIO_CFGLR_IN_FLOAT );

	uint8_t rep[88];
	uint8_t * t = rep;
	t[0]='G'; t[1]='B'; t[2]='A'; t[3]='C'; t[4]='R'; t[5]=a_pd; t[6]=a_pu; t[7]=0;
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
	memcpy( t + 44, &b_pd, 2 );      // GPIOB プルダウン時
	memcpy( t + 46, &b_pu, 2 );      // GPIOB プルアップ時
	memcpy( t + 48, b_bit_trans, 32 ); // GPIOB ビット別エッジ数 (u16×16)
	t[80] = c_or; t[81] = c_and;     // PC4/PC5 (bit0=PC4, bit1=PC5)
	memcpy( t + 82, &c_trans, 4 );   // GPIOC 変化回数
	SendEp4( rep, 88 );
}

// 'A': PA0(DCK)/PA1(LP)/PA2(SPS)/PC4(LDR1)/PC5(LDR2) のアナログ電圧を測る。
// 各ch 16回サンプルの min/max (ADC生値 0-4095, Vref=3.3V) を返す。
// GND浮きなら全ピンがVDD付近に張り付き、本物のGBAロジックHなら~2.9V/3.3*4095≈3600
// V30x(F1型)のADC: SWSTART をトリガ源に選ばないと変換が始まらない
// (ch32fun の funAnalogInit/Read はこれを設定せず無限ループする → 自前実装)
static void AdcInit( void )
{
	RCC->APB2PCENR |= RCC_APB2Periph_ADC1;
	RCC->CFGR0 |= RCC_ADCPRE_DIV8; // 144/8=18MHz (診断用途なので許容)
	ADC1->CTLR1 = 0;
	ADC1->CTLR2 = ADC_ADON | ADC_EXTSEL | ADC_EXTTRIG; // EXTSEL=111: SWSTART
	Delay_Ms( 1 );
	ADC1->CTLR2 |= ADC_RSTCAL; while( ADC1->CTLR2 & ADC_RSTCAL );
	ADC1->CTLR2 |= ADC_CAL;    while( ADC1->CTLR2 & ADC_CAL );
	ADC1->SAMPTR2 = 0x2DB6DB6D; // 全ch サンプル時間 0b101
	ADC1->SAMPTR1 = 0x00B6DB6D;
	ADC1->RSQR1 = 0; // 変換1回
}

static uint16_t AdcRead( uint8_t ch )
{
	ADC1->RSQR3 = ch;
	ADC1->CTLR2 |= ADC_SWSTART;
	uint32_t t0 = SysTick->CNT;
	while( !( ADC1->STATR & ADC_EOC ) )
		if( (uint32_t)( SysTick->CNT - t0 ) > Ticks_from_Ms( 10 ) ) return 0xFFFF;
	return (uint16_t)ADC1->RDATAR;
}

static void ReplyAdc( void )
{
	static const uint8_t chans[5] = { 0, 1, 2, 14, 15 }; // ADC-IN番号
	uint16_t vmin[5], vmax[5];

	AdcInit();
	// 対象ピンをアナログ入力に
	funPinMode( PA0, GPIO_CFGLR_IN_ANALOG );
	funPinMode( PA1, GPIO_CFGLR_IN_ANALOG );
	funPinMode( PA2, GPIO_CFGLR_IN_ANALOG );
	funPinMode( PC4, GPIO_CFGLR_IN_ANALOG );
	funPinMode( PC5, GPIO_CFGLR_IN_ANALOG );

	for( int c = 0; c < 5; c++ )
	{
		vmin[c] = 0xFFFF; vmax[c] = 0;
		for( int i = 0; i < 16; i++ )
		{
			uint16_t v = AdcRead( chans[c] );
			if( v < vmin[c] ) vmin[c] = v;
			if( v > vmax[c] ) vmax[c] = v;
		}
	}

	// デジタル入力へ戻す
	funPinMode( PA0, GPIO_CFGLR_IN_FLOAT );
	funPinMode( PA1, GPIO_CFGLR_IN_FLOAT );
	funPinMode( PA2, GPIO_CFGLR_IN_FLOAT );
	funPinMode( PC4, GPIO_CFGLR_IN_FLOAT );
	funPinMode( PC5, GPIO_CFGLR_IN_FLOAT );

	uint8_t rep[28];
	rep[0]='G'; rep[1]='B'; rep[2]='A'; rep[3]='C'; rep[4]='A'; rep[5]=0; rep[6]=0; rep[7]=0;
	for( int c = 0; c < 5; c++ )
	{
		memcpy( rep + 8 + c * 4, &vmin[c], 2 );
		memcpy( rep + 10 + c * 4, &vmax[c], 2 );
	}
	SendEp4( rep, 28 );
}

static void ReplyFreq( void )
{
	uint8_t rep[20];
	uint8_t * t = rep;
	t[0]='G'; t[1]='B'; t[2]='A'; t[3]='C'; t[4]='F'; t[5]=0; t[6]=0; t[7]=0;
	memcpy( t + 8,  &g_dck_per_s, 4 );
	memcpy( t + 12, &g_lp_per_s, 4 );
	memcpy( t + 16, &g_sps_per_s, 4 );
	SendEp4( rep, 20 );
}

// ストリーミング送出: EP4/EP1/EP3 のうち空いているエンドポイントで1ライン送る。
// 1EPあたり実効~3.3MB/s(NAK後のホスト再ポーリングがマイクロフレーム単位のため)
// なので3本に分散して 60fps に必要な ~6.9MB/s を確保する。
// cbase はサーキュラバッファ内のサンプル開始位置 (240の倍数なのでライン跨ぎなし)
static const uint8_t k_stream_eps[3] = { 4, 1, 3 };
static uint8_t g_eptx[3][484];
static int TrySendLine( uint8_t frame, uint16_t line_no, uint32_t cbase )
{
	const uint16_t * bsrc = (const uint16_t *)g_capbuf + cbase;
	const uint8_t * csrc = (const uint8_t *)g_capbuf_c + cbase;
	for( int k = 0; k < 3; k++ )
	{
		int ep = k_stream_eps[k];
		if( HSUSBCTX.USBHS_Endp_Busy[ep] & 1 ) continue;
		uint8_t * pkt = g_eptx[k];
		pkt[0] = 0xA5;
		pkt[1] = frame;
		memcpy( pkt + 2, &line_no, 2 );
		uint16_t * dst = (uint16_t *)( pkt + 4 );
		for( int j = 0; j < 240; j++ )
			dst[j] = bsrc[j] | ( ( csrc[j] & 0x10 ) >> 2 ) | ( ( csrc[j] & 0x20 ) << 1 );
		// UEPn_TX_DMA は UEP1 から連続配置
		( (volatile uint32_t *)&USBHSD->UEP1_TX_DMA )[ep - 1] = (uintptr_t)pkt;
		USBHS_SendEndpoint( ep, 484, 0 );
		return 0;
	}
	return -1;
}

// ラインパケット v3 (484B): [0xA5][frame][line u16][240×u16]
// u16 の空きビットに赤下位2bitを合成する: bit2=LDR1(赤bit0), bit6=LDR2(赤bit1)
// (GPIOB実配線で bit2/6/7 は未使用。これで1ライン=バルク1パケットに収まり帯域半減)
static void BuildLinePacket( uint8_t * pkt, uint8_t frame, uint16_t line_no, int slot )
{
	pkt[0] = 0xA5;
	pkt[1] = frame;
	memcpy( pkt + 2, &line_no, 2 );
	const uint16_t * b = g_capbuf[slot];
	const uint8_t * c = g_capbuf_c[slot];
	uint16_t * dst = (uint16_t *)( pkt + 4 );
	for( int j = 0; j < 240; j++ )
		dst[j] = b[j] | ( ( c[j] & 0x10 ) >> 2 ) | ( ( c[j] & 0x20 ) << 1 );
}

// 'L': ラインキャプチャ。"GBAC"+'L'+pad1+u16開始ライン+u16ライン数(≤32)
// 成功: ラインパケットv3 (484B) × ライン数 / 失敗: 8B応答 {..'l', status, filled, line}
static void HandleCaptureCmd( const uint8_t * cmd )
{
	uint16_t start, count;
	memcpy( &start, cmd + 6, 2 );
	memcpy( &count, cmd + 8, 2 );
	if( count == 0 || count > CAP_MAX_LINES || start + count > 160 )
	{
		Reply8( 'l', 1, 0, 0 );
		return;
	}

	RCC->AHBPCENR |= RCC_AHBPeriph_DMA1;
	g_cap_start = start;
	g_cap_count = count;
	Tim2ConfigCapture();
	g_cap_state = 1; // 次のSPSからキャプチャ開始

	uint32_t t0 = SysTick->CNT;
	while( g_cap_state != 3 && (uint32_t)( SysTick->CNT - t0 ) < Ticks_from_Ms( 100 ) );

	uint8_t done = ( g_cap_state == 3 );
	uint16_t filled = g_cap_filled, lastline = g_cap_line;
	g_dbg_last_cntr = (uint16_t)DMA1_Channel5->CNTR; // 最後にアームしたスロットの残転送数
	g_cap_state = 0;
	DMA1_Channel5->CFGR &= ~DMA_CFGR1_EN;
	DMA1_Channel7->CFGR &= ~DMA_CFGR1_EN;
	Tim2ConfigCount();

	if( !done )
	{
		Reply8( 'l', 2, (uint8_t)filled, (uint8_t)lastline );
		return;
	}

	static uint8_t pkt[484];
	uint8_t frame = (uint8_t)g_sps_count;
	for( int i = 0; i < count; i++ )
	{
		BuildLinePacket( pkt, frame, start + i, i );
		// 注意: g_txpkt に直接組み立ててはならない（前パケット送信中に上書きされ
		// 全パケットが1本ズレるバグの原因だった）。ステージング経由で送る
		if( SendEp4( pkt, 484 ) ) return;
	}
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

	// ハング自動復旧: IWDG 約6.5秒。メインループが固まるとリセット→IAP→APPで復帰
	// (純正ISP ROMもIWDGを自前で管理するので救済経路とは干渉しない)
	IWDG->CTLR = 0x5555; IWDG->PSCR = 4;     // 40kHz/64
	IWDG->CTLR = 0x5555; IWDG->RLDR = 0xFFF; // ~6.5s
	IWDG->CTLR = 0xCCCC;                     // 開始

	uint32_t last_toggle = 0;
	uint32_t last_meas = 0;
	uint32_t prev_dck = 0, prev_lp = 0, prev_sps = 0;
	int led = 0;

	while(1)
	{
		IWDG->CTLR = 0xAAAA; // 餌やり

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
			case 'A':
				ReplyAdc();
				break;
			case 'D': // 診断: 直近 'L' の最終スロットDMA残量
				Reply8( 'd', (uint8_t)( g_dbg_last_cntr & 0xFF ),
				        (uint8_t)( g_dbg_last_cntr >> 8 ), 0 );
				break;
			case 'L':
				HandleCaptureCmd( (const uint8_t *)HSUSBCTX.ENDPOINTS[5] );
				break;
			case 'S': // ストリーミング開始 (サーキュラDMAを1回だけアーム)
				RCC->AHBPCENR |= RCC_AHBPeriph_DMA1;
				Tim2ConfigCapture();
				DMA1_Channel5->CFGR &= ~DMA_CFGR1_EN;
				DMA1_Channel5->PADDR = (uintptr_t)&GPIOB->INDR;
				DMA1_Channel5->MADDR = (uintptr_t)g_capbuf;
				DMA1_Channel5->CNTR = STR_RING_SAMPLES;
				DMA1_Channel5->CFGR = DMA_Priority_VeryHigh | DMA_MemoryDataSize_HalfWord
				                    | DMA_PeripheralDataSize_HalfWord | DMA_MemoryInc_Enable
				                    | DMA_Mode_Circular | DMA_CFGR1_EN;
				DMA1_Channel7->CFGR &= ~DMA_CFGR1_EN;
				DMA1_Channel7->PADDR = (uintptr_t)&GPIOC->INDR;
				DMA1_Channel7->MADDR = (uintptr_t)g_capbuf_c;
				DMA1_Channel7->CNTR = STR_RING_SAMPLES;
				DMA1_Channel7->CFGR = DMA_Priority_VeryHigh | DMA_MemoryDataSize_Byte
				                    | DMA_PeripheralDataSize_HalfWord | DMA_MemoryInc_Enable
				                    | DMA_Mode_Circular | DMA_CFGR1_EN;
				g_stream = 1;
				break;
			case 'X': // ストリーミング停止
				g_stream = 0;
				Delay_Ms( 2 );
				DMA1_Channel5->CFGR &= ~DMA_CFGR1_EN;
				DMA1_Channel7->CFGR &= ~DMA_CFGR1_EN;
				Tim2ConfigCount();
				break;
			case 'U':
				EnterIapUpdater(); // 戻らない
				break;
			}
		}

		// ストリーミング消費側: DMA位置を監視し、VBLANK休止でフレーム同期、
		// 完成ラインを EP1/3/4 の空いているエンドポイントへ送出する
		if( g_stream )
		{
			uint32_t tnow = SysTick->CNT;
			uint32_t cur = STR_RING_SAMPLES - DMA1_Channel5->CNTR;
			uint32_t delta = ( cur + STR_RING_SAMPLES - g_st_lastcur ) % STR_RING_SAMPLES;
			if( delta )
			{
				g_st_total += delta;
				g_st_lastcur = cur;
				g_st_lastchange = tnow;
				g_st_install = 0;
			}
			else if( !g_st_install
				&& (uint32_t)( tnow - g_st_lastchange ) > ( Ticks_from_Ms( 1 ) / 4 ) )
			{
				// 250µs以上DMAが停止 = VBLANK (H-Blankは~16µsなので誤検出しない)
				// → 次に来るサンプルがライン0の先頭。ここがフレーム境界
				g_st_anchor = g_st_total;
				g_st_sent = g_st_total; // フレーム途中の未送信分は破棄
				g_st_anchored = 1;
				g_st_install = 1;
				g_st_frame++;
			}

			while( g_st_anchored && g_st_total - g_st_sent >= 240 )
			{
				// リング一周分以上遅れたら(上書きされたら)このフレームは諦める
				if( g_st_total - g_st_sent > STR_RING_SAMPLES - 240 )
				{
					g_st_anchored = 0;
					break;
				}
				uint32_t line = ( g_st_sent - g_st_anchor ) / 240;
				if( line >= 160 )
				{
					g_st_anchored = 0; // 次のVBLANKで再アンカー
					break;
				}
				if( TrySendLine( g_st_frame, (uint16_t)line, g_st_sent % STR_RING_SAMPLES ) )
					break; // 全EP使用中 → 次周で再試行
				g_st_sent += 240;
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
