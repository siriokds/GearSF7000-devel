// Sega Master System SN76489 PSG sound chip emulator

// Sms_Snd_Emu 0.1.4
#ifndef SMS_APU_H
#define SMS_APU_H

#include "Sms_Oscs.h"

#include <iosfwd>

class Sms_Apu {
public:
	struct WriteInfo
	{
		unsigned char rawData = 0;
		unsigned char reg = 0;
		unsigned short value = 0;
	};
	// Set overall volume of all oscillators, where 1.0 is full volume
	void volume( double );
	
	// Set treble equalization
	void treble_eq( const blip_eq_t& );
	
	// Outputs can be assigned to a single buffer for mono output, or to three
	// buffers for stereo output (using Stereo_Buffer to do the mixing).
	
	// Assign all oscillator outputs to specified buffer(s). If buffer
	// is NULL, silences all oscillators.
	void output( Blip_Buffer* mono );
	void output( Blip_Buffer* center, Blip_Buffer* left, Blip_Buffer* right );
	
	// Assign single oscillator output to buffer(s). Valid indicies are 0 to 3,
	// which refer to Square 1, Square 2, Square 3, and Noise. If buffer is NULL,
	// silences oscillator.
	enum { osc_count = 4 };
	void osc_output( int index, Blip_Buffer* mono );
	void osc_output( int index, Blip_Buffer* center, Blip_Buffer* left, Blip_Buffer* right );
	
	// Reset oscillators and internal state
	void reset( unsigned noise_feedback = 0, int noise_width = 0 );
	
	// Write GameGear left/right assignment byte
	void write_ggstereo( blip_time_t, int );
	
	// Write to data port
	void write_data( blip_time_t, int );

	// Run all oscillators up to specified time, end current frame, then
	// start a new frame at time 0.
	void end_frame( blip_time_t );

	// Registers, oscillator phase and noise shifter. The Blip_Buffer outputs
	// and the synths are rebuilt by output()/osc_output() rather than saved:
	// they are where the samples go, not what the chip has been told.
	void SaveState( std::ostream& stream ) const;
	bool LoadState( std::istream& stream );

	void GetRegs(int* periods, unsigned char* volumes);
	const WriteInfo& GetLastWriteInfo() const { return last_write; }
	void EnableChannel(int chn, bool onoff);

	// Mute is not disable. EnableChannel/chnEnable drops register writes for
	// the channel - as if it did not exist in hardware, which is what makes
	// it useful as a recording filter (SC-3000's SN76489 has one shared
	// write port, so isolating one channel's sequence means dropping the
	// others' writes before they interleave). Mute never touches write_data:
	// the game keeps writing, reg_periods/reg_volumes keep reflecting the
	// truth, only the audible contribution to the mix is silenced. Live UI
	// state only - reset() clears it like chnEnable, and it is deliberately
	// not part of SaveState: a listening preference, not machine state.
	void MuteChannel(int chn, bool onoff);
	bool IsChannelMuted(int chn) const;
	bool IsChannelEnabled(int chn) const;

	// Debug waveform tap (GearSF7000, ported from Gearsystem's Sms_Apu):
	// an isolated per-channel Blip_Buffer fed alongside the real mix, not
	// instead of it - the panel gets one channel's own waveform without
	// touching what the player hears. Opt-in and cheap to leave off: only
	// the debug window enables it, and only while it is open.
	void init_debug_buffers(int sample_rate, long clock_rate);
	void disable_debug_buffers();
	bool is_debug_enabled() const { return debug_enabled; }
	// Drains whatever the debug buffer for this channel has accumulated
	// since the last call - callers own the pacing (once per drawn frame).
	long read_debug_samples(int channel, blip_sample_t* out, long max_samples);
public:
	Sms_Apu();
	~Sms_Apu();
private:
	// noncopyable
	Sms_Apu( const Sms_Apu& );
	Sms_Apu& operator = ( const Sms_Apu& );
	
	bool	chnEnable[4];
	bool	mute[4];

	unsigned char reg_volumes[osc_count];
	int			  reg_periods[osc_count - 1];
	int			  reg_maxperiodVal;
	const int* reg_noise_period;
	unsigned char reg_noise_parm;

	Sms_Osc*    oscs [osc_count];	// Pointers to: squares[0], squares[1], squares[2], noise
	Sms_Square  squares [3];
	Sms_Square::Synth square_synth; // used by squares
	blip_time_t last_time;
	int         latch;
	WriteInfo   last_write;
	Sms_Noise   noise;
	unsigned    noise_feedback;
	unsigned    looped_feedback;

	bool debug_enabled;
	Blip_Buffer debug_bufs[osc_count];

	unsigned int ggstereo_save;
	
	void run_until( blip_time_t );
};

inline void Sms_Apu::output( Blip_Buffer* b ) { output( b, b, b ); }

inline void Sms_Apu::osc_output( int i, Blip_Buffer* b ) { osc_output( i, b, b, b ); }

#endif
