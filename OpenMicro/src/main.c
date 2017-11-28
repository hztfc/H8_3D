/*
The MIT License (MIT)

Copyright (c) 2016 silverx

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/


// STM32 acro firmware
// files of this project should be assumed MIT licence unless otherwise noted


#include "project.h"

#include "led.h"
#include "util.h"
#include "sixaxis.h"
#include "drv_adc.h"
#include "drv_time.h"
#include "drv_softi2c.h"
#include "config.h"
#include "drv_pwm.h"
#include "drv_adc.h"
#include "drv_gpio.h"
#include "drv_serial.h"
#include "rx.h"
#include "drv_spi.h"
#include "control.h"
#include "defines.h"
#include "drv_i2c.h"

#include "drv_softi2c.h"

#include "drv_serial.h"

#include "binary.h"

#include <stdio.h>

#include <inttypes.h>



#ifdef DEBUG
#include "debug.h"
debug_type debug;
#endif




// hal
void clk_init(void);

void imu_init(void);

// looptime in seconds
float looptime;
// filtered battery in volts
float vbattfilt = 0.0f;
float vbatt_comp = 4.2;
float vbatt_filt_kill = 4.2f;
// voltage reference for vcc compensation
float vreffilt = 1.0;
// average of all motors
float thrfilt = 0;

unsigned int lastlooptime;
// signal for lowbattery
int lowbatt = 1;	

//float vref = 1.0;
//float startvref;

// holds the main four channels, roll, pitch , yaw , throttle
float rx[4];

// holds auxilliary channels
// the last 2 are always on and off respectively
char aux[AUXNUMBER] = { 0 ,0 ,0 , 0 , 0 , 0};
char lastaux[AUXNUMBER];
// if an aux channel has just changed
char auxchange[AUXNUMBER];

// bind / normal rx mode
extern int rxmode;
// failsafe on / off
extern int failsafe;

float vbatt = 4.20;

// for led flash on gestures
int ledcommand = 0;
unsigned long ledcommandtime = 0;

void failloop( int val);

int main(void)
{
	
	delay(1000);


#ifdef ENABLE_OVERCLOCK
clk_init();
#endif
	
  gpio_init();


	i2c_init();	
	
	
	spi_init();
	
  time_init();

	delay(100000);

	pwm_init();

  pwm_dir(FREE);

	pwm_set( MOTOR_BL , 0);
	pwm_set( MOTOR_FL , 0);	 
	pwm_set( MOTOR_FR , 0); 
	pwm_set( MOTOR_BR , 0); 


	sixaxis_init();
	
	if ( sixaxis_check() ) 
	{
		#ifdef SERIAL_INFO	
		printf( " MPU found \n" );
		#endif
	}
	else 
	{
		#ifdef SERIAL_INFO	
		printf( "ERROR: MPU NOT FOUND \n" );	
		#endif
		failloop(4);
	}
	
	adc_init();
//set always on channel to on
aux[CH_ON] = 1;	
	
#ifdef AUX1_START_ON
aux[CH_AUX1] = 1;
#endif
	rx_init();

/*
if ( RCC->CSR & 0x80000000 )
{
	// low power reset flag
	// not functioning
	failloop(3);
}
*/
	
int count = 0;
	
while ( count < 64 )
{
	vbattfilt += adc_read(0);
//	startvref += adc_read(1);
	delay(1000);
	count++;
}

 vbattfilt = vbattfilt/64;	
// startvref = startvref/64;

	
#ifdef STOP_LOWBATTERY
// infinite loop
if ( vbattfilt < (float) STOP_LOWBATTERY_TRESH) failloop(2);
#endif



	gyro_cal();

#ifdef SERIAL_ENABLE
serial_init();
#endif

#ifdef SERIAL_INFO	
		printf( "Vbatt %2.2f \n", vbattfilt );
		#ifdef NOMOTORS
    printf( "NO MOTORS\n" );
		#warning "NO MOTORS"
		#endif
#endif

#ifndef ACRO_ONLY
	imu_init();
	
// read accelerometer calibration values from option bytes ( 2* 8bit)
extern float accelcal[3];
extern int readdata( int datanumber);

 accelcal[0] = readdata( OB->DATA0 ) - 127;
 accelcal[1] = readdata( OB->DATA1 ) - 127;
#endif


extern unsigned int liberror;
if ( liberror ) 
{
	  #ifdef SERIAL_INFO	
		printf( "ERROR: I2C \n" );	
		#endif
		failloop(7);
}



 lastlooptime = gettime();

 float thrfilt;

//
//
// 		MAIN LOOP
//
//


	while(1)
	{
		// gettime() needs to be called at least once per second 
		unsigned long time = gettime(); 
		looptime = ((uint32_t)( time - lastlooptime));
		if ( looptime <= 0 ) looptime = 1;
		looptime = looptime * 1e-6f;
		if ( looptime > 0.02f ) // max loop 20ms
		{
			failloop( 6);	
			//endless loop			
		}
	
		#ifdef DEBUG				
		debug.totaltime += looptime;
		lpf ( &debug.timefilt , looptime, 0.998 );
		#endif
		lastlooptime = time;
		
		if ( liberror > 20) 
		{
			failloop(8);
			// endless loop
		}


		#ifdef ACRO_ONLY
		gyro_read();
		#else		
		sixaxis_read();
		
		extern void imu_calc(void);		
		imu_calc();		
		#endif

		float battadc = adc_read(0);
		vbatt = battadc;
		
// all flight calculations and motors
		control();

		

		// average of all 4 motor thrusts
		// should be proportional with battery current			
		extern float thrsum; // from control.c
	
		// filter motorpwm so it has the same delay as the filtered voltage
		// ( or they can use a single filter)		
		lpf ( &thrfilt , thrsum , 0.9968f);	// 0.5 sec at 1.6ms loop time	

        static float vbattfilt_corr = 4.2;
        // li-ion battery model compensation time decay ( 18 seconds )
        lpf ( &vbattfilt_corr , vbattfilt , FILTERCALC( 1000 , 18000e3) );
	
        lpf ( &vbattfilt , battadc , 0.9968f);


// compensation factor for li-ion internal model
// zero to bypass
#define CF1 0.25f

        float tempvolt = vbattfilt*( 1.00f + CF1 )  - vbattfilt_corr* ( CF1 );

#ifdef AUTO_VDROP_FACTOR

static float lastout[12];
static float lastin[12];
static float vcomp[12];
static float score[12];
static int z = 0;
static int minindex = 0;
static int firstrun = 1;


if( thrfilt > 0.1f )
{
	vcomp[z] = tempvolt + (float) z *0.1f * thrfilt;
		
	if ( firstrun ) 
    {
        for (int y = 0 ; y < 12; y++) lastin[y] = vcomp[z];
        firstrun = 0;
    }
	float ans;
	//	y(n) = x(n) - x(n-1) + R * y(n-1) 
	//  out = in - lastin + coeff*lastout
		// hpf
	ans = vcomp[z] - lastin[z] + FILTERCALC( 1000*12 , 6000e3) *lastout[z];
	lastin[z] = vcomp[z];
	lastout[z] = ans;
	lpf ( &score[z] , ans*ans , FILTERCALC( 1000*12 , 60e6 ) );	
	z++;
       
    if ( z >= 12 )
    {
        z = 0;
        float min = score[0]; 
        for ( int i = 0 ; i < 12; i++ )
        {
         if ( (score[i]) < min )  
            {
                min = (score[i]);
                minindex = i;
                // add an offset because it seems to be usually early
                minindex++;
            }
        }   
    }

}

#undef VDROP_FACTOR
#define VDROP_FACTOR  minindex * 0.1f
#endif

    float hyst;
    if ( lowbatt ) hyst = HYST;
    else hyst = 0.0f;

    if (( tempvolt + (float) VDROP_FACTOR * thrfilt <(float) VBATTLOW + hyst )
        || ( vbattfilt < ( float ) 2.7f ) )
        lowbatt = 1;
    else lowbatt = 0;

    vbatt_comp = tempvolt + (float) VDROP_FACTOR * thrfilt; 	

           
#ifdef DEBUG
	debug.vbatt_comp = vbatt_comp ;
#endif		
	

#if ( LED_NUMBER > 0)
// led flash logic	
if ( lowbatt )
	ledflash ( 500000 , 8);
else
{
		if ( rxmode == RXMODE_BIND)
		{// bind mode
		ledflash ( 100000, 12);
		}else
		{// non bind
			if ( failsafe) 
				{
					ledflash ( 500000, 15);			
				}
			else 
			{
				#ifdef GESTURES2_ENABLE
				if (ledcommand)
						  {
							  if (!ledcommandtime)
								  ledcommandtime = gettime();
							  if (gettime() - ledcommandtime > 500000)
							    {
								    ledcommand = 0;
								    ledcommandtime = 0;
							    }
							  ledflash(100000, 8);
						  }
						else
					#endif // end gesture led flash
				if ( aux[LEDS_ON] )
				#if( LED_BRIGHTNESS != 15)	
				led_pwm(LED_BRIGHTNESS);
				#else
				ledon( 255);
				#endif
				else 
				ledoff( 255);
			}
		} 		
		
	}
#endif
#if ( AUX_LED_NUMBER > 0)		
//AUX led flash logic		
		if ( lowbatt2 ) 
				auxledflash ( 250000 , 8);	
		else 
		{
			if ( rxmode == RXMODE_BIND)
			{// bind mode
			auxledflash ( 100000 , 12);
			}
			else auxledoff( 255);
		}
#endif

checkrx();
	
extern void osdcycle();	
osdcycle();
		
// the delay is required or it becomes endless loop ( truncation in time routine)
while ( (gettime() - time) < LOOPTIME ) delay(10); 		

		
	}// end loop
	

}

// 2 - low battery at powerup - if enabled by config

// 4 - Gyro not found
// 5 - clock , intterrupts , systick
// 7 - i2c error 
// 8 - i2c error main loop
// 6 - loop time issue

void failloop( int val)
{
	for ( int i = 0 ; i <= 3 ; i++)
	{
		pwm_set( i ,0 );
	}	

	while(1)
	{
		for ( int i = 0 ; i < val; i++)
		{
		 ledon( 255);		
		 delay(200000);
		 ledoff( 255);	
		 delay(200000);			
		}
		delay(800000);
	}	
	
}


void HardFault_Handler(void)
{
	failloop(5);
}
void MemManage_Handler(void) 
{
	failloop(5);
}
void BusFault_Handler(void) 
{
	failloop(5);
}
void UsageFault_Handler(void) 
{
	failloop(5);
}





