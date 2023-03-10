#include	"temp.h"

/** \file
	\brief Manage temperature sensors

	\note \b ALL temperatures are stored as 14.2 fixed point in teacup, so we have a range of 0 - 16383.75 celsius and a precision of 0.25 celsius. That includes the ThermistorTable, which is why you can't copy and paste one from other firmwares which don't do this.
*/

#define PSTR(x) x
#include	<stdlib.h>
#include	<stdio.h>
//#include	<avr/eeprom.h>
//#include	<avr/pgmspace.h>

//#include	"arduino.h"
#include	"./clock/delay.h"
#include	"debug.h"
#ifndef	EXTRUDER
	#include	"./serial/sersendf.h"
#endif
#include	"./heater/heater.h"
#ifdef	TEMP_INTERCOM
	#include	"intercom.h"
#endif

#ifdef	TEMP_MAX6675
#endif

#ifdef	TEMP_THERMISTOR
//#include	"analog.h"
//#include	"ThermistorTable.h"
#endif

#ifdef	TEMP_AD595
#include	"analog.h"
#endif

#ifdef TEMP_NONE
// no actual sensor, just store the target temp
#endif

typedef enum {
	PRESENT,
	TCOPEN
} temp_flags_enum;

/// holds metadata for each temperature sensor
typedef struct {
	temp_type_t temp_type; ///< type of sensor
	uint8_t     temp_pin;  ///< pin that sensor is on
	heater_t    heater;    ///< associated heater if any
	uint8_t		additional; ///< additional, sensor type specifc config
} temp_sensor_definition_t;

#undef DEFINE_TEMP_SENSOR
/// help build list of sensors from entries in config.h
#define DEFINE_TEMP_SENSOR(name, type, pin, additional) { (type), (pin), (HEATER_ ## name), (additional) },
static const temp_sensor_definition_t temp_sensors[NUM_TEMP_SENSORS] =
{
	#include	"config.h"
};
#undef DEFINE_TEMP_SENSOR

/// this struct holds the runtime sensor data- read temperatures, targets, etc
struct {
	temp_flags_enum		temp_flags;     ///< flags

	uint16_t					last_read_temp; ///< last received reading
	uint16_t					target_temp;		///< manipulate attached heater to attempt to achieve this value

	uint16_t					temp_residency; ///< how long have we been close to target temperature in temp ticks?

	uint16_t					next_read_time; ///< how long until we can read this sensor again?
} temp_sensors_runtime[NUM_TEMP_SENSORS];

/// set up temp sensors. Currently only the 'intercom' sensor needs initialisation.
void temp_init() {
	temp_sensor_t i;
	for (i = 0; i < NUM_TEMP_SENSORS; i++) {
		switch(temp_sensors[i].temp_type) {
		#ifdef	TEMP_MAX6675
			// initialised when read
/*			case TT_MAX6675:
				break;*/
		#endif

		#ifdef	TEMP_THERMISTOR
			// handled by analog_init()
/*			case TT_THERMISTOR:
				break;*/
		#endif

		#ifdef	TEMP_AD595
			// handled by analog_init()
/*			case TT_AD595:
				break;*/
		#endif

		#ifdef	TEMP_INTERCOM
			case TT_INTERCOM:
				intercom_init();
				send_temperature(0, 0);
				break;
		#endif

		#ifdef  TEMP_NONE
			case TT_NONE:
				// nothing to do
				break;
		#endif

			default: /* prevent compiler warning */
				break;
		}
	}
}

/// called every 10ms from clock.c - check all temp sensors that are ready for checking
void temp_sensor_tick() {
	temp_sensor_t i = 0;
	for (; i < NUM_TEMP_SENSORS; i++) {
		if (temp_sensors_runtime[i].next_read_time) {
			temp_sensors_runtime[i].next_read_time--;
		}
		else {
			uint16_t	temp = 0;
			//time to deal with this temp sensor
			switch(temp_sensors[i].temp_type) {
				#ifdef	TEMP_MAX6675
				case TT_MAX6675:
					#ifdef	PRR
						PRR &= ~MASK(PRSPI);
					#elif defined PRR0
						PRR0 &= ~MASK(PRSPI);
					#endif

					SPCR = MASK(MSTR) | MASK(SPE) | MASK(SPR0);

					// enable TT_MAX6675
					WRITE(SS, 0);

					// No delay required, see
					// https://github.com/triffid/Teacup_Firmware/issues/22

					// read MSB
					SPDR = 0;
					for (;(SPSR & MASK(SPIF)) == 0;);
					temp = SPDR;
					temp <<= 8;

					// read LSB
					SPDR = 0;
					for (;(SPSR & MASK(SPIF)) == 0;);
					temp |= SPDR;

					// disable TT_MAX6675
					WRITE(SS, 1);

					temp_sensors_runtime[i].temp_flags = 0;
					if ((temp & 0x8002) == 0) {
						// got "device id"
						temp_sensors_runtime[i].temp_flags |= PRESENT;
						if (temp & 4) {
							// thermocouple open
							temp_sensors_runtime[i].temp_flags |= TCOPEN;
						}
						else {
							temp = temp >> 3;
						}
					}

					// this number depends on how frequently temp_sensor_tick is called. the MAX6675 can give a reading every 0.22s, so set this to about 250ms
					temp_sensors_runtime[i].next_read_time = 25;

					break;
				#endif	/* TEMP_MAX6675	*/

				#ifdef	TEMP_THERMISTOR
#ifdef TRASH
				case TT_THERMISTOR:

					do {
						uint8_t j;//, table_num;
						//Read current temperature
						temp = 1;//analog_read(temp_sensors[i].temp_pin);
						// for thermistors the thermistor table number is in the additional field
						table_num = temp_sensors[i].additional;

						//Calculate real temperature based on lookup table
						for (j = 1; j < NUMTEMPS; j++) {
							if (pgm_read_word(&(temptable[table_num][j][0])) > temp) {
								// Thermistor table is already in 14.2 fixed point
								#ifndef	EXTRUDER
								if (DEBUG_PID && (debug_flags & DEBUG_PID))
									sersendf_P(PSTR("pin:%d Raw ADC:%d table entry: %d"),temp_sensors[i].temp_pin,temp,j);
								#endif
								// Linear interpolating temperature value
								// y = ((x - x???)y??? + (x???-x)y??? ) / (x??? - x???)
								// y = temp
								// x = ADC reading
								// x???= temptable[j-1][0]
								// x???= temptable[j][0]
								// y???= temptable[j-1][1]
								// y???= temptable[j][1]
								// y =
								// Wikipedia's example linear interpolation formula.
								temp = (
								//     ((x - x???)y???
									((uint32_t)temp - pgm_read_word(&(temptable[table_num][j-1][0]))) * pgm_read_word(&(temptable[table_num][j][1]))
								//                 +
									+
								//                   (x???-x)
									(pgm_read_word(&(temptable[table_num][j][0])) - (uint32_t)temp)
								//                         y??? )
									* pgm_read_word(&(temptable[table_num][j-1][1])))
								//                              /
									/
								//                                (x??? - x???)
									(pgm_read_word(&(temptable[table_num][j][0])) - pgm_read_word(&(temptable[table_num][j-1][0])));
								#ifndef	EXTRUDER
								if (DEBUG_PID && (debug_flags & DEBUG_PID))
									sersendf_P(PSTR(" temp:%d.%d"),temp/4,(temp%4)*25);
								#endif
								break;

							}
						}

						#ifndef	EXTRUDER
						if (DEBUG_PID && (debug_flags & DEBUG_PID))
							sersendf_P(PSTR(" Sensor:%d\n"),i);
						#endif


						//Clamp for overflows
						if (j == NUMTEMPS)
							temp = temptable[table_num][NUMTEMPS-1][1];

						temp_sensors_runtime[i].next_read_time = 0;
					} while (0);
					break;
				#endif	/* TEMP_THERMISTOR */
#endif //#ifdef TRASH

				#ifdef	TEMP_AD595
				case TT_AD595:
					temp = analog_read(temp_sensors[i].temp_pin);

					// convert
					// >>8 instead of >>10 because internal temp is stored as 14.2 fixed point
					temp = (temp * 500L) >> 8;

					temp_sensors_runtime[i].next_read_time = 0;

					break;
				#endif	/* TEMP_AD595 */

				#ifdef	TEMP_PT100
				case TT_PT100:
					#warning TODO: PT100 code
					break
				#endif	/* TEMP_PT100 */

				#ifdef	TEMP_INTERCOM
				case TT_INTERCOM:
					temp = read_temperature(temp_sensors[i].temp_pin);

					temp_sensors_runtime[i].next_read_time = 25;

					break;
				#endif	/* TEMP_INTERCOM */

				#ifdef	TEMP_NONE
				case TT_NONE:
					temp_sensors_runtime[i].last_read_temp =
					  temp_sensors_runtime[i].target_temp; // for get_temp()
					temp_sensors_runtime[i].next_read_time = 25;

					break;
				#endif	/* TEMP_NONE */

				#ifdef	TEMP_DUMMY
				case TT_DUMMY:
					temp = temp_sensors_runtime[i].last_read_temp;

					if (temp_sensors_runtime[i].target_temp > temp)
						temp++;
					else if (temp_sensors_runtime[i].target_temp < temp)
						temp--;

					temp_sensors_runtime[i].next_read_time = 0;

					break;
				#endif	/* TEMP_DUMMY */

				default: /* prevent compiler warning */
					break;
			}
			temp_sensors_runtime[i].last_read_temp = temp;
		}
		if (labs((int16_t)(temp_sensors_runtime[i].last_read_temp - temp_sensors_runtime[i].target_temp)) < (TEMP_HYSTERESIS*4)) {
			if (temp_sensors_runtime[i].temp_residency < (TEMP_RESIDENCY_TIME*100))
				temp_sensors_runtime[i].temp_residency++;
		}
		else {
			temp_sensors_runtime[i].temp_residency = 0;
		}

		if (temp_sensors[i].heater < NUM_HEATERS) {
			heater_tick(temp_sensors[i].heater, temp_sensors[i].temp_type, temp_sensors_runtime[i].last_read_temp, temp_sensors_runtime[i].target_temp);
		}
	}
}

/// report whether all temp sensors are reading their target temperatures
/// used for M109 and friends
uint8_t	temp_achieved() {
	temp_sensor_t i;
	uint8_t all_ok = 255;

	for (i = 0; i < NUM_TEMP_SENSORS; i++) {
		if (temp_sensors_runtime[i].temp_residency < (TEMP_RESIDENCY_TIME*100))
			all_ok = 0;
	}
	return all_ok;
}

/// specify a target temperature
/// \param index sensor to set a target for
/// \param temperature target temperature to aim for
void temp_set(temp_sensor_t index, uint16_t temperature) {
	if (index >= NUM_TEMP_SENSORS)
		return;

	// only reset residency if temp really changed
	if (temp_sensors_runtime[index].target_temp != temperature) {
		temp_sensors_runtime[index].target_temp = temperature;
		temp_sensors_runtime[index].temp_residency = 0;
	#ifdef	TEMP_INTERCOM
		if (temp_sensors[index].temp_type == TT_INTERCOM)
			send_temperature(temp_sensors[index].temp_pin, temperature);
	#endif
	}
}

/// return most recent reading for a sensor
/// \param index sensor to read
uint16_t temp_get(temp_sensor_t index) {
	if (index >= NUM_TEMP_SENSORS)
		return 0;

	return temp_sensors_runtime[index].last_read_temp;
}

uint8_t temp_all_zero() {
	uint8_t i;
	for (i = 0; i < NUM_TEMP_SENSORS; i++) {
		if (temp_sensors[i].heater < NUM_HEATERS) {
			if (temp_sensors_runtime[i].target_temp)
				return 0;
		}
	}
	return 255;
}

// extruder doesn't have sersendf_P
#ifndef	EXTRUDER
/// send temperatures to host
/// \param index sensor value to send
void temp_print(temp_sensor_t index) {
	uint8_t c = 0;

	if (index >= NUM_TEMP_SENSORS)
		return;

	temp_sensors_runtime[index].last_read_temp = 1416-(uint32_t)(1116*ADC_results[0]/ADC_results[1]);
	c = temp_sensors_runtime[index].last_read_temp & 3;

	#if REPRAP_HOST_COMPATIBILITY >= 20110509
		sersendf_P(PSTR("T:%u.%u"), temp_sensors_runtime[index].last_read_temp >> 2, c);
	#else
		printf("\nT:%u.%u0",  temp_sensors_runtime[index].last_read_temp >> 2, c);
	#endif
	#ifdef HEATER_BED
		uint8_t b = 0;
		temp_sensors_runtime[HEATER_BED].last_read_temp = 1416-(uint32_t)(1116*ADC_results[1]/ADC_results[1]);
		b = temp_sensors_runtime[HEATER_BED].last_read_temp & 3;

		printf(" B:%u.%u", temp_sensors_runtime[HEATER_BED].last_read_temp >> 2 , b);
	#endif

}
#endif
