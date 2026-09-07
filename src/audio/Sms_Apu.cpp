// Sms_Snd_Emu 0.1.4. http://www.slack.net/~ant/

#include "Sms_Apu.h"

#include "SaveStateStream.h"

/* Copyright (C) 2003-2006 Shay Green. This module is free software; you
can redistribute it and/or modify it under the terms of the GNU Lesser
General Public License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version. This
module is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for more
details. You should have received a copy of the GNU Lesser General Public
License along with this module; if not, write to the Free Software Foundation,
Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA */

#include "blargg_source.h"

// Sms_Osc

Sms_Osc::Sms_Osc()
{
	output = 0;
	outputs [0] = 0; // always stays NULL
	outputs [1] = 0;
	outputs [2] = 0;
	outputs [3] = 0;

	enabled = true;
	delay = 0;
	last_amp = 0;
	volume = 0;
	output_select = 3;
	output = outputs[3];

	debug_buf = 0;
	debug_last_amp = 0;
}

void Sms_Osc::reset()
{
	delay = 0;
	last_amp = 0;
	volume = 0;
	output_select = 3;
	output = outputs [3];

	debug_last_amp = 0;
}

// Sms_Square

// Tone periods at or below this collapse to a steady half-amplitude level
// instead of being synthesized as a square wave. `period` here is the 10-bit
// register value times 16, so the frequency is clock / (2 * period).
//
// The upstream value was 128 (register 8, 13984 Hz at 3.58 MHz) with a comment
// claiming "16kHz and higher" - the number never matched the comment, and both
// were too low. On real hardware the tone is still audible well above that:
// register 6 (18643 Hz) is the highest note that produces anything, measured
// by ear on a real SC-3000 through its A/V output, and independently reported
// by SMS Power from the same test on an SMS2. Cutting at 128 silenced
// registers 8, 7 and 6 - roughly 14 to 18.6 kHz, where bright effects live.
//
// 80 is register 5, i.e. 22375 Hz. The cut therefore sits just above the
// limit of human hearing rather than inside it: everything a listener can
// actually hear is synthesized, while genuinely ultrasonic periods stay
// flattened. That guard is worth keeping - above 22 kHz a square wave can
// only fold back as aliasing at a 48 kHz output rate, and it also bounds the
// worst-case edge rate. Registers 0 and 1 stay flat as well, which is what
// makes the PSG-as-DAC sample playback trick behave like the real chip.
//
// See DOCS/SN76489_HIFI_REVIEW.md for the measurements and the sources.
static int const inaudible_tone_period = 80;

inline void Sms_Square::reset()
{
	period = 0;
	phase = 0;
	Sms_Osc::reset();
}

void Sms_Square::run( blip_time_t time, blip_time_t end_time )
{
	int amp = (!enabled) ? 0 : volume;

    if ( period > inaudible_tone_period )
        amp = amp << 1 & -phase;

    {
        int delta = amp - last_amp;
        if ( delta )
        {
            last_amp = amp;
            synth->offset( time, delta, output );
        }
        // Debug waveform tap: fed alongside the real mix from its own
        // last-amplitude tracker, so a panel plotting one channel never
        // perturbs what the player actually hears.
        if ( debug_buf )
        {
            int dbg_delta = amp - debug_last_amp;
            if ( dbg_delta )
            {
                debug_last_amp = amp;
                synth->offset( time, dbg_delta, debug_buf );
            }
        }
    }

    time += delay;
    delay = 0;
    if ( period )
    {
        if ( time < end_time )
        {
            if (!enabled || !volume || period <= inaudible_tone_period )
            {
                // keep calculating phase
                int count = (end_time - time + period - 1) / period;
                phase = (phase + count) & 1;
                time += count * period;
            }
            else
            {
                Blip_Buffer* const output_ = this->output;
                Blip_Buffer* const dbg_buf = this->debug_buf;
                int delta = amp * 2 - volume * 2;
                do
                {
                    delta = -delta;
                    synth->offset_inline( time, delta, output_ );
                    if ( dbg_buf )
                        synth->offset_inline( time, delta, dbg_buf );
                    time += period;
                }
                while ( time < end_time );

                last_amp = (delta >> 1) + volume;
                debug_last_amp = last_amp;
                phase = (delta >= 0);
            }
        }
        delay = time - end_time;
    }
}

// Sms_Noise

static int const noise_periods [3] = { 0x100, 0x200, 0x400 };

inline void Sms_Noise::reset()
{
	period = &noise_periods [0];
	shifter = 0x8000;
	feedback = 0x9000;
	Sms_Osc::reset();
}

void Sms_Noise::run( blip_time_t time, blip_time_t end_time )
{
	int amp = ((shifter & 1) || (!enabled)) ? 0 : volume;

	{
		int delta = amp - last_amp;
		if ( delta )
		{
			last_amp = amp;
			synth.offset( time, delta, output );
		}
		if ( debug_buf )
		{
			int dbg_delta = amp - debug_last_amp;
			if ( dbg_delta )
			{
				debug_last_amp = amp;
				synth.offset( time, dbg_delta, debug_buf );
			}
		}
	}

	time += delay;
	if ( !volume )
		time = end_time;

	if ( time < end_time )
	{
		Blip_Buffer* const output_ = this->output;
		Blip_Buffer* const dbg_buf = this->debug_buf;
		unsigned shifter_ = this->shifter;
		int delta = (shifter_ & 1) ? -volume : volume;
		int period_ = *this->period * 2;
		if ( !period_ )
			period_ = 16;

		do
		{
			int changed = shifter_ + 1;
			shifter_ = (feedback & -(shifter_ & 1)) ^ (shifter_ >> 1);
			if ( changed & 2 ) // true if bits 0 and 1 differ
			{
				amp = (shifter_ & 1) ? 0 : volume;
				delta = -delta;
				synth.offset_inline( time, delta, output_ );
				if ( dbg_buf )
					synth.offset_inline( time, delta, dbg_buf );
				last_amp = amp;
				debug_last_amp = amp;
			}
			time += period_;
		}
		while ( time < end_time );

		this->shifter = shifter_;
		this->last_amp = (shifter_ & 1) ? 0 : volume; //delta >> 1;
		this->debug_last_amp = this->last_amp;
	}
	delay = time - end_time;
}

// The tape monitor used to live here as a fifth Sms_Osc. It was removed
// (GearSF7000): SR1000Speaker owns that job and does it properly, with
// timestamp safety, event coalescing and measured frequency reporting.
// Nothing ever called Audio::WriteTapeRegister(), so the oscillator was
// unreachable - and its run() loop advanced `time++` one Z80 clock at a
// time (~71,600 iterations per PAL frame) while never updating `shifter_`,
// so the edge test was loop-invariant: had anything ever driven it, it
// would have emitted a synth offset on every single clock. Dead code that
// was one function call away from costing milliseconds per frame.
// Dropping it also takes osc_count from 5 to 4, which is what the chip
// really has; see Audio::Init() for the matching volume recalibration.

// Sms_Apu

// Per-step amplitudes for the SN76489's 4-bit attenuator.
//
// The law is right and unchanged: the datasheet gives the attenuator bits
// weights of 2, 4, 8 and 16 dB, so value n attenuates by 2n dB up to 28 dB at
// n = 14, and n = 15 (all bits) is off rather than -30 dB. amplitude[n] is
// therefore full_scale * 10^(-2n/20).
//
// What changed is the resolution the law is expressed at. The table used to
// run to a full scale of 64, and six bits cannot hold 28 dB of range: the
// error is negligible over the first eight steps but collapses in the tail,
// where the quietest step held 1 instead of 2.55 - off by 8.13 dB. Rendered
// output showed it plainly, stepping -6 dB between the last two levels where
// the chip steps -2 dB. Audibly that shortens decay tails and fade-outs,
// which PSG music leans on constantly.
//
// Full scale 128 was measured against 256, 512, 1024 and 8192. It removes
// essentially all of the error - mean 1.62 -> 0.11 dB, worst 8.13 -> 0.58 dB,
// the quietest step -8.13 -> -0.17 dB - and it is the largest scale that
// costs nothing. Beyond it, Blip_Synth_::volume_unit() has to right-shift
// kernel_unit to cope with the smaller volume unit, which loses impulse
// precision and shifts the output level by about -0.7 dB. Those larger
// scales buy tenths of a dB nobody can hear in exchange for a coarser
// synthesis kernel and a recalibration, so 128 is where this stops.
//
// The divisor in Sms_Apu::volume() below carries the same full scale and must
// stay in step with this table. Because it does, Audio.cpp's APU_VOLUME needs
// no change: output level is unchanged, measured at +0.00 dB.
//
// Values are nominal, from the datasheet's attenuation law - not measurements
// of a real SN76489AN, whose resistor ladder has its own tolerances. They are
// certainly closer to it than an 8 dB quantisation error was.
//
// See DOCS/SN76489_HIFI_REVIEW.md section 3.1 for the full measurements.
static unsigned char const volumes[16] = {
	128, 102,  81,  64,  51,  40,  32,  26,
	 20,  16,  13,  10,   8,   6,   5,   0,
};


Sms_Apu::Sms_Apu()
{
	for ( int i = 0; i < 3; i++ )
	{
		squares [i].synth = &square_synth;
		oscs [i] = &squares [i];

		reg_periods[i] = 0x3FF;
		reg_volumes[i] = 15;

	}
	oscs [3] = &noise;

	reg_volumes[3] = 15;
	reg_noise_period = 0;

	chnEnable[0] = true;
	chnEnable[1] = true;
	chnEnable[2] = true;
	chnEnable[3] = true;

	mute[0] = false;
	mute[1] = false;
	mute[2] = false;
	mute[3] = false;

	debug_enabled = false;

	volume( 1.0 );
	reset();
}

Sms_Apu::~Sms_Apu()
{
}

void Sms_Apu::GetRegs(int* periods, unsigned char* volumes)
{
	periods[0] = reg_periods[0];
	periods[1] = reg_periods[1];
	periods[2] = reg_periods[2];
	periods[3] = reg_noise_parm; // reg_noise_period != 0 ? *reg_noise_period : 0;
	periods[4] = reg_maxperiodVal;

	volumes[0] = reg_volumes[0];
	volumes[1] = reg_volumes[1];
	volumes[2] = reg_volumes[2];
	volumes[3] = reg_volumes[3];
}

void Sms_Apu::volume( double vol )
{
	// The 128 is the amplitude table's full scale; keep the two in step.
	vol *= 0.85 / (osc_count * 128 * 2);
	square_synth.volume( vol );
	noise.synth.volume( vol );
}

void Sms_Apu::treble_eq( const blip_eq_t& eq )
{
	square_synth.treble_eq( eq );
	noise.synth.treble_eq( eq );
}

void Sms_Apu::osc_output( int index, Blip_Buffer* center, Blip_Buffer* left, Blip_Buffer* right )
{
	require( (unsigned) index < osc_count );
	require( (center && left && right) || (!center && !left && !right) );
	Sms_Osc& osc = *oscs [index];
	osc.outputs [1] = right;
	osc.outputs [2] = left;
	osc.outputs [3] = center;
	osc.output = osc.outputs [osc.output_select];
}

void Sms_Apu::output( Blip_Buffer* center, Blip_Buffer* left, Blip_Buffer* right )
{
	for ( int i = 0; i < osc_count; i++ )
		osc_output( i, center, left, right );
}

void Sms_Apu::reset( unsigned feedback, int noise_width )
{
	last_time = 0;
	latch = 0;
	last_write = {};
	ggstereo_save = 0xFF;

	// Keep the PSG register mirror and runtime state in the same known,
	// silent state. This matters after a machine reset and before the first
	// register write, independently of the platform allocator.
	for ( int i = 0; i < 3; ++i )
	{
		reg_periods[i] = 0x3FF;
		reg_volumes[i] = 15;
		chnEnable[i] = true;
		mute[i] = false;
	}
	reg_volumes[3] = 15;
	reg_noise_parm = 0;
	reg_noise_period = &noise_periods[0];
	chnEnable[3] = true;
	mute[3] = false;
	reg_maxperiodVal = 0;
	
	if ( !feedback || !noise_width )
	{
		feedback = 0x0003;
		noise_width = 15;
	}
	// convert to "Galios configuration"
	looped_feedback = 1 << (noise_width - 1);
	noise_feedback  = 0;
	while ( noise_width-- )
	{
		noise_feedback = (noise_feedback << 1) | (feedback & 1);
		feedback >>= 1;
	}
	
	squares [0].reset();
	squares [1].reset();
	squares [2].reset();
	noise.reset();
}


void Sms_Apu::EnableChannel(int chn, bool onoff)
{
	switch (chn)
	{
	case 0:
		chnEnable[0] = onoff;

		if (!onoff)
		{
			oscs[0]->volume = 0;
			reg_volumes[0] = 15;
		}
		break;

	case 1:
		chnEnable[1] = onoff;
		if (!onoff)
		{
			oscs[1]->volume = 0;
			reg_volumes[1] = 15;
		}
		break;

	case 2:
		chnEnable[2] = onoff;
		if (!onoff)
		{
			oscs[2]->volume = 0;
			reg_volumes[2] = 15;
		}
		break;

	case 3:
		chnEnable[3] = onoff;

		if (!onoff)
		{
			oscs[3]->volume = 0;
			reg_volumes[3] = 15;
		}

		break;
	}


}

void Sms_Apu::MuteChannel(int chn, bool onoff)
{
	if (chn >= 0 && chn < 4)
		mute[chn] = onoff;
}

bool Sms_Apu::IsChannelMuted(int chn) const
{
	return (chn >= 0 && chn < 4) ? mute[chn] : false;
}

bool Sms_Apu::IsChannelEnabled(int chn) const
{
	return (chn >= 0 && chn < 4) ? chnEnable[chn] : false;
}

void Sms_Apu::init_debug_buffers(int sample_rate, long clock_rate)
{
	debug_enabled = true;

	for (int i = 0; i < osc_count; i++)
	{
		debug_bufs[i].set_sample_rate(sample_rate);
		debug_bufs[i].clock_rate(clock_rate);
		debug_bufs[i].clear();
		oscs[i]->debug_buf = &debug_bufs[i];
	}
}

void Sms_Apu::disable_debug_buffers()
{
	debug_enabled = false;
	for (int i = 0; i < osc_count; i++)
		oscs[i]->debug_buf = 0;
}

long Sms_Apu::read_debug_samples(int channel, blip_sample_t* out, long max_samples)
{
	if (!debug_enabled || channel < 0 || channel >= osc_count)
		return 0;

	long avail = debug_bufs[channel].samples_avail();
	if (avail > max_samples)
		avail = max_samples;
	if (avail > 0)
		debug_bufs[channel].read_samples(out, avail);
	return avail;
}


void Sms_Apu::run_until(blip_time_t end_time)
{
	require(end_time >= last_time);

	if (end_time > last_time)
	{
		for (int i = 0; i < osc_count; ++i)
		{
			Sms_Osc& osc = *oscs[i];
			if (osc.output)
			{
				// chnEnable (disable) and mute reach this same silencing path for
				// a different reason each: disable already dropped this
				// channel's register writes in write_data, so its state is
				// frozen and skipping run() here just stops it from
				// re-deriving sound from stale values. mute never touched
				// write_data - the channel's period/volume keep tracking the
				// game normally - it only reaches here because the audible
				// half of "silence this channel" is the same regardless of
				// which reason caused it.
				if (!chnEnable[i] || mute[i])
				{
					// Se il canale è spento ma l'ultima ampiezza non era 0
					if (osc.last_amp != 0)
					{
						// Generiamo un delta negativo per azzerare il segnale nel buffer
						int delta = -osc.last_amp;
						if (i < 3)
							square_synth.offset(last_time, delta, osc.output);
						else if (i == 3)
							noise.synth.offset(last_time, delta, osc.output);

						osc.last_amp = 0;
					}
					continue; // Salta il run() per questo canale
				}

				switch (i)
				{
				case 0: squares[0].run(last_time, end_time); break;
				case 1: squares[1].run(last_time, end_time); break;
				case 2: squares[2].run(last_time, end_time); break;
				case 3: noise.run(last_time, end_time); break;
				}
			}
		}
		last_time = end_time;
	}
}
//void Sms_Apu::run_until( blip_time_t end_time )
//{
//	require( end_time >= last_time ); // end_time must not be before previous time
//	
//	if ( end_time > last_time )
//	{
//		// run oscillators
//		for ( int i = 0; i < osc_count; ++i )
//		{
//			Sms_Osc& osc = *oscs [i];
//			if ( osc.output )
//			{
//				switch (i)
//				{
//					case 0:
//						squares[0].enabled = chnEnable[0];
//						squares[0].run(last_time, end_time);
//						break;
//					case 1:
//						squares[1].enabled = chnEnable[1];
//						squares[1].run(last_time, end_time);
//						break;
//					case 2:
//						squares[2].enabled = chnEnable[2];
//						squares[2].run(last_time, end_time);
//						break;
//					case 3:
//						noise.enabled = chnEnable[3];
//						noise.run(last_time, end_time);
//						break;
//					case 4:
//						tape.run(last_time, end_time);
//						break;
//				}
//
//				//if ( i < 3 )
//				//	squares [i].run( last_time, end_time );
//				//else
//				//	noise.run( last_time, end_time );
//			}
//		}
//		
//		last_time = end_time;
//	}
//}

void Sms_Apu::end_frame( blip_time_t end_time )
{
	if ( end_time > last_time )
		run_until( end_time );

	assert( last_time >= end_time );
	last_time -= end_time;

	if ( debug_enabled )
	{
		for ( int i = 0; i < osc_count; i++ )
			debug_bufs[i].end_frame( end_time );
	}
}

void Sms_Apu::write_ggstereo( blip_time_t time, int data )
{
	require( (unsigned) data <= 0xFF );
	
	ggstereo_save = data;

	run_until( time );
	
	for ( int i = 0; i < osc_count; i++ )
	{
		Sms_Osc& osc = *oscs [i];
		int flags = data >> i;
		Blip_Buffer* old_output = osc.output;
		osc.output_select = (flags >> 3 & 2) | (flags & 1);
		osc.output = osc.outputs [osc.output_select];
		if ( osc.output != old_output && osc.last_amp )
		{
			if ( old_output )
			{
				square_synth.offset( time, -osc.last_amp, old_output );
			}
			osc.last_amp = 0;
		}
	}
}



void Sms_Apu::write_data(blip_time_t time, int data)
{
	//require( (unsigned) data <= 0xFF );

	run_until(time);

	if (data & 0x80)
		latch = data;

	int index = (latch >> 5) & 3;
	const int reg = index * 2 + ((latch & 0x10) ? 1 : 0);
	if (latch & 0x10)
	{
		if (chnEnable[index])
		{
			oscs[index]->volume = volumes[data & 15];
			reg_volumes[index] = data & 15;
		}
		else
		{
			oscs[index]->volume = volumes[0];
		}
	}
	else if (index < 3)
	{
		if (chnEnable[index])
		{
			//Sms_Square& sq = squares [index];
			//if ( data & 0x80 )
			//	sq.period = (sq.period & 0xFF00) | (data << 4 & 0x00FF);
			//else
			//	sq.period = (sq.period & 0x00FF) | (data << 8 & 0x3F00);

			Sms_Square& sq = squares[index];
			if (data & 0x80)
			{
				sq.period = (sq.period & 0xFF00) | (data << 4 & 0x00FF);
			}
			else
			{

				sq.period = (sq.period & 0x00FF) | (data << 8 & 0x3F00);

				int per = (sq.period & 0x00FF) | (data << 8);
				if (per > 0x3FF0)
				{
					reg_maxperiodVal = per;
				}
			}


			reg_periods[index] = sq.period >> 4;
		}
		else
		{
			oscs[index]->volume = volumes[0];
		}
	}
	else
	{
		//int select = data & 3;
		//if ( select < 3 )
		//	noise.period = &noise_periods [select];
		//else
		//	noise.period = &squares [2].period;
		//
		//noise.feedback = (data & 0x04) ? noise_feedback : looped_feedback;
		//noise.shifter = 0x8000;
		if (chnEnable[3])
		{

			reg_noise_parm = data & 7;

			int select = data & 3;
			if (select < 3)
			{
				noise.period = &noise_periods[select];
				reg_noise_period = noise.period;
			}
			else
			{
				noise.period = &squares[2].period;
				reg_noise_period = noise.period;
			}
		}
		else
		{
			oscs[index]->volume = volumes[0];
		}

		noise.feedback = (data & 0x04) ? noise_feedback : looped_feedback;
		noise.shifter = 0x8000;

	}

	// This is a diagnostic mirror only.  It does not participate in the PSG
	// synthesis path, so debug tracing cannot alter timing or audio output.
	last_write.rawData = static_cast<unsigned char>(data);
	last_write.reg = static_cast<unsigned char>(reg);
	if (latch & 0x10)
		last_write.value = reg_volumes[index];
	else if (index < 3)
		last_write.value = static_cast<unsigned short>(reg_periods[index]);
	else
		last_write.value = reg_noise_parm;
}


//void Sms_Apu::write_data(blip_time_t time, int data)
//{
//	require((unsigned)data <= 0xFF);
//
//	run_until(time);
//
//	if (data & 0x80)
//		latch = data;
//
//	int index = (latch >> 5) & 3;
//	if (latch & 0x10)
//	{
//		oscs[index]->volume = volumes[data & 15];
//	}
//	else if (index < 3)
//	{
//		Sms_Square& sq = squares[index];
//		if (data & 0x80)
//			sq.period = (sq.period & 0xFF00) | (data << 4 & 0x00FF);
//		else
//			sq.period = (sq.period & 0x00FF) | (data << 8 & 0x3F00);
//	}
//	else
//	{
//		int select = data & 3;
//		if (select < 3)
//			noise.period = &noise_periods[select];
//		else
//			noise.period = &squares[2].period;
//
//		noise.feedback = (data & 0x04) ? noise_feedback : looped_feedback;
//		noise.shifter = 0x8000;
//	}
//}

// ---------------------------------------------------------------------------
// Save state
// ---------------------------------------------------------------------------

namespace
{
constexpr std::uint16_t kSmsApuStateVersion = 1;

// noise.period and reg_noise_period are raw pointers into either the static
// noise_periods table or square 2's period field, so they are stored as a
// small index. -1 means the pointer is null, which is how reset() leaves
// reg_noise_period.
int NoisePeriodIndex( const int* period, const int* noisePeriods,
                      const int* squareTwoPeriod )
{
	if ( period == 0 )
		return -1;
	if ( period == squareTwoPeriod )
		return 3;
	for ( int i = 0; i < 3; ++i )
	{
		if ( period == &noisePeriods [i] )
			return i;
	}
	return -1;
}

void SaveOsc( StateWriter& w, const Sms_Osc& osc )
{
	w.I32( osc.output_select );
	w.I32( osc.delay );
	w.I32( osc.last_amp );
	w.I32( osc.volume );
	w.Bool( osc.enabled );
}

void LoadOsc( StateReader& r, Sms_Osc& osc )
{
	osc.output_select = r.I32();
	osc.delay = r.I32();
	osc.last_amp = r.I32();
	osc.volume = r.I32();
	osc.enabled = r.Bool();
}
}

void Sms_Apu::SaveState( std::ostream& stream ) const
{
	StateWriter w( stream );

	w.U16( kSmsApuStateVersion );

	for ( int i = 0; i < osc_count; ++i )
		w.Bool( chnEnable [i] );

	for ( int i = 0; i < osc_count; ++i )
		w.U8( reg_volumes [i] );
	for ( int i = 0; i < osc_count - 1; ++i )
		w.I32( reg_periods [i] );
	w.I32( reg_maxperiodVal );
	w.U8( reg_noise_parm );
	w.I32( NoisePeriodIndex( reg_noise_period, noise_periods,
	                         &squares [2].period ) );

	w.I32( last_time );
	w.I32( latch );
	w.U32( noise_feedback );
	w.U32( looped_feedback );
	w.U32( ggstereo_save );

	w.U8( last_write.rawData );
	w.U8( last_write.reg );
	w.U16( last_write.value );

	for ( int i = 0; i < 3; ++i )
	{
		SaveOsc( w, squares [i] );
		w.I32( squares [i].period );
		w.I32( squares [i].phase );
	}

	SaveOsc( w, noise );
	w.I32( NoisePeriodIndex( noise.period, noise_periods,
	                         &squares [2].period ) );
	w.U32( noise.shifter );
	w.U32( noise.feedback );
}

bool Sms_Apu::LoadState( std::istream& stream )
{
	StateReader r( stream );

	if ( r.U16() != kSmsApuStateVersion )
		return false;

	bool enable [osc_count];
	for ( int i = 0; i < osc_count; ++i )
		enable [i] = r.Bool();

	unsigned char volumes [osc_count];
	for ( int i = 0; i < osc_count; ++i )
		volumes [i] = r.U8();
	int periods [osc_count - 1];
	for ( int i = 0; i < osc_count - 1; ++i )
		periods [i] = r.I32();
	const int maxPeriod = r.I32();
	const unsigned char noiseParm = r.U8();
	const int regNoiseIndex = r.I32();

	const int lastTime = r.I32();
	const int latchValue = r.I32();
	const unsigned noiseFeedback = r.U32();
	const unsigned loopedFeedback = r.U32();
	const unsigned ggStereo = r.U32();

	WriteInfo lastWrite;
	lastWrite.rawData = r.U8();
	lastWrite.reg = r.U8();
	lastWrite.value = r.U16();

	if ( !r.Ok() )
		return false;

	for ( int i = 0; i < 3; ++i )
	{
		LoadOsc( r, squares [i] );
		squares [i].period = r.I32();
		squares [i].phase = r.I32();
	}

	LoadOsc( r, noise );
	const int noiseIndex = r.I32();
	noise.shifter = r.U32();
	noise.feedback = r.U32();

	if ( !r.Ok() )
		return false;

	for ( int i = 0; i < osc_count; ++i )
		chnEnable [i] = enable [i];
	for ( int i = 0; i < osc_count; ++i )
		reg_volumes [i] = volumes [i];
	for ( int i = 0; i < osc_count - 1; ++i )
		reg_periods [i] = periods [i];
	reg_maxperiodVal = maxPeriod;
	reg_noise_parm = noiseParm;

	last_time = lastTime;
	latch = latchValue;
	noise_feedback = noiseFeedback;
	looped_feedback = loopedFeedback;
	ggstereo_save = ggStereo;
	last_write = lastWrite;

	const int* const squareTwoPeriod = &squares [2].period;
	reg_noise_period = regNoiseIndex < 0    ? 0
	                   : regNoiseIndex == 3 ? squareTwoPeriod
	                                        : &noise_periods [regNoiseIndex];
	noise.period = noiseIndex < 0    ? &noise_periods [0]
	               : noiseIndex == 3 ? squareTwoPeriod
	                                 : &noise_periods [noiseIndex];

	return true;
}
