// M0: 開発基盤の確立用ファームウェア
// - PA3 の LED を 5Hz で点滅（書き込み成功が目視でわかる）
// 次段 (M0.5) で USB ベンダーコマンドによるブートローダ突入を追加する
#include "ch32fun.h"

int main(void)
{
	SystemInit();

	RCC->APB2PCENR |= RCC_APB2Periph_GPIOA;

	// PA3: push-pull 出力 (LED)
	GPIOA->CFGLR &= ~((uint32_t)0x0F << (4 * 3));
	GPIOA->CFGLR |= (uint32_t)(GPIO_Speed_10MHz | GPIO_CNF_OUT_PP) << (4 * 3);

	while (1)
	{
		GPIOA->BSHR = (1 << 3);        // LED ON
		Delay_Ms(100);
		GPIOA->BSHR = (1 << (16 + 3)); // LED OFF
		Delay_Ms(100);
	}
}
