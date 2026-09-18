/* Ранний прототип (стори 0.3): проверка сборки на компьютере без ESP32 —
 * пишет 1 секунду тона 440 Гц в WAV. Сэмплера здесь нет: настоящий
 * звуковой движок — firmware/audio_engine.*, он же рендерит экспорт в WAV. */

#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <string.h>

#define SAMPLE_RATE 44100
#define DURATION_SEC 1
#define FREQ_HZ 440.0
#define AMPLITUDE 0.3

typedef struct {
    char     riff_id[4];
    uint32_t riff_size;
    char     wave_id[4];
    char     fmt_id[4];
    uint32_t fmt_size;
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char     data_id[4];
    uint32_t data_size;
} WavHeader;

static void write_wav_header(FILE *f, uint32_t num_samples) {
    WavHeader h;
    uint32_t data_size = num_samples * sizeof(int16_t);

    memcpy(h.riff_id, "RIFF", 4);
    h.riff_size = 36 + data_size;
    memcpy(h.wave_id, "WAVE", 4);
    memcpy(h.fmt_id, "fmt ", 4);
    h.fmt_size = 16;
    h.audio_format = 1; /* PCM */
    h.num_channels = 1; /* моно — стерео добавим в эпике 5 */
    h.sample_rate = SAMPLE_RATE;
    h.bits_per_sample = 16;
    h.block_align = h.num_channels * h.bits_per_sample / 8;
    h.byte_rate = h.sample_rate * h.block_align;
    memcpy(h.data_id, "data", 4);
    h.data_size = data_size;

    fwrite(&h, sizeof(WavHeader), 1, f);
}

int main(int argc, char **argv) {
    const char *out_path = (argc > 1) ? argv[1] : "test_tone.wav";
    uint32_t num_samples = SAMPLE_RATE * DURATION_SEC;

    FILE *f = fopen(out_path, "wb");
    if (!f) {
        fprintf(stderr, "Не удалось открыть файл для записи: %s\n", out_path);
        return 1;
    }

    write_wav_header(f, num_samples);

    for (uint32_t i = 0; i < num_samples; i++) {
        double t = (double)i / SAMPLE_RATE;
        double sample = AMPLITUDE * sin(2.0 * M_PI * FREQ_HZ * t);
        int16_t pcm = (int16_t)(sample * 32767.0);
        fwrite(&pcm, sizeof(int16_t), 1, f);
    }

    fclose(f);
    printf("Записано %u сэмплов в %s (%d Гц, %.0f сек, тон %.0f Гц)\n",
           num_samples, out_path, SAMPLE_RATE, (double)DURATION_SEC, FREQ_HZ);
    return 0;
}
