#ifndef __KEY_H
#define __KEY_H

#define KEY_COUNT				7

#define K_UP					0
#define K_DOWN			    	1
#define K_LEFT			    	2
#define K_RIGHT			    	3
#define K_PRESS			    	4
#define K_ENC				    5
#define K_TRG				    6

#define KEY_HOLD				0x01
#define KEY_DOWN				0x02
#define KEY_UP					0x04
#define KEY_SINGLE				0x08
#define KEY_DOUBLE				0x10
#define KEY_LONG				0x20
#define KEY_REPEAT				0x40

void Key_Init(void);
uint8_t Key_Check(uint8_t n, uint8_t Flag);
void Key_Tick(void);

#endif
