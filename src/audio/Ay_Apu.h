// AY-3-8910 sound chip emulator

// Game_Music_Emu https://bitbucket.org/mpyne/game-music-emu/
//
// MODIFIED from upstream (ayfxedit-wx, 2026-08-20): added Chip_Type
// selection (ay_3_8910 / ym2149) so the same emulator can model either
// chip's DAC characteristics — different per-step amplitude tables (derived
// with MAME's per-channel resistor-ladder model, ay8910.cpp's
// build_single_table, from its published ay8910_param/ym2149_param/
// ym2149_param_env measurements) and, for ym2149, a 32-step envelope
// instead of 16. See Ay_Apu.cpp for the added chip_type()/build_env_modes_().
//
// MODIFIED again (GearSF7000, 2026-08): vendored from ayfxedit-wx. Two
// changes were needed to host it here:
//   - <cstdint> is included explicitly. This project's Blip_Buffer.h is a
//     later Blargg revision that defines blip_long/blip_ulong itself instead
//     of pulling in <stdint.h>, so uint32_t/int32_t are not in scope by
//     accident the way they were upstream.
//   - set_clock_divider() makes the tone/noise/envelope prescaler runtime
//     configurable. See the comment on that method.
#ifndef AY_APU_H
#define AY_APU_H

#include "blargg_common.h"
#include "Blip_Buffer.h"

#include <cstdint>
#include <iosfwd>

class Ay_Apu {
public:
	enum class Chip_Type { ay_3_8910, ym2149 };

	// Ratio between the Blip time domain and the AY's own clock.
	//
	// Upstream assumes blip_time_t is counted in AY clocks and hardwires a
	// prescaler of 16. GearSF7000 runs a single Blip time domain for the whole
	// machine, clocked at the Z80 rate, and Stereo_Buffer has exactly three
	// fixed Blip_Buffers - so a second chip cannot be given a time domain of
	// its own, it has to share this one.
	//
	// An SGM-style expansion derives the AY clock by halving the 3.58 MHz CPU
	// clock, which is also what MSX does (3.579545 / 2 = 1.7897725 MHz). Pass
	// 2 for that: dividing 3.58 MHz by 32 is exactly dividing 1.79 MHz by 16,
	// so tone, noise and envelope all land on the right frequencies without a
	// second clock domain. Pass 1 if the chip is ever wired to the full CPU
	// clock. Call before writing registers; it takes effect on the next
	// period reload.
	void set_clock_divider( int divider );

	// Selects which chip's DAC/envelope characteristics to emulate
	// (ZX Spectrum: ay_3_8910; most MSX: ym2149). Call before writing any
	// registers; changing it later re-derives the envelope table but does
	// not retroactively fix up amplitudes already in flight.
	void chip_type( Chip_Type );

	// Set buffer to generate all sound into, or disable sound if NULL
	void output( Blip_Buffer* );

	// Reset sound chip
	void reset();

	// Write to register at specified time
	static const unsigned int reg_count = 16;
	void write( blip_time_t time, int addr, int data );

	// Read back a latched register. Added for GearSF7000: software commonly
	// reads a register back after writing it, and upstream had no accessor
	// because the players it was written for never needed one. Registers 14
	// and 15 are the chip's own parallel I/O ports; an SGM-style expansion
	// leaves them unconnected, so they read back as written rather than
	// floating.
	int read( int addr ) const { return regs [addr & 0x0F]; }

	// Run sound to specified time, end current time frame, then start a new
	// time frame at time 0. Time frames have no effect on emulation and each
	// can be whatever length is convenient.
	void end_frame( blip_time_t length );

// Additional features

	// Set sound output of specific oscillator to buffer, where index is
	// 0, 1, or 2. If buffer is NULL, the specified oscillator is muted.
	static const int osc_count = 3;
	void osc_output( int index, Blip_Buffer* );

	// Set overall volume (default is 1.0)
	void volume( double );

	// Set treble equalization (see documentation)
	void treble_eq( blip_eq_t const& );

	// Registers, oscillator phase, noise LFSR and envelope position. The
	// Blip_Buffer outputs and the envelope waveform table are rebuilt rather
	// than stored: the first is where samples go, the second is derived from
	// the chip type.
	void SaveState( std::ostream& stream ) const;
	bool LoadState( std::istream& stream );

	// Disable/mute, matching Sms_Apu's interface (GearSF7000): same names,
	// same distinction. Disable drops register writes for a channel as if it
	// did not exist in hardware, the isolation filter a capture uses to get
	// one channel's own write sequence. Mute never touches a register - the
	// game keeps writing, GetRegs keeps reflecting the truth, only the
	// audible contribution is silenced.
	//
	// The AY cannot give disable the same guarantee the PSG's does, and this
	// is a fact about the chip, not a shortcoming of this emulation: on the
	// SN76489 every write belongs to exactly one channel, so dropping it
	// isolates that channel cleanly. On the AY three registers are SHARED
	// across all three channels - the mixer (R7, tone/noise enable bits for
	// A/B/C in one byte), the noise period (R6) and the envelope
	// period/shape (R11-13, one generator). Disabling channel A must not
	// drop writes to those, or channel B/C's mixer and envelope stop working
	// the moment A is disabled. So disable only gates the three registers
	// exclusive to a channel - period (2 registers) and volume/envelope-
	// enable (1) - and audible silence is forced separately in run_until,
	// the same way mute is. A capture isolating one AY channel will
	// therefore still carry the shared registers' writes if another channel
	// needs them; this is what a real AY-3-8910's own architecture allows.
	void EnableChannel(int chn, bool onoff);
	void MuteChannel(int chn, bool onoff);
	bool IsChannelMuted(int chn) const;
	bool IsChannelEnabled(int chn) const;

	// Registers as GetRegs-style summaries, decoded the way the AY panel
	// reads them today (period, volume/envelope-enable): period[i] and
	// volume[i] for i in 0..2 (A, B, C). Kept alongside read(addr) rather
	// than replacing it - existing debug/MCP code already calls read() for
	// the raw register file.
	void GetRegs(int* periods, unsigned char* volumes) const;

	// Debug waveform tap, matching Sms_Apu's interface exactly. Opt-in and
	// cheap when off: only the debug window enables it, and only while open.
	void init_debug_buffers(int sample_rate, long clock_rate);
	void disable_debug_buffers();
	bool is_debug_enabled() const { return debug_enabled_; }
	long read_debug_samples(int channel, blip_sample_t* out, long max_samples);

public:
	Ay_Apu();
	typedef unsigned char byte;
private:
	struct osc_t
	{
		blip_time_t period;
		blip_time_t delay;
		short last_amp;
		short phase;
		Blip_Buffer* output;
		// Debug waveform tap (GearSF7000). Fed the same deltas as output,
		// alongside it rather than instead of it - see init_debug_buffers().
		Blip_Buffer* debug_buf = nullptr;
		short debug_last_amp = 0;
	} oscs [osc_count];
	blip_time_t last_time;
	byte regs [reg_count];

	struct {
		blip_time_t delay;
		uint32_t lfsr;
	} noise;

	// Sized for the largest case (ym2149: 3 segments of 32); ay_3_8910 only
	// uses the first 48 (3 segments of 16) of each row.
	struct {
		blip_time_t delay;
		byte const* wave;
		int pos;
		int steps;        // 16 (ay_3_8910) or 32 (ym2149)
		int reset_pos;     // -3 * steps
		int repeat_pos;    // -2 * steps
		byte modes [8] [3*32];
	} env;

	bool chnEnable_[osc_count] = { true, true, true };
	bool mute_[osc_count] = { false, false, false };

	bool debug_enabled_ = false;
	Blip_Buffer debug_bufs_[osc_count];

	Chip_Type chip_type_ = Chip_Type::ay_3_8910;
	// Blip clocks per AY internal tick; 16 * clock divider. See
	// set_clock_divider().
	int period_factor_ = 16;

	void run_until( blip_time_t );
	void write_data_( int addr, int data );
	void build_env_modes_();
public:
	static const int amp_range = 255;
	Blip_Synth<blip_good_quality,1> synth_;
};

inline void Ay_Apu::volume( double v ) { synth_.volume( 0.7 / osc_count / amp_range * v ); }

inline void Ay_Apu::treble_eq( blip_eq_t const& eq ) { synth_.treble_eq( eq ); }

inline void Ay_Apu::write( blip_time_t time, int addr, int data )
{
	run_until( time );
	write_data_( addr, data );
}

inline void Ay_Apu::osc_output( int i, Blip_Buffer* buf )
{
	assert( (unsigned) i < osc_count );
	oscs [i].output = buf;
}

inline void Ay_Apu::output( Blip_Buffer* buf )
{
	osc_output( 0, buf );
	osc_output( 1, buf );
	osc_output( 2, buf );
}

inline void Ay_Apu::end_frame( blip_time_t time )
{
	if ( time > last_time )
		run_until( time );

	assert( last_time >= time );
	last_time -= time;

	if ( debug_enabled_ )
		for ( int i = 0; i < osc_count; i++ )
			debug_bufs_ [i].end_frame( time );
}

#endif
