/**
 * @file bsp_led_hal.c
 * @brief LED硬件抽象层实现文件 - STM32平台
 * @author Your Name
 * @date 2024
 * 
 * 实现了STM32平台的LED硬件抽象接口。
 */

#include <led_hal.h>
#include "main.h"
#include <stdio.h>

/* ==================== LED引脚配置表 ==================== */

/**
 * @brief LED引脚配置表
 * 
 * 用户需要根据实际硬件修改此表。
 * 每个LED都有明确的GPIO端口和引脚定义。
 */
const led_pin_config_t led_pin_configs[LED_PIN_TABLE_SIZE] = {
    // 索引0: 状态指示灯
    {
        .port = LED_STATUS_PORT,
        .pin = LED_STATUS_PIN,
        .description = "状态指示灯"
    },
    // 索引1: 用户LED
//    {
//        .port = LED2_GPIO_Port,
//        .pin = LED2_Pin,
//        .description = "用户LED"
//    }
};


/* ==================== 内部引脚映射表 ==================== */

/**
 * @brief 内部引脚映射结构体
 */
typedef struct {
    GPIO_TypeDef* port;           // GPIO端口
    uint16_t pin;                 // GPIO引脚
    bool is_initialized;          // 是否已初始化
} led_pin_map_t;

/**
 * @brief 内部引脚映射表
 * 
 * 基于引脚配置表生成的内部映射表。
 * 每个LED都有明确的GPIO端口和引脚定义。
 */
static led_pin_map_t led_pin_table[LED_MAX_COUNT] = {
    // 使用引脚配置表中的端口和引脚
    {LED_STATUS_PORT, LED_STATUS_PIN, false}      // LED0: 状态指示灯
};

/* ==================== PWM配置表 ==================== */

/**
 * @brief PWM配置结构体
 */
typedef struct {
    TIM_HandleTypeDef* htim;      // 定时器句柄
    uint32_t channel;             // 定时器通道
    bool is_initialized;          // 是否已初始化
} led_pwm_map_t;

/**
 * @brief PWM配置表
 * 
 * 用户需要根据实际硬件配置修改此表。
 * 将逻辑通道号映射到实际的定时器和通道。
 */
static led_pwm_map_t led_pwm_table[LED_MAX_COUNT] = {
    // 示例：逻辑通道0映射到TIM3, CH1
    {NULL, TIM_CHANNEL_1, false},
//    {NULL, TIM_CHANNEL_2, false},
//    {NULL, TIM_CHANNEL_3, false},
//    {NULL, TIM_CHANNEL_4, false},
//    {NULL, TIM_CHANNEL_1, false},
//    {NULL, TIM_CHANNEL_2, false},
//    {NULL, TIM_CHANNEL_3, false},
//    {NULL, TIM_CHANNEL_4, false},
//    {NULL, TIM_CHANNEL_1, false},
//    {NULL, TIM_CHANNEL_2, false},
//    {NULL, TIM_CHANNEL_3, false},
//    {NULL, TIM_CHANNEL_4, false},
//    {NULL, TIM_CHANNEL_1, false},
//    {NULL, TIM_CHANNEL_2, false},
//    {NULL, TIM_CHANNEL_3, false},
//    {NULL, TIM_CHANNEL_4, false}
};

/* ==================== 内部函数声明 ==================== */

static void enable_gpio_clock(GPIO_TypeDef* port);
static void enable_tim_clock(TIM_TypeDef* tim);
static bool is_valid_pin(uint8_t pin);
static bool is_valid_channel(uint8_t channel);

/* ==================== GPIO 适配实现 ==================== */

/**
 * @brief 初始化GPIO引脚为输出
 * @param pin 逻辑引脚号（需映射到实际GPIO）
 * @param active_high 高电平有效
 * @note 用户需根据实际硬件将pin映射到GPIO端口和引脚
 */
void stm32_gpio_init(uint8_t pin, bool active_high) {
    if (!is_valid_pin(pin)) {
        return;
    }
    
    led_pin_map_t* pin_map = &led_pin_table[pin];
    
    // 如果已经初始化，先反初始化
    if (pin_map->is_initialized) {
        HAL_GPIO_DeInit(pin_map->port, pin_map->pin);
    }
    
    // 启用GPIO时钟
    enable_gpio_clock(pin_map->port);
    
    // 配置GPIO
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin = pin_map->pin;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    
    HAL_GPIO_Init(pin_map->port, &GPIO_InitStruct);
    
    // 设置初始电平
    HAL_GPIO_WritePin(pin_map->port, pin_map->pin, 
                      active_high ? GPIO_PIN_RESET : GPIO_PIN_SET);
    
    // 标记为已初始化
    pin_map->is_initialized = true;
}

/**
 * @brief 设置GPIO引脚电平
 * @param pin 逻辑引脚号
 * @param state GPIO_PIN_SET/GPIO_PIN_RESET
 * @note 用户需根据实际硬件将pin映射到GPIO端口和引脚
 */
void stm32_gpio_set_level(uint8_t pin, GPIO_PinState state) {
    if (!is_valid_pin(pin)) {
        return;
    }
    
    led_pin_map_t* pin_map = &led_pin_table[pin];
    
    if (!pin_map->is_initialized) {
        return;
    }
    
    HAL_GPIO_WritePin(pin_map->port, pin_map->pin, state);
}

/* ==================== PWM 适配实现 ==================== */

/**
 * @brief 初始化PWM通道
 * @param channel PWM通道号
 * @param freq 频率（Hz）
 * @param pin 逻辑引脚号
 * @note 用户需将channel映射到实际定时器和通道，并初始化
 */
void stm32_pwm_init(uint8_t channel, uint32_t freq, uint8_t pin) {
    if (!is_valid_channel(channel)) {
        return;
    }
    
    led_pwm_map_t* pwm_map = &led_pwm_table[channel];
    
    // 如果已经初始化，先反初始化
    if (pwm_map->is_initialized) {
        HAL_TIM_PWM_Stop(pwm_map->htim, pwm_map->channel);
        HAL_TIM_PWM_DeInit(pwm_map->htim);
    }
    
    // 启用定时器时钟
    enable_tim_clock(pwm_map->htim->Instance);
    
    // 配置定时器
    TIM_OC_InitTypeDef sConfigOC = {0};
    sConfigOC.OCMode = TIM_OCMODE_PWM1;
    sConfigOC.Pulse = 0;
    sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
    sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
    
    // 初始化定时器
    if (HAL_TIM_PWM_Init(pwm_map->htim) != HAL_OK) {
        return;
    }
    
    // 配置PWM通道
    if (HAL_TIM_PWM_ConfigChannel(pwm_map->htim, &sConfigOC, pwm_map->channel) != HAL_OK) {
        return;
    }
    
    // 启动PWM输出
    HAL_TIM_PWM_Start(pwm_map->htim, pwm_map->channel);
    
    // 标记为已初始化
    pwm_map->is_initialized = true;
}

/**
 * @brief 设置PWM占空比
 * @param channel PWM通道号
 * @param duty 占空比（0-255）
 * @note 用户需将channel映射到实际定时器和通道
 */
void stm32_pwm_set_duty(uint8_t channel, uint8_t duty) {
    if (!is_valid_channel(channel)) {
        return;
    }
    
    led_pwm_map_t* pwm_map = &led_pwm_table[channel];
    
    if (!pwm_map->is_initialized) {
        return;
    }
    
    // 将8位占空比转换为定时器比较值
    uint32_t compare_value = (duty * pwm_map->htim->Init.Period) / 255;
    __HAL_TIM_SET_COMPARE(pwm_map->htim, pwm_map->channel, compare_value);
}

/* ==================== WS2812 适配实现 ==================== */

/**
 * @brief 初始化WS2812灯带
 * @param pin 数据引脚
 * @param led_num 灯珠数量
 * @note 建议使用CubeWS2812等第三方库
 */
void stm32_ws2812_init(uint8_t pin, uint16_t led_num) {
    // 示例：调用CubeWS2812库初始化
    // ws2812_init(pin, led_num);
    
    // 用户需要根据实际使用的WS2812库实现此函数
    // 常见的库包括：
    // - CubeWS2812
    // - FastLED
    // - NeoPixel
}

/**
 * @brief 设置WS2812单颗灯颜色
 * @param led_index 灯珠索引
 * @param r,g,b 颜色值
 */
void stm32_ws2812_set_color(uint16_t led_index, uint8_t r, uint8_t g, uint8_t b) {
    // 示例：调用CubeWS2812库设置颜色
    // ws2812_set_pixel(led_index, r, g, b);
    
    // 用户需要根据实际使用的WS2812库实现此函数
}

/**
 * @brief 刷新WS2812灯带
 * @note 设置完所有颜色后需调用
 */
void stm32_ws2812_refresh(void) {
    // 示例：调用CubeWS2812库刷新
    // ws2812_show();
    
    // 用户需要根据实际使用的WS2812库实现此函数
}

/* ==================== 内部函数实现 ==================== */

/**
 * @brief 启用GPIO时钟
 * 
 * 根据GPIO端口启用对应的时钟。
 * 
 * @param port GPIO端口
 */
static void enable_gpio_clock(GPIO_TypeDef* port) {
    if (port == GPIOA) {
        __HAL_RCC_GPIOA_CLK_ENABLE();
    } else if (port == GPIOB) {
        __HAL_RCC_GPIOB_CLK_ENABLE();
    } else if (port == GPIOC) {
        __HAL_RCC_GPIOC_CLK_ENABLE();
    } else if (port == GPIOD) {
        __HAL_RCC_GPIOD_CLK_ENABLE();
    } else if (port == GPIOE) {
        __HAL_RCC_GPIOE_CLK_ENABLE();
//    } else if (port == GPIOF) {
//        __HAL_RCC_GPIOF_CLK_ENABLE();
//    } else if (port == GPIOG) {
//        __HAL_RCC_GPIOG_CLK_ENABLE();
    }
    // 根据实际芯片添加更多端口
}

/**
 * @brief 启用定时器时钟
 * 
 * 根据定时器启用对应的时钟。
 * 
 * @param tim 定时器
 */
static void enable_tim_clock(TIM_TypeDef* tim) {
    if (tim == TIM1) {
        __HAL_RCC_TIM1_CLK_ENABLE();
    } else if (tim == TIM2) {
        __HAL_RCC_TIM2_CLK_ENABLE();
    } else if (tim == TIM3) {
        __HAL_RCC_TIM3_CLK_ENABLE();
    } else if (tim == TIM4) {
        __HAL_RCC_TIM4_CLK_ENABLE();
//    } else if (tim == TIM5) {
//        __HAL_RCC_TIM5_CLK_ENABLE();
//    } else if (tim == TIM6) {
//        __HAL_RCC_TIM6_CLK_ENABLE();
//    } else if (tim == TIM7) {
//        __HAL_RCC_TIM7_CLK_ENABLE();
//    } else if (tim == TIM8) {
//        __HAL_RCC_TIM8_CLK_ENABLE();
    }
    // 根据实际芯片添加更多定时器
}

/**
 * @brief 检查引脚号是否有效
 * 
 * @param pin 引脚号
 * @return true 有效，false 无效
 */
static bool is_valid_pin(uint8_t pin) {
    return (pin < LED_MAX_COUNT);
}

/**
 * @brief 检查通道号是否有效
 * 
 * @param channel 通道号
 * @return true 有效，false 无效
 */
static bool is_valid_channel(uint8_t channel) {
    return (channel < LED_MAX_COUNT);
}

/* ==================== LED引脚配置函数实现 ==================== */

/**
 * @brief 获取LED引脚配置
 * @param led_index LED索引 (0-15)
 * @return 引脚配置指针，如果索引无效返回NULL
 */
const led_pin_config_t* get_led_pin_config(uint8_t led_index) {
    if (led_index >= LED_PIN_TABLE_SIZE) {
        return NULL;
    }
    return &led_pin_configs[led_index];
}

/**
 * @brief 获取LED端口
 * @param led_index LED索引
 * @return GPIO端口，如果索引无效返回NULL
 */
GPIO_TypeDef* get_led_port(uint8_t led_index) {
    const led_pin_config_t* config = get_led_pin_config(led_index);
    return config ? config->port : NULL;
}

/**
 * @brief 获取LED引脚
 * @param led_index LED索引
 * @return GPIO引脚，如果索引无效返回0
 */
uint16_t get_led_pin(uint8_t led_index) {
    const led_pin_config_t* config = get_led_pin_config(led_index);
    return config ? config->pin : 0;
}

/**
 * @brief 获取LED描述
 * @param led_index LED索引
 * @return LED描述字符串，如果索引无效返回NULL
 */
const char* get_led_description(uint8_t led_index) {
    const led_pin_config_t* config = get_led_pin_config(led_index);
    return config ? config->description : NULL;
}

/**
 * @brief 打印所有LED引脚配置
 */
//void print_led_pin_configs(void) {
//    printf("=== LED引脚配置表 ===\n");
//    for (int i = 0; i < LED_PIN_TABLE_SIZE; i++) {
//        const led_pin_config_t* config = &led_pin_configs[i];
//        if (config->port != NULL) {
//            printf("LED[%2d]: %s -> %s_PIN_%d\n",
//                   i,
//                   config->description,
//                   (config->port == GPIOA) ? "GPIOA" :
//                   (config->port == GPIOB) ? "GPIOB" :
//                   (config->port == GPIOC) ? "GPIOC" :
//                   (config->port == GPIOD) ? "GPIOD" :
//                   (config->port == GPIOE) ? "GPIOE" : "GPIO?",
//                   (config->pin == GPIO_PIN_0) ? 0 :
//                   (config->pin == GPIO_PIN_1) ? 1 :
//                   (config->pin == GPIO_PIN_2) ? 2 :
//                   (config->pin == GPIO_PIN_3) ? 3 :
//                   (config->pin == GPIO_PIN_4) ? 4 :
//                   (config->pin == GPIO_PIN_5) ? 5 :
//                   (config->pin == GPIO_PIN_6) ? 6 :
//                   (config->pin == GPIO_PIN_7) ? 7 :
//                   (config->pin == GPIO_PIN_8) ? 8 :
//                   (config->pin == GPIO_PIN_9) ? 9 :
//                   (config->pin == GPIO_PIN_10) ? 10 :
//                   (config->pin == GPIO_PIN_11) ? 11 :
//                   (config->pin == GPIO_PIN_12) ? 12 :
//                   (config->pin == GPIO_PIN_13) ? 13 :
//                   (config->pin == GPIO_PIN_14) ? 14 :
//                   (config->pin == GPIO_PIN_15) ? 15 : -1);
//        }
//    }
//    printf("==================\n");
////}
