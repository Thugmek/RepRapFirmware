#ifndef HX711_MULTI_h
#define HX711_MULTI_h

#include "RepRapFirmware.h"

class HX711MULTI
{
	private:
		size_t PD_SCK;	// Power Down and Serial Clock Input Pin
		size_t COUNT;		// The number of channels to read
		size_t *DOUT;		// Serial Data Output Pin
		size_t GAIN;		// amplification factor

		bool debugEnabled; //print debug messages?

		long *OFFSETS;	// used for tare weight
		float SCALE;	// used to return weight in grams, kg, ounces, whatever

	public:
		// define clock and data pin, channel, and gain factor
		// channel selection is made by passing the appropriate gain: 128 or 64 for channel A, 32 for channel B
		// count: the number of channels
		// dout: an array of pin numbers, of length 'count', one entry per channel
		HX711MULTI();

		void Init(int count, size_t *dout, size_t pd_sck, size_t gain = 128);

		//returns the number of channels
		size_t get_count();

		// check if HX711 is ready
		// from the datasheet: When output data is not ready for retrieval, digital output pin DOUT is high. Serial clock
		// input PD_SCK should be low. When DOUT goes to low, it indicates data is ready for retrieval.
		bool is_ready();

		// set the gain factor; takes effect only after a call to read()
		// channel A can be set for a 128 or 64 gain; channel B has a fixed 32 gain
		// depending on the parameter, the channel is also set to either A or B
		void set_gain(size_t gain = 128);

		// waits for the chip to be ready and returns a reading
		void read(long *result = NULL);

		// same as read, but does not offset the values according to the tare
		void readRaw(long *result = NULL);

		// set the OFFSET value for tare weight
		// times: how many times to read the tare value
		// returns true iff the offsets have been reset for the scale during this call.
		// tolerance: the maximum deviation of samples, above which to reject the attempt to tare. (if set to 0, ignored)
		bool tare(size_t times = 10, uint16_t tolerance = 0);

		// puts the chip into power down mode
		void power_down();

		// wakes up the chip after power down mode
		void power_up();

		void setDebugEnable(bool debugEnable = true);
};

#endif /* HX711_MULTI_h */
