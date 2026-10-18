/* ppa_probe.c — PPA SRM の実機検証(段階 A の拡大前段を DMA に移せるかの調査)。
 * 録画経路は触らない。コマンド 'k' 専用。
 * r87 の実測: 並びは O_UYY_E_VYY でソフトと同じ。scale4 で 6.92ms/枚
 * (ソフト 19.08ms)。ずれるのは色の値と 4x の位相。ここで切り分ける。
 *   A: 白の縦線 1 本 → 4x が最近傍か、何画素ずれるか、ぼやけないか
 *   B: グラデーション → 4x4 ブロック内が均一でない場所を 10 件と mod4 分布
 *   C: 単色 6 色 → yuv_range / yuv_std の違いで SW の値にどれだけ近づくか */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "driver/ppa.h"
#include "gbacap.h"
#include "ppa_probe.h"

static const char *TAG = "ppaprobe";

#define PP_W   GBAC_WIDTH    /* 240 */
#define PP_H   GBAC_HEIGHT   /* 160 */
#define PP_SC  4
#define PP_OW  (PP_W * PP_SC)  /* 960 */
#define PP_OH  (PP_H * PP_SC)  /* 640 */
#define PP_LEN GBAC_OUYY_SIZE(PP_W, PP_H, PP_SC)  /* 921600 */
#define PP_STRIDE GBAC_OUYY_STRIDE(PP_W, PP_SC)   /* 1440 */
#define PP_S1LEN GBAC_OUYY_SIZE(PP_W, PP_H, 1)    /* 57600 */
#define PP_S1STRIDE GBAC_OUYY_STRIDE(PP_W, 1)     /* 360 */

/* 出力行の X の Y バイト(行頭からのバイトオフセット) */
#define YBYTE(row, X) ((row) + (size_t)((X) / 2) * 3 + 1 + ((X) & 1))
#define CBYTE(row, X) ((row) + (size_t)((X) / 2) * 3)

static ppa_client_handle_t s_cl;
static uint16_t *s_src;
static uint8_t *s_ppa;   /* PPA scale4 */
static uint8_t *s_p1;    /* PPA scale1(色変換のみ) */
static uint8_t *s_sw;    /* gbac_scale_ouyy scale4 */

static void fill_line(void)
{
    /* 黒背景に x=8 の縦線だけ白。4x 最近傍なら出力の白は 32..35 のはず */
    memset(s_src, 0, PP_W * PP_H * 2);
    for (int y = 0; y < PP_H; y++) s_src[y * PP_W + 8] = 0xFFFF;
}

static void fill_grad(void)
{
    for (int y = 0; y < PP_H; y++) {
        for (int x = 0; x < PP_W; x++) {
            int r = (x * 255) / (PP_W - 1);
            int g = (y * 255) / (PP_H - 1);
            int b = ((x + y) * 255) / (PP_W + PP_H - 2);
            if (((x >> 4) + (y >> 4)) & 1) {
                r = 255 - r;
                g = 255 - g;
                b = 255 - b;
            }
            s_src[y * PP_W + x] =
                (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
    }
}

static esp_err_t ppa_run(int scale, ppa_color_range_t range,
                         ppa_color_conv_std_rgb_yuv_t std,
                         uint8_t *out, uint32_t len)
{
    ppa_srm_oper_config_t c = {0};
    c.in.buffer = s_src;
    c.in.pic_w = PP_W;
    c.in.pic_h = PP_H;
    c.in.block_w = PP_W;   /* 全画面を 1 回で */
    c.in.block_h = PP_H;
    c.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    c.out.buffer = out;
    c.out.buffer_size = len;
    c.out.pic_w = PP_W * scale;
    c.out.pic_h = PP_H * scale;
    c.out.srm_cm = PPA_SRM_COLOR_MODE_YUV420;
    c.out.yuv_range = range;
    c.out.yuv_std = std;
    c.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
    c.scale_x = (float)scale;
    c.scale_y = (float)scale;
    c.mode = PPA_TRANS_MODE_BLOCKING;
    return ppa_do_scale_rotate_mirror(s_cl, &c);
}

static void ymap(const char *label, const uint8_t *row, int x0, int npix)
{
    char buf[136];
    int k = 0;
    if (npix > 128) npix = 128;
    for (int i = 0; i < npix; i++) {
        int x = x0 + i;
        buf[k++] = *YBYTE(row, x) > 128 ? '#' : '.';
    }
    buf[k] = 0;
    ESP_LOGI(TAG, "%s x%d: %s", label, x0, buf);
}

static void yvals(const char *label, const uint8_t *row, int x0, int n)
{
    char buf[3 * 48 + 4];
    int k = 0;
    if (n > 48) n = 48;
    for (int i = 0; i < n; i++) {
        k += snprintf(buf + k, sizeof(buf) - k, "%02x ", *YBYTE(row, x0 + i));
    }
    ESP_LOGI(TAG, "%s x%d: %s", label, x0, buf);
}

/* B: PPA 出力の 4x4 ブロック(元画素 1 個ぶん)が Y について均一か。
 * 均一でない場所を 10 件、mod4 の分布も出す */
static void check_uniform(const uint8_t *p4)
{
    int badblk = 0, badsmp = 0, first = -1;
    int mx[4] = {0}, my[4] = {0};
    for (int y = 0; y < PP_H && first < 10; y++) {
        for (int x = 0; x < PP_W; x++) {
            uint8_t ref = *YBYTE(p4 + (size_t)4 * y * PP_STRIDE, 4 * x);
            int bad = 0;
            for (int r = 0; r < 4; r++) {
                const uint8_t *row = p4 + (size_t)(4 * y + r) * PP_STRIDE;
                for (int p = 0; p < 4; p++) {
                    if (*YBYTE(row, 4 * x + p) != ref) {
                        bad++;
                        mx[p]++;
                        my[r]++;
                    }
                }
            }
            if (bad) {
                badblk++;
                badsmp += bad;
                if (first < 0) {
                    ESP_LOGI(TAG, "不均一 %d件目: 元(%d,%d) 不一致%d/16 基準Y=%02x",
                             badblk, x, y, bad, ref);
                    first = 1;
                } else if (first < 10) {
                    ESP_LOGI(TAG, "不均一 %d件目: 元(%d,%d) 不一致%d/16", badblk, x, y, bad);
                    first++;
                }
            }
        }
    }
    /* 全体でも数える(表示は 10 件まで) */
    int total = 0;
    for (int y = 0; y < PP_H; y++) {
        for (int x = 0; x < PP_W; x++) {
            uint8_t ref = *YBYTE(p4 + (size_t)4 * y * PP_STRIDE, 4 * x);
            for (int r = 0; r < 4; r++) {
                const uint8_t *row = p4 + (size_t)(4 * y + r) * PP_STRIDE;
                for (int p = 0; p < 4; p++) {
                    if (*YBYTE(row, 4 * x + p) != ref) total++;
                }
            }
        }
    }
    ESP_LOGI(TAG, "Y 不均一: ブロック %d/%d 個、サンプル %d/%d 個",
             badblk, PP_W * PP_H, total, PP_OW * PP_OH);
    ESP_LOGI(TAG, "不一致の X%%4 分布 %d %d %d %d / Y%%4 分布 %d %d %d %d",
             mx[0], mx[1], mx[2], mx[3], my[0], my[1], my[2], my[3]);
    (void)badsmp;
}

/* C: 単色 1 枚で PPA の Y/U/V と SW のそれを比べる */
static void color_case(const char *name, uint16_t rgb565)
{
    static const struct {
        ppa_color_range_t r;
        ppa_color_conv_std_rgb_yuv_t s;
        const char *tag;
    } cfg[] = {
        {PPA_COLOR_RANGE_FULL, PPA_COLOR_CONV_STD_RGB_YUV_BT601, "full/601"},
        {PPA_COLOR_RANGE_LIMIT, PPA_COLOR_CONV_STD_RGB_YUV_BT601, "lim/601"},
        {PPA_COLOR_RANGE_FULL, PPA_COLOR_CONV_STD_RGB_YUV_BT709, "full/709"},
    };
    for (int y = 0; y < PP_H; y++) {
        for (int x = 0; x < PP_W; x++) s_src[y * PP_W + x] = rgb565;
    }
    gbac_scale_ouyy(s_src, PP_W, PP_H, 1, s_p1);
    /* SW の値(行 0 の三つ組 0: U, Y, Y / 行 1: V, Y, Y) */
    int su = s_p1[0], sy = s_p1[1], sv = s_p1[PP_S1STRIDE];
    for (unsigned i = 0; i < sizeof(cfg) / sizeof(cfg[0]); i++) {
        esp_err_t e = ppa_run(1, cfg[i].r, cfg[i].s, s_ppa, PP_S1LEN);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "%s %s: %s", name, cfg[i].tag, esp_err_to_name(e));
            continue;
        }
        int pu = s_ppa[0], py = s_ppa[1], pv = s_ppa[PP_S1STRIDE];
        ESP_LOGI(TAG, "%s %s: Y %d(sw %d d%d) U %d(d%d) V %d(d%d)", name, cfg[i].tag,
                 py, sy, py - sy, pu, pu - su, pv, pv - sv);
    }
}

void gbac_ppa_probe(void)
{
    if (s_src == NULL) {
        s_src = heap_caps_aligned_alloc(128, PP_W * PP_H * 2,
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        if (s_src == NULL) s_src = heap_caps_aligned_alloc(128, PP_W * PP_H * 2, MALLOC_CAP_INTERNAL);
        if (s_src == NULL) s_src = heap_caps_aligned_alloc(128, PP_W * PP_H * 2, MALLOC_CAP_SPIRAM);
        s_ppa = heap_caps_aligned_alloc(128, PP_LEN, MALLOC_CAP_SPIRAM);
        s_sw = heap_caps_aligned_alloc(128, PP_LEN, MALLOC_CAP_SPIRAM);
        s_p1 = heap_caps_aligned_alloc(128, PP_S1LEN, MALLOC_CAP_SPIRAM);
        if (!s_src || !s_ppa || !s_sw || !s_p1) {
            ESP_LOGE(TAG, "alloc fail src=%p ppa=%p sw=%p p1=%p",
                     (void *)s_src, (void *)s_ppa, (void *)s_sw, (void *)s_p1);
            return;
        }
        ppa_client_config_t cc = {
            .oper_type = PPA_OPERATION_SRM,
            .max_pending_trans_num = 1,
        };
        esp_err_t e = ppa_register_client(&cc, &s_cl);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "ppa_register_client: %s", esp_err_to_name(e));
            return;
        }
        ESP_LOGI(TAG, "src=%dbyte @%s", PP_W * PP_H * 2,
                 esp_ptr_internal(s_src) ? "internal" : "psram");
    }

    /* --- A: 白の縦線で 4x の位相を見る --- */
    fill_line();
    esp_err_t e1 = ppa_run(PP_SC, PPA_COLOR_RANGE_FULL,
                           PPA_COLOR_CONV_STD_RGB_YUV_BT601, s_ppa, PP_LEN);
    esp_err_t e2 = ppa_run(1, PPA_COLOR_RANGE_FULL,
                           PPA_COLOR_CONV_STD_RGB_YUV_BT601, s_p1, PP_S1LEN);
    ESP_LOGI(TAG, "A: err4=%s err1=%s", esp_err_to_name(e1), esp_err_to_name(e2));
    if (e1 == ESP_OK && e2 == ESP_OK) {
        ymap("A 元", s_p1, 0, 32);
        ymap("A p4", s_ppa, 0, 64);
        ymap("A p4", s_ppa, 64, 64);
        yvals("A p4", s_ppa, 24, 24);
        yvals("A 元", s_p1, 4, 16);
        for (int r = 1; r < 4; r++) {
            ymap("A p4 row+", s_ppa + (size_t)r * PP_STRIDE, 0, 64);
        }
        check_uniform(s_ppa);
    }

    /* --- B: グラデーションで不均一の出方を見る --- */
    fill_grad();
    if (ppa_run(PP_SC, PPA_COLOR_RANGE_FULL, PPA_COLOR_CONV_STD_RGB_YUV_BT601,
                s_ppa, PP_LEN) == ESP_OK) {
        check_uniform(s_ppa);
    }

    /* --- C: 単色で変換式を見る --- */
    color_case("red  ", 0xF800);
    color_case("green", 0x07E0);
    color_case("blue ", 0x001F);
    color_case("black", 0x0000);
    color_case("white", 0xFFFF);
    color_case("gray ", 0x7BEF);

    /* --- 速さ --- */
    if (ppa_run(PP_SC, PPA_COLOR_RANGE_FULL, PPA_COLOR_CONV_STD_RGB_YUV_BT601,
                s_ppa, PP_LEN) == ESP_OK) {
        int64_t t0 = esp_timer_get_time();
        for (int i = 0; i < 20; i++) {
            ppa_run(PP_SC, PPA_COLOR_RANGE_FULL, PPA_COLOR_CONV_STD_RGB_YUV_BT601, s_ppa, PP_LEN);
        }
        int64_t t1 = esp_timer_get_time();
        ESP_LOGI(TAG, "PPA scale4 %.2f ms/frame", (double)(t1 - t0) / 20000.0);
        int64_t t2 = esp_timer_get_time();
        for (int i = 0; i < 20; i++) {
            ppa_run(1, PPA_COLOR_RANGE_FULL, PPA_COLOR_CONV_STD_RGB_YUV_BT601, s_p1, PP_S1LEN);
        }
        ESP_LOGI(TAG, "PPA scale1 %.2f ms/frame",
                 (double)(esp_timer_get_time() - t2) / 20000.0);
    }
    for (int i = 0; i < 4; i++) gbac_scale_prof[i] = 0;
}
