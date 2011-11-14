/**
 ******************************************************************************
 * @addtogroup OpenPilotModules OpenPilot Modules
 * @{
 * @addtogroup Attitude Copter Control Attitude Estimation
 * @brief Acquires sensor data and computes attitude estimate
 * Specifically updates the the @ref AttitudeActual "AttitudeActual" and @ref AttitudeRaw "AttitudeRaw" settings objects
 * @{
 *
 * @file       attitude.c
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2010.
 * @brief      Module to handle all comms to the AHRS on a periodic basis.
 *
 * @see        The GNU Public License (GPL) Version 3
 *
 ******************************************************************************/
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

/**
 * Input objects: None, takes sensor data via pios
 * Output objects: @ref AttitudeRaw @ref AttitudeActual
 *
 * This module computes an attitude estimate from the sensor data
 *
 * The module executes in its own thread.
 *
 * UAVObjects are automatically generated by the UAVObjectGenerator from
 * the object definition XML file.
 *
 * Modules have no API, all communication to other modules is done through UAVObjects.
 * However modules may use the API exposed by shared libraries.
 * See the OpenPilot wiki for more details.
 * http://www.openpilot.org/OpenPilot_Application_Architecture
 *
 */

#include "pios.h"
#include "attitude.h"
#include "attituderaw.h"
#include "attitudeactual.h"
#include "attitudesettings.h"
#include "baroaltitude.h"
#include "flightstatus.h"
#include "CoordinateConversions.h"

// Private constants
#define STACK_SIZE_BYTES 540
#define TASK_PRIORITY (tskIDLE_PRIORITY+3)

#define PI_MOD(x) (fmod(x + M_PI, M_PI * 2) - M_PI)
// Private types

// Private variables
static xTaskHandle taskHandle;

// Private functions
static void AttitudeTask(void *parameters);

static float gyro_correct_int[3] = {0,0,0};

static int8_t updateSensors(AttitudeRawData *);
static void updateAttitude(AttitudeRawData *);
static void settingsUpdatedCb(UAVObjEvent * objEv);

static float accelKi = 0;
static float accelKp = 0;
static float yawBiasRate = 0;
static float gyroGain = 0.42;
static int16_t accelbias[3];
static float q[4] = {1,0,0,0};
static float R[3][3];
static int8_t rotate = 0;
static bool zero_during_arming = false;
static bool bias_correct_gyro = true;

/**
 * Initialise the module, called on startup
 * \returns 0 on success or -1 if initialisation failed
 */
int32_t AttitudeStart(void)
{

	// Start main task
	xTaskCreate(AttitudeTask, (signed char *)"Attitude", STACK_SIZE_BYTES/4, NULL, TASK_PRIORITY, &taskHandle);
	TaskMonitorAdd(TASKINFO_RUNNING_ATTITUDE, taskHandle);
	PIOS_WDG_RegisterFlag(PIOS_WDG_ATTITUDE);

	return 0;
}

/**
 * Initialise the module, called on startup
 * \returns 0 on success or -1 if initialisation failed
 */
int32_t AttitudeInitialize(void)
{
	AttitudeActualInitialize();
	AttitudeRawInitialize();
	AttitudeSettingsInitialize();
	BaroAltitudeInitialize();
	
	// Initialize quaternion
	AttitudeActualData attitude;
	AttitudeActualGet(&attitude);
	attitude.q1 = 1;
	attitude.q2 = 0;
	attitude.q3 = 0;
	attitude.q4 = 0;
	AttitudeActualSet(&attitude);

	// Cannot trust the values to init right above if BL runs
	gyro_correct_int[0] = 0;
	gyro_correct_int[1] = 0;
	gyro_correct_int[2] = 0;

	q[0] = 1;
	q[1] = 0;
	q[2] = 0;
	q[3] = 0;
	for(uint8_t i = 0; i < 3; i++)
		for(uint8_t j = 0; j < 3; j++)
			R[i][j] = 0;

	AttitudeSettingsConnectCallback(&settingsUpdatedCb);

	return 0;
}

MODULE_INITCALL(AttitudeInitialize, AttitudeStart)

int32_t accel_test;
int32_t gyro_test;
int32_t mag_test;
int32_t pressure_test;

/**
 * Module thread, should not return.
 */
static void AttitudeTask(void *parameters)
{
	uint8_t init = 0;
	AlarmsClear(SYSTEMALARMS_ALARM_ATTITUDE);

	// Force settings update to make sure rotation loaded
	settingsUpdatedCb(AttitudeSettingsHandle());

	accel_test = PIOS_BMA180_Test();
	gyro_test = PIOS_MPU6050_Test();
	mag_test = PIOS_HMC5883_Test();
	pressure_test = PIOS_BMP085_Test();

	// Kick of pressure conversions
	PIOS_BMP085_StartADC(TemperatureConv);
	
	// Main task loop
	while (1) {
	
		FlightStatusData flightStatus;
		FlightStatusGet(&flightStatus);

		if((xTaskGetTickCount() < 7000) && (xTaskGetTickCount() > 1000)) {
			// For first 7 seconds use accels to get gyro bias
			accelKp = 1;
			accelKi = 0.9;
			yawBiasRate = 0.23;
			init = 0;
		}
		else if (zero_during_arming && (flightStatus.Armed == FLIGHTSTATUS_ARMED_ARMING)) {
			accelKp = 1;
			accelKi = 0.9;
			yawBiasRate = 0.23;
			init = 0;
		} else if (init == 0) {
			// Reload settings (all the rates)
			AttitudeSettingsAccelKiGet(&accelKi);
			AttitudeSettingsAccelKpGet(&accelKp);
			AttitudeSettingsYawBiasRateGet(&yawBiasRate);
			init = 1;
		}

		PIOS_WDG_UpdateFlag(PIOS_WDG_ATTITUDE);

		AttitudeRawData attitudeRaw;
		AttitudeRawGet(&attitudeRaw);
		if(updateSensors(&attitudeRaw) != 0)
			AlarmsSet(SYSTEMALARMS_ALARM_ATTITUDE, SYSTEMALARMS_ALARM_ERROR);
		else {
			// Only update attitude when sensor data is good
			//updateAttitude(&attitudeRaw);
			AttitudeRawSet(&attitudeRaw);
			AlarmsClear(SYSTEMALARMS_ALARM_ATTITUDE);
		}
		
		vTaskDelay(1);
	}
}


uint32_t accel_samples;
uint32_t gyro_samples;
struct pios_bma180_data accel;
struct pios_mpu6050_data gyro;
AttitudeRawData raw;
int32_t accel_accum[3] = {0, 0, 0};
int32_t gyro_accum[3] = {0,0,0};
float scaling;

/**
 * Get an update from the sensors
 * @param[in] attitudeRaw Populate the UAVO instead of saving right here
 * @return 0 if successfull, -1 if not
 */
static int8_t updateSensors(AttitudeRawData * attitudeRaw)
{
	int32_t read_good;
	int32_t count;
	
	for (int i = 0; i < 3; i++) {
		accel_accum[i] = 0;
		gyro_accum[i] = 0;
	}
	accel_samples = 0;
	gyro_samples = 0;
	
	// Make sure we get one sample
	count = 0;
	while((read_good = PIOS_BMA180_ReadFifo(&accel)) != 0);
	while(read_good == 0) {	
		count++;
		
		accel_accum[0] += accel.x;
		accel_accum[1] += accel.y;
		accel_accum[2] += accel.z;
		
		read_good = PIOS_BMA180_ReadFifo(&accel);
	}
	accel_samples = count;

	// Not the swaping of channel orders
	scaling = PIOS_BMA180_GetScale() / accel_samples;
	attitudeRaw->accels[ATTITUDERAW_ACCELS_X] = accel_accum[0] * scaling;
	attitudeRaw->accels[ATTITUDERAW_ACCELS_Y] = accel_accum[1] * scaling;
	attitudeRaw->accels[ATTITUDERAW_ACCELS_Z] = accel_accum[2] * scaling;


	
	// Make sure we get one sample
	count = 0;
	while((read_good = PIOS_MPU6050_ReadFifo(&gyro)) != 0);
	while(read_good == 0) {
		count++;
		
		gyro_accum[0] += gyro.gyro_x;
		gyro_accum[1] += gyro.gyro_y;
		gyro_accum[2] += gyro.gyro_z;
		
		read_good = PIOS_MPU6050_ReadFifo(&gyro);
	}
	gyro_samples = count;	
	
	scaling = PIOS_MPU6050_GetScale() / gyro_samples;
	attitudeRaw->gyros[ATTITUDERAW_GYROS_X] = -((float) gyro_accum[1]) * scaling;
	attitudeRaw->gyros[ATTITUDERAW_GYROS_Y] = -((float) gyro_accum[0]) * scaling;
	attitudeRaw->gyros[ATTITUDERAW_GYROS_Z] = -((float) gyro_accum[2]) * scaling;
	
	// From data sheet 35 deg C corresponds to -13200, and 280 LSB per C
	attitudeRaw->temperature[ATTITUDERAW_TEMPERATURE_GYRO] = gyro.temperature = 35.0f + ((float) gyro.temperature + 13200) / 280;
	
	// From the data sheet 25 deg C corresponds to 2 and 2 LSB per C
	attitudeRaw->temperature[ATTITUDERAW_TEMPERATURE_ACCEL] = 25.0f + ((float) accel.temperature - 2) / 2;
	
	if(bias_correct_gyro) {
		// Applying integral component here so it can be seen on the gyros and correct bias
		attitudeRaw->gyros[ATTITUDERAW_GYROS_X] += gyro_correct_int[0];
		attitudeRaw->gyros[ATTITUDERAW_GYROS_Y] += gyro_correct_int[1];
		attitudeRaw->gyros[ATTITUDERAW_GYROS_Z] += gyro_correct_int[2];
	}

	// Because most crafts wont get enough information from gravity to zero yaw gyro, we try
	// and make it average zero (weakly)
	gyro_correct_int[2] += - attitudeRaw->gyros[ATTITUDERAW_GYROS_Z] * yawBiasRate;


	if (PIOS_HMC5883_NewDataAvailable()) {
		int16_t values[3];
		PIOS_HMC5883_ReadMag(values);
		attitudeRaw->magnetometers[ATTITUDERAW_MAGNETOMETERS_X] = -values[0];
		attitudeRaw->magnetometers[ATTITUDERAW_MAGNETOMETERS_Y] = -values[1];
		attitudeRaw->magnetometers[ATTITUDERAW_MAGNETOMETERS_Z] = -values[2];
	}

	AttitudeRawSet(&raw);
	
	int32_t retval = PIOS_BMP085_ReadADC();
	if (retval == 0) { // Conversion completed 
		static uint32_t baro_conversions;
		
		if((baro_conversions++) % 2)
			PIOS_BMP085_StartADC(PressureConv);
		else {
			PIOS_BMP085_StartADC(TemperatureConv);

			float pressure;
			
			pressure = PIOS_BMP085_GetPressure();
			
			BaroAltitudeData data;
			BaroAltitudeGet(&data);
			data.Altitude = (1.0f - powf(pressure / BMP085_P0, (1.0f / 5.255f))) * 44330.0f;
			data.Pressure = pressure  / 1000.0f;
			data.Temperature = PIOS_BMP085_GetTemperature() / 10.0f;  // Convert to deg C
			BaroAltitudeSet(&data);

		}
	}
	
	return 0;
}

static void updateAttitude(AttitudeRawData * attitudeRaw)
{
	float dT;
	portTickType thisSysTime = xTaskGetTickCount();
	static portTickType lastSysTime = 0;

	dT = (thisSysTime == lastSysTime) ? 0.001 : (portMAX_DELAY & (thisSysTime - lastSysTime)) / portTICK_RATE_MS / 1000.0f;
	lastSysTime = thisSysTime;

	// Bad practice to assume structure order, but saves memory
	float gyro[3];
	gyro[0] = attitudeRaw->gyros[0];
	gyro[1] = attitudeRaw->gyros[1];
	gyro[2] = attitudeRaw->gyros[2];

	{
		float * accels = attitudeRaw->accels;
		float grot[3];
		float accel_err[3];

		// Rotate gravity to body frame and cross with accels
		grot[0] = -(2 * (q[1] * q[3] - q[0] * q[2]));
		grot[1] = -(2 * (q[2] * q[3] + q[0] * q[1]));
		grot[2] = -(q[0] * q[0] - q[1]*q[1] - q[2]*q[2] + q[3]*q[3]);
		CrossProduct((const float *) accels, (const float *) grot, accel_err);

		// Account for accel magnitude
		float accel_mag = sqrt(accels[0]*accels[0] + accels[1]*accels[1] + accels[2]*accels[2]);
		accel_err[0] /= accel_mag;
		accel_err[1] /= accel_mag;
		accel_err[2] /= accel_mag;

		// Accumulate integral of error.  Scale here so that units are (deg/s) but Ki has units of s
		gyro_correct_int[0] += accel_err[0] * accelKi;
		gyro_correct_int[1] += accel_err[1] * accelKi;
			
		//gyro_correct_int[2] += accel_err[2] * settings.AccelKI * dT;

		// Correct rates based on error, integral component dealt with in updateSensors
		gyro[0] += accel_err[0] * accelKp / dT;
		gyro[1] += accel_err[1] * accelKp / dT;
		gyro[2] += accel_err[2] * accelKp / dT;
	}

	{ // scoping variables to save memory
		// Work out time derivative from INSAlgo writeup
		// Also accounts for the fact that gyros are in deg/s
		float qdot[4];
		qdot[0] = (-q[1] * gyro[0] - q[2] * gyro[1] - q[3] * gyro[2]) * dT * M_PI / 180 / 2;
		qdot[1] = (q[0] * gyro[0] - q[3] * gyro[1] + q[2] * gyro[2]) * dT * M_PI / 180 / 2;
		qdot[2] = (q[3] * gyro[0] + q[0] * gyro[1] - q[1] * gyro[2]) * dT * M_PI / 180 / 2;
		qdot[3] = (-q[2] * gyro[0] + q[1] * gyro[1] + q[0] * gyro[2]) * dT * M_PI / 180 / 2;

		// Take a time step
		q[0] = q[0] + qdot[0];
		q[1] = q[1] + qdot[1];
		q[2] = q[2] + qdot[2];
		q[3] = q[3] + qdot[3];
		
		if(q[0] < 0) {
			q[0] = -q[0];
			q[1] = -q[1];
			q[2] = -q[2];
			q[3] = -q[3];
		}
	}

	// Renomalize
	float qmag = sqrt(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
	q[0] = q[0] / qmag;
	q[1] = q[1] / qmag;
	q[2] = q[2] / qmag;
	q[3] = q[3] / qmag;

	// If quaternion has become inappropriately short or is nan reinit.
	// THIS SHOULD NEVER ACTUALLY HAPPEN
	if((fabs(qmag) < 1e-3) || (qmag != qmag)) {
		q[0] = 1;
		q[1] = 0;
		q[2] = 0;
		q[3] = 0;
	}

	AttitudeActualData attitudeActual;
	AttitudeActualGet(&attitudeActual);

	quat_copy(q, &attitudeActual.q1);

	// Convert into eueler degrees (makes assumptions about RPY order)
	Quaternion2RPY(&attitudeActual.q1,&attitudeActual.Roll);

	AttitudeActualSet(&attitudeActual);
}

static void settingsUpdatedCb(UAVObjEvent * objEv) {
	AttitudeSettingsData attitudeSettings;
	AttitudeSettingsGet(&attitudeSettings);


	accelKp = attitudeSettings.AccelKp;
	accelKi = attitudeSettings.AccelKi;
	yawBiasRate = attitudeSettings.YawBiasRate;
	gyroGain = attitudeSettings.GyroGain;

	zero_during_arming = attitudeSettings.ZeroDuringArming == ATTITUDESETTINGS_ZERODURINGARMING_TRUE;
	bias_correct_gyro = attitudeSettings.BiasCorrectGyro == ATTITUDESETTINGS_BIASCORRECTGYRO_TRUE;

	accelbias[0] = attitudeSettings.AccelBias[ATTITUDESETTINGS_ACCELBIAS_X];
	accelbias[1] = attitudeSettings.AccelBias[ATTITUDESETTINGS_ACCELBIAS_Y];
	accelbias[2] = attitudeSettings.AccelBias[ATTITUDESETTINGS_ACCELBIAS_Z];

	gyro_correct_int[0] = attitudeSettings.GyroBias[ATTITUDESETTINGS_GYROBIAS_X] / 100.0f;
	gyro_correct_int[1] = attitudeSettings.GyroBias[ATTITUDESETTINGS_GYROBIAS_Y] / 100.0f;
	gyro_correct_int[2] = attitudeSettings.GyroBias[ATTITUDESETTINGS_GYROBIAS_Z] / 100.0f;

	// Indicates not to expend cycles on rotation
	if(attitudeSettings.BoardRotation[0] == 0 && attitudeSettings.BoardRotation[1] == 0 &&
	   attitudeSettings.BoardRotation[2] == 0) {
		rotate = 0;

		// Shouldn't be used but to be safe
		float rotationQuat[4] = {1,0,0,0};
		Quaternion2R(rotationQuat, R);
	} else {
		float rotationQuat[4];
		const float rpy[3] = {attitudeSettings.BoardRotation[ATTITUDESETTINGS_BOARDROTATION_ROLL],
			attitudeSettings.BoardRotation[ATTITUDESETTINGS_BOARDROTATION_PITCH],
			attitudeSettings.BoardRotation[ATTITUDESETTINGS_BOARDROTATION_YAW]};
		RPY2Quaternion(rpy, rotationQuat);
		Quaternion2R(rotationQuat, R);
		rotate = 1;
	}
}
/**
  * @}
  * @}
  */
