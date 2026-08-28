#ifndef __PULSE_H
#define __PULSE_H

#define CH_NONE                         0
#define CH1                             1
#define CH2                             2
#define CH3                             3
#define CH4                             4
#define CH5                             5
#define CH6                             6

#define PULSE_MODE_SINGLE               0
#define PULSE_MODE_NPULSE               1
#define PULSE_MODE_DPULSE               2
#define PULSE_MODE_PWM                  3
#define PULSE_MODE_NONE                 4
#define PULSE_MODE_SINGLE_LONG          5
#define PULSE_MODE_NPULSE_LONG          6
#define PULSE_MODE_PWM_LONG             7

#define PULSE_POLARITY_HIGH             0
#define PULSE_POLARITY_LOW              1


#define US_TO_S(us)         ((float)(us) / 1000000.0f)


void Pulse_Select_Output(uint8_t CHx);
void Pulse_Enable_Output(void);
void Pulse_Disable_Output(void);
void Pulse_SetPulsePolarity_High(void);
void Pulse_SetPulsePolarity_Low(void);
bool Pulse_nPulse_SetPW(float pw);
void Pulse_nPulse_Init(void);
bool Pulse_dPulse_SetPW(int32_t pw1, int32_t interval, int32_t pw2);
void Pulse_dPulse_Init(void);
bool Pulse_PWM_SetPW(float period_us, int32_t duty_cycle_percent);
void Pulse_PWM_Init(void);
void Pulse_slPulse_SetPW(float pw);
void Pulse_slPulse_Init(void);
void Pulse_lPWM_SetPW(float period_s, float duty_cycle_percent);
void Pulse_lPWM_Init(void);
GPIO_TypeDef *Pulse_GetLongPulsePort(void);
uint16_t Pulse_GetLongPulsePin(void);



#endif