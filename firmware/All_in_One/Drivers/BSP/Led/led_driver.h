/**
 * @file led_driver.h
 * @brief LED驱动头文件 - 支持多种LED类型和控制模式
 * @author Your Name
 * @date 2024
 * 
 * 实现了基于表驱动设计模式的通用LED驱动功能。
 * 支持多种LED类型的统一管理和控制。
 */

#ifndef LED_DRIVER_H
#define LED_DRIVER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STM32_PLATFORM

/* ==================== 常量定义 ==================== */

#define LED_MAX_COUNT             1      // 最大LED数量
#define LED_MAX_BRIGHTNESS        255     // 最大亮度值
#define LED_MIN_BLINK_PERIOD     50      // 最小闪烁周期（毫秒）
#define LED_MIN_FADE_PERIOD      100     // 最小渐变周期（毫秒）

/* ==================== 类型定义 ==================== */

/**
 * @brief LED类型枚举
 */
typedef enum {
    LED_TYPE_GPIO = 0,            // GPIO控制LED
    LED_TYPE_PWM,                 // PWM控制LED
    LED_TYPE_I2C,                 // I2C控制LED
    LED_TYPE_SPI,                 // SPI控制LED
    LED_TYPE_WS2812,              // WS2812 RGB LED
    LED_TYPE_MAX
} led_type_t;

/**
 * @brief LED状态枚举
 */
typedef enum {
    LED_STATE_OFF = 0,            // 关闭状态
    LED_STATE_ON,                 // 常亮状态
    LED_STATE_BLINK,              // 闪烁状态
    LED_STATE_FADE,               // 渐变状态
    LED_STATE_MAX
} led_state_t;

/**
 * @brief LED配置结构体
 */
typedef struct {
    uint8_t pin;                  // LED引脚号
    led_type_t type;              // LED类型
    bool active_high;             // 高电平有效
    uint8_t brightness;           // 亮度值（0-255）
    uint16_t blink_period;        // 闪烁周期（毫秒）
    uint16_t fade_period;         // 渐变周期（毫秒）
    uint8_t channel;              // PWM通道号（PWM类型使用）
    uint32_t frequency;           // PWM频率（Hz，PWM类型使用）
    uint8_t led_id;               // LED ID（WS2812类型使用）
} led_config_t;

/**
 * @brief LED状态结构体
 */
typedef struct {
    led_state_t state;            // 当前状态
    uint8_t brightness;           // 当前亮度
    uint32_t last_update;         // 最后更新时间戳
    uint32_t counter;             // 计数器
    bool state_flag;              // 状态标志（用于闪烁）
} led_status_t;

/**
 * @brief LED操作函数表结构体
 */
typedef struct {
    led_type_t type;              // LED类型
    void (*init_func)(const led_config_t* config);           // 初始化函数
    void (*set_state_func)(const led_config_t* config, led_state_t state); // 状态设置函数
    void (*set_brightness_func)(const led_config_t* config, uint8_t brightness); // 亮度设置函数
    void (*update_func)(const led_config_t* config, led_status_t* status); // 状态更新函数
} led_ops_table_t;

/**
 * @brief LED驱动管理器结构体
 */
typedef struct {
    led_config_t* configs;        // LED配置数组
    led_status_t* status;         // LED状态数组
    uint8_t led_count;            // LED数量
    uint32_t system_time;         // 系统时间
} led_driver_t;


/* ==================== 函数声明 ==================== */

/**
 * @brief 初始化LED驱动
 * @param driver LED驱动管理器指针
 * @param configs LED配置数组指针
 * @param status LED状态数组指针
 * @param count LED数量
 */
void led_driver_init(led_driver_t* driver, led_config_t* configs, 
                     led_status_t* status, uint8_t count);

/**
 * @brief 更新LED驱动状态
 * @param driver LED驱动管理器指针
 */
void led_driver_update(led_driver_t* driver);

/**
 * @brief 设置LED状态
 * @param driver LED驱动管理器指针
 * @param led_id LED标识符
 * @param state 目标状态
 */
void led_set_state(led_driver_t* driver, uint8_t led_id, led_state_t state);

/**
 * @brief 设置LED亮度
 * @param driver LED驱动管理器指针
 * @param led_id LED标识符
 * @param brightness 亮度值（0-255）
 */
void led_set_brightness(led_driver_t* driver, uint8_t led_id, uint8_t brightness);

/**
 * @brief 设置LED闪烁周期
 * @param driver LED驱动管理器指针
 * @param led_id LED标识符
 * @param period 闪烁周期（毫秒）
 */
void led_set_blink_period(led_driver_t* driver, uint8_t led_id, uint16_t period);

/**
 * @brief 设置LED渐变周期
 * @param driver LED驱动管理器指针
 * @param led_id LED标识符
 * @param period 渐变周期（毫秒）
 */
void led_set_fade_period(led_driver_t* driver, uint8_t led_id, uint16_t period);

/* ==================== 内部函数声明 ==================== */

/**
 * @brief 更新系统时间
 * @param system_time 系统时间指针
 */
void led_system_time_update(uint32_t* system_time);

/**
 * @brief 计算两个时间戳之间的差值（处理溢出情况）
 * @param current_time 当前时间戳
 * @param previous_time 之前的时间戳
 * @retval 时间差（毫秒）
 */
uint32_t led_get_time_diff(uint32_t current_time, uint32_t previous_time);

#ifdef __cplusplus
}
#endif

#endif // LED_DRIVER_H
