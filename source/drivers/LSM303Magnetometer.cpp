/*
The MIT License (MIT)

Copyright (c) 2017 Lancaster University.

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.
*/

/**
 * Class definition for a LSM303 3 axis magnetometer.
 *
 * Represents an implementation of the ST LSM303 3 axis magnetometer
 */
#include "CodalConfig.h"
#include "CodalComponent.h"
#include "CodalDmesg.h"
#include "CodalUtil.h"
#include "CoordinateSystem.h"
#include "LSM303Magnetometer.h"
#include "Compass.h"
#include "ErrorNo.h"
#include "Event.h"

using namespace codal;

//
// Configuration table for available data update frequency.
// maps microsecond period -> LSM303_CFG_REG_A_M data rate selection bits [2..3]
//
static const KeyValueTableEntry magnetometerPeriodData[] = {
    {10000, 0x0C},             // 100 Hz
    {20000, 0x08},             // 50 Hz
    {50000, 0x04},             // 20 Hz
    {100000, 0x00}             // 10 Hz
};
CREATE_KEY_VALUE_TABLE(magnetometerPeriod, magnetometerPeriodData);


/**
 * Configures the compass for the sample rate defined in this object. 
 * The nearest values are chosen to those defined
 * that are supported by the hardware. The instance variables are then
 * updated to reflect reality.
 *
 * @return MICROBIT_OK on success, MICROBIT_I2C_ERROR if the compass could not be configured.
 */
int LSM303Magnetometer::configure()
{
    int result;

    // First find the nearest sample rate to that specified.
    samplePeriod = magnetometerPeriod.getKey(samplePeriod * 1000) / 1000;

    // Now configure the magnetometer for the requested sample rate, low power continuous mode with temperature compensation disabled
    // TODO: Review if temperature compensation improves performance.
    result = i2c.writeRegister(address, LSM303_CFG_REG_A_M, magnetometerPeriod.get(samplePeriod * 1000));
    if (result != DEVICE_OK)
    {
        DMESG("LSM303 INIT: ERROR WRITING LSM303_CFG_REG_A_M");
        return DEVICE_I2C_ERROR;
    }

    // Enable Data Ready interrupt, with buffering of data to avoid race conditions.
    result = i2c.writeRegister(address, LSM303_CFG_REG_C_M, 0x01);
    if (result != DEVICE_OK)
    {
        DMESG("LSM303 INIT: ERROR WRITING LSM303_CFG_REG_C_M");
        return DEVICE_I2C_ERROR;
    }

    return DEVICE_OK;
}

/**
  * Constructor.
  * Create a software abstraction of an FXSO8700 combined magnetometer/magnetometer
  *
  * @param _i2c an instance of I2C used to communicate with the device.
  *
  * @param address the default I2C address of the magnetometer. Defaults to: FXS8700_DEFAULT_ADDR.
  *
 */
LSM303Magnetometer::LSM303Magnetometer(I2C &_i2c, Pin &_int1, bool activeHi, CoordinateSpace &coordinateSpace, uint16_t address, uint16_t id) : Compass(coordinateSpace, id), i2c(_i2c), int1(_int1), irqLevel(activeHi)
{
    // Store our identifiers.
    this->address = address;

    // Configure and enable the magnetometer.
    configure();
}


/**
 * Poll to see if new data is available from the hardware. If so, update it.
 * n.b. it is not necessary to explicitly call this funciton to update data
 * (it normally happens in the background when the scheduler is idle), but a check is performed
 * if the user explicitly requests up to date data.
 *
 * @return MICROBIT_OK on success, MICROBIT_I2C_ERROR if the update fails.
 *
 * @note This method should be overidden by the hardware driver to implement the requested
 * changes in hardware.
 */
int LSM303Magnetometer::requestUpdate()
{
    // Ensure we're scheduled to update the data periodically
    status |= DEVICE_COMPONENT_STATUS_IDLE_TICK;

    // Poll interrupt line from device (ACTIVE LO)
    if(int1.getDigitalValue() == irqLevel)
    {
        uint8_t data[6];
        int result;
        int16_t *x;
        int16_t *y;
        int16_t *z;

#if CONFIG_ENABLED(DEVICE_I2C_IRQ_SHARED)
        // Determine if this device has all its data ready (we may be on a shared IRQ line)
        uint8_t status_reg = i2c.readRegister(address, LSM303_STATUS_REG_M);
        if((status_reg & LSM303_M_STATUS_DATA_READY) != LSM303_M_STATUS_DATA_READY)
            return DEVICE_OK;
#endif

        // Read the combined accelerometer and magnetometer data.
        result = i2c.readRegister(address, LSM303_OUTX_L_REG_M | 0x80, data, 6);

        if (result !=0)
            return DEVICE_I2C_ERROR;

        // Read in each reading as a 16 bit little endian value, and scale to 10 bits.
        x = ((int16_t *) &data[0]);
        y = ((int16_t *) &data[2]);
        z = ((int16_t *) &data[4]);

        // Align to ENU coordinate system
        Sample3D s;
        s.x = LSM303_M_NORMALIZE_SAMPLE(-((int)(*y)));
        s.y = LSM303_M_NORMALIZE_SAMPLE(-((int)(*x)));
        s.z = LSM303_M_NORMALIZE_SAMPLE(((int)(*z)));

        // indicate that new data is available.
        update(s);
    }

    return DEVICE_OK;
}


/**
  * A periodic callback invoked by the fiber scheduler idle thread.
  *
  * Internally calls updateSample().
  */
void LSM303Magnetometer::idleCallback()
{
    requestUpdate();
}

/**
 * Attempts to read the 8 bit WHO_AM_I value from the accelerometer
 *
 * @return true if the WHO_AM_I value is succesfully read. false otherwise.
 */
int LSM303Magnetometer::isDetected(I2C &i2c, uint16_t address)
{
    return i2c.readRegister(address, LSM303_WHO_AM_I_M) == LSM303_M_WHOAMI_VAL;
}

/**
  * Destructor for FXS8700, where we deregister from the array of fiber components.
  */
LSM303Magnetometer::~LSM303Magnetometer()
{
}
