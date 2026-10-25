/*
 * audio_player —— I2S + minimp3 流式播放
 */

#include "audio_player.h"

#include <stdio.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"

/* minimp3 是单文件实现：只在一个 .c 里定义 MINIMP3_IMPLEMENTATION */
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#include "minimp3.h"

static const char *TAG = "audio";

#define PIN_BCK   4
#define PIN_LRCK  5
#define PIN_DIN   6

#define IN_BUF_SIZE  (16 * 1024)
#define READ_CHUNK   (4 * 1024)

typedef enum {
    CMD_PLAY,
    CMD_PAUSE,
    CMD_RESUME,
    CMD_STOP,
    CMD_VOL,
} cmd_type_t;

typedef struct {
    cmd_type_t type;
    char       path[160];
    uint32_t   duration_ms;
    uint8_t    vol;
} audio_cmd_t;

static i2s_chan_handle_t s_tx;
static QueueHandle_t     s_q;

static volatile audio_state_t s_state = AUDIO_IDLE;
static volatile uint32_t      s_pos_ms;
static volatile uint32_t      s_dur_ms;
static volatile uint8_t       s_vol = 60;
static volatile uint32_t      s_hz  = 44100;

/* 解码缓冲必须 static：mp3dec_decode_frame() 里有个约 15KB 的局部 scratch，
   放在任务栈上会把 32KB 栈吃掉一半；mp3dec_t 本身也有十几 KB。 */
static mp3dec_t s_dec;
static int16_t  s_pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
static int16_t  s_stereo[MINIMP3_MAX_SAMPLES_PER_FRAME * 2];

static inline int16_t gain_sample(int16_t v, uint32_t gain)
{
    int32_t x = (int32_t)v * (int32_t)gain >> 8;
    if (x > 32767)  x = 32767;
    if (x < -32768) x = -32768;
    return (int16_t)x;
}

static void audio_task(void *arg);

esp_err_t audio_init(void)
{
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = 6;
    chan_cfg.dma_frame_num = 256;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, NULL),
                        TAG, "创建 I2S 通道失败");

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,   /* PCM5102A 的 SCK 接地，用内部 PLL */
            .bclk = PIN_BCK,
            .ws   = PIN_LRCK,
            .dout = PIN_DIN,
            .din  = I2S_GPIO_UNUSED,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg),
                        TAG, "配置 I2S 标准模式失败");

    s_q = xQueueCreate(6, sizeof(audio_cmd_t));
    if (!s_q) {
        return ESP_ERR_NO_MEM;
    }

    /* 32KB 栈是给 minimp3 的 scratch 留的。
       内部 RAM 被屏幕的整帧缓冲占掉了，所以任务栈放 PSRAM。 */
    if (xTaskCreateWithCaps(audio_task, "audio", 32768, NULL, 6, NULL,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "创建音频任务失败（内存不够？）");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "I2S 就绪：BCK=GPIO%d LRCK=GPIO%d DIN=GPIO%d",
             PIN_BCK, PIN_LRCK, PIN_DIN);
    return ESP_OK;
}

static bool send_cmd(const audio_cmd_t *cmd)
{
    if (!s_q) {
        return false;
    }
    return xQueueSend(s_q, cmd, 0) == pdTRUE;
}

esp_err_t audio_play(const char *path, uint32_t duration_ms)
{
    audio_cmd_t cmd = { .type = CMD_PLAY, .duration_ms = duration_ms };
    strncpy(cmd.path, path, sizeof(cmd.path) - 1);
    return send_cmd(&cmd) ? ESP_OK : ESP_ERR_TIMEOUT;
}

void audio_pause(void)
{
    audio_cmd_t cmd = { .type = CMD_PAUSE };
    send_cmd(&cmd);
}

void audio_resume(void)
{
    audio_cmd_t cmd = { .type = CMD_RESUME };
    send_cmd(&cmd);
}

void audio_stop(void)
{
    audio_cmd_t cmd = { .type = CMD_STOP };
    send_cmd(&cmd);
}

void audio_set_volume(uint8_t vol)
{
    if (vol > 100) {
        vol = 100;
    }
    s_vol = vol;
    audio_cmd_t cmd = { .type = CMD_VOL, .vol = vol };
    send_cmd(&cmd);
}

audio_state_t audio_state(void)     { return s_state; }
uint32_t      audio_pos_ms(void)    { return s_pos_ms; }
uint32_t      audio_duration_ms(void) { return s_dur_ms; }

/* ------------------------------------------------------------------ */
/* 播放循环                                                            */
/* ------------------------------------------------------------------ */

/* 非阻塞处理一条命令。返回 false 表示要跳出播放循环。 */
static bool handle_cmd(audio_cmd_t *cmd)
{
    switch (cmd->type) {
    case CMD_STOP:
        return false;

    case CMD_VOL:
        /* 音量是每帧现算的，这里不用做事 */
        break;

    case CMD_PAUSE:
        if (s_state == AUDIO_PLAYING) {
            s_state = AUDIO_PAUSED;
            i2s_channel_disable(s_tx);
        }
        /* 暂停后阻塞等命令，直到恢复或停止 */
        for (;;) {
            if (xQueueReceive(s_q, cmd, portMAX_DELAY) != pdTRUE) {
                continue;
            }
            if (cmd->type == CMD_RESUME) {
                s_state = AUDIO_PLAYING;
                i2s_channel_enable(s_tx);
                break;
            }
            if (cmd->type == CMD_STOP) {
                return false;
            }
            if (cmd->type == CMD_VOL) {
                s_vol = cmd->vol;
            }
            if (cmd->type == CMD_PLAY) {
                /* 暂停时收到新的播放请求：退出当前曲目 */
                return false;
            }
        }
        break;

    default:
        break;
    }
    return true;
}

/* 返回 false 表示中途被停止 / 出错 */
static bool play_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "打不开文件: %s", path);
        return false;
    }

    static uint8_t buf[IN_BUF_SIZE];
    const uint8_t *p    = buf;
    size_t         fill = 0;
    uint32_t       gain = (uint32_t)s_vol * 256 / 100;
    uint32_t       frames = 0;

    s_pos_ms = 0;
    s_state  = AUDIO_PLAYING;
    i2s_channel_enable(s_tx);

    for (;;) {
        /* 补充数据 */
        if (fill < READ_CHUNK) {
            if (p != buf) {
                memmove(buf, p, fill);
                p = buf;
            }
            size_t n = fread(buf + fill, 1, sizeof(buf) - fill, f);
            fill += n;
            if (n == 0 && fill == 0) {
                break;                       /* 正常播完 */
            }
        }

        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(&s_dec, p, (int)fill, s_pcm, &info);

        if (info.frame_bytes <= 0) {
            /* 找不到帧头：丢一个字节继续找 */
            p++;
            fill--;
            if (fill == 0) {
                break;
            }
            continue;
        }
        p    += info.frame_bytes;
        fill -= info.frame_bytes;

        /* 采样率不一致就重配时钟 */
        if (info.hz > 0 && (uint32_t)info.hz != s_hz) {
            i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(info.hz);
            i2s_channel_reconfig_std_clock(s_tx, &clk);
            s_hz = (uint32_t)info.hz;
            ESP_LOGI(TAG, "采样率切换：%luHz", (unsigned long)s_hz);
        }

        if (samples <= 0) {
            continue;                        /* ID3 / 填充帧 */
        }

        gain = (uint32_t)s_vol * 256 / 100;

        if (info.channels == 1) {
            for (int i = 0; i < samples; i++) {
                int16_t v = gain_sample(s_pcm[i], gain);
                s_stereo[i * 2]     = v;
                s_stereo[i * 2 + 1] = v;
            }
        } else {
            for (int i = 0; i < samples * 2; i++) {
                s_stereo[i] = gain_sample(s_pcm[i], gain);
            }
        }

        size_t written = 0;
        esp_err_t err = i2s_channel_write(s_tx, s_stereo,
                                          (size_t)samples * 2 * sizeof(int16_t),
                                          &written, pdMS_TO_TICKS(200));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "I2S 写入失败: %s", esp_err_to_name(err));
            break;
        }

        s_pos_ms += (uint32_t)((uint64_t)samples * 1000 / s_hz);

        /* 让出 CPU，同时处理命令 */
        if ((frames++ % 8) == 0) {
            vTaskDelay(1);
        }

        audio_cmd_t cmd;
        while (xQueueReceive(s_q, &cmd, 0) == pdTRUE) {
            if (!handle_cmd(&cmd)) {
                fclose(f);
                return false;
            }
        }
    }

    fclose(f);
    return true;
}

static void audio_task(void *arg)
{
    audio_cmd_t cmd;
    for (;;) {
        if (xQueueReceive(s_q, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (cmd.type != CMD_PLAY) {
            continue;
        }

        ESP_LOGI(TAG, "播放：%s", cmd.path);
        s_dur_ms = cmd.duration_ms;
        mp3dec_init(&s_dec);

        play_file(cmd.path);

        s_state  = AUDIO_IDLE;
        s_pos_ms = 0;
        i2s_channel_disable(s_tx);
        ESP_LOGI(TAG, "播放结束");
    }
}
