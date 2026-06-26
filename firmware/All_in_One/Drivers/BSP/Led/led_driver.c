/**
 * @file led_driver.c
 * @brief LED驱动实现文件 - 表驱动设计模式
 * @author Your Name
 * @date 2024
 * 
 * 实现了基于表驱动设计模式的通用LED驱动功能。
 * 支持多种LED类型的统一管理和控制。
 */

#include <led_hal.h>
#include "led_driver.h"
#include <string.h>

/* ==================== 前向声明 ==================== */

// GPIO LED操作函数
static void led_gpio_init(const led_config_t* config);
static void led_gpio_set_state(const led_config_t* config, led_state_t state);
static void led_gpio_set_brightness(const led_config_t* config, uint8_t brightness);
static void led_gpio_update(const led_config_t* config, led_status_t* status);

// PWM LED操作函数
static void led_pwm_init(const led_config_t* config);
static void led_pwm_set_state(const led_config_t* config, led_state_t state);
static void led_pwm_set_brightness(const led_config_t* config, uint8_t brightness);
static void led_pwm_update(const led_config_t* config, led_status_t* status);

// WS2812 LED操作函数
static void led_ws2812_init(const led_config_t* config);
static void led_ws2812_set_state(const led_config_t* config, led_state_t state);
static void led_ws2812_set_brightness(const led_config_t* config, uint8_t brightness);
static void led_ws2812_update(const led_config_t* config, led_status_t* status);

/* ==================== 表驱动设计模式核心 ==================== */

/**
 * @brief LED操作表 - 表驱动设计模式的核心
 * 
 * 这个表为每种LED类型定义了对应的操作函数。
 * 通过这个表，系统可以根据LED类型自动调用正确的操作函数，
 * 实现了多态性和代码的模块化。
 * 
 * 表结构说明：
 * - type: LED类型标识
 * - init_func: 初始化函数指针
 * - set_state_func: 状态设置函数指针
 * - set_brightness_func: 亮度设置函数指针
 * - update_func: 状态更新函数指针
 */
static const led_ops_table_t led_ops_table[] = {
    {LED_TYPE_GPIO,  led_gpio_init,  led_gpio_set_state,  led_gpio_set_brightness,  led_gpio_update},
    {LED_TYPE_PWM,   led_pwm_init,   led_pwm_set_state,   led_pwm_set_brightness,   led_pwm_update},
    {LED_TYPE_I2C,   NULL,           NULL,                NULL,                     NULL}, // 待实现
    {LED_TYPE_SPI,   NULL,           NULL,                NULL,                     NULL}, // 待实现
    {LED_TYPE_WS2812, led_ws2812_init, led_ws2812_set_state, led_ws2812_set_brightness, led_ws2812_update},
};

/**
 * @brief 获取LED操作函数
 * 
 * 根据LED类型查找对应的操作函数表。
 * 这是表驱动设计模式的关键函数，实现了类型到操作的映射。
 * 
 * @param type LED类型
 * @return 对应的操作函数表指针，如果未找到返回NULL
 * 
 * @note 
 * - 使用线性查找，时间复杂度O(n)
 * - 对于大量LED类型，可以考虑使用哈希表优化
 * - 函数会检查类型是否在有效范围内
 */
static const led_ops_table_t* get_led_ops(led_type_t type) {
    // 边界检查
    if (type >= LED_TYPE_MAX) {
        return NULL;
    }
    
    // 线性查找对应的操作函数
    for (int i = 0; i < sizeof(led_ops_table) / sizeof(led_ops_table[0]); i++) {
        if (led_ops_table[i].type == type) {
            return &led_ops_table[i];
        }
    }
    
    return NULL;
}

/* ==================== 核心API函数实现 ==================== */

/**
 * @brief 初始化LED驱动
 * 
 * 初始化LED驱动管理器，设置所有LED的初始状态。
 * 这是使用LED驱动的第一步，必须在其他操作之前调用。
 * 
 * @param driver LED驱动管理器指针
 * @param configs LED配置数组指针
 * @param status LED状态数组指针
 * @param count LED数量
 * 
 * @note 
 * - 函数会进行参数有效性检查
 * - 每个LED都会调用对应类型的初始化函数
 * - 所有LED初始状态设置为关闭
 */
void led_driver_init(led_driver_t* driver, led_config_t* configs, led_status_t* status, uint8_t count) {
    // 参数有效性检查
    if (!driver || !configs || !status || count == 0 || count > LED_MAX_COUNT) {
        return;
    }
    
    // 初始化驱动管理器
    driver->configs = configs;
    driver->status = status;
    driver->led_count = count;
    driver->system_time = 0;
    
    // 初始化每个LED
    for (uint8_t i = 0; i < count; i++) {
        // 初始化状态结构体
        memset(&status[i], 0, sizeof(led_status_t));
        status[i].state = LED_STATE_OFF;
        status[i].brightness = configs[i].brightness;
        
        // 根据LED类型调用对应的初始化函数
        const led_ops_table_t* ops = get_led_ops(configs[i].type);
        if (ops && ops->init_func) {
            ops->init_func(&configs[i]);
        }
    }
}

/**
 * @brief 更新LED驱动状态
 * 
 * 这个函数需要在主循环中定期调用，用于更新所有LED的状态。
 * 函数会处理闪烁、渐变等需要时间控制的效果。
 * 
 * @param driver LED驱动管理器指针
 * 
 * @note 
 * - 函数会更新系统时间
 * - 每个LED都会调用对应类型的更新函数
 * - 建议在主循环中每10-50ms调用一次
 */
void led_driver_update(led_driver_t* driver) {
    // 参数检查
    if (!driver) {
        return;
    }
    
    // 更新系统时间
    led_system_time_update(&driver->system_time);
    
    // 更新每个LED状态
    for (uint8_t i = 0; i < driver->led_count; i++) {
        const led_ops_table_t* ops = get_led_ops(driver->configs[i].type);
        if (ops && ops->update_func) {
            // 将计数器与系统时间对齐
            driver->status[i].counter = driver->system_time;
            ops->update_func(&driver->configs[i], &driver->status[i]);
        }
    }
}

/**
 * @brief 设置LED状态
 * 
 * 设置指定LED的工作状态，如常亮、闪烁、渐变等。
 * 状态改变会立即生效。
 * 
 * @param driver LED驱动管理器指针
 * @param led_id LED标识符
 * @param state 目标状态
 * 
 * @note 
 * - 函数会检查LED ID的有效性
 * - 状态改变会重置内部计数器
 * - 会立即调用对应类型的状态设置函数
 */
void led_set_state(led_driver_t* driver, uint8_t led_id, led_state_t state) {
    // 参数检查
    if (!driver || led_id >= driver->led_count || state >= LED_STATE_MAX) {
        return;
    }
    
    // 更新LED状态
    driver->status[led_id].state = state;
    driver->status[led_id].last_update = driver->system_time;
    driver->status[led_id].counter = 0;
    driver->status[led_id].state_flag = false;
    
    // 调用对应类型的状态设置函数
    const led_ops_table_t* ops = get_led_ops(driver->configs[led_id].type);
    if (ops && ops->set_state_func) {
        ops->set_state_func(&driver->configs[led_id], state);
    }
}

/**
 * @brief 设置LED亮度
 * 
 * 设置指定LED的亮度等级，仅对支持亮度调节的LED类型有效。
 * 
 * @param driver LED驱动管理器指针
 * @param led_id LED标识符
 * @param brightness 亮度值（0-255）
 * 
 * @note 
 * - 函数会检查LED ID的有效性
 * - 亮度值会被限制在0-255范围内
 * - 会立即调用对应类型的亮度设置函数
 */
void led_set_brightness(led_driver_t* driver, uint8_t led_id, uint8_t brightness) {
    // 参数检查
    if (!driver || led_id >= driver->led_count) {
        return;
    }
    
    // 限制亮度范围
    if (brightness > LED_MAX_BRIGHTNESS) {
        brightness = LED_MAX_BRIGHTNESS;
    }
    
    // 更新配置和状态
    driver->configs[led_id].brightness = brightness;
    driver->status[led_id].brightness = brightness;
    
    // 调用对应类型的亮度设置函数
    const led_ops_table_t* ops = get_led_ops(driver->configs[led_id].type);
    if (ops && ops->set_brightness_func) {
        ops->set_brightness_func(&driver->configs[led_id], brightness);
    }
}

/**
 * @brief 设置LED闪烁周期
 * 
 * 设置指定LED的闪烁周期，仅对闪烁状态有效。
 * 
 * @param driver LED驱动管理器指针
 * @param led_id LED标识符
 * @param period 闪烁周期（毫秒）
 * 
 * @note 
 * - 函数会检查LED ID的有效性
 * - 周期值会被限制在最小值以上
 * - 设置后需要重新设置LED状态为BLINK才能生效
 */
void led_set_blink_period(led_driver_t* driver, uint8_t led_id, uint16_t period) {
    // 参数检查
    if (!driver || led_id >= driver->led_count) {
        return;
    }
    
    // 限制最小周期
    if (period < LED_MIN_BLINK_PERIOD) {
        period = LED_MIN_BLINK_PERIOD;
    }
    
    driver->configs[led_id].blink_period = period;
}

/**
 * @brief 设置LED渐变周期
 * 
 * 设置指定LED的渐变周期，仅对渐变状态有效。
 * 
 * @param driver LED驱动管理器指针
 * @param led_id LED标识符
 * @param period 渐变周期（毫秒）
 * 
 * @note 
 * - 函数会检查LED ID的有效性
 * - 周期值会被限制在最小值以上
 * - 设置后需要重新设置LED状态为FADE才能生效
 */
void led_set_fade_period(led_driver_t* driver, uint8_t led_id, uint16_t period) {
    // 参数检查
    if (!driver || led_id >= driver->led_count) {
        return;
    }
    
    // 限制最小周期
    if (period < LED_MIN_FADE_PERIOD) {
        period = LED_MIN_FADE_PERIOD;
    }
    
    driver->configs[led_id].fade_period = period;
}

/* ==================== GPIO LED 实现 ==================== */

/**
 * @brief GPIO LED初始化
 *
 * 针对STM32平台，建议在bsp_led_hal.c中实现如下函数：
 *   void stm32_gpio_init(uint8_t pin, bool active_high);
 *
 * @param config LED配置参数
 */
static void led_gpio_init(const led_config_t* config) {
#ifdef STM32_PLATFORM
    // 用户需在bsp_led_hal.c中实现此函数，调用HAL_GPIO_Init
    stm32_gpio_init(config->pin, config->active_high);
#else
    // 其他平台实现
#endif
}

/**
 * @brief GPIO LED状态设置
 *
 * 针对STM32平台，建议在bsp_led_hal.c中实现如下函数：
 *   void stm32_gpio_set_level(uint8_t pin, GPIO_PinState state);
 *
 * @param config LED配置参数
 * @param state 目标状态
 */
static void led_gpio_set_state(const led_config_t* config, led_state_t state) {
#ifdef STM32_PLATFORM
    if (state == LED_STATE_OFF) {
        stm32_gpio_set_level(config->pin, config->active_high ? GPIO_PIN_RESET : GPIO_PIN_SET);
    } else if (state == LED_STATE_ON) {
        stm32_gpio_set_level(config->pin, config->active_high ? GPIO_PIN_SET : GPIO_PIN_RESET);
    }
#else
    // 其他平台实现
#endif
}

/**
 * @brief GPIO LED亮度设置
 *
 * GPIO LED不支持亮度调节，只有开关状态。
 *
 * @param config LED配置参数
 * @param brightness 亮度值（0-255）
 */
static void led_gpio_set_brightness(const led_config_t* config, uint8_t brightness) {
#ifdef STM32_PLATFORM
    if (brightness > 0) {
        stm32_gpio_set_level(config->pin, config->active_high ? GPIO_PIN_SET : GPIO_PIN_RESET);
    } else {
        stm32_gpio_set_level(config->pin, config->active_high ? GPIO_PIN_RESET : GPIO_PIN_SET);
    }
#else
    // 其他平台实现
#endif
}

/**
 * @brief GPIO LED状态更新
 *
 * 处理GPIO LED的闪烁效果。
 *
 * @param config LED配置参数
 * @param status LED状态
 */
static void led_gpio_update(const led_config_t* config, led_status_t* status) {
    if (status->state == LED_STATE_BLINK) {
        uint32_t elapsed = led_get_time_diff(status->counter, status->last_update);
        if (elapsed >= config->blink_period) {
            status->state_flag = !status->state_flag;
            status->last_update = status->counter;
#ifdef STM32_PLATFORM
            stm32_gpio_set_level(config->pin, config->active_high ? (status->state_flag ? GPIO_PIN_SET : GPIO_PIN_RESET)
                                                                 : (status->state_flag ? GPIO_PIN_RESET : GPIO_PIN_SET));
#else
            // 其他平台实现
#endif
        }
    }
}

/* ==================== PWM LED 实现 ==================== */

/**
 * @brief PWM LED初始化
 *
 * 针对STM32平台，建议在bsp_led_hal.c中实现如下函数：
 *   void stm32_pwm_init(uint8_t channel, uint32_t freq, uint8_t pin);
 *
 * @param config LED配置参数
 */
static void led_pwm_init(const led_config_t* config) {
#ifdef STM32_PLATFORM
    stm32_pwm_init(config->channel, config->frequency, config->pin);
#else
    // 其他平台实现
#endif
}

/**
 * @brief PWM LED状态设置
 *
 * 针对STM32平台，建议在bsp_led_hal.c中实现如下函数：
 *   void stm32_pwm_set_duty(uint8_t channel, uint8_t duty);
 *
 * @param config LED配置参数
 * @param state 目标状态
 */
static void led_pwm_set_state(const led_config_t* config, led_state_t state) {
#ifdef STM32_PLATFORM
    if (state == LED_STATE_OFF) {
        stm32_pwm_set_duty(config->channel, 0);
    } else if (state == LED_STATE_ON) {
        stm32_pwm_set_duty(config->channel, config->brightness);
    }
#else
    // 其他平台实现
#endif
}

/**
 * @brief PWM LED亮度设置
 *
 * @param config LED配置参数
 * @param brightness 亮度值（0-255）
 */
static void led_pwm_set_brightness(const led_config_t* config, uint8_t brightness) {
#ifdef STM32_PLATFORM
    stm32_pwm_set_duty(config->channel, brightness);
#else
    // 其他平台实现
#endif
}

/**
 * @brief PWM LED状态更新
 *
 * 处理PWM LED的闪烁和渐变效果。
 *
 * @param config LED配置参数
 * @param status LED状态
 */
static void led_pwm_update(const led_config_t* config, led_status_t* status) {
    if (status->state == LED_STATE_BLINK) {
        uint32_t elapsed = led_get_time_diff(status->counter, status->last_update);
        if (elapsed >= config->blink_period) {
            status->state_flag = !status->state_flag;
            status->last_update = status->counter;
            uint8_t duty = status->state_flag ? config->brightness : 0;
#ifdef STM32_PLATFORM
            stm32_pwm_set_duty(config->channel, duty);
#else
            // 其他平台实现
#endif
        }
    } else if (status->state == LED_STATE_FADE) {
        uint32_t elapsed = led_get_time_diff(status->counter, status->last_update);
        if (elapsed >= (config->fade_period / 255)) {
            status->last_update = status->counter;
            uint8_t fade_brightness = (status->counter * 255) / config->fade_period;
            if (fade_brightness > 255) fade_brightness = 255;
#ifdef STM32_PLATFORM
            stm32_pwm_set_duty(config->channel, fade_brightness);
#else
            // 其他平台实现
#endif
        }
    }
}

/* ==================== WS2812 LED 实现 ==================== */

/**
 * @brief WS2812 LED初始化
 * 
 * 初始化WS2812 RGB LED，设置数据引脚。
 * 
 * @param config LED配置参数
 * 
 * @note 
 * - 需要根据具体平台实现WS2812初始化
 * - 设置数据引脚和时序参数
 */
static void led_ws2812_init(const led_config_t* config) {
    // TODO: 根据具体平台实现WS2812初始化
    // 示例（使用FastLED库）：
    // FastLED.addLeds<WS2812, config->pin, GRB>(leds, config->led_id + 1);
    // FastLED.setBrightness(config->brightness);
}

/**
 * @brief WS2812 LED状态设置
 * 
 * 设置WS2812 LED的开关状态。
 * 
 * @param config LED配置参数
 * @param state 目标状态
 * 
 * @note 
 * - 关闭状态设置RGB值为(0,0,0)
 * - 常亮状态设置RGB值为(brightness,brightness,brightness)
 */
static void led_ws2812_set_state(const led_config_t* config, led_state_t state) {
    if (state == LED_STATE_OFF) {
        // TODO: 根据平台实现WS2812设置
        // ws2812_set_color(config->led_id, 0, 0, 0);
    } else if (state == LED_STATE_ON) {
        // TODO: 根据平台实现WS2812设置
        // ws2812_set_color(config->led_id, config->brightness, config->brightness, config->brightness);
    }
}

/**
 * @brief WS2812 LED亮度设置
 * 
 * 设置WS2812 LED的亮度等级。
 * 
 * @param config LED配置参数
 * @param brightness 亮度值（0-255）
 * 
 * @note 
 * - 亮度值影响RGB颜色的强度
 * - 可以设置全局亮度或单个LED亮度
 */
static void led_ws2812_set_brightness(const led_config_t* config, uint8_t brightness) {
    // TODO: 根据平台实现WS2812设置
    // ws2812_set_brightness(config->led_id, brightness);
}

/**
 * @brief WS2812 LED状态更新
 * 
 * 处理WS2812 LED的闪烁和渐变效果。
 * 
 * @param config LED配置参数
 * @param status LED状态
 * 
 * @note 
 * - 闪烁效果：在亮和暗之间切换
 * - 渐变效果：亮度在0到最大值之间平滑变化
 * - 支持彩色渐变效果
 */
static void led_ws2812_update(const led_config_t* config, led_status_t* status) {
    if (status->state == LED_STATE_BLINK) {
        uint32_t elapsed = led_get_time_diff(status->counter, status->last_update);
        if (elapsed >= config->blink_period) {
            // 切换闪烁状态
            status->state_flag = !status->state_flag;
            status->last_update = status->counter;
            
            if (status->state_flag) {
                // TODO: 根据平台实现WS2812设置
                // ws2812_set_color(config->led_id, config->brightness, config->brightness, config->brightness);
            } else {
                // TODO: 根据平台实现WS2812设置
                // ws2812_set_color(config->led_id, 0, 0, 0);
            }
        }
    } else if (status->state == LED_STATE_FADE) {
        uint32_t elapsed = led_get_time_diff(status->counter, status->last_update);
        if (elapsed >= (config->fade_period / 255)) {
            status->last_update = status->counter;
            
            // 计算渐变亮度
            uint8_t fade_brightness = (status->counter * 255) / config->fade_period;
            if (fade_brightness > 255) fade_brightness = 255;
            
            // TODO: 根据平台实现WS2812设置
            // ws2812_set_color(config->led_id, fade_brightness, fade_brightness, fade_brightness);
        }
    }
} 

#include "stm32f1xx_hal.h"
#include "cmsis_os.h"

/**
 * @brief 计算两个时间戳之间的差值（处理溢出情况）
 * @param current_time 当前时间戳
 * @param previous_time 之前的时间戳
 * @retval 时间差（毫秒）
 */
uint32_t led_get_time_diff(uint32_t current_time, uint32_t previous_time) {
    // 利用无符号整数的特性处理溢出情况
    return current_time - previous_time;
}

void led_system_time_update(uint32_t* system_time) {
#ifdef STM32_PLATFORM
    // 若有HAL或RTOS时基，直接读系统毫秒计数
    *system_time = osKernelSysTick();   // 使用CMSIS-RTOS API获取系统时间
#else
    // 通用回退：假设每次调用间隔为1ms
    (*system_time)++;
#endif
}
