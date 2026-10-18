/* ppa_probe.h — PPA SRM の実機検証(段階 A の拡大前段を DMA に移せるかの調査) */
#pragma once

/* 1 枚だけ PPA で作って gbac_scale_ouyy の出力と全バイト比較し、
 * 一致数・先頭 24 バイトの 16 進・所要 ms を出す。コマンド 'k' 専用。 */
void gbac_ppa_probe(void);
