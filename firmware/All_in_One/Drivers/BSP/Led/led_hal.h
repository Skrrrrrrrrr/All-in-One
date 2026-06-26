#ifndef LED_HAL_H
#define LED_HAL_H

#include <stdint.h>
#include <stdbool.h>
#include "stm32f4xx_hal.h" // 根据实际芯片修改

#include "led_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== LED引脚与外设宏（集中在驱动） ==================== */

// LED数量上限
#ifndef LED_PIN_TABLE_SIZE
#define LED_PIN_TABLE_SIZE    16
#endif

// LED引脚配置结构体
typedef struct {
    GPIO_TypeDef* port;       // GPIO端口 (GPIOA, GPIOB, GPIOC, GPIOD, GPIOE)
    uint16_t pin;             // GPIO引脚 (GPIO_PIN_0 ~ GPIO_PIN_15)
    const char* description;  // 引脚描述（可用于日志/调试）
} led_pin_config_t;

// 常用LED引脚定义（根据实际硬件修改）
#ifndef LED_STATUS_PORT
#define LED_STATUS_PORT        LED_GPIO_Port
#endif
#ifndef LED_STATUS_PIN
#define LED_STATUS_PIN         LED_Pin
#endif

#define SYS_STATUS_LED_SN 0

// GPIO适配接口
void stm32_gpio_init(uint8_t pin, bool active_high);
void stm32_gpio_set_level(uint8_t pin, GPIO_PinState state);

// PWM适配接口
void stm32_pwm_init(uint8_t channel, uint32_t freq, uint8_t pin);
void stm32_pwm_set_duty(uint8_t channel, uint8_t duty);

// WS2812适配接口（建议使用第三方库，如CubeWS2812）
// 这里只声明接口，具体实现可参考CubeWS2812库
void stm32_ws2812_init(uint8_t pin, uint16_t led_num);
void stm32_ws2812_set_color(uint16_t led_index, uint8_t r, uint8_t g, uint8_t b);
void stm32_ws2812_refresh(void);

/* ==================== LED引脚配置接口 ==================== */

/**
 * @brief 获取LED引脚配置
 * @param led_index LED索引 (0-15)
 * @return 引脚配置指针，如果索引无效返回NULL
 */
const led_pin_config_t* get_led_pin_config(uint8_t led_index);

/**
 * @brief 获取LED端口
 * @param led_index LED索引
 * @return GPIO端口，如果索引无效返回NULL
 */
GPIO_TypeDef* get_led_port(uint8_t led_index);

/**
 * @brief 获取LED引脚
 * @param led_index LED索引
 * @return GPIO引脚，如果索引无效返回0
 */
uint16_t get_led_pin(uint8_t led_index);

/**
 * @brief 获取LED描述
 * @param led_index LED索引
 * @return LED描述字符串，如果索引无效返回NULL
 */
const char* get_led_description(uint8_t led_index);

/**
 * @brief 打印所有LED引脚配置
 */
void print_led_pin_configs(void);

#ifdef __cplusplus
}
#endif

#endif // LED_HAL_H
