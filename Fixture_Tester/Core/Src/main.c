/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * @file           : main.c
 * @brief          : Main program body
 ******************************************************************************
 * @attention
 *
 * Copyright (c) 2025 STMicroelectronics.
 * All rights reserved.
 *
 * This software is licensed under terms which can be found in the LICENSE file
 * in the root directory of this software component.
 * It is provided AS-IS.
 *
 ******************************************************************************
 */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "adc.h"
#include "dma.h"
#include "i2c.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "../../icode/oled.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
// 定义一个枚举类型来表示测量过程的不同状态
typedef enum {
    STATE_IDLE,         // 空闲状态，等待按键开始测量
    STATE_WAIT_FOR_2S,  // 等待状态，用于稳定和采集背景值
    STATE_CAPTURE_R,    // 捕获状态，采集最终测量值
    STATE_DONE          // 完成状态，显示结果并等待复位
} MeasurementState;

/* Private define ------------------------------------------------------------*/
#define TX_R_MIN  28.0   // 发送端电阻最小值
#define TX_R_MAX  29.0   // 发送端电阻最大值
#define RX_R_MIN  38.0   // 接收端电阻最小值
#define RX_R_MAX  39.0   // 接收端电阻最大值
#define EPS 0.05         // 允许的误差范围，用于放宽判定条件
#define FILTER_DEPTH 16  // 移动平均滤波器的深度（采样点数）
#define VREF 3.3         // ADC的参考电压

/* Private variables ---------------------------------------------------------*/
volatile uint16_t ADC_value[4] = {0}; // 用于存放DMA传输的4通道ADC采样原始值
volatile uint8_t adc_ready = 0;       // ADC采样完成标志位，在DMA中断中设置

// 移动平均滤波相关变量
double tx_R_buf[FILTER_DEPTH]={0}, rx_R_buf[FILTER_DEPTH]={0}, tx_B_buf[FILTER_DEPTH]={0}, rx_B_buf[FILTER_DEPTH]={0}; // 存储四个测量值的历史数据缓冲区
double tx_R_sum=0, rx_R_sum=0, tx_B_sum=0, rx_B_sum=0; // 四个测量值的累加和
int tx_R_idx=0, rx_R_idx=0, tx_B_idx=0, rx_B_idx=0;   // 当前缓冲区索引

// 状态机和计时相关变量
MeasurementState current_state = STATE_IDLE; // 当前状态机的状态，初始为空闲
uint32_t capture_tick = 0;                   // 用于计时的变量，记录某一时刻的系统滴答数

// 测量结果相关变量
double tx_B_offset=0, rx_B_offset=0;         // 磁场测量的背景偏移量
double final_tx_R=0, final_rx_R=0, final_tx_B=0, final_rx_B=0; // 存储最终计算出的四个测量值

uint8_t show_welcome_flag=1; // 是否显示欢迎界面的标志位

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
void UI_Init(void);            // 初始化OLED显示界面的函数

/* Private user code ---------------------------------------------------------*/
/**
  * @brief  移动平均滤波函数
  * @param  new_val: 新的采样值
  * @param  buf: 存放历史数据的缓冲区
  * @param  sum: 历史数据的累加和
  * @param  idx: 当前缓冲区的索引
  * @retval 滤波后的平均值
  */
double filter_average(double new_val, double *buf, double *sum, int *idx) {
    *sum -= buf[*idx];
    buf[*idx] = new_val;
    *sum += new_val;
    *idx = (*idx + 1) % FILTER_DEPTH;
    return *sum / FILTER_DEPTH;
}

/**
  * @brief  初始化OLED的用户界面（UI）
  * @note   显示固定的中文标签，如“发送端”、“回收端”等
  */
void UI_Init(void) {
    OLED_ShowCHinese(28,0,0,0); OLED_ShowCHinese(44,0,1,0); OLED_ShowCHinese(60,0,2,0);
    OLED_ShowCHinese(82,0,3,0); OLED_ShowCHinese(97,0,4,0); OLED_ShowCHinese(114,0,5,0);
    OLED_ShowCHinese(0,2,6,0); OLED_ShowCHinese(16,2,7,0);
    OLED_ShowCHinese(0,6,8,0); OLED_ShowCHinese(16,6,9,0);
}

/**
  * @brief  主函数
  */
int main(void)
{
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init(); 
		MX_DMA_Init(); 
		MX_ADC1_Init();
		MX_I2C1_Init(); 
		MX_USART3_UART_Init(); 
		MX_TIM2_Init();

    OLED_Init(); 
		OLED_Clear();
	
    HAL_ADCEx_Calibration_Start(&hadc1);
    HAL_ADC_Start_DMA(&hadc1, (uint32_t*)ADC_value, 4);
		
		// 初始化控制继电器的GPIO引脚为低电平
    HAL_GPIO_WritePin(R_C_GPIO_Port, R_C_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(T_C_GPIO_Port, T_C_Pin, GPIO_PIN_RESET);

    while (1)
    {
				// ------------- 状态: 空闲 (STATE_IDLE) -------------
        // 等待用户按下按键开始测量
        if (current_state == STATE_IDLE) {
            if (show_welcome_flag) {
                OLED_Clear();
                OLED_ShowCHinese(16, 3, 10, 0); 
								OLED_ShowCHinese(32, 3, 11, 0); 
								OLED_ShowCHinese(48, 3, 12, 0);
                OLED_ShowCHinese(64, 3, 13, 0);
								OLED_ShowCHinese(80, 3, 14, 0); 
								OLED_ShowCHinese(96, 3, 15, 0);
                show_welcome_flag = 0;
            }
						
						// 检测按键是否按下
            if (HAL_GPIO_ReadPin(KEY_C_GPIO_Port, KEY_C_Pin) == GPIO_PIN_SET) {
                HAL_Delay(20);
                if (HAL_GPIO_ReadPin(KEY_C_GPIO_Port, KEY_C_Pin) == GPIO_PIN_SET) {
                    OLED_Clear(); 
										UI_Init();
                    capture_tick = HAL_GetTick(); // 记录当前时间
                    current_state = STATE_WAIT_FOR_2S; // 切换到等待状态
                    while(HAL_GPIO_ReadPin(KEY_C_GPIO_Port, KEY_C_Pin) == GPIO_PIN_SET);
                }
            }
        }
				
				// ------------- 状态: 完成 (STATE_DONE) -------------
        // 等待用户按下按键返回空闲状态
        if (current_state == STATE_DONE) {
            if (HAL_GPIO_ReadPin(KEY_C_GPIO_Port, KEY_C_Pin) == GPIO_PIN_SET) {
                HAL_Delay(20);
                if (HAL_GPIO_ReadPin(KEY_C_GPIO_Port, KEY_C_Pin) == GPIO_PIN_SET) {
                    current_state = STATE_IDLE;
                    show_welcome_flag = 1;
                    while(HAL_GPIO_ReadPin(KEY_C_GPIO_Port, KEY_C_Pin) == GPIO_PIN_SET);
                }
            }
        }
				
				// 当ADC通过DMA完成一次4通道的转换后，adc_ready标志位会被置1
        if (adc_ready) {
            adc_ready = 0;
						
						// 1. 将ADC原始值转换为电压值
            double v2 = ADC_value[0]*VREF/4095.0, v3 = ADC_value[1]*VREF/4095.0;
            double v6 = ADC_value[2]*VREF/4095.0, v7 = ADC_value[3]*VREF/4095.0;
	
						// 2. 根据电路设计，将电压值转换为实际物理量（电阻和磁场强度），并进行滤波
            double tx_R_f = filter_average((3.3-v2)*56.0/1.25, tx_R_buf, &tx_R_sum, &tx_R_idx); //2.6
            double rx_R_f = filter_average((3.3-v3)*56.0/1.25, rx_R_buf, &rx_R_sum, &rx_R_idx); //2.45
            double tx_B_f = filter_average((v7-1-.86)/0.013, tx_B_buf, &tx_B_sum, &tx_B_idx);
            double rx_B_f = filter_average((v6-1.90)/0.013, rx_B_buf, &rx_B_sum, &rx_B_idx);
					
						tx_R_f -= 2;
						rx_R_f -= 2;
						
						// ------------- 状态: 等待 (STATE_WAIT_FOR_2S) -------------
            // 此阶段用于测量背景磁场值作为偏移量，并测量电阻值
            if (current_state == STATE_WAIT_FOR_2S && HAL_GetTick() - capture_tick > 500) {
								// 等待1.5秒后，数据稳定，此时捕获的值作为背景值和电阻值
								tx_B_offset = tx_B_f;	// 捕获发送端磁场背景值
                rx_B_offset = rx_B_f; // 捕获接收端磁场背景值
                final_tx_R = tx_R_f;	// 捕获发送端电阻最终值
                final_rx_R = rx_R_f;	// 捕获接收端电阻最终值

								// 启动测量磁场的电路
                HAL_GPIO_WritePin(R_C_GPIO_Port, R_C_Pin, GPIO_PIN_SET);
                HAL_GPIO_WritePin(T_C_GPIO_Port, T_C_Pin, GPIO_PIN_SET);

								// 在OLED上显示电阻值
                OLED_ShowFloat(32, 2, final_tx_R, 2, 1, 16, 0);
								OLED_ShowFloat(85, 2, final_rx_R, 2, 1, 16, 0);

                capture_tick = HAL_GetTick();    // 重新计时
                current_state = STATE_CAPTURE_R; // 切换到捕获状态
            }
						// ------------- 状态: 捕获 (STATE_CAPTURE_R) -------------
            // 此阶段用于测量包含信号的磁场值
            else if (current_state == STATE_CAPTURE_R && HAL_GetTick() - capture_tick > 1000) {
								// 再等待3秒后，数据稳定，此时捕获的值为最终的磁场值
                final_tx_B = tx_B_f - tx_B_offset;	// 最终磁场值 = 当前测量值 - 背景值
                final_rx_B = rx_B_f - rx_B_offset;
	
								// 关闭测量磁场的电路
                HAL_GPIO_WritePin(R_C_GPIO_Port, R_C_Pin, GPIO_PIN_RESET);
                HAL_GPIO_WritePin(T_C_GPIO_Port, T_C_Pin, GPIO_PIN_RESET);

								// 在OLED上显示磁场值
                OLED_ShowFloat(32, 6, final_tx_B, 2, 1, 16, 0);
								OLED_ShowFloat(85, 6, final_rx_B, 2, 1, 16, 0);
							

                // 3. 判定逻辑：检查所有四个测量值是否在预设的范围内
                if (final_tx_R >= TX_R_MIN-EPS && final_tx_R <= TX_R_MAX+EPS &&
                    final_rx_R >= RX_R_MIN-EPS && final_rx_R <= RX_R_MAX+EPS &&
                    final_tx_B >= 0 &&
                    final_rx_B >= 0)
                {
										// 如果所有值都在范围内，显示“测试合格”
                    OLED_ShowCHinese(32, 4, 14, 0); OLED_ShowCHinese(48, 4, 15, 0);
                    OLED_ShowCHinese(64, 4, 17, 0); OLED_ShowCHinese(80, 4, 18, 0);
                } else {
										// // 否则，显示“测试不合格”
                    OLED_ShowCHinese(24, 4, 14, 0); OLED_ShowCHinese(40, 4, 15, 0);
                    OLED_ShowCHinese(56, 4, 16, 0); OLED_ShowCHinese(72, 4, 17, 0);
                    OLED_ShowCHinese(88, 4, 18, 0);
                }

                current_state = STATE_DONE; // 切换到完成状态
            }
        }
    }
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};

  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) { Error_Handler(); }

  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK) { Error_Handler(); }

  PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_ADC;
  PeriphClkInit.AdcClockSelection = RCC_ADCPCLK2_DIV6;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK) { Error_Handler(); }
}

/* USER CODE BEGIN 4 */
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc->Instance == ADC1) {
        adc_ready = 1;   // 一轮规则通道序列完成
    }
}
/* USER CODE END 4 */

void Error_Handler(void)
{
  __disable_irq();
  while (1) {}
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line) {}
#endif
