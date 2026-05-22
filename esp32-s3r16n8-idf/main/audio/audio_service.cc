#ifdef IO
#undef IO
#endif
#ifdef IOR
#undef IOR
#endif
#ifdef IOW
#undef IOW
#endif

#include "audio_service.h"
#include <esp_log.h>
#include <cstring>
#include <cmath>          // <--- 【新增】彻底解决 'sqrt' was not declared 报错
#include <driver/uart.h>  // <--- 【新增】彻底解决 uart_write_bytes 和 UART_NUM_1 报错
#include "dsps_fft2r.h"
#include "dsps_wind_hann.h"

#define RATE_CVT_CFG(_src_rate, _dest_rate, _channel)        \
    (esp_ae_rate_cvt_cfg_t)                                  \
    {                                                        \
        .src_rate        = (uint32_t)(_src_rate),            \
        .dest_rate       = (uint32_t)(_dest_rate),           \
        .channel         = (uint8_t)(_channel),              \
        .bits_per_sample = ESP_AUDIO_BIT16,                  \
        .complexity      = 2,                                \
        .perf_type       = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,  \
    }

#define OPUS_DEC_CFG(_sample_rate, _frame_duration_ms)                                                    \
    (esp_opus_dec_cfg_t)                                                                                  \
    {                                                                                                     \
        .sample_rate    = (uint32_t)(_sample_rate),                                                       \
        .channel        = ESP_AUDIO_MONO,                                                                 \
        .frame_duration = (esp_opus_dec_frame_duration_t)AS_OPUS_GET_FRAME_DRU_ENUM(_frame_duration_ms),  \
        .self_delimited = false,                                                                          \
    }

#if CONFIG_USE_AUDIO_PROCESSOR
#include "processors/afe_audio_processor.h"
#else
#include "processors/no_audio_processor.h"
#endif

#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4
#include "wake_words/afe_wake_word.h"
#include "wake_words/custom_wake_word.h"
#else
#include "wake_words/esp_wake_word.h"
#endif

/* // =================== 【🌟新增：经典蓝牙 A2DP Sink 硬件底层依赖】 ===================
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_bt_device.h>
#include <esp_gap_bt_api.h>
#include <esp_a2dp_api.h>

// 声明底层经典蓝牙高频抛出音乐 PCM 数据的硬中断回调函数
static void bluetooth_a2dp_sink_data_cb(const uint8_t *data, uint32_t len);
// ==============================================================================
 */
#define TAG "AudioService"

AudioService::AudioService() {
    event_group_ = xEventGroupCreate();
}

AudioService::~AudioService() {
    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
    }
    if (opus_encoder_ != nullptr) {
        esp_opus_enc_close(opus_encoder_);
    }
    if (opus_decoder_ != nullptr) {
        esp_opus_dec_close(opus_decoder_);
    }
    if (input_resampler_ != nullptr) {
        esp_ae_rate_cvt_close(input_resampler_);
    }
    if (output_resampler_ != nullptr) {
        esp_ae_rate_cvt_close(output_resampler_);
    }
   /*  // =================== 【🌟新增：系统析构时彻底解绑并注销蓝牙协议栈】 ===================
    if (bluetooth_speaker_initialized_) {
        esp_a2d_sink_deinit();
        esp_bluedroid_deinit();
        esp_bluedroid_release();
        esp_bt_controller_deinit();
        esp_bt_controller_release();
    }
    // ================================================================================ */
}

void AudioService::Initialize(AudioCodec* codec) {
    codec_ = codec;
    codec_->Start();

    esp_opus_dec_cfg_t opus_dec_cfg = OPUS_DEC_CFG(codec->output_sample_rate(), OPUS_FRAME_DURATION_MS);
    auto ret = esp_opus_dec_open(&opus_dec_cfg, sizeof(esp_opus_dec_cfg_t), &opus_decoder_);
    if (opus_decoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create audio decoder, error code: %d", ret);
    } else {
        decoder_sample_rate_ = codec->output_sample_rate();
        decoder_duration_ms_ = OPUS_FRAME_DURATION_MS;
        decoder_frame_size_ = decoder_sample_rate_ / 1000 * OPUS_FRAME_DURATION_MS;
    }
    esp_opus_enc_config_t opus_enc_cfg = AS_OPUS_ENC_CONFIG();
    ret = esp_opus_enc_open(&opus_enc_cfg, sizeof(esp_opus_enc_config_t), &opus_encoder_);
    if (opus_encoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create audio encoder, error code: %d", ret);
    } else {
        encoder_sample_rate_ = 16000;
        encoder_duration_ms_ = OPUS_FRAME_DURATION_MS;
        esp_opus_enc_get_frame_size(opus_encoder_, &encoder_frame_size_, &encoder_outbuf_size_);
        encoder_frame_size_ = encoder_frame_size_ / sizeof(int16_t);
    }

    if (codec->input_sample_rate() != 16000) {
        esp_ae_rate_cvt_cfg_t input_resampler_cfg = RATE_CVT_CFG(
            codec->input_sample_rate(), ESP_AUDIO_SAMPLE_RATE_16K, codec->input_channels());
        auto resampler_ret = esp_ae_rate_cvt_open(&input_resampler_cfg, &input_resampler_);
        if (input_resampler_ == nullptr) {
            ESP_LOGE(TAG, "Failed to create input resampler, error code: %d", resampler_ret);
        }
    }

#if CONFIG_USE_AUDIO_PROCESSOR
    audio_processor_ = std::make_unique<AfeAudioProcessor>();
#else
    audio_processor_ = std::make_unique<NoAudioProcessor>();
#endif

    audio_processor_->OnOutput([this](std::vector<int16_t>&& data) {
        PushTaskToEncodeQueue(kAudioTaskTypeEncodeToSendQueue, std::move(data));
    });

    audio_processor_->OnVadStateChange([this](bool speaking) {
        voice_detected_ = speaking;
        if (callbacks_.on_vad_change) {
            callbacks_.on_vad_change(speaking);
        }
    });

    esp_timer_create_args_t audio_power_timer_args = {
        .callback = [](void* arg) {
            AudioService* audio_service = (AudioService*)arg;
            audio_service->CheckAndUpdateAudioPowerState();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "audio_power_timer",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&audio_power_timer_args, &audio_power_timer_);
}

void AudioService::Start() {
    is_music_mode_ = false;
    service_stopped_ = false;
    xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING | AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING);

    esp_timer_start_periodic(audio_power_timer_, 1000000);

#if CONFIG_USE_AUDIO_PROCESSOR
    /* Start the audio input task */
    xTaskCreatePinnedToCore([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioInputTask();
        vTaskDelete(NULL);
    }, "audio_input", 2048 * 3, this, 8, &audio_input_task_handle_, 0);

    /* Start the audio output task */
    xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioOutputTask();
        vTaskDelete(NULL);
    }, "audio_output", 2048 * 4, this, 4, &audio_output_task_handle_);
#else
    /* Start the audio input task */
    xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioInputTask();
        vTaskDelete(NULL);
    }, "audio_input", 2048 * 4, this, 8, &audio_input_task_handle_);

    /* Start the audio output task */
    xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioOutputTask();
        vTaskDelete(NULL);
    }, "audio_output", 2048 * 4, this, 4, &audio_output_task_handle_);
#endif

    /* Start the opus codec task */
    xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->OpusCodecTask();
        vTaskDelete(NULL);
    }, "opus_codec", 2048 * 12, this, 2, &opus_codec_task_handle_);
}

void AudioService::Stop() {
    is_music_mode_ = false;
    esp_timer_stop(audio_power_timer_);
    service_stopped_ = true;
    xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING |
        AS_EVENT_WAKE_WORD_RUNNING |
        AS_EVENT_AUDIO_PROCESSOR_RUNNING);

    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    audio_encode_queue_.clear();
    audio_decode_queue_.clear();
    audio_playback_queue_.clear();
    audio_testing_queue_.clear();
    audio_queue_cv_.notify_all();
}

bool AudioService::ReadAudioData(std::vector<int16_t>& data, int sample_rate, int samples) {
    if (!codec_->input_enabled()) {
        esp_timer_stop(audio_power_timer_);
        esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
        codec_->EnableInput(true);
    }

    if (codec_->input_sample_rate() != sample_rate) {
        data.resize(samples * codec_->input_sample_rate() / sample_rate * codec_->input_channels());
        if (!codec_->InputData(data)) {
            return false;
        }
        if (input_resampler_ != nullptr) {
            std::lock_guard<std::mutex> lock(input_resampler_mutex_);
            uint32_t in_sample_num = data.size() / codec_->input_channels();
            uint32_t output_samples = 0;
            esp_ae_rate_cvt_get_max_out_sample_num(input_resampler_, in_sample_num, &output_samples);
            auto resampled = std::vector<int16_t>(output_samples * codec_->input_channels());
            uint32_t actual_output = output_samples;
            esp_ae_rate_cvt_process(input_resampler_, (esp_ae_sample_t)data.data(), in_sample_num,
                                   (esp_ae_sample_t)resampled.data(), &actual_output);
            resampled.resize(actual_output * codec_->input_channels());
            data = std::move(resampled);
        }
    } else {
        data.resize(samples * codec_->input_channels());
        if (!codec_->InputData(data)) {
            return false;
        }
    }

    /* Update the last input time */
    last_input_time_ = std::chrono::steady_clock::now();
    debug_statistics_.input_count++;

#if CONFIG_USE_AUDIO_DEBUGGER
    // 音频调试：发送原始音频数据
    if (audio_debugger_ == nullptr) {
        audio_debugger_ = std::make_unique<AudioDebugger>();
    }
    audio_debugger_->Feed(data);
#endif

    return true;
}

void AudioService::AudioInputTask() {
    while (true) {
        EventBits_t bits = xEventGroupWaitBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING |
            AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING,
            pdFALSE, pdFALSE, portMAX_DELAY);

        if (service_stopped_) {
            break;
        }
        if (audio_input_need_warmup_) {
            audio_input_need_warmup_ = false;
            vTaskDelay(pdMS_TO_TICKS(120));
            continue;
        }

        /* Used for audio testing in NetworkConfiguring mode by clicking the BOOT button */
        if (bits & AS_EVENT_AUDIO_TESTING_RUNNING) {
            if (audio_testing_queue_.size() >= AUDIO_TESTING_MAX_DURATION_MS / OPUS_FRAME_DURATION_MS) {
                ESP_LOGW(TAG, "Audio testing queue is full, stopping audio testing");
                EnableAudioTesting(false);
                continue;
            }
            std::vector<int16_t> data;
            int samples = OPUS_FRAME_DURATION_MS * 16000 / 1000;
            if (ReadAudioData(data, 16000, samples)) {
                // If input channels is 2, we need to fetch the left channel data
                if (codec_->input_channels() == 2) {
                    auto mono_data = std::vector<int16_t>(data.size() / 2);
                    for (size_t i = 0, j = 0; i < mono_data.size(); ++i, j += 2) {
                        mono_data[i] = data[j];
                    }
                    data = std::move(mono_data);
                }
                PushTaskToEncodeQueue(kAudioTaskTypeEncodeToTestingQueue, std::move(data));
                continue;
            }
        }

        /* Feed the wake word and/or audio processor */
        if (bits & (AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING)) {
            int samples = 160; // 10ms
            std::vector<int16_t> data;
            if (ReadAudioData(data, 16000, samples)) {
                if (bits & AS_EVENT_WAKE_WORD_RUNNING) {
                    wake_word_->Feed(data);
                }
                if (bits & AS_EVENT_AUDIO_PROCESSOR_RUNNING) {
                    audio_processor_->Feed(std::move(data));
                }
                continue;
            }
        }

        // Read timeout/error should not terminate the input task.
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGW(TAG, "Audio input task stopped");
}

void AudioService::AudioOutputTask() {
    while (true) {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        // 🌟【核心修复：为蓝牙模式开辟生存豁免权】
        // 只有在（队列为空且系统停止）且（蓝牙音箱模式也没有运行）时，线程才可以安全 break 退出。
        // 这样即使 service_stopped_ 为 true，只要你的手机蓝牙还在推流，这个任务就永远不会自杀！
        /* audio_queue_cv_.wait(lock, [this]() { 
            return !audio_playback_queue_.empty() || 
                   (service_stopped_ && !GetMusicMode()); 
        }); */
        audio_queue_cv_.wait(lock, [this]() { 
            return !audio_playback_queue_.empty() || service_stopped_; 
        });

        if (service_stopped_) {
            break;
        }

        auto task = std::move(audio_playback_queue_.front());
        audio_playback_queue_.pop_front();
        audio_queue_cv_.notify_all();
        lock.unlock();

        if (!codec_->output_enabled()) {
            esp_timer_stop(audio_power_timer_);
            esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
            codec_->EnableOutput(true);
        }

        codec_->OutputData(task->pcm);

/* // ==================== 【真FFT音律拦截中枢 - 24kHz专属校准版】====================
if (!task->pcm.empty()) {
    const int FFT_SIZE = 512;
    const int SEND_EVERY = 3;

    static std::vector<int16_t> fft_accum;
    static int frame_skip_counter = 0;
    static float hann_win[FFT_SIZE];
    static bool hann_ready = false;

    if (!hann_ready) {
        dsps_wind_hann_f32(hann_win, FFT_SIZE);
        dsps_fft2r_init_fc32(nullptr, FFT_SIZE);
        hann_ready = true;
    }

    fft_accum.insert(fft_accum.end(), task->pcm.begin(), task->pcm.end());

    do {
        if ((int)fft_accum.size() < FFT_SIZE) break;

        frame_skip_counter++;
        if (frame_skip_counter < SEND_EVERY) {
            fft_accum.erase(fft_accum.begin(), fft_accum.begin() + FFT_SIZE);
            break;
        }
        frame_skip_counter = 0;

        static float fft_buf[FFT_SIZE * 2];
        for (int i = 0; i < FFT_SIZE; i++) {
            fft_buf[i * 2]     = (float)fft_accum[i] / 32768.0f * hann_win[i];
            fft_buf[i * 2 + 1] = 0.0f;
        }

        dsps_fft2r_fc32(fft_buf, FFT_SIZE);
        dsps_bit_rev_fc32(fft_buf, FFT_SIZE);

        static float magnitude[FFT_SIZE / 2];
        for (int i = 0; i < FFT_SIZE / 2; i++) {
            float re = fft_buf[i * 2];
            float im = fft_buf[i * 2 + 1];
            magnitude[i] = sqrtf(re * re + im * im);
        }

        float sum_sq = 0.0f;
        for (int i = 0; i < FFT_SIZE; i++) {
            float s = (float)fft_accum[i] / 32768.0f;
            sum_sq += s * s;
        }
        float rms = sqrtf(sum_sq / FFT_SIZE);

        const int band_edges[17] = {
            1, 2, 3, 4, 5, 7, 9, 12, 16, 22, 30, 41, 56, 76, 103, 140, 255
        };

        int bands[16] = {0};
        for (int b = 0; b < 16; b++) {
            float band_sum = 0.0f;
            int count = band_edges[b + 1] - band_edges[b];
            for (int i = band_edges[b]; i < band_edges[b + 1]; i++) {
                band_sum += magnitude[i];
            }
            float avg = band_sum / count;
            float weighted = avg * (1.0f + rms * 3.0f);
            int val = (int)(weighted * 200.0f);
            if (val > 15) val = 15;
            if (val < 0)  val = 0;
            bands[b] = val;
        }

        static float prev_low_energy = 0.0f;
        float low_energy = 0.0f;
        for (int i = band_edges[0]; i < band_edges[4]; i++) {
            low_energy += magnitude[i];
        }
        int is_beat = (low_energy > prev_low_energy * 1.4f && low_energy > 0.3f && rms > 0.02f) ? 1 : 0;
        prev_low_energy = low_energy * 0.6f + prev_low_energy * 0.4f;

        static char fft_cmd[128];
        int len = snprintf(fft_cmd, sizeof(fft_cmd), "f,%d", is_beat);
        for (int i = 0; i < 16; i++) {
            len += snprintf(fft_cmd + len, sizeof(fft_cmd) - len, ",%d", bands[i]);
        }
        snprintf(fft_cmd + len, sizeof(fft_cmd) - len, "\n");
        uart_write_bytes(UART_NUM_1, fft_cmd, strlen(fft_cmd));

        ESP_LOGI(TAG, "FFT beat=%d rms=%.3f | %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d",
                 is_beat, rms,
                 bands[0], bands[1], bands[2], bands[3],
                 bands[4], bands[5], bands[6], bands[7],
                 bands[8], bands[9], bands[10], bands[11],
                 bands[12], bands[13], bands[14], bands[15]);

        fft_accum.erase(fft_accum.begin(), fft_accum.begin() + FFT_SIZE);

    } while (0);
} */
/* if (!task->pcm.empty()) {
    const int FFT_SIZE = 512;
    const int SEND_EVERY = 3;

    static std::vector<int16_t> fft_accum;
    static int frame_skip_counter = 0;
    static float hann_win[FFT_SIZE];
    static bool hann_ready = false;
    static float peak_tracker = 1.0f; // 🌟 引入自适应峰值追踪，防止卡死

    if (!hann_ready) {
        dsps_wind_hann_f32(hann_win, FFT_SIZE);
        dsps_fft2r_init_fc32(nullptr, FFT_SIZE);
        hann_ready = true;
    }

    fft_accum.insert(fft_accum.end(), task->pcm.begin(), task->pcm.end());

    do {
        if ((int)fft_accum.size() < FFT_SIZE) break;

        frame_skip_counter++;
        if (frame_skip_counter < SEND_EVERY) {
            fft_accum.erase(fft_accum.begin(), fft_accum.begin() + FFT_SIZE);
            break;
        }
        frame_skip_counter = 0;

        static float fft_buf[FFT_SIZE * 2];
        for (int i = 0; i < FFT_SIZE; i++) {
            fft_buf[i * 2]     = (float)fft_accum[i] / 32768.0f * hann_win[i];
            fft_buf[i * 2 + 1] = 0.0f;
        }

        dsps_fft2r_fc32(fft_buf, FFT_SIZE);
        dsps_bit_rev_fc32(fft_buf, FFT_SIZE);

        static float magnitude[FFT_SIZE / 2];
        for (int i = 0; i < FFT_SIZE / 2; i++) {
            float re = fft_buf[i * 2];
            float im = fft_buf[i * 2 + 1];
            magnitude[i] = sqrtf(re * re + im * im);
        }

        float sum_sq = 0.0f;
        for (int i = 0; i < FFT_SIZE; i++) {
            float s = (float)fft_accum[i] / 32768.0f;
            sum_sq += s * s;
        }
        float rms = sqrtf(sum_sq / FFT_SIZE);

        const int band_edges[17] = {
            1, 2, 3, 4, 5, 7, 9, 12, 16, 22, 30, 41, 56, 76, 103, 140, 255
        };

        // 🌟【自适应扫描优化块】
        float raw_avgs[16] = {0.0f};
        float current_max_avg = 0.0f;
        for (int b = 0; b < 16; b++) {
            float band_sum = 0.0f;
            int count = band_edges[b + 1] - band_edges[b];
            for (int i = band_edges[b]; i < band_edges[b + 1]; i++) {
                band_sum += magnitude[i];
            }
            raw_avgs[b] = band_sum / count;
            if (raw_avgs[b] > current_max_avg) {
                current_max_avg = raw_avgs[b];
            }
        }
        peak_tracker *= 0.98f; 
        if (current_max_avg > peak_tracker) peak_tracker = current_max_avg;
        if (peak_tracker < 0.01f) peak_tracker = 0.01f;

        int bands[16] = {0};
        for (int b = 0; b < 16; b++) {
            int val = (int)((raw_avgs[b] / peak_tracker) * 15.0f); // 归一化计算
            if (b >= 10) val = (int)(val * 1.2f);
            if (val > 15) val = 15;
            if (val < 0)  val = 0;
            bands[b] = val;
        }

        static float prev_low_energy = 0.0f;
        float low_energy = 0.0f;
        for (int i = band_edges[0]; i < band_edges[4]; i++) {
            low_energy += magnitude[i];
        }
        int is_beat = (low_energy > prev_low_energy * 1.3f && low_energy > 0.15f && rms > 0.015f) ? 1 : 0; // 优化突变系数
        prev_low_energy = low_energy * 0.6f + prev_low_energy * 0.4f;

        static char fft_cmd[128];
        int len = snprintf(fft_cmd, sizeof(fft_cmd), "f,%d", is_beat);
        for (int i = 0; i < 16; i++) {
            len += snprintf(fft_cmd + len, sizeof(fft_cmd) - len, ",%d", bands[i]);
        }
        snprintf(fft_cmd + len, sizeof(fft_cmd) - len, "\n");
        uart_write_bytes(UART_NUM_1, fft_cmd, strlen(fft_cmd));

        ESP_LOGI(TAG, "FFT beat=%d rms=%.3f | %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d",
                 is_beat, rms,
                 bands[0], bands[1], bands[2], bands[3],
                 bands[4], bands[5], bands[6], bands[7],
                 bands[8], bands[9], bands[10], bands[11],
                 bands[12], bands[13], bands[14], bands[15]);

        fft_accum.erase(fft_accum.begin(), fft_accum.begin() + FFT_SIZE);

    } while (0);
} */
/* // ==================== 【真FFT音律拦截中枢 - 仅音乐模式激活】====================
if (!task->pcm.empty() && is_music_mode_) {
    const int FFT_SIZE = 512;
    const int SEND_EVERY = 3;

    static std::vector<int16_t> fft_accum;
    static int frame_skip_counter = 0;
    static float hann_win[FFT_SIZE];
    static bool hann_ready = false;
    static float peak_tracker = 0.01f;
    static float prev_low_energy = 0.0f;

    if (!hann_ready) {
        dsps_wind_hann_f32(hann_win, FFT_SIZE);
        dsps_fft2r_init_fc32(nullptr, FFT_SIZE);
        hann_ready = true;
    }

    fft_accum.insert(fft_accum.end(), task->pcm.begin(), task->pcm.end());

    do {
        if ((int)fft_accum.size() < FFT_SIZE) break;

        // 先算RMS，静音直接归零并清空缓冲
        float sum_sq = 0.0f;
        for (int i = 0; i < FFT_SIZE; i++) {
            float s = (float)fft_accum[i] / 32768.0f;
            sum_sq += s * s;
        }
        float rms = sqrtf(sum_sq / FFT_SIZE);

        if (rms < 0.005f) {
            uart_write_bytes(UART_NUM_1, "f,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0\n", 34);
            fft_accum.clear();
            prev_low_energy = 0.0f;
            peak_tracker = 0.01f;
            break;
        }

        // 限速控制
        frame_skip_counter++;
        if (frame_skip_counter < SEND_EVERY) {
            fft_accum.erase(fft_accum.begin(), fft_accum.begin() + FFT_SIZE);
            break;
        }
        frame_skip_counter = 0;

        // FFT计算
        static float fft_buf[FFT_SIZE * 2];
        for (int i = 0; i < FFT_SIZE; i++) {
            fft_buf[i * 2]     = (float)fft_accum[i] / 32768.0f * hann_win[i];
            fft_buf[i * 2 + 1] = 0.0f;
        }

        dsps_fft2r_fc32(fft_buf, FFT_SIZE);
        dsps_bit_rev_fc32(fft_buf, FFT_SIZE);

        static float magnitude[FFT_SIZE / 2];
        for (int i = 0; i < FFT_SIZE / 2; i++) {
            float re = fft_buf[i * 2];
            float im = fft_buf[i * 2 + 1];
            magnitude[i] = sqrtf(re * re + im * im);
        }

        // 24kHz专属对数频带边界
        const int band_edges[17] = {
            1, 2, 3, 4, 5, 7, 9, 12, 16, 22, 30, 41, 56, 76, 103, 140, 200
        };

        // 自适应峰值归一化
        float raw_avgs[16] = {0.0f};
        float current_max = 0.0f;
        for (int b = 0; b < 16; b++) {
            float band_sum = 0.0f;
            int count = band_edges[b + 1] - band_edges[b];
            for (int i = band_edges[b]; i < band_edges[b + 1]; i++) {
                band_sum += magnitude[i];
            }
            raw_avgs[b] = band_sum / count;
            if (raw_avgs[b] > current_max) current_max = raw_avgs[b];
        }

        // 峰值追踪：快速上升，缓慢衰减
        peak_tracker *= 0.98f;
        if (current_max > peak_tracker) peak_tracker = current_max;
        if (peak_tracker < 0.01f) peak_tracker = 0.01f;

        // 频带映射
        int bands[16] = {0};
        for (int b = 0; b < 16; b++) {
            float normalized = raw_avgs[b] / peak_tracker;
            // 对数映射拉开差异
            float log_val = logf(1.0f + normalized * 10.0f) / logf(11.0f);
            int val = (int)(log_val * 15.0f);
            // 高频微增益，让镲片等高频细节更明显
            if (b >= 10) val = (int)(val * 1.0f);
            if (b >= 13) val = val / 2;
            if (val > 15) val = 15;
            if (val < 0)  val = 0;
            bands[b] = val;
        }

        // 鼓点检测
        float low_energy = 0.0f;
        for (int i = band_edges[0]; i < band_edges[4]; i++) {
            low_energy += magnitude[i];
        }
        int is_beat = (low_energy > prev_low_energy * 1.3f &&
                       low_energy > 0.15f &&
                       rms > 0.015f) ? 1 : 0;
        prev_low_energy = low_energy * 0.6f + prev_low_energy * 0.4f;

        // 发送串口
        static char fft_cmd[128];
        int len = snprintf(fft_cmd, sizeof(fft_cmd), "f,%d", is_beat);
        for (int i = 0; i < 16; i++) {
            len += snprintf(fft_cmd + len, sizeof(fft_cmd) - len, ",%d", bands[i]);
        }
        snprintf(fft_cmd + len, sizeof(fft_cmd) - len, "\n");
        uart_write_bytes(UART_NUM_1, fft_cmd, strlen(fft_cmd));

        ESP_LOGI(TAG, "FFT beat=%d rms=%.3f | %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d",
                 is_beat, rms,
                 bands[0],  bands[1],  bands[2],  bands[3],
                 bands[4],  bands[5],  bands[6],  bands[7],
                 bands[8],  bands[9],  bands[10], bands[11],
                 bands[12], bands[13], bands[14], bands[15]);

        fft_accum.erase(fft_accum.begin(), fft_accum.begin() + FFT_SIZE);

    } while (0);

} else if (!is_music_mode_) {
    // 非音乐模式：确保灯光退出律动状态（可选，取决于WROOM-32的逻辑）
    // 如果需要在对话时停止FFT数据流，这里什么都不做即可
}
// ================================================================================= */
/* // 🌟🌟🌟【一键拔闸调试宏：想关闭打印时，只需在前面加上 // 注释掉此行即可】🌟🌟🌟
#define CONFIG_DEBUG_FFT_STREAM 1
// ==================== 【真FFT音律拦截中枢 - 零积压极致刷新完全体】====================
if (!task->pcm.empty() && is_music_mode_) {
    const int FFT_SIZE = 512;
    const int SEND_EVERY = 1; // 🌟 每次都有充足的新鲜数据，直接跑满帧率，不再跳帧

    static std::vector<int16_t> fft_accum;
    static float hann_win[FFT_SIZE];
    static bool hann_ready = false;
    static float peak_tracker = 0.5f;   // 初始合理的增益防线
    static float prev_low_energy = 0.0f;

    if (!hann_ready) {
        dsps_wind_hann_f32(hann_win, FFT_SIZE);
        dsps_fft2r_init_fc32(nullptr, FFT_SIZE);
        hann_ready = true;
    }

    // 🌟🌟🌟【黄金核心修复：斩断积压，直接截取最新一帧的 512 个样本】🌟🌟🌟
    if ((int)task->pcm.size() >= FFT_SIZE) {
        // 24kHz/60ms 下每帧 1440 个样本 >= 512 点：直接取最后 512 点，彻底解决积压问题
        fft_accum.assign(task->pcm.end() - FFT_SIZE, task->pcm.end());
    } else {
        // 兜底缓冲防线（若网络抖动下发了小数据包）：追加但严格限制总量
        fft_accum.insert(fft_accum.end(), task->pcm.begin(), task->pcm.end());
        if ((int)fft_accum.size() > FFT_SIZE * 2) {
            fft_accum.erase(fft_accum.begin(), fft_accum.begin() + (fft_accum.size() - FFT_SIZE));
        }
    }

    do {
        // 再次安全检查，不够 512 点则跳过本帧
        if ((int)fft_accum.size() < FFT_SIZE) break;

        // 1. 先算 RMS 能量
        float sum_sq = 0.0f;
        for (int i = 0; i < FFT_SIZE; i++) {
            float s = (float)fft_accum[i] / 32768.0f;
            sum_sq += s * s;
        }
        float rms = sqrtf(sum_sq / FFT_SIZE);

        // 🌟【硬核熔断机制】静音直接归零，不带任何历史包袱
        if (rms < 0.005f) {
            uart_write_bytes(UART_NUM_1, "f,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0\n", 36);
            fft_accum.clear();
            prev_low_energy = 0.0f;
            peak_tracker = 0.5f; // 重置增益防线，防止空载暴走
            break;
        }

        // 2. FFT 前置准备：实部归一化加汉宁窗，虚部清零
        static float fft_buf[FFT_SIZE * 2];
        for (int i = 0; i < FFT_SIZE; i++) {
            fft_buf[i * 2]     = (float)fft_accum[i] / 32768.0f * hann_win[i];
            fft_buf[i * 2 + 1] = 0.0f;
        }

        // 3. 执行汇编级硬件加速快速傅里叶变换
        dsps_fft2r_fc32(fft_buf, FFT_SIZE);
        dsps_bit_rev_fc32(fft_buf, FFT_SIZE);

        // 4. 解算幅度谱（由于对称性只取前半段）
        static float magnitude[FFT_SIZE / 2];
        for (int i = 0; i < FFT_SIZE / 2; i++) {
            float re = fft_buf[i * 2];
            float im = fft_buf[i * 2 + 1];
            magnitude[i] = sqrtf(re * re + im * im);
        }

        // 5. 24kHz 专属对数频带边界系数（完美贴合人耳听觉切片）
        const int band_edges[17] = {
            1, 2, 3, 4, 5, 7, 9, 12, 16, 22, 30, 41, 56, 76, 103, 140, 200
        };

        // 6. 自适应扫描：捕捉当前帧各频带的原始平均值及最高峰值
        float raw_avgs[16] = {0.0f};
        float current_max = 0.0f;
        for (int b = 0; b < 16; b++) {
            float band_sum = 0.0f;
            int count = band_edges[b + 1] - band_edges[b];
            for (int i = band_edges[b]; i < band_edges[b + 1]; i++) {
                band_sum += magnitude[i];
            }
            raw_avgs[b] = band_sum / count;
            if (raw_avgs[b] > current_max) current_max = raw_avgs[b];
        }

        // 7. 峰值追踪 AGC 自适应自校准：快速上升，平滑衰减
        peak_tracker *= 0.94f; // 提高释放灵敏度，使平缓乐段更灵动
        if (current_max > peak_tracker) {
            peak_tracker = current_max;
        }
        if (peak_tracker < 0.05f) peak_tracker = 0.05f; // 提高安全低位保护线

        // 8. 频带非线性对数映射与压缩
        int bands[16] = {0};
        for (int b = 0; b < 16; b++) {
            float normalized = raw_avgs[b] / peak_tracker;
            // 完美的对数感知映射拉开差异
            float log_val = logf(1.0f + normalized * 10.0f) / logf(11.0f);
            int val = (int)(log_val * 13.5f); // 留出顶层视觉安全裕度

            // 局部增益微调加固
            if (b >= 10) val = (int)(val * 1.1f); // 适当强化高频细节
            if (b >= 13) val = val / 2;           // 极高频区做适当衰减控噪

            if (val > 15) val = 15;
            if (val < 0)  val = 0;
            bands[b] = val;
        }

        // 9. 瞬态低频重音鼓点（Beat）检测
        float low_energy = 0.0f;
        for (int i = band_edges[0]; i < band_edges[4]; i++) {
            low_energy += magnitude[i];
        }
        int is_beat = (low_energy > prev_low_energy * 1.3f &&
                       low_energy > 0.15f &&
                       rms > 0.015f) ? 1 : 0;
        prev_low_energy = low_energy * 0.6f + prev_low_energy * 0.4f;

        // 10. 串口下发：极致紧凑压缩，删除一切空格符号，降低 50% 的串行开销
        static char fft_cmd[96]; 
        int len = snprintf(fft_cmd, sizeof(fft_cmd), "f,%d", is_beat);
        for (int i = 0; i < 16; i++) {
            len += snprintf(fft_cmd + len, sizeof(fft_cmd) - len, ",%d", bands[i]);
        }
        snprintf(fft_cmd + len, sizeof(fft_cmd) - len, "\n");
        uart_write_bytes(UART_NUM_1, fft_cmd, strlen(fft_cmd));

        // 🌟🌟🌟【新增：随时可一键注释掉的条件编译打印块】🌟🌟🌟
        #ifdef CONFIG_DEBUG_FFT_STREAM
        // 采用静态计数器限速：控制台打印没必要每秒刷60次，每10帧在终端放行一次（约每200ms刷一次），既能看清数据，又绝不卡死主频！
        static int log_throttle_counter = 0;
        log_throttle_counter++;
        if (log_throttle_counter >= 10) {
            log_throttle_counter = 0;
            ESP_LOGI(TAG, "FFT beat=%d rms=%.3f | %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d",
                    is_beat, rms,
                    bands[0],  bands[1],  bands[2],  bands[3],
                    bands[4],  bands[5],  bands[6],  bands[7],
                    bands[8],  bands[9],  bands[10], bands[11],
                    bands[12], bands[13], bands[14], bands[15]);
        }
        #endif // CONFIG_DEBUG_FFT_STREAM
        // ==========================================================

        // 内存平衡维护：当前 assign 的数据已消费完毕，直接清空以迎接下一帧
        fft_accum.clear();

    } while (0);

} else if (!is_music_mode_) {
    // 非音乐模式：如果对话时需要向副板打招呼或静音，可以在此处实现
}
// ================================================================================= */
// ================================================================================= */
/* // 🌟🌟🌟【一键拔闸调试宏：想关闭打印时，只需在前面加上 // 注释掉此行即可】🌟🌟🌟
#define CONFIG_DEBUG_FFT_STREAM 1

// ==================== 【真FFT音律拦截中枢 - 零积压无丢失终极完全体】====================
if (!task->pcm.empty() && is_music_mode_) {
    const int FFT_SIZE = 512;

    static std::vector<int16_t> fft_accum;
    static float hann_win[FFT_SIZE];
    static bool hann_ready = false;
    static float peak_tracker = 0.5f;   // 初始合理的增益防线
    static float prev_low_energy = 0.0f;

    if (!hann_ready) {
        dsps_wind_hann_f32(hann_win, FFT_SIZE);
        dsps_fft2r_init_fc32(nullptr, FFT_SIZE);
        hann_ready = true;
    }

    // 🌟🌟🌟【黄金核心修复：动态吞入并实行硬限速拦截，拒绝全清】🌟🌟🌟
    // 1. 平滑吞入当前音频碎包
    fft_accum.insert(fft_accum.end(), task->pcm.begin(), task->pcm.end());

    // 2. 核心防积压硬拦截：如果 vector 积压超过 2 个完整窗口，直接精简掉旧数据，死死控住堆栈大小
    if ((int)fft_accum.size() > FFT_SIZE * 2) {
        fft_accum.erase(fft_accum.begin(), fft_accum.end() - FFT_SIZE);
    }

    do {
        // 3. 攒够 512 点才放行进入傅里叶中心，不够就 break 等待下一帧拼接
        if ((int)fft_accum.size() < FFT_SIZE) break;

        // 先算 RMS 能量
        float sum_sq = 0.0f;
        for (int i = 0; i < FFT_SIZE; i++) {
            float s = (float)fft_accum[i] / 32768.0f;
            sum_sq += s * s;
        }
        float rms = sqrtf(sum_sq / FFT_SIZE);

        // 硬核熔断机制：静音直接归零
        if (rms < 0.005f) {
            uart_write_bytes(UART_NUM_1, "f,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0\n", 34);
            fft_accum.clear();
            prev_low_energy = 0.0f;
            peak_tracker = 0.5f; // 重置增益防线
            break;
        }

        // 4. FFT 前置准备：实部归一化加汉宁窗，虚部清零
        static float fft_buf[FFT_SIZE * 2];
        for (int i = 0; i < FFT_SIZE; i++) {
            fft_buf[i * 2]     = (float)fft_accum[i] / 32768.0f * hann_win[i];
            fft_buf[i * 2 + 1] = 0.0f;
        }

        // 5. 执行汇编级硬件加速快速傅里叶变换
        dsps_fft2r_fc32(fft_buf, FFT_SIZE);
        dsps_bit_rev_fc32(fft_buf, FFT_SIZE);

        // 6. 解算幅度谱（由于对称性只取前半段）
        static float magnitude[FFT_SIZE / 2];
        for (int i = 0; i < FFT_SIZE / 2; i++) {
            float re = fft_buf[i * 2];
            float im = fft_buf[i * 2 + 1];
            magnitude[i] = sqrtf(re * re + im * im);
        }

        // 24kHz 专属对数频带边界系数（完全贴合人耳听觉切片）
        const int band_edges[17] = {
            1, 2, 3, 4, 5, 7, 9, 12, 16, 22, 30, 41, 56, 76, 103, 140, 200
        };

        // 7. 自适应扫描：捕捉最高峰值
        float raw_avgs[16] = {0.0f};
        float current_max = 0.0f;
        for (int b = 0; b < 16; b++) {
            float band_sum = 0.0f;
            int count = band_edges[b + 1] - band_edges[b];
            for (int i = band_edges[b]; i < band_edges[b + 1]; i++) {
                band_sum += magnitude[i];
            }
            raw_avgs[b] = band_sum / count;
            if (raw_avgs[b] > current_max) current_max = raw_avgs[b];
        }

        // 8. 峰值追踪 AGC 自适应自校准：快速上升，平滑衰减
        peak_tracker *= 0.94f; // 提高释放灵敏度
        if (current_max > peak_tracker) {
            peak_tracker = current_max;
        }
        if (peak_tracker < 0.05f) peak_tracker = 0.05f; // 提高安全低位保护线

        // 9. 频带非线性对数映射与压缩
        int bands[16] = {0};
        for (int b = 0; b < 16; b++) {
            float normalized = raw_avgs[b] / peak_tracker;
            float log_val = logf(1.0f + normalized * 10.0f) / logf(11.0f);
            int val = (int)(log_val * 13.5f); // 留出顶层视觉安全裕度

            // 局部增益微调加固
            if (b >= 10) val = (int)(val * 1.1f); // 适当强化高频细节
            if (b >= 13) val = val / 2;           // 极高频区做适当衰减控噪

            if (val > 15) val = 15;
            if (val < 0)  val = 0;
            bands[b] = val;
        }

        // 10. 瞬态低频重音鼓点（Beat）检测
        float low_energy = 0.0f;
        for (int i = band_edges[0]; i < band_edges[4]; i++) {
            low_energy += magnitude[i];
        }
        int is_beat = (low_energy > prev_low_energy * 1.3f &&
                       low_energy > 0.15f &&
                       rms > 0.015f) ? 1 : 0;
        prev_low_energy = low_energy * 0.6f + prev_low_energy * 0.4f;

        // 11. 串口下发：极致紧凑压缩
        static char fft_cmd[96]; 
        int len = snprintf(fft_cmd, sizeof(fft_cmd), "f,%d", is_beat);
        for (int i = 0; i < 16; i++) {
            len += snprintf(fft_cmd + len, sizeof(fft_cmd) - len, ",%d", bands[i]);
        }
        snprintf(fft_cmd + len, sizeof(fft_cmd) - len, "\n");
        uart_write_bytes(UART_NUM_1, fft_cmd, strlen(fft_cmd));

        // 12. 降频限速条件编译打印块
        #ifdef CONFIG_DEBUG_FFT_STREAM
        static int log_throttle_counter = 0;
        log_throttle_counter++;
        if (log_throttle_counter >= 15) { // 每 15 帧在控制台放行一次
            log_throttle_counter = 0;
            ESP_LOGI(TAG, "FFT beat=%d rms=%.3f | %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d",
                    is_beat, rms,
                    bands[0],  bands[1],  bands[2],  bands[3],
                    bands[4],  bands[5],  bands[6],  bands[7],
                    bands[8],  bands[9],  bands[10], bands[11],
                    bands[12], bands[13], bands[14], bands[15]);
        }
        #endif // CONFIG_DEBUG_FFT_STREAM

        // 🌟🌟🌟【终极修复死穴：滑动擦除】🌟🌟🌟
        // 计算完毕后，仅弹出当前处理完的 512 点，保留剩余碎包与下一帧无缝拼接！
        fft_accum.erase(fft_accum.begin(), fft_accum.begin() + FFT_SIZE);

    } while (0);

} else if (!is_music_mode_) {
    // 非音乐模式
}
// ================================================================================= */
/* // 🌟🌟🌟【一键拔闸调试宏：想关闭打印时，只需在前面加上 // 注释掉此行即可】🌟🌟🌟
#define CONFIG_DEBUG_FFT_STREAM 1

// ==================== 【真FFT音律拦截中枢 - 动态底噪双向防线完全体】====================
if (!task->pcm.empty() && is_music_mode_) {
    const int FFT_SIZE = 512;

    static std::vector<int16_t> fft_accum;
    static float hann_win[FFT_SIZE];
    static bool hann_ready = false;
    static float peak_tracker = 0.5f;   // 自适应主增益追踪器
    static float prev_low_energy = 0.0f;

    if (!hann_ready) {
        dsps_wind_hann_f32(hann_win, FFT_SIZE);
        dsps_fft2r_init_fc32(nullptr, FFT_SIZE);
        hann_ready = true;
    }

    // 1. 平滑将当前碎包拷入级联缓冲区
    fft_accum.insert(fft_accum.end(), task->pcm.begin(), task->pcm.end());

    // 2. 堆栈防溢出拦截：若积压超过 2 个完整窗口，强制修剪旧数据
    if ((int)fft_accum.size() > FFT_SIZE * 2) {
        fft_accum.erase(fft_accum.begin(), fft_accum.end() - FFT_SIZE);
    }

    // 3. 展开标准滑动窗口消费循环
    while ((int)fft_accum.size() >= FFT_SIZE) {
        
        // 计算当前 512 点的 RMS 能量
        float sum_sq = 0.0f;
        for (int i = 0; i < FFT_SIZE; i++) {
            float s = (float)fft_accum[i] / 32768.0f;
            sum_sq += s * s;
        }
        float rms = sqrtf(sum_sq / FFT_SIZE);

        // 核心防御一：提升熔断线到 0.012f，彻底斩断尾部底噪带来的频谱假死
        if (rms < 0.012f) {
            uart_write_bytes(UART_NUM_1, "f,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0\n", 34);
            peak_tracker = 0.2f; // 静音时主动调低增益线，为下一次大动态爆发做蓄力
            prev_low_energy = 0.0f;
            fft_accum.erase(fft_accum.begin(), fft_accum.begin() + FFT_SIZE);
            continue; // 跳过当前帧，继续消费后续可能存在的有效碎包
        }

        // 4. FFT 实部加汉宁窗，虚部清零
        static float fft_buf[FFT_SIZE * 2];
        for (int i = 0; i < FFT_SIZE; i++) {
            fft_buf[i * 2]     = ((float)fft_accum[i] / 32768.0f) * hann_win[i];
            fft_buf[i * 2 + 1] = 0.0f;
        }

        // 5. 执行硬件加速快速傅里叶变换
        dsps_fft2r_fc32(fft_buf, FFT_SIZE);
        dsps_bit_rev_fc32(fft_buf, FFT_SIZE);

        // 6. 提取单边谱幅度谱
        static float magnitude[FFT_SIZE / 2];
        for (int i = 0; i < FFT_SIZE / 2; i++) {
            float re = fft_buf[i * 2];
            float im = fft_buf[i * 2 + 1];
            magnitude[i] = sqrtf(re * re + im * im);
        }

        // 24kHz 对数频带切片边界
        const int band_edges[17] = {
            1, 2, 3, 4, 5, 7, 9, 12, 16, 22, 30, 41, 56, 76, 103, 140, 200
        };

        // 7. 扫描各个频段的原始平均能量
        float raw_avgs[16] = {0.0f};
        float current_max = 0.0f;
        for (int b = 0; b < 16; b++) {
            float band_sum = 0.0f;
            int count = band_edges[b + 1] - band_edges[b];
            for (int i = band_edges[b]; i < band_edges[b + 1]; i++) {
                band_sum += magnitude[i];
            }
            raw_avgs[b] = band_sum / count;
            if (raw_avgs[b] > current_max) {
                current_max = raw_avgs[b];
            }
        }

        // 8. 核心防线二：根据当前整体 RMS 的强弱，动态控制 peak_tracker 的释放速度
        if (rms < 0.05f) {
            // 歌曲处于弱音区或淡出期：强力加速下坠释放（系数改到 0.85），防止频谱挂在高位
            peak_tracker *= 0.85f; 
        } else {
            // 歌曲处于正常激昂区：保持平滑释放（0.96）
            peak_tracker *= 0.96f; 
        }

        if (current_max > peak_tracker) {
            peak_tracker = current_max; // 快速上升响应不变
        }
        
        // 维持合理的底线，杜绝分母过小引发的数据暴走
        if (peak_tracker < 0.05f) {
            peak_tracker = 0.05f; 
        }

        // 9. 频带非线性对数映射与动态门限剥离
        int bands[16] = {0};
        for (int b = 0; b < 16; b++) {
            // 核心防御三：单频带极弱信号强行压制，剥离环境杂散底噪
            if (raw_avgs[b] < 0.002f) {
                bands[b] = 0;
                continue;
            }

            float normalized = raw_avgs[b] / peak_tracker;
            float log_val = logf(1.0f + normalized * 9.0f) / logf(10.0f);
            int val = (int)(log_val * 14.0f); 

            // 局部频段视觉修正
            if (b >= 10) val = (int)(val * 1.1f);
            if (b >= 13) val = val / 2;

            if (val > 15) val = 15;
            if (val < 0)  val = 0;
            bands[b] = val;
        }

        // 10. 瞬态低频重音鼓点（Beat）检测
        float low_energy = 0.0f;
        for (int i = band_edges[0]; i < band_edges[4]; i++) {
            low_energy += magnitude[i];
        }
        int is_beat = (low_energy > prev_low_energy * 1.4f &&
                       low_energy > 0.25f &&
                       rms > 0.030f) ? 1 : 0;
        prev_low_energy = low_energy * 0.5f + prev_low_energy * 0.5f;

        // 11. 串口压缩下发
        static char fft_cmd[96]; 
        int len = snprintf(fft_cmd, sizeof(fft_cmd), "f,%d", is_beat);
        for (int i = 0; i < 16; i++) {
            len += snprintf(fft_cmd + len, sizeof(fft_cmd) - len, ",%d", bands[i]);
        }
        snprintf(fft_cmd + len, sizeof(fft_cmd) - len, "\n");
        uart_write_bytes(UART_NUM_1, fft_cmd, strlen(fft_cmd));

        // 12. 控制台降频打印
        #ifdef CONFIG_DEBUG_FFT_STREAM
        static int log_throttle_counter = 0;
        log_throttle_counter++;
        if (log_throttle_counter >= 12) { 
            log_throttle_counter = 0;
            ESP_LOGI("AudioService", "FFT beat=%d rms=%.3f peak=%.3f | %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d",
                    is_beat, rms, peak_tracker,
                    bands[0],  bands[1],  bands[2],  bands[3],
                    bands[4],  bands[5],  bands[6],  bands[7],
                    bands[8],  bands[9],  bands[10], bands[11],
                    bands[12], bands[13], bands[14], bands[15]);
        }
        #endif 

        // 🌟🌟🌟【平移滑动窗口】🌟🌟🌟
        fft_accum.erase(fft_accum.begin(), fft_accum.begin() + FFT_SIZE);
    }
} else if (!is_music_mode_) {
    // 非音乐模式处理...
} */
// ==================== 【真FFT音律拦截中枢 - 完全体流畅无抖动版】====================
if (!task->pcm.empty() && is_music_mode_) {
    const int FFT_SIZE = 512;
    // 🌟【限速优化】：既然我们采用了“新鲜数据全包干”策略，直接将 SEND_EVERY 设为 1
    // 腾出全部主频算力保证音频解码线程拥有最高优先级
    const int SEND_EVERY = 1; 

    static std::vector<int16_t> fft_accum;
    static int frame_skip_counter = 0;
    static float hann_win[FFT_SIZE];
    static bool hann_ready = false;
    static float peak_tracker = 0.01f;
    static float prev_low_energy = 0.0f;

    if (!hann_ready) {
        dsps_wind_hann_f32(hann_win, FFT_SIZE);
        dsps_fft2r_init_fc32(nullptr, FFT_SIZE);
        hann_ready = true;
    }

    // 🌟🌟🌟【核心修复：滑动窗口追加与防溢出保护机制】🌟🌟🌟
    // 将新到来的 PCM 样本追加到缓冲区，如果积压超过了 3 个完整窗口，裁剪掉多余 of 旧数据
    fft_accum.insert(fft_accum.end(), task->pcm.begin(), task->pcm.end());
    if ((int)fft_accum.size() > FFT_SIZE * 3) {
        fft_accum.erase(fft_accum.begin(), fft_accum.end() - FFT_SIZE * 2);
    }

    // 🌟🌟🌟【核心修复：通过 while 循环连续消耗样本以实现 100% 数据覆盖】🌟🌟🌟
    while ((int)fft_accum.size() >= FFT_SIZE) {
        // 先算RMS，静音直接归零并清空缓冲
        float sum_sq = 0.0f;
        for (int i = 0; i < FFT_SIZE; i++) {
            float s = (float)fft_accum[i] / 32768.0f;
            sum_sq += s * s;
        }
        float rms = sqrtf(sum_sq / FFT_SIZE);

        if (rms < 0.005f) {
            uart_write_bytes(UART_NUM_1, "f,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0\n", 34);
            fft_accum.clear();
            prev_low_energy = 0.0f;
            peak_tracker = 0.01f;
            break;
        }

        // 限速控制
        frame_skip_counter++;
        if (frame_skip_counter < SEND_EVERY) {
            fft_accum.erase(fft_accum.begin(), fft_accum.begin() + FFT_SIZE);
            continue; 
        }
        frame_skip_counter = 0;

        // FFT计算
        static float fft_buf[FFT_SIZE * 2];
        for (int i = 0; i < FFT_SIZE; i++) {
            fft_buf[i * 2]     = (float)fft_accum[i] / 32768.0f * hann_win[i];
            fft_buf[i * 2 + 1] = 0.0f;
        }

        dsps_fft2r_fc32(fft_buf, FFT_SIZE);
        dsps_bit_rev_fc32(fft_buf, FFT_SIZE);

        static float magnitude[FFT_SIZE / 2];
        for (int i = 0; i < FFT_SIZE / 2; i++) {
            float re = fft_buf[i * 2];
            float im = fft_buf[i * 2 + 1];
            magnitude[i] = sqrtf(re * re + im * im);
        }

        // 24kHz专属对数频带边界
        const int band_edges[17] = {
            1, 2, 3, 4, 5, 7, 9, 12, 16, 22, 30, 41, 56, 76, 103, 140, 200
        };

        // 自适应峰值归一化
        float raw_avgs[16] = {0.0f};
        float current_max = 0.0f;
        for (int b = 0; b < 16; b++) {
            float band_sum = 0.0f;
            int count = band_edges[b + 1] - band_edges[b];
            for (int i = band_edges[b]; i < band_edges[b + 1]; i++) {
                band_sum += magnitude[i];
            }
            raw_avgs[b] = band_sum / count;
            if (raw_avgs[b] > current_max) current_max = raw_avgs[b];
        }

        // 峰值追踪：快速上升，缓慢衰减
        peak_tracker *= 0.98f;
        if (current_max > peak_tracker) peak_tracker = current_max;
        if (peak_tracker < 0.01f) peak_tracker = 0.01f;

        // 频带映射
        int bands[16] = {0};
        for (int b = 0; b < 16; b++) {
            float normalized = raw_avgs[b] / peak_tracker;
            float log_val = logf(1.0f + normalized * 10.0f) / logf(11.0f);
            int val = (int)(log_val * 15.0f);
            if (b >= 10) val = (int)(val * 1.0f);
            if (b >= 13) val = val / 2;
            if (val > 15) val = 15;
            if (val < 0)  val = 0;
            bands[b] = val;
        }

        // 鼓点检测
        float low_energy = 0.0f;
        for (int i = band_edges[0]; i < band_edges[4]; i++) {
            low_energy += magnitude[i];
        }
        int is_beat = (low_energy > prev_low_energy * 1.3f &&
                       low_energy > 0.15f &&
                       rms > 0.015f) ? 1 : 0;
        prev_low_energy = low_energy * 0.6f + prev_low_energy * 0.4f;

        // 发送串口
        static char fft_cmd[128];
        int len = snprintf(fft_cmd, sizeof(fft_cmd), "f,%d", is_beat);
        for (int i = 0; i < 16; i++) {
            len += snprintf(fft_cmd + len, sizeof(fft_cmd) - len, ",%d", bands[i]);
        }
        snprintf(fft_cmd + len, sizeof(fft_cmd) - len, "\n");
        uart_write_bytes(UART_NUM_1, fft_cmd, strlen(fft_cmd));

        ESP_LOGI(TAG, "FFT beat=%d rms=%.3f | %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d",
                 is_beat, rms,
                 bands[0],  bands[1],  bands[2],  bands[3],
                 bands[4],  bands[5],  bands[6],  bands[7],
                 bands[8],  bands[9],  bands[10], bands[11],
                 bands[12], bands[13], bands[14], bands[15]);

        // 🌟【核心修复：滑动窗口移位】🌟
        fft_accum.erase(fft_accum.begin(), fft_accum.begin() + FFT_SIZE);
    }

} else if (!is_music_mode_) {
    // 非音乐模式静置
}

        /* Update the last output time */
        last_output_time_ = std::chrono::steady_clock::now();
        debug_statistics_.playback_count++;

#if CONFIG_USE_SERVER_AEC
        /* Record the timestamp for server AEC */
        if (task->timestamp > 0) {
            lock.lock();
            timestamp_queue_.push_back(task->timestamp);
        }
#endif
    }

    ESP_LOGW(TAG, "Audio output task stopped");
}

void AudioService::OpusCodecTask() {
    while (true) {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        audio_queue_cv_.wait(lock, [this]() {
            return service_stopped_ ||
                (!audio_encode_queue_.empty() && audio_send_queue_.size() < MAX_SEND_PACKETS_IN_QUEUE) ||
                (!audio_decode_queue_.empty() && audio_playback_queue_.size() < MAX_PLAYBACK_TASKS_IN_QUEUE);
        });
        if (service_stopped_) {
            break;
        }

        /* Decode the audio from decode queue */
        if (!audio_decode_queue_.empty() && audio_playback_queue_.size() < MAX_PLAYBACK_TASKS_IN_QUEUE) {
            auto packet = std::move(audio_decode_queue_.front());
            audio_decode_queue_.pop_front();
            audio_queue_cv_.notify_all();
            lock.unlock();

            auto task = std::make_unique<AudioTask>();
            task->type = kAudioTaskTypeDecodeToPlaybackQueue;
            task->timestamp = packet->timestamp;

            SetDecodeSampleRate(packet->sample_rate, packet->frame_duration);
            if (opus_decoder_ != nullptr) {
                task->pcm.resize(decoder_frame_size_);
                esp_audio_dec_in_raw_t raw = {
                    .buffer = (uint8_t *)(packet->payload.data()),
                    .len = (uint32_t)(packet->payload.size()),
                    .consumed = 0,
                    .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
                };
                esp_audio_dec_out_frame_t out_frame = {
                    .buffer = (uint8_t *)(task->pcm.data()),
                    .len = (uint32_t)(task->pcm.size() * sizeof(int16_t)),
                    .decoded_size = 0,
                };
                esp_audio_dec_info_t dec_info = {};
                std::unique_lock<std::mutex> decoder_lock(decoder_mutex_);
                auto ret = esp_opus_dec_decode(opus_decoder_, &raw, &out_frame, &dec_info);
                decoder_lock.unlock();
                if (ret == ESP_AUDIO_ERR_OK) {
                    task->pcm.resize(out_frame.decoded_size / sizeof(int16_t));
                    if (decoder_sample_rate_ != codec_->output_sample_rate() && output_resampler_ != nullptr) {
                        uint32_t target_size = 0;
                        esp_ae_rate_cvt_get_max_out_sample_num(output_resampler_, task->pcm.size(), &target_size);
                        std::vector<int16_t> resampled(target_size);
                        uint32_t actual_output = target_size;
                        esp_ae_rate_cvt_process(output_resampler_, (esp_ae_sample_t)task->pcm.data(), task->pcm.size(),
                                                (esp_ae_sample_t)resampled.data(), &actual_output);
                        resampled.resize(actual_output);
                        task->pcm = std::move(resampled);
                    }
                    lock.lock();
                    audio_playback_queue_.push_back(std::move(task));
                    audio_queue_cv_.notify_all();
                    debug_statistics_.decode_count++;
                } else {
                    ESP_LOGE(TAG, "Failed to decode audio after resize, error code: %d", ret);
                    lock.lock();
                }
            } else {
                ESP_LOGE(TAG, "Audio decoder is not configured");
                lock.lock();
            }
            debug_statistics_.decode_count++;
        }
        /* Encode the audio to send queue */
        if (!audio_encode_queue_.empty() && audio_send_queue_.size() < MAX_SEND_PACKETS_IN_QUEUE) {
            auto task = std::move(audio_encode_queue_.front());
            audio_encode_queue_.pop_front();
            audio_queue_cv_.notify_all();
            lock.unlock();

            auto packet = std::make_unique<AudioStreamPacket>();
            packet->frame_duration = OPUS_FRAME_DURATION_MS;
            packet->sample_rate = 16000;
            packet->timestamp = task->timestamp;

            if (opus_encoder_ != nullptr && task->pcm.size() == encoder_frame_size_) {
                std::vector<uint8_t> buf(encoder_outbuf_size_);
                esp_audio_enc_in_frame_t in = {
                    .buffer = (uint8_t *)(task->pcm.data()),
                    .len = (uint32_t)(encoder_frame_size_ * sizeof(int16_t)),
                };
                esp_audio_enc_out_frame_t out = {
                    .buffer = buf.data(),
                    .len = (uint32_t)encoder_outbuf_size_,
                    .encoded_bytes = 0,
                };
                auto ret = esp_opus_enc_process(opus_encoder_, &in, &out);
                if (ret == ESP_AUDIO_ERR_OK) {
                    packet->payload.assign(buf.data(), buf.data() + out.encoded_bytes);

                    if (task->type == kAudioTaskTypeEncodeToSendQueue) {
                        {
                            std::lock_guard<std::mutex> lock2(audio_queue_mutex_);
                            audio_send_queue_.push_back(std::move(packet));
                        }
                        if (callbacks_.on_send_queue_available) {
                            callbacks_.on_send_queue_available();
                        }
                    } else if (task->type == kAudioTaskTypeEncodeToTestingQueue) {
                        std::lock_guard<std::mutex> lock2(audio_queue_mutex_);
                        audio_testing_queue_.push_back(std::move(packet));
                    }
                    debug_statistics_.encode_count++;
                } else {
                    ESP_LOGE(TAG, "Failed to encode audio, error code: %d", ret);
                }
            } else {
                ESP_LOGE(TAG, "Failed to encode audio: encoder not configured or invalid frame size (got %u, expected %u)",
                         task->pcm.size(), encoder_frame_size_);
            }
            lock.lock();
        }
    }

    ESP_LOGW(TAG, "Opus codec task stopped");
}

void AudioService::SetDecodeSampleRate(int sample_rate, int frame_duration) {
    if (decoder_sample_rate_ == sample_rate && decoder_duration_ms_ == frame_duration) {
        return;
    }
    std::unique_lock<std::mutex> decoder_lock(decoder_mutex_);
    if (opus_decoder_ != nullptr) {
        esp_opus_dec_close(opus_decoder_);
        opus_decoder_ = nullptr;
    }
    decoder_lock.unlock();
    esp_opus_dec_cfg_t opus_dec_cfg = OPUS_DEC_CFG(sample_rate, frame_duration);
    auto ret = esp_opus_dec_open(&opus_dec_cfg, sizeof(esp_opus_dec_cfg_t), &opus_decoder_);
    if (opus_decoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create audio decoder, error code: %d", ret);
        return;
    }
    decoder_sample_rate_ = sample_rate;
    decoder_duration_ms_ = frame_duration;
    decoder_frame_size_ = decoder_sample_rate_ / 1000 * frame_duration;

    auto codec = Board::GetInstance().GetAudioCodec();
    if (decoder_sample_rate_ != codec->output_sample_rate()) {
        ESP_LOGI(TAG, "Resampling audio from %d to %d", decoder_sample_rate_, codec->output_sample_rate());
        if (output_resampler_ != nullptr) {
            esp_ae_rate_cvt_close(output_resampler_);
            output_resampler_ = nullptr;
        }
        esp_ae_rate_cvt_cfg_t output_resampler_cfg = RATE_CVT_CFG(
            decoder_sample_rate_, codec->output_sample_rate(), ESP_AUDIO_MONO);
        auto resampler_ret = esp_ae_rate_cvt_open(&output_resampler_cfg, &output_resampler_);
        if (output_resampler_ == nullptr) {
            ESP_LOGE(TAG, "Failed to create output resampler, error code: %d", resampler_ret);
        }
    }
}

void AudioService::PushTaskToEncodeQueue(AudioTaskType type, std::vector<int16_t>&& pcm) {
    auto task = std::make_unique<AudioTask>();
    task->type = type;
    task->pcm = std::move(pcm);
    /* Push the task to the encode queue */
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);

    /* If the task is to send queue, we need to set the timestamp */
    if (type == kAudioTaskTypeEncodeToSendQueue && !timestamp_queue_.empty()) {
        if (timestamp_queue_.size() <= MAX_TIMESTAMPS_IN_QUEUE) {
            task->timestamp = timestamp_queue_.front();
        } else {
            ESP_LOGW(TAG, "Timestamp queue (%u) is full, dropping timestamp", timestamp_queue_.size());
        }
        timestamp_queue_.pop_front();
    }

    audio_queue_cv_.wait(lock, [this]() { return audio_encode_queue_.size() < MAX_ENCODE_TASKS_IN_QUEUE; });
    audio_encode_queue_.push_back(std::move(task));
    audio_queue_cv_.notify_all();
}

bool AudioService::PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet, bool wait) {
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    if (audio_decode_queue_.size() >= MAX_DECODE_PACKETS_IN_QUEUE) {
        if (wait) {
            audio_queue_cv_.wait(lock, [this]() { return audio_decode_queue_.size() < MAX_DECODE_PACKETS_IN_QUEUE; });
        } else {
            return false;
        }
    }
    audio_decode_queue_.push_back(std::move(packet));
    audio_queue_cv_.notify_all();
    return true;
}

std::unique_ptr<AudioStreamPacket> AudioService::PopPacketFromSendQueue() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    if (audio_send_queue_.empty()) {
        return nullptr;
    }
    auto packet = std::move(audio_send_queue_.front());
    audio_send_queue_.pop_front();
    audio_queue_cv_.notify_all();
    return packet;
}

void AudioService::EncodeWakeWord() {
    if (wake_word_) {
        wake_word_->EncodeWakeWordData();
    }
}

const std::string& AudioService::GetLastWakeWord() const {
    return wake_word_->GetLastDetectedWakeWord();
}

std::unique_ptr<AudioStreamPacket> AudioService::PopWakeWordPacket() {
    auto packet = std::make_unique<AudioStreamPacket>();
    if (wake_word_->GetWakeWordOpus(packet->payload)) {
        return packet;
    }
    return nullptr;
}

void AudioService::EnableWakeWordDetection(bool enable) {
    if (!wake_word_) {
        return;
    }

    ESP_LOGD(TAG, "%s wake word detection", enable ? "Enabling" : "Disabling");
    if (enable) {
        if (!wake_word_initialized_) {
            if (!wake_word_->Initialize(codec_, models_list_)) {
                ESP_LOGE(TAG, "Failed to initialize wake word");
                return;
            }
            wake_word_initialized_ = true;
        }
        // Reset input resampler to clear cached data from previous mode (e.g. AudioProcessor)
        // This prevents buffer overflow when switching between different feed sizes
        {
            std::lock_guard<std::mutex> lock(input_resampler_mutex_);
            if (input_resampler_ != nullptr) {
                esp_ae_rate_cvt_reset(input_resampler_);
            }
        }
        wake_word_->Start();
        xEventGroupSetBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
    } else {
        wake_word_->Stop();
        xEventGroupClearBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
    }
}

void AudioService::EnableVoiceProcessing(bool enable) {
    ESP_LOGD(TAG, "%s voice processing", enable ? "Enabling" : "Disabling");
    if (enable) {
        if (!audio_processor_initialized_) {
            audio_processor_->Initialize(codec_, OPUS_FRAME_DURATION_MS, models_list_);
            audio_processor_initialized_ = true;
        }

        /* We should make sure no audio is playing */
        ResetDecoder();
        audio_input_need_warmup_ = true;
        // Reset input resampler to clear cached data from previous mode (e.g. WakeWord)
        // This prevents buffer overflow when switching between different feed sizes
        {
            std::lock_guard<std::mutex> lock(input_resampler_mutex_);
            if (input_resampler_ != nullptr) {
                esp_ae_rate_cvt_reset(input_resampler_);
            }
        }
        audio_processor_->Start();
        xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
    } else {
        audio_processor_->Stop();
        xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
    }
}

void AudioService::EnableAudioTesting(bool enable) {
    ESP_LOGI(TAG, "%s audio testing", enable ? "Enabling" : "Disabling");
    if (enable) {
        xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING);
    } else {
        xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING);
        /* Copy audio_testing_queue_ to audio_decode_queue_ */
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        audio_decode_queue_ = std::move(audio_testing_queue_);
        audio_queue_cv_.notify_all();
    }
}

void AudioService::EnableDeviceAec(bool enable) {
    ESP_LOGI(TAG, "%s device AEC", enable ? "Enabling" : "Disabling");
    if (!audio_processor_initialized_) {
        audio_processor_->Initialize(codec_, OPUS_FRAME_DURATION_MS, models_list_);
        audio_processor_initialized_ = true;
    }

    audio_processor_->EnableDeviceAec(enable);
}

void AudioService::SetCallbacks(AudioServiceCallbacks& callbacks) {
    callbacks_ = callbacks;
}

void AudioService::PlaySound(const std::string_view& ogg) {
    if (!codec_->output_enabled()) {
        esp_timer_stop(audio_power_timer_);
        esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
        codec_->EnableOutput(true);
    }

    const auto* buf = reinterpret_cast<const uint8_t*>(ogg.data());
    size_t size = ogg.size();

    auto demuxer = std::make_unique<OggDemuxer>();
    demuxer->OnDemuxerFinished([this](const uint8_t* data, int sample_rate, size_t size){
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->sample_rate = sample_rate;
        packet->frame_duration = 60;
        packet->payload.resize(size);
        std::memcpy(packet->payload.data(), data, size);
        PushPacketToDecodeQueue(std::move(packet), true);
    });
    demuxer->Reset();
    demuxer->Process(buf, size);
}

bool AudioService::IsIdle() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    return audio_encode_queue_.empty() && audio_decode_queue_.empty() && audio_playback_queue_.empty() && audio_testing_queue_.empty();
}

void AudioService::WaitForPlaybackQueueEmpty() {
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    audio_queue_cv_.wait(lock, [this]() { 
        return service_stopped_ || (audio_decode_queue_.empty() && audio_playback_queue_.empty()); 
    });
}

void AudioService::ResetDecoder() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    std::unique_lock<std::mutex> decoder_lock(decoder_mutex_);
    if (opus_decoder_ != nullptr) {
        esp_opus_dec_reset(opus_decoder_);
    }
    decoder_lock.unlock();
    timestamp_queue_.clear();
    audio_decode_queue_.clear();
    audio_playback_queue_.clear();
    audio_testing_queue_.clear();
    audio_queue_cv_.notify_all();
}

void AudioService::CheckAndUpdateAudioPowerState() {
    auto now = std::chrono::steady_clock::now();
    auto input_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_input_time_).count();
    auto output_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_output_time_).count();
    if (input_elapsed > AUDIO_POWER_TIMEOUT_MS && codec_->input_enabled()) {
        codec_->EnableInput(false);
    }
    if (output_elapsed > AUDIO_POWER_TIMEOUT_MS && codec_->output_enabled()) {
        // Keep TX clock when duplex RX is active; otherwise RX may stall on some boards.
        if (!(codec_->duplex() && codec_->input_enabled())) {
            codec_->EnableOutput(false);
        }
    }
    if (!codec_->input_enabled() && !codec_->output_enabled()) {
        esp_timer_stop(audio_power_timer_);
    }
}

void AudioService::SetModelsList(srmodel_list_t* models_list) {
    models_list_ = models_list;

#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4
    if (esp_srmodel_filter(models_list_, ESP_MN_PREFIX, NULL) != nullptr) {
        wake_word_ = std::make_unique<CustomWakeWord>();
    } else if (esp_srmodel_filter(models_list_, ESP_WN_PREFIX, NULL) != nullptr) {
        wake_word_ = std::make_unique<AfeWakeWord>();
    } else {
        wake_word_ = nullptr;
    }
#else
    if (esp_srmodel_filter(models_list_, ESP_WN_PREFIX, NULL) != nullptr) {
        wake_word_ = std::make_unique<EspWakeWord>();
    } else {
        wake_word_ = nullptr;
    }
#endif

    if (wake_word_) {
        wake_word_->OnWakeWordDetected([this](const std::string& wake_word) {
            if (callbacks_.on_wake_word_detected) {
                callbacks_.on_wake_word_detected(wake_word);
            }
        });
    }
}

bool AudioService::IsAfeWakeWord() {
#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4
    return wake_word_ != nullptr && dynamic_cast<AfeWakeWord*>(wake_word_.get()) != nullptr;
#else
    return false;
#endif
}

/* // =================== 【🌟完全体：经典蓝牙音箱的全面接管与注销状态机】 ===================

void AudioService::StartBluetoothSpeakerMode() {
    if (IsBluetoothSpeakerRunning()) return;

    ESP_LOGE("BT_SINK", "🚀 正在全面接管音频总线：重置对讲队列，启动经典蓝牙 A2DP Sink...");

    // 1. 强行清空大模型原有的网络解码对讲队列，腾出空闲硬件通道
    ResetDecoder();

    // 2. 强行锁定超然已经写好的本地 FFT 律动开关，并设置事件状态标志位
    SetMusicMode(true);
    xEventGroupSetBits(event_group_, AS_EVENT_BLUETOOTH_SPEAKER_RUNNING);

    // 3. 如果是开机后第一次进入听歌模式，初始化乐鑫经典蓝牙控制器
    if (!bluetooth_speaker_initialized_) {
        esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        esp_bt_controller_init(&bt_cfg);
        esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
        esp_bluedroid_init();
        esp_bluedroid_enable();

        // 设置蓝牙广播名称
        esp_bt_dev_set_device_name("Kurisu_A2DP_Speaker");

        // 初始化 Sink 架构，并挂载我们的核心流拦截钩子
        esp_a2d_sink_init();
        esp_a2d_sink_register_data_callback(bluetooth_a2dp_sink_data_cb);

        bluetooth_speaker_initialized_ = true;
    }

    // 4. 开启蓝牙对外的可见性与可连接性（允许手机配对连接）
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    
    // 5. 【🌟终极合龙补丁】确保重采样器在蓝牙模式下 100% 实例化就绪
    // 将手机传过来的 44100Hz 单声道数据，强行降频转码为小智扬声器和你的 FFT 最喜欢的 16000Hz 黄金格式
    if (output_resampler_ == nullptr) {
        ESP_LOGE("BT_SINK", "⚙️ 正在为经典蓝牙流动态创建 44.1kHz -> 16kHz 降频重采样矩阵...");
        esp_ae_rate_cvt_cfg_t output_resampler_cfg = RATE_CVT_CFG(44100, 16000, ESP_AUDIO_MONO);
        esp_ae_rate_cvt_open(&output_resampler_cfg, &output_resampler_);
    }

    // 6. 强行拉起板载音频音频功放芯片的输出使能
    codec_->EnableOutput(true);
}

void AudioService::StopBluetoothSpeakerMode() {
    if (!IsBluetoothSpeakerRunning()) return;

    ESP_LOGW("BT_SINK", "🛑 收到大模型打断聊天指令：断开手机蓝牙连接，释放硬件控制权...");
    if (bluetooth_speaker_initialized_) {
        esp_a2d_sink_register_data_callback(nullptr);
    }
    // 1. 掐断蓝牙对外的可见性
    esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);

    // 2. 解锁放歌状态，清空残留音律队列
    xEventGroupClearBits(event_group_, AS_EVENT_BLUETOOTH_SPEAKER_RUNNING);
    SetMusicMode(false);
    ResetDecoder();

    // 3. 【🌟退场补丁】安全销毁蓝牙临时的重采样器，防止污染原生语音通话通道
    std::unique_lock<std::mutex> decoder_lock(decoder_mutex_);
    if (output_resampler_ != nullptr) {
        esp_ae_rate_cvt_close(output_resampler_);
        output_resampler_ = nullptr;
    }

    if (bluetooth_speaker_initialized_) {
        esp_a2d_sink_register_data_callback(bluetooth_a2dp_sink_data_cb);
    }
}

// 🌟【修复：补齐 AudioService:: 限定符】新增公开类成员函数，提供合法的内部线程安全通道
void AudioService::PostBluetoothData(std::unique_ptr<AudioTask> task) {
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    if (audio_playback_queue_.size() < 6) {
        audio_playback_queue_.push_back(std::move(task));
        audio_queue_cv_.notify_all(); // 瞬间唤醒你的 AudioOutputTask 跑 FFT
    }
}

// 🌟 手机源源不断通过蓝牙吐过来的标准 44100Hz 双声道原始 PCM 信号在此被我们截获
static void bluetooth_a2dp_sink_data_cb(const uint8_t *data, uint32_t len) {
    auto& audio_svc = Application::GetInstance().GetAudioService();
    
    if (!audio_svc.GetMusicMode()) {
        return;
    }

    // 强行将字节指针转换为 16-bit 有符号音频数据指针
    const int16_t* pcm_src = (const int16_t*)data;
    size_t total_samples = len / sizeof(int16_t);

    // 1. 高级转码优化：双声道融合成单声道
    std::vector<int16_t> mono_44k(total_samples / 2);
    for (size_t i = 0, j = 0; i < mono_44k.size(); i++, j += 2) {
        mono_44k[i] = (int16_t)(((int32_t)pcm_src[j] + (int32_t)pcm_src[j + 1]) / 2);
    }

    // 2. 创建小智原生的 AudioTask 承载结构体
    auto task = std::make_unique<AudioTask>();
    task->type = kAudioTaskTypeDecodeToPlaybackQueue;
    task->timestamp = esp_timer_get_time() / 1000;

    // 3. 强制调用乐鑫底层的 output_resampler_ 将 44.1kHz 重采样至标准 16kHz
    if (audio_svc.output_resampler_ != nullptr) {
        uint32_t in_samples = mono_44k.size(); // 🌟 显式转为标准无符号标量
        uint32_t target_size = 0;
        esp_ae_rate_cvt_get_max_out_sample_num(audio_svc.output_resampler_, mono_44k.size(), &target_size);
        task->pcm.resize(target_size);
        
        uint32_t actual_output = target_size;
        void* in_buffers[1] = { mono_44k.data() };
        void* out_buffers[1] = { task->pcm.data() };
        esp_ae_rate_cvt_process(audio_svc.output_resampler_, in_buffers, in_samples,
                                out_buffers, &actual_output);
        task->pcm.resize(actual_output);
    } else {
        // 如果重采样未就绪，则使用裸单声道兜底
        task->pcm = std::move(mono_44k);
    }

    // 4. 🌟【修复：完美合龙】直接调用我们刚刚做好的类安全通道，顺畅入队并通知条件变量！
    audio_svc.PostBluetoothData(std::move(task));
}
// ==================================================================================================== */