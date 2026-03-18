#include "compact_wifi_board.h"
#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "display/oled_display.h"
#include "system_reset.h"
#include "application.h"
#include "device_state.h"
#include "protocol.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "lamp_controller.h"
#include "led/single_led.h"
#include "assets/lang_config.h"
#include <wifi_manager.h>

// Forward declaration for Application function
extern "C" void HandleMotorActionForApplication(int direction, int speed, int duration_ms, int priority);
extern "C" void HandleMotorActionForEmotion(const char* emotion);



#include <esp_log.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <utility>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include <atomic>
//新
#include <esp_http_client.h>
#include <cJSON.h>
#include <mp3dec.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#ifdef SH1106
#include <esp_lcd_panel_sh1106.h>
#endif

#define TAG "CompactWifiBoard"

// 新音乐服务器配置
#define MUSIC_SERVER_IP "8.136.110.80"
#define MUSIC_SERVER_PORT 3000
#define AUDIO_DECODE_SERVER_IP "8.136.110.80"
#define AUDIO_DECODE_SERVER_PORT 8080
#define MUSIC_COOKIE "MUSIC_U=00F051AD5CE4072B723DCD39C345C4D12CA696B1E2006F52C42A74590A741868160C06260F3F0D128BAFECDB596EA237DA73A9964EA66715544A1659E3D09C36DF90FC020BC9B2DD742C45EF3E0923C8DD60BF45DF937EDAE0B5848FE830AFF3BEED853E7F8F22A4B044DAD521FC590E71B12ADCF7B923C48ED8D01CE43A98BF984C79279AD06264073EDA68E82BC436AFF7F2F42D02D4014113FC283BB48002C14A99A4A7B542FA0128CF8B4D2CE8E00F7AD7E02A100F17E92E709BB648B28938F57FC8A5FEBB9B3406BA09F1AE7AF5CCDBAD44A7BD526322747E871E1F75935DFB25FBCFC0F182D8807F66547A519F43B4FF9E1292EA73701E0CB455A4EEF664AADC2643809516889F86D07CA601625919EB9EBB294A97CB3C66A56A4DA936DAF14F749A76BB3652D709CDAAA53502890180A7EFC928C406A22926C67265EDA68B26D0613B0D8D11BA5002B421A93E6079AC0342B45A140C951861F5648F6CBDF4AAB304906CB50E59DD4E6C22A7B680B41264B341D39F8A2982B493C123FE293CFE95A482B223E24026C1FA7162DDCC047197025AD58DF453E1E8C12DBEC4CC"

// Global pointer to motor controller for Application callbacks
class CompactWifiBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t display_i2c_bus_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;
    Button boot_button_;
    Button touch_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    
    // 音乐播放任务句柄
    TaskHandle_t music_playback_task_ = nullptr;
    
    // 音乐播放状态标志
    std::atomic<bool> is_playing_music_{false};

    void InitializeDisplayI2c() {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = (i2c_port_t)0,
            .sda_io_num = DISPLAY_SDA_PIN,
            .scl_io_num = DISPLAY_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &display_i2c_bus_));
    }

    void InitializeSsd1306Display() {
        // SSD1306 config
        esp_lcd_panel_io_i2c_config_t io_config = {
            .dev_addr = 0x3C,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .control_phase_bytes = 1,
            .dc_bit_offset = 6,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .flags = {
                .dc_low_on_data = 0,
                .disable_control_phase = 0,
            },
            .scl_speed_hz = 400 * 1000,
        };

        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c_v2(display_i2c_bus_, &io_config, &panel_io_));

        ESP_LOGI(TAG, "Install SSD1306 driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = -1;
        panel_config.bits_per_pixel = 1;

        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };
        panel_config.vendor_config = &ssd1306_config;

#ifdef SH1106
        ESP_ERROR_CHECK(esp_lcd_new_panel_sh1106(panel_io_, &panel_config, &panel_));
#else
        ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_));
#endif
        ESP_LOGI(TAG, "SSD1306 driver installed");

        // Reset the display
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize display");
            display_ = new NoDisplay();
            return;
        }
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, false));

        // Set the display to on
        ESP_LOGI(TAG, "Turning display on");
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

        display_ = new OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
    }

    void InitializeButtons() {
        boot_button_.OnClick([]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                // Get the current board instance and call EnterWifiConfigMode
                auto board = static_cast<WifiBoard*>(&Board::GetInstance());
                board->EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
        touch_button_.OnPressDown([]() {
            Application::GetInstance().StartListening();
        });
        touch_button_.OnPressUp([]() {
            Application::GetInstance().StopListening();
        });

        volume_up_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() + 10;
            if (volume > 100) {
                volume = 100;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });

        volume_up_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(100);
            GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
        });

        volume_down_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() - 10;
            if (volume < 0) {
                volume = 0;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });

        volume_down_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(0);
            GetDisplay()->ShowNotification(Lang::Strings::MUTED);
        });
    }

    // 物联网初始化，逐步迁移到 MCP 协议
    void InitializeTools() {
        static LampController lamp(LAMP_GPIO);
        // No longer using MotorController - all motor control goes through Application

        // Add motor control tools to MCP server
        auto& mcp_server = McpServer::GetInstance();

        mcp_server.AddTool("self.motor.move_forward",
            "Move the robot forward with specified speed and duration.\n"
            "Args:\n"
            "  `speed_percent`: Motor speed (0-100), default 100\n"
            "  `duration_ms`: Movement duration in milliseconds, default 5000\n"
            "Return:\n"
            "  Success message with parameters",
            PropertyList({
                Property("speed_percent", kPropertyTypeInteger, 100, 0, 100),
                Property("duration_ms", kPropertyTypeInteger, 5000, 100, 10000)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                int speed = properties["speed_percent"].value<int>();
                int duration = properties["duration_ms"].value<int>();
                MotorMoveForward(duration, speed);
                return std::string("Moved forward at ") + std::to_string(speed) + "% speed for " + std::to_string(duration) + "ms";
            });

        mcp_server.AddTool("self.motor.move_backward",
            "Move the robot backward with specified speed and duration.\n"
            "Args:\n"
            "  `speed_percent`: Motor speed (0-100), default 100\n"
            "  `duration_ms`: Movement duration in milliseconds, default 5000\n"
            "Return:\n"
            "  Success message with parameters",
            PropertyList({
                Property("speed_percent", kPropertyTypeInteger, 100, 0, 100),
                Property("duration_ms", kPropertyTypeInteger, 5000, 100, 10000)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                int speed = properties["speed_percent"].value<int>();
                int duration = properties["duration_ms"].value<int>();
                MotorMoveBackward(duration, speed);
                return std::string("Moved backward at ") + std::to_string(speed) + "% speed for " + std::to_string(duration) + "ms";
            });

        mcp_server.AddTool("self.motor.spin_around",
            "Spin the robot around in a full circle with specified speed.\n"
            "Args:\n"
            "  `speed_percent`: Motor speed (0-100), default 100\n"
            "Return:\n"
            "  Success message",
            PropertyList({
                Property("speed_percent", kPropertyTypeInteger, 100, 0, 100)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                int speed = properties["speed_percent"].value<int>();
                MotorSpinAround(speed);
                return std::string("Spin around completed at ") + std::to_string(speed) + "% speed";
            });

        mcp_server.AddTool("self.motor.turn_left",
            "Turn the robot left with specified speed and duration.\n"
            "Args:\n"
            "  `speed_percent`: Motor speed (0-100), default 100\n"
            "  `duration_ms`: Turn duration in milliseconds, default 600 (approx 90 degrees)\n"
            "Return:\n"
            "  Success message with parameters",
            PropertyList({
                Property("speed_percent", kPropertyTypeInteger, 100, 0, 100),
                Property("duration_ms", kPropertyTypeInteger, 600, 100, 5000)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                int speed = properties["speed_percent"].value<int>();
                int duration = properties["duration_ms"].value<int>();
                MotorTurnLeftDuration(duration, speed);
                return std::string("Turned left at ") + std::to_string(speed) + "% speed for " + std::to_string(duration) + "ms";
            });

        mcp_server.AddTool("self.motor.turn_right",
            "Turn the robot right with specified speed and duration.\n"
            "Args:\n"
            "  `speed_percent`: Motor speed (0-100), default 100\n"
            "  `duration_ms`: Turn duration in milliseconds, default 600 (approx 90 degrees)\n"
            "Return:\n"
            "  Success message with parameters",
            PropertyList({
                Property("speed_percent", kPropertyTypeInteger, 100, 0, 100),
                Property("duration_ms", kPropertyTypeInteger, 600, 100, 5000)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                int speed = properties["speed_percent"].value<int>();
                int duration = properties["duration_ms"].value<int>();
                MotorTurnRightDuration(duration, speed);
                return std::string("Turned right at ") + std::to_string(speed) + "% speed for " + std::to_string(duration) + "ms";
            });

        mcp_server.AddTool("self.motor.quick_forward",
            "Quick forward movement for 0.5 seconds with specified speed.\n"
            "Args:\n"
            "  `speed_percent`: Motor speed (0-100), default 100\n"
            "Return:\n"
            "  Success message",
            PropertyList({
                Property("speed_percent", kPropertyTypeInteger, 100, 0, 100)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                int speed = properties["speed_percent"].value<int>();
                MotorQuickForward(speed);
                return std::string("Quick forward movement completed at ") + std::to_string(speed) + "% speed";
            });

        mcp_server.AddTool("self.motor.quick_backward",
            "Quick backward movement for 0.5 seconds with specified speed.\n"
            "Args:\n"
            "  `speed_percent`: Motor speed (0-100), default 100\n"
            "Return:\n"
            "  Success message",
            PropertyList({
                Property("speed_percent", kPropertyTypeInteger, 100, 0, 100)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                int speed = properties["speed_percent"].value<int>();
                MotorQuickBackward(speed);
                return std::string("Quick backward movement completed at ") + std::to_string(speed) + "% speed";
            });

        mcp_server.AddTool("self.motor.wiggle",
            "Make the robot perform a quick wiggle movement (turn right briefly).\n"
            "Args:\n"
            "  `speed_percent`: Motor speed (0-100), default 100\n"
            "Return:\n"
            "  Success message",
            PropertyList({
                Property("speed_percent", kPropertyTypeInteger, 100, 0, 100)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                int speed = properties["speed_percent"].value<int>();
                MotorWiggle(speed);
                return std::string("Wiggle movement completed at ") + std::to_string(speed) + "% speed";
            });

        mcp_server.AddTool("self.motor.dance",
            "Make the robot perform a quick dance movement (move forward briefly).\n"
            "Args:\n"
            "  `speed_percent`: Motor speed (0-100), default 100\n"
            "Return:\n"
            "  Success message",
            PropertyList({
                Property("speed_percent", kPropertyTypeInteger, 100, 0, 100)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                int speed = properties["speed_percent"].value<int>();
                MotorDance(speed);
                return std::string("Dance movement completed at ") + std::to_string(speed) + "% speed";
            });

        mcp_server.AddTool("self.motor.stop",
            "Stop all motor movement immediately",
            PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                HandleMotorActionForApplication(0, 0, 0, 2); // Stop, high priority
                return std::string("Motor stopped");
            });

        // Animation actions
        mcp_server.AddTool("self.motor.wake_up",
            "Play wake up animation - excited movement to greet the user",
            PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                HandleMotorActionForEmotion("wake");
                return std::string("Wake up animation played");
            });

        mcp_server.AddTool("self.motor.happy",
            "Play happy animation - playful movements to show joy",
            PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                HandleMotorActionForEmotion("happy");
                return std::string("Happy animation played");
            });

        mcp_server.AddTool("self.motor.sad",
            "Play sad animation - slow backward movements to show sadness",
            PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                HandleMotorActionForEmotion("sad");
                return std::string("Sad animation played");
            });

        mcp_server.AddTool("self.motor.thinking",
            "Play thinking animation - small left-right movements",
            PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                HandleMotorActionForEmotion("thinking");
                return std::string("Thinking animation played");
            });

        mcp_server.AddTool("self.motor.listening",
            "Play listening animation - gentle swaying movements",
            PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                HandleMotorActionForEmotion("listening");
                return std::string("Listening animation played");
            });

        mcp_server.AddTool("self.motor.speaking",
            "Play speaking animation - forward thrusts",
            PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                HandleMotorActionForEmotion("speaking");
                return std::string("Speaking animation played");
            });

        mcp_server.AddTool("self.motor.excited",
            "Play excited animation - fast movements in multiple directions",
            PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                HandleMotorActionForEmotion("excited");
                return std::string("Excited animation played");
            });

        mcp_server.AddTool("self.motor.loving",
            "Play loving animation - gentle forward movements",
            PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                HandleMotorActionForEmotion("loving");
                return std::string("Loving animation played");
            });

        mcp_server.AddTool("self.motor.angry",
            "Play angry animation - strong backward and forward movements",
            PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                HandleMotorActionForEmotion("angry");
                return std::string("Angry animation played");
            });

        mcp_server.AddTool("self.motor.surprised",
            "Play surprised animation - quick backward then forward movement",
            PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                HandleMotorActionForEmotion("surprised");
                return std::string("Surprised animation played");
            });

        mcp_server.AddTool("self.motor.confused",
            "Play confused animation - hesitant left-right movements",
            PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                HandleMotorActionForEmotion("confused");
                return std::string("Confused animation played");
            });

        mcp_server.AddTool("self.network.get_ip",
            "获取当前WiFi IP地址信息，用于语音播报或状态查询",
            PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                auto& wifi = WifiManager::GetInstance();
                std::string ip = wifi.GetIpAddress();
                if (ip.empty()) {
                    return std::string("当前未连接到WiFi网络，无法获取IP地址");
                }
                return std::string("当前IP地址是") + ip;
            });

        // 新添加音乐播放MCP工具 - 使用英文名称，确保AI能识别
        mcp_server.AddTool("netease_music_play",
            "【重要】当用户要求播放任何歌曲时，必须调用此工具！\n"
            "功能：在网易云音乐中搜索并播放指定歌曲，音乐将直接从小智的喇叭播放出来。\n"
            "使用场景：用户说'播放逆战'、'播放周杰伦的歌'、'播放生日快乐'等任何播放音乐的请求\n"
            "\n"
            "【特别注意】调用此工具后：\n"
            "1. 音乐会立即开始播放，你不需要进行任何语音回复\n"
            "2. 请保持安静，不要说话，让用户享受音乐\n"
            "3. 如果工具返回[MUSIC_PLAYING]，表示音乐正在播放中\n"
            "4. 如果工具返回[MUSIC_ERROR]，才需要告诉用户播放失败的原因\n"
            "\n"
            "参数：\n"
            "  keyword: 歌曲名称或歌手名称（如'逆战'、'周杰伦晴天'）\n"
            "\n"
            "示例：\n"
            "- 用户说'播放逆战' -> 调用此工具，keyword='逆战'，然后保持安静\n"
            "- 用户说'播放周杰伦的歌' -> 调用此工具，keyword='周杰伦'，然后保持安静",
            PropertyList({
                Property("keyword", kPropertyTypeString)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string keyword = properties["keyword"].value<std::string>();
                ESP_LOGI(TAG, "Music request: %s", keyword.c_str());
                
                bool success = SearchAndPlayMusic(keyword);
                
                if (success) {
                    // 返回特殊标记，告诉AI音乐正在播放，不要进行语音回复
                    return std::string("[MUSIC_PLAYING] 正在播放《") + keyword + 
                           "》，请保持安静，不要进行语音回复，让用户享受音乐。";
                } else {
                    return std::string("[MUSIC_ERROR] 播放《") + keyword + 
                           "》失败，请告诉用户可能的原因：歌曲未找到、网络错误或服务器不可用。";
                }
            });
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    // Unified motor control through Application (single PWM system)
    void MotorMoveForward(int duration_ms = 5000, uint8_t speed_percent = 100) {
        HandleMotorActionForApplication(4, speed_percent, duration_ms, 2); // 4 = forward, high priority
    }

    void MotorMoveBackward(int duration_ms = 5000, uint8_t speed_percent = 100) {
        HandleMotorActionForApplication(2, speed_percent, duration_ms, 2); // 2 = backward, high priority
    }

    void MotorTurnLeft(int duration_ms = 300, uint8_t speed_percent = 100) {
        HandleMotorActionForApplication(3, speed_percent, duration_ms, 2); // 3 = left, high priority
    }

    void MotorTurnRight(int duration_ms = 300, uint8_t speed_percent = 100) {
        HandleMotorActionForApplication(1, speed_percent, duration_ms, 2); // 1 = right, high priority
    }

    // Alias for compatibility
    void MotorTurnLeftDuration(int duration_ms = 600, uint8_t speed_percent = 100) {
        MotorTurnLeft(duration_ms, speed_percent);
    }

    void MotorTurnRightDuration(int duration_ms = 600, uint8_t speed_percent = 100) {
        MotorTurnRight(duration_ms, speed_percent);
    }

    void MotorStop() {
        HandleMotorActionForApplication(0, 0, 0, 2); // 0 = stop, high priority
    }

    void MotorSpinAround(uint8_t speed_percent = 100) {
        HandleMotorActionForApplication(3, speed_percent, 2000, 2); // Spin = turn left longer, high priority
    }

    void MotorQuickForward(uint8_t speed_percent = 100) {
        HandleMotorActionForApplication(4, speed_percent, 500, 2); // Quick forward, high priority
    }

    void MotorQuickBackward(uint8_t speed_percent = 100) {
        HandleMotorActionForApplication(2, speed_percent, 500, 2); // Quick backward, high priority
    }

    void MotorWiggle(uint8_t speed_percent = 100) {
        // Simple wiggle - just a quick left-right movement for MCP calls
        // Since MCP calls are synchronous, we can't use delays
        HandleMotorActionForApplication(1, speed_percent, 300, 2); // right turn, high priority
    }

public:
    void MotorDance(uint8_t speed_percent = 100) {
        ESP_LOGI(TAG, "电机跳舞: 执行完整的舞蹈序列 (速度: %d%%)", speed_percent);

        // 舞蹈序列：前进 -> 左转 -> 右转 -> 后退 -> 前进 -> 左转 -> 右转 -> 结束
        // 使用高优先级确保舞蹈动作不被其他动作打断

        // 第一步：快速前进
        HandleMotorActionForApplication(4, speed_percent, 300, 2); // forward
        vTaskDelay(pdMS_TO_TICKS(350));

        // 第二步：左转
        HandleMotorActionForApplication(3, speed_percent, 250, 2); // left
        vTaskDelay(pdMS_TO_TICKS(300));

        // 第三步：右转
        HandleMotorActionForApplication(1, speed_percent, 250, 2); // right
        vTaskDelay(pdMS_TO_TICKS(300));

        // 第四步：后退
        HandleMotorActionForApplication(2, speed_percent, 300, 2); // backward
        vTaskDelay(pdMS_TO_TICKS(350));

        // 第五步：前进
        HandleMotorActionForApplication(4, speed_percent, 200, 2); // forward
        vTaskDelay(pdMS_TO_TICKS(250));

        // 第六步：左转
        HandleMotorActionForApplication(3, speed_percent, 200, 2); // left
        vTaskDelay(pdMS_TO_TICKS(250));

        // 第七步：右转
        HandleMotorActionForApplication(1, speed_percent, 200, 2); // right
        vTaskDelay(pdMS_TO_TICKS(250));

        // 第八步：最终前进结束舞蹈
        HandleMotorActionForApplication(4, speed_percent, 400, 2); // forward
    }

public:
    CompactWifiBoard() :
        boot_button_(BOOT_BUTTON_GPIO),
        touch_button_(TOUCH_BUTTON_GPIO),
        volume_up_button_(VOLUME_UP_BUTTON_GPIO),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {
        InitializeDisplayI2c();
        InitializeSsd1306Display();
        InitializeButtons();
        InitializeTools();
    }
    virtual AudioCodec* GetAudioCodec() override {
#ifdef AUDIO_I2S_METHOD_SIMPLEX
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
#else
        static NoAudioCodecDuplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
#endif
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    // Motor control interface for different emotions and actions
    void OnWakeUp() {
        ESP_LOGI(TAG, "电机情感: 唤醒被触发 - 执行兴奋动作");
        PerformMotorAction(1, 300); // FORWARD for 300ms - 兴奋的前进动作
    }

    void OnHappy() {
        ESP_LOGI(TAG, "电机情感: 开心被触发 - 执行欢快动作");
        PerformMotorAction(1, 200); // FORWARD for 200ms - 简单的开心动作
        vTaskDelay(pdMS_TO_TICKS(100));
        PerformMotorAction(3, 200); // LEFT for 200ms - 左转表示开心
    }

    void OnSad() {
        ESP_LOGI(TAG, "电机情感: 悲伤被触发 - 执行缓慢动作");
        PerformMotorAction(2, 400); // BACKWARD for 400ms - 缓慢后退表示悲伤
    }

    void OnThinking() {
        ESP_LOGI(TAG, "电机情感: 思考被触发 - 执行轻微动作");
        PerformMotorAction(3, 150); // LEFT for 150ms - 轻微左转表示思考
        vTaskDelay(pdMS_TO_TICKS(200));
        PerformMotorAction(4, 150); // RIGHT for 150ms - 右转表示思考
    }

    void OnListening() {
        ESP_LOGI(TAG, "电机情感: 倾听被触发 - 执行轻柔动作");
        PerformMotorAction(3, 100); // LEFT for 100ms - 轻柔左转
        vTaskDelay(pdMS_TO_TICKS(150));
        PerformMotorAction(4, 100); // RIGHT for 100ms - 右转表示倾听
    }

    void OnSpeaking() {
        ESP_LOGI(TAG, "电机情感: 说话被触发 - 执行前进动作");
        PerformMotorAction(1, 250); // FORWARD for 250ms - 前进表示说话
    }

    void OnExcited() {
        ESP_LOGI(TAG, "电机情感: 兴奋被触发 - 执行快速动作");
        PerformMotorAction(1, 150); // FORWARD for 150ms - 快速前进
        vTaskDelay(pdMS_TO_TICKS(50));
        PerformMotorAction(3, 150); // LEFT for 150ms - 快速左转
        vTaskDelay(pdMS_TO_TICKS(50));
        PerformMotorAction(4, 150); // RIGHT for 150ms - 快速右转
    }

    void OnLoving() {
        ESP_LOGI(TAG, "电机情感: 爱慕被触发 - 执行温柔动作");
        PerformMotorAction(1, 300); // FORWARD for 300ms - 温柔前进
        vTaskDelay(pdMS_TO_TICKS(200));
        PerformMotorAction(3, 200); // LEFT for 200ms - 轻柔左转
    }

    void OnAngry() {
        ESP_LOGI(TAG, "电机情感: 生气被触发 - 执行强烈动作");
        PerformMotorAction(2, 200); // BACKWARD for 200ms - 后退表示生气
        vTaskDelay(pdMS_TO_TICKS(100));
        PerformMotorAction(1, 200); // FORWARD for 200ms - 前冲表示生气
    }

    void OnSurprised() {
        ESP_LOGI(TAG, "电机情感: 惊讶被触发 - 执行突然动作");
        PerformMotorAction(2, 100); // BACKWARD for 100ms - 快速后退
        vTaskDelay(pdMS_TO_TICKS(150));
        PerformMotorAction(1, 200); // FORWARD for 200ms - 前进表示惊讶
    }

    void OnConfused() {
        ESP_LOGI(TAG, "电机情感: 困惑被触发 - 执行犹豫动作");
        PerformMotorAction(3, 100); // LEFT for 100ms - 犹豫左转
        vTaskDelay(pdMS_TO_TICKS(200));
        PerformMotorAction(4, 100); // RIGHT for 100ms - 犹豫右转
        vTaskDelay(pdMS_TO_TICKS(200));
        PerformMotorAction(3, 100); // LEFT for 100ms - 再次犹豫
    }

    void OnIdle() {
        if ((esp_random() % 100) < 50) { // 5% chance for random movement
            ESP_LOGI(TAG, "电机空闲: 随机动作被触发 (5%概率)");
            // Simple random movement using Application's motor control
            int random_action = (esp_random() % 4) + 1; // 1-4 for different directions
            HandleMotorActionForApplication(random_action, 60, 500, 0); // 60% speed, 500ms duration, low priority
        }
    }

    // 电机动作执行函数实现 - 使用Application的统一PWM系统
    void PerformMotorAction(int action, int duration_ms) {
        uint8_t speed_percent = 80; // 使用80%的速度进行情感表达动作

        // Emotion actions use medium priority (1) - can be interrupted by MCP commands (2) but not by lower priority
        switch (action) {
            case 1: // FORWARD
                HandleMotorActionForApplication(4, speed_percent, duration_ms, 1);
                break;
            case 2: // BACKWARD
                HandleMotorActionForApplication(2, speed_percent, duration_ms, 1);
                break;
            case 3: // LEFT
                HandleMotorActionForApplication(3, speed_percent, duration_ms, 1);
                break;
            case 4: // RIGHT
                HandleMotorActionForApplication(1, speed_percent, duration_ms, 1);
                break;
            default:
                break;
        }
    }

    // ========== 新音乐播放功能实现 ==========
    
    std::string HttpGet(const std::string& url) {
        ESP_LOGI(TAG, "HTTP GET: %s", url.c_str());
        
        esp_http_client_config_t config = {
            .url = url.c_str(),
            .method = HTTP_METHOD_GET,
            .timeout_ms = 15000,
            .buffer_size = 4096,
            .buffer_size_tx = 1024,
        };
        
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            ESP_LOGE(TAG, "Failed to init HTTP client");
            return "";
        }
        
        // 使用 open -> fetch_headers -> read 的方式
        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
            esp_http_client_cleanup(client);
            return "";
        }
        
        int content_length = esp_http_client_fetch_headers(client);
        int status_code = esp_http_client_get_status_code(client);
        
        ESP_LOGI(TAG, "HTTP Status: %d, Content-Length: %d", status_code, content_length);
        
        if (status_code != 200 || content_length <= 0) {
            ESP_LOGE(TAG, "HTTP error or empty content");
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return "";
        }
        
        // 读取响应内容
        std::string result;
        result.resize(content_length);
        
        int total_read = 0;
        int read_len = 0;
        
        while (total_read < content_length) {
            read_len = esp_http_client_read(client, 
                                            &result[total_read], 
                                            content_length - total_read);
            if (read_len <= 0) {
                ESP_LOGE(TAG, "HTTP read failed, read_len: %d", read_len);
                break;
            }
            total_read += read_len;
        }
        
        result.resize(total_read);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        
        ESP_LOGI(TAG, "HTTP Response length: %d", total_read);
        if (total_read > 0) {
            ESP_LOGI(TAG, "HTTP Response: %s", result.substr(0, 200).c_str());
        }
        return result;
    }

    std::string UrlEncode(const std::string& value) {
        std::ostringstream escaped;
        escaped.fill('0');
        escaped << std::hex;
        
        for (unsigned char c : value) {
            if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                escaped << c;
            } else {
                escaped << std::uppercase;
                escaped << '%' << std::setw(2) << int(c);
                escaped << std::nouppercase;
            }
        }
        return escaped.str();
    }

    bool SearchAndPlayMusic(const std::string& keyword) {//服务器若访问不到，则降级到原来的播放音乐方法只有固定几首歌曲
        // 1. 搜索歌曲
        // 使用std::string拼接URL，避免缓冲区溢出
        std::string search_url = "http://" + std::string(MUSIC_SERVER_IP) + ":" + 
                                std::to_string(MUSIC_SERVER_PORT) + 
                                "/search?keywords=" + UrlEncode(keyword) + 
                                "&limit=1&cookie=" + MUSIC_COOKIE;
        
        std::string search_response = HttpGet(search_url);
        if (search_response.empty()) {
            ESP_LOGE(TAG, "Search failed");
            return false;
        }
        
        // 2. 解析JSON获取歌曲ID
        cJSON* search_json = cJSON_Parse(search_response.c_str());
        if (!search_json) {
            ESP_LOGE(TAG, "Failed to parse search result");
            return false;
        }
        
        cJSON* result = cJSON_GetObjectItem(search_json, "result");
        cJSON* songs = cJSON_GetObjectItem(result, "songs");
        
        if (!cJSON_IsArray(songs) || cJSON_GetArraySize(songs) == 0) {
            ESP_LOGE(TAG, "No songs found");
            cJSON_Delete(search_json);
            return false;
        }
        
        cJSON* first_song = cJSON_GetArrayItem(songs, 0);
        cJSON* id_item = cJSON_GetObjectItem(first_song, "id");
        cJSON* name_item = cJSON_GetObjectItem(first_song, "name");
        
        if (!id_item || !name_item) {
            ESP_LOGE(TAG, "Invalid song data");
            cJSON_Delete(search_json);
            return false;
        }
        
        int song_id = id_item->valueint;
        const char* song_name = name_item->valuestring;
        
        ESP_LOGI(TAG, "Found song: %s (id: %d)", song_name, song_id);
        cJSON_Delete(search_json);
        
        // 3. 获取播放URL
        std::string url_request = "http://" + std::string(MUSIC_SERVER_IP) + ":" + 
                                 std::to_string(MUSIC_SERVER_PORT) + 
                                 "/song/url?id=" + std::to_string(song_id) + 
                                 "&br=128000&cookie=" + MUSIC_COOKIE;
        
        std::string url_response = HttpGet(url_request);
        if (url_response.empty()) {
            ESP_LOGE(TAG, "Failed to get music URL");
            return false;
        }
        
        cJSON* url_json = cJSON_Parse(url_response.c_str());
        if (!url_json) {
            ESP_LOGE(TAG, "Failed to parse URL response");
            return false;
        }
        
        cJSON* data = cJSON_GetObjectItem(url_json, "data");
        if (!cJSON_IsArray(data) || cJSON_GetArraySize(data) == 0) {
            ESP_LOGE(TAG, "No URL data");
            cJSON_Delete(url_json);
            return false;
        }
        
        cJSON* first_data = cJSON_GetArrayItem(data, 0);
        cJSON* url_item = cJSON_GetObjectItem(first_data, "url");
        
        if (!url_item || !cJSON_IsString(url_item)) {
            ESP_LOGE(TAG, "No play URL available (may need VIP)");
            cJSON_Delete(url_json);
            return false;
        }
        
        const char* music_url = url_item->valuestring;
        ESP_LOGI(TAG, "Music URL: %s", music_url);
        
        // 4. 构建音频解码服务器URL
        std::string decode_url = "http://" + std::string(AUDIO_DECODE_SERVER_IP) + ":" + 
                                std::to_string(AUDIO_DECODE_SERVER_PORT) + 
                                "/decode?url=" + UrlEncode(music_url);
        ESP_LOGI(TAG, "Audio decode URL: %s", decode_url.c_str());
        
        // 5. 启动后台任务播放PCM音频（避免阻塞MCP调用）
        StartMusicPlaybackTask(decode_url.c_str());
        
        cJSON_Delete(url_json);
        return true; // 立即返回，音乐在后台播放
    }
    
    // ========== MP3流式播放功能 ==========
    
    // PCM音频播放配置
    static constexpr int PCM_BUFFER_SIZE = 11520;     // 约240ms音频数据 (24000Hz * 2字节 * 0.24s)
    std::atomic<bool> playback_active_{false};
    
public:
    // 检查是否正在播放音乐（覆盖基类虚函数）
    bool IsPlayingMusic() const override {
        return is_playing_music_.load();
    }
    
    // 停止音乐播放（覆盖基类虚函数，供外部调用，如唤醒词检测）
    void StopMusicPlayback() override {
        if (is_playing_music_.load()) {
            ESP_LOGI(TAG, "Stopping music playback due to wake word detection");
            playback_active_.store(false);
            
            // 等待播放任务结束
            int wait_count = 0;
            while (is_playing_music_.load() && wait_count < 50) {
                vTaskDelay(pdMS_TO_TICKS(10));
                wait_count++;
            }
            
            if (music_playback_task_ != nullptr) {
                // 不要删除任务，让它自己结束
                music_playback_task_ = nullptr;
            }
            
            // 恢复音量和麦克风
            auto codec = GetAudioCodec();
            if (codec) {
                codec->SetOutputVolume(100);  // 恢复默认音量
                codec->EnableInput(true);      // 启用麦克风
                ESP_LOGI(TAG, "Audio settings restored after stopping music");
            }
            
            ESP_LOGI(TAG, "Music playback stopped");
        }
    }
    
private:
    // 启动音乐播放后台任务
    void StartMusicPlaybackTask(const std::string& music_url) {
        // 如果已有播放任务在运行，先停止它
        if (music_playback_task_ != nullptr) {
            playback_active_.store(false);
            vTaskDelay(pdMS_TO_TICKS(100));
            music_playback_task_ = nullptr;
        }
        
        // 复制URL到堆内存，供任务使用
        std::string* url_copy = new std::string(music_url);
        
        xTaskCreatePinnedToCore(
            MusicPlaybackTask,
            "music_playback",
            8192,   // 栈大小8KB（PCM模式无需解码，占用更少内存）
            url_copy,
            5,      // 优先级5
            &music_playback_task_,
            1       // 在核心1上运行
        );
    }
    
    // PCM音频播放任务 - 直接接收PCM数据并播放（无需解码）
    static void PcmPlayTask(void* param) {
        std::string* url = (std::string*)param;
        ESP_LOGI(TAG, "PCM Play task started, URL: %s", url->c_str());
        
        auto board = static_cast<CompactWifiBoard*>(&Board::GetInstance());
        AudioCodec* codec = board->GetAudioCodec();
        
        // 初始化HTTP客户端
        esp_http_client_config_t config = {
            .url = url->c_str(),
            .method = HTTP_METHOD_GET,
            .timeout_ms = 30000,
            .buffer_size = 8192,
            .buffer_size_tx = 1024,
        };
        
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            ESP_LOGE(TAG, "Failed to init HTTP client");
            delete url;
            vTaskDelete(nullptr);
            return;
        }
        
        if (esp_http_client_open(client, 0) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to open HTTP connection");
            esp_http_client_cleanup(client);
            delete url;
            vTaskDelete(nullptr);
            return;
        }
        
        int content_length = esp_http_client_fetch_headers(client);
        int status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "PCM stream - Status: %d, Content-Length: %d", status_code, content_length);
        
        if (status_code != 200) {
            ESP_LOGE(TAG, "HTTP error: %d", status_code);
            // 读取错误信息
            char error_buf[256];
            int error_len = esp_http_client_read(client, error_buf, sizeof(error_buf) - 1);
            if (error_len > 0) {
                error_buf[error_len] = '\0';
                ESP_LOGE(TAG, "Error response: %s", error_buf);
            }
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            delete url;
            vTaskDelete(nullptr);
            return;
        }
        
        // 分配PCM缓冲区（每个缓冲区约240ms音频: 24000Hz * 2字节 * 0.24s = 11520字节）
        const int PCM_BUFFER_SIZE = 11520;
        int16_t* pcm_buffer = (int16_t*)heap_caps_malloc(PCM_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!pcm_buffer) {
            ESP_LOGE(TAG, "Failed to allocate PCM buffer");
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            delete url;
            vTaskDelete(nullptr);
            return;
        }
        
        ESP_LOGI(TAG, "Starting PCM playback...");
        int total_bytes = 0;
        int buffer_count = 0;
        
        while (board->playback_active_.load()) {
            // 读取PCM数据
            int bytes_read = esp_http_client_read(client, (char*)pcm_buffer, PCM_BUFFER_SIZE);
            
            if (bytes_read < 0) {
                ESP_LOGE(TAG, "HTTP read error");
                break;
            } else if (bytes_read == 0) {
                ESP_LOGI(TAG, "PCM stream ended");
                break;
            }
            
            // 调试：打印前几个字节
            if (buffer_count == 0) {
                ESP_LOGI(TAG, "First bytes: %02X %02X %02X %02X %02X %02X %02X %02X",
                        ((uint8_t*)pcm_buffer)[0], ((uint8_t*)pcm_buffer)[1],
                        ((uint8_t*)pcm_buffer)[2], ((uint8_t*)pcm_buffer)[3],
                        ((uint8_t*)pcm_buffer)[4], ((uint8_t*)pcm_buffer)[5],
                        ((uint8_t*)pcm_buffer)[6], ((uint8_t*)pcm_buffer)[7]);
            }
            
            // 直接输出到I2S（服务器已经处理好格式：16位，单声道，24000Hz）
            int samples = bytes_read / sizeof(int16_t);
            std::vector<int16_t> audio_data(pcm_buffer, pcm_buffer + samples);
            codec->OutputData(audio_data);
            
            total_bytes += bytes_read;
            buffer_count++;
            
            // 每100个缓冲区打印一次进度
            if (buffer_count % 100 == 0) {
                ESP_LOGI(TAG, "Playing... buffers: %d, bytes: %d", buffer_count, total_bytes);
            }
            
            // 让出CPU时间
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        
        ESP_LOGI(TAG, "PCM playback finished, total buffers: %d, total bytes: %d",
                 buffer_count, total_bytes);

        // 通知主任务播放已完成
        board->playback_active_.store(false);
        ESP_LOGI(TAG, "Playback active set to false");

        heap_caps_free(pcm_buffer);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        delete url;
        vTaskDelete(nullptr);
    }
    
    // 音乐播放主任务 - 使用PCM音频解码服务器
    static void MusicPlaybackTask(void* param) {
        std::string* url = (std::string*)param;
        ESP_LOGI(TAG, "Music playback task started (PCM mode)");
        
        auto board = static_cast<CompactWifiBoard*>(&Board::GetInstance());
        board->is_playing_music_.store(true);
        
        // 获取Application实例
        Application& app = Application::GetInstance();
        
        // 停止当前的TTS播放
        app.AbortSpeaking(kAbortReasonNone);
        ESP_LOGI(TAG, "Aborted current TTS for music playback");
        vTaskDelay(pdMS_TO_TICKS(100));
        
        // 保存当前状态并切换到Idle模式
        DeviceState previous_state = app.GetDeviceState();
        ESP_LOGI(TAG, "Previous state: %d, switching to Idle", previous_state);
        app.SetDeviceState(kDeviceStateIdle);
        
        // 获取音频编解码器
        AudioCodec* codec = board->GetAudioCodec();
        int original_volume = 70;
        
        if (codec) {
            original_volume = codec->output_volume();
            codec->SetOutputVolume(original_volume);
            ESP_LOGI(TAG, "Volume set to %d%% for music playback", original_volume);
            codec->EnableInput(false);
            ESP_LOGI(TAG, "Microphone disabled for music playback");
        }
        
        // 设置播放状态
        board->playback_active_.store(true);
        
        // 创建PCM播放任务（简化版，无需解码）
        std::string* url_copy = new std::string(*url);
        BaseType_t ret = xTaskCreatePinnedToCore(
            PcmPlayTask,
            "pcm_play",
            8192,   // 8KB栈空间足够（无需解码）
            url_copy,
            5,      // 优先级5
            &board->music_playback_task_,
            1       // 在核心1上运行
        );
        
        if (ret != pdPASS) {
            ESP_LOGE(TAG, "Failed to create PCM play task, error: %d", ret);
            delete url_copy;
            board->playback_active_.store(false);
        } else {
            ESP_LOGI(TAG, "PCM play task created successfully");
            
            // 等待播放完成
            while (board->playback_active_.load()) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            
            ESP_LOGI(TAG, "PCM playback finished");
        }
        
        // 恢复设置
        if (codec) {
            codec->SetOutputVolume(original_volume);
            ESP_LOGI(TAG, "Volume restored to %d%%", original_volume);
            codec->EnableInput(true);
            ESP_LOGI(TAG, "Microphone re-enabled after music playback");
        }
        
        if (previous_state == kDeviceStateListening || previous_state == kDeviceStateSpeaking) {
            app.SetDeviceState(kDeviceStateListening);
            ESP_LOGI(TAG, "State restored to Listening");
        }
        
        board->is_playing_music_.store(false);
        delete url;
        board->music_playback_task_ = nullptr;
        vTaskDelete(nullptr);
    }
    
    // 注意：使用PCM音频解码服务器，小智端无需解码
};

// Public wrapper functions for motor control
extern "C" void HandleMotorActionForEmotion(const char* emotion) {
    auto board = static_cast<CompactWifiBoard*>(&Board::GetInstance());
    if (emotion) {
        std::string emotion_str(emotion);
        if (emotion_str == "happy" || emotion_str == "joy") {
            board->OnHappy();
        } else if (emotion_str == "excited") {
            board->OnExcited();
        } else if (emotion_str == "sad" || emotion_str == "unhappy") {
            board->OnSad();
        } else if (emotion_str == "thinking") {
            board->OnThinking();
        } else if (emotion_str == "confused") {
            board->OnConfused();
        } else if (emotion_str == "listening" || emotion_str == "curious") {
            board->OnListening();
        } else if (emotion_str == "speaking" || emotion_str == "talking") {
            board->OnSpeaking();
        } else if (emotion_str == "wake" || emotion_str == "wakeup") {
            board->OnWakeUp();
        } else if (emotion_str == "loving") {
            board->OnLoving();
        } else if (emotion_str == "angry") {
            board->OnAngry();
        } else if (emotion_str == "surprised") {
            board->OnSurprised();
        } else {
            ESP_LOGW(TAG, "Unknown emotion: %s", emotion_str.c_str());
        }
    }
}

// Global function for idle motor movements
extern "C" void HandleMotorIdleAction(void) {
    auto board = static_cast<CompactWifiBoard*>(&Board::GetInstance());
    board->OnIdle();
}

// Global function for dance motor action
extern "C" void HandleMotorActionForDance(uint8_t speed_percent) {
    auto board = static_cast<CompactWifiBoard*>(&Board::GetInstance());
    board->MotorDance(speed_percent);
}

DECLARE_BOARD(CompactWifiBoard);
