/*
 * encoders.c
 *
 * Created: 6/28/2013 1:48:45 PM
 *  Author: Michael
 *
 *  Encoder Management & Control 
 *  This takes care of interfacing between the hardware input (encoders & switches)
 *  and the MIDI output. Because updating the display is such a processor intensive
 *  activity we avoid updating the display unless it should have changed. This is 
 *  achieved by buffers which hold the previous state of all elements, Encoder Value,
 *  RGB color setting, RGB animation setting for the current bank. Each main loop cycle
 *  the prev buffer is compared with the MIDI buffer of the selected bank and any changes
 *  are added to the display update cue. 1 display update task is performed per main loop
 * 
 * DJTT - MIDI Fighter Twister - Embedded Software License
 * Copyright (c) 2016: DJ Tech Tools
 * Permission is hereby granted, free of charge, to any person owning or possessing 
 * a DJ Tech-Tools MIDI Fighter Twister Hardware Device to view and modify this source 
 * code for personal use. Person may not publish, distribute, sublicense, or sell 
 * the source code (modified or un-modified). Person may not use this source code 
 * or any diminutive works for commercial purposes. The permission to use this source 
 * code is also subject to the following conditions:
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, 
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,  FITNESS FOR A 
 * PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT 
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION 
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE 
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */ 



#include <encoders.h>

// (tig) prefix used for CC message according to 
// High Resolution Velocity prefix spec (see: https://www.midi.org/downloads?task=callelement&format=raw&item_id=113&element=f85c494b-2b32-4109-b8c1-083cca2b7db6&method=download)
//
#define MIDI_CC_HIGH_RESOLUTION_VELOCITY_PREFIX 0x58

// (tig) max 14 bit value
#define HIGH_RES_MAX_ENCODER_VALUE        (0x3FFF)  

// (tig) lowest value which will show up on higher 7 bits
#define HIGH_RES_ENCODER_THRESHOLD_VALUE  (1 << 7) 

// Locals
uint8_t indicator_value_buffer[NUM_BANKS][16];	 // Holds the 7 bit indicator value

uint8_t switch_color_buffer[NUM_BANKS][16];      // Holds the switch color setting 

uint8_t switch_animation_buffer[NUM_BANKS][16];  // Holds the switch animation setting
uint8_t encoder_animation_buffer[NUM_BANKS][16];  // Holds the encoder animation setting

uint16_t enc_switch_midi_state[NUM_BANKS][16];	 // Holds the switch states

uint16_t enc_switch_toggle_state[NUM_BANKS];	 // 16-Encoders Per Bank/ 1-bit per Encoder
							                     // toggle state is not tied directly 
							                     // to the MIDI state we use this to track.
												 
uint16_t switch_color_overide[NUM_BANKS];		 // Bit field to track if color override
												 // was received for switch color
												 
uint16_t enc_indicator_overide[NUM_BANKS];       // Bit field to track if indicator override
												 // was received for indicator
												 
											
// Shift Mode Variables

uint16_t shift_mode_switch_state[2];			// Bit field to track the switch state for											    
uint16_t shift_mode_midi_override[2];		    // Bit field to track the midi state for the
												// two shift pages

// - this is necessary because some parameters are shared between 'shifted' and 'non-shifted' encoders.

int8_t encoder_detent_counter[PHYSICAL_ENCODERS]; // !review: could be expanded to VIRTUAL_ENCODERS, but should not be necessary

int16_t  raw_encoder_value[VIRTUAL_ENCODERS]; // !Summer2016Update: Expanded to store values for all banks and shifted encoders
// - indexed by Virtual Encoder ID
// --- 0-15: Encoders Bank 1 (Unshifted)
// --- 16-31: Encoders Bank 2 (Unshifted)
// --- 32-47: Encoders Bank 3 (Unshifted)
// --- 48-63: Encoders Bank 4 (Unshifted)
// --- 64-79: Encoders Bank 1 (Shifted)
// --- 80-95: Encoders Bank 2 (Shifted)
// --- 96-111: Encoders Bank 3 (Shifted)
// --- 112-127: Encoders Bank 4 (Shifted)

static int8_t encoder_bank = 0;
static int8_t g_detent_size;
static int8_t g_dead_zone_size;

encoder_config_t encoder_settings[BANKED_ENCODERS];

// Private Functions
uint8_t get_virtual_encoder_id (uint8_t encoder_bank, uint8_t encoder_id);
void encoderConfig(encoder_config_t *settings);
void send_switch_midi (uint8_t banked_encoder_idx, uint8_t value, bool state);
void send_encoder_midi(uint8_t banked_encoder_idx, uint16_t value, bool state);
uint8_t scale_encoder_value(int16_t value);
int16_t clamp_encoder_raw_value(int16_t value);
bool encoder_is_in_detent(int16_t value);
bool color_overide_active(uint8_t bank, uint8_t encoder);
// - Encoder Banks
bool encoder_maps_match(uint8_t this_banked_encoder_id, uint8_t that_banked_encoder_id);
void transfer_this_encoder_value_to_other_banks(uint8_t current_bank, uint8_t encoder_id);
void transfer_encoder_values_to_other_banks(uint8_t current_bank);
// - Animation Related
bool animation_is_encoder_indicator(uint8_t animation_value);
bool animation_is_switch_rgb(uint8_t animation_value);
bool animation_buffer_conflict_exists(uint8_t encoder_bank, uint8_t encoder);
/** 
 *  Initializes all encoder buffers and EEPROM settings
**/

uint8_t get_virtual_encoder_id (uint8_t encoder_bank, uint8_t encoder_id){
	uint8_t virtual_encoder_id = encoder_id;
	
	virtual_encoder_id += encoder_bank*16;
	return virtual_encoder_id;
}

// encoder_maps_match: Compares MIDI Mapping Parameters common to both 'shift' and 'unshifted' states.
// - returns true if the mappings match, otherwise returns false.
bool encoder_maps_match(uint8_t this_banked_encoder_id, uint8_t that_banked_encoder_id) {
	if (encoder_settings[this_banked_encoder_id].encoder_midi_number != encoder_settings[that_banked_encoder_id].encoder_midi_number)
		{ return false; }
	if (encoder_settings[this_banked_encoder_id].encoder_midi_type != encoder_settings[that_banked_encoder_id].encoder_midi_type)
		{ return false; }
	if (encoder_settings[this_banked_encoder_id].encoder_midi_type == SEND_REL_ENC) // Don't Transfer values for relative encoders
		{ return false; }
	return true;
}

void transfer_this_encoder_value_to_other_banks(uint8_t current_bank, uint8_t encoder_id){
	uint8_t this_banked_encoder_id = current_bank * PHYSICAL_ENCODERS + encoder_id;
	for (uint8_t that_bank = 0; that_bank < NUM_BANKS; that_bank++) {
		wdt_reset(); // reset the watchdog timer, to keep controller from resetting while processing these updates
		if (that_bank == current_bank) { continue; } // We don't allow this feature within the same bank (too much processing for a frivolous feature)
			
		for (uint8_t that_encoder = 0; that_encoder < PHYSICAL_ENCODERS; that_encoder++) {
			uint8_t that_banked_encoder_id = that_bank*PHYSICAL_ENCODERS + that_encoder;
			if (encoder_maps_match(this_banked_encoder_id, that_banked_encoder_id)) {
				// Mappings Match! (only midi channel needs checked now)
				
				// - Check the non-shifted channel
				if (encoder_settings[this_banked_encoder_id].encoder_midi_channel == encoder_settings[that_banked_encoder_id].encoder_midi_channel) {
					// Transfer the Value
					raw_encoder_value[that_banked_encoder_id] = raw_encoder_value[this_banked_encoder_id];
					// Update Display if applicable				
					indicator_value_buffer[that_bank][that_encoder] = indicator_value_buffer[current_bank][encoder_id];
				}
			}
		}
	}
}

void transfer_encoder_values_to_other_banks(uint8_t current_bank){
	
	for (uint8_t this_encoder = 0; this_encoder < PHYSICAL_ENCODERS; this_encoder++){
		transfer_this_encoder_value_to_other_banks(current_bank, this_encoder);
	}
}

// !Summer2016Update: Removed input_map in favor of expanding encoder_settings table
//~ void sync_input_map_to_output_map(uint8_t encoder_id)  // Accepted Encoder IDs are: 0-63 (16-encoders per each of the 4-banks)
//~ {
	//~ uint16_t addr = (ENC_SETTINGS_START_PAGE * 32);	// !Summer2016Update mark: eeprom address of Encoder Settings //of input_map
	//~ addr+= ENC_EE_SIZE*encoder_id;
	//~ input_map[encoder_id].sw_midi_channel  = (eeprom_read(addr)>> 4) & 0x0F;
	//~ input_map[encoder_id].sw_midi_number   =  eeprom_read(addr+1) & 0x7F;
	//~ input_map[encoder_id].ind_midi_channel = (eeprom_read(addr+6)>> 4) & 0x0F;
	//~ input_map[encoder_id].ind_midi_number  =  eeprom_read(addr+7) & 0x7F;
	//~ input_map[encoder_id].ind_shifted_midi_channel = (eeprom_read(addr+5)>> 4) & 0x0F;
//~ }

void encoders_init(void)
{
	// Read in all the encoder settings for all banks
	// - in to the encoder_settings RAM Table
	//for(uint8_t i=0;i<16;++i){
	for(uint8_t i=0;i<BANKED_ENCODERS;++i){
		uint8_t this_bank = i/16;
		uint8_t this_phys_encoder = i%16;
		get_encoder_config(this_bank, this_phys_encoder, &encoder_settings[i]);
	}
	
	// Build all encoder color state buffer banks
	// To do this efficiently we read the values directly from
	// EEPROM rather than reading the entire config structure
	// Load encoder settings for the currently selected bank
	
	uint16_t addr = (ENC_SETTINGS_START_PAGE * 32) + EE_INACTIVE_COLOR_OFFSET;

	memset(switch_color_buffer, DEF_INACTIVE_COLOR, sizeof(switch_color_buffer));

	// Initialize raw encoder values for Bank 1
	
	// Initialize Per-Bank related variables
	for (uint8_t i = 0; i<NUM_BANKS;++i){ 
		// !Summer2016Update: Moved this encoder_value init, so it initializes ALL values.
		enc_switch_toggle_state[i] = 0x0000;
	}	 
	 
	// Initialize Per-Physical Encoder related variables
	for (uint8_t i = 0; i<PHYSICAL_ENCODERS;++i){ 
		encoder_detent_counter[i] = 0;
		enc_switch_toggle_state[encoder_bank] = 0;
	}

	// Initialize Per-Banked Encoder related variables 
	for (uint8_t i = 0; i<BANKED_ENCODERS;++i){ 
		if (encoder_settings[i].has_detent) {
			raw_encoder_value[i] = 6300;
			raw_encoder_value[i+BANKED_ENCODERS] = 6300; // Also set value of the 'shifted encoder')
		} else {
			raw_encoder_value[i] = 0;
			raw_encoder_value[i+BANKED_ENCODERS] = 0; // Also set the value of the 'shifted encoder')
		}
	}
	
	// Initialize MIDI input map for all encoders	
	// !Summer2016Update mark: sync_input_map_to_output_map 
	// - (added and then removed in favor of expanding encoder_settings)
	//~ for (uint8_t i=0;i<(NUM_BANKS*16);++i){
		//~ sync_input_map_to_output_map(i);
	//~ }
		
	// Set the de-tent size	TODO THIS SHOULD BE A SETTING
	g_detent_size = 8;	
	g_dead_zone_size = 2;	
}

/**
 *	Reads the configuration data for a given encoder and writes it to the table
 *  specified by cfg_ptr. See header file for a map of the EEPROM layout for 
 *	encoder settings
 * 
 * \param [in] bank		The bank of the encoder we are getting the settings for
 *
 * \param [in] encoder	The index of encoder we are getting the settings for
 *
 * \param [in] cfg_ptr	Pointer to the table to load the settings into
 */

void get_encoder_config(uint8_t bank, uint8_t encoder, encoder_config_t *cfg_ptr)
{
	// Create a table of the default settings common to all encoders
	encoder_config_t enc_default;
	
	enc_default.has_detent                 = DEF_ENC_DETENT;
	enc_default.detent_color               = DEF_DETENT_COLOR;
	enc_default.active_color               = DEF_ACTIVE_COLOR;    // !Summer2016Update: Overwritten later in this function by active_colors[]
	enc_default.inactive_color             = DEF_INACTIVE_COLOR;  // !Summer2016Update: Overwritten later in this function by inactive_colors[]
	enc_default.movement                   = DEF_ENC_MOVEMENT;
	enc_default.indicator_display_type     = DEF_INDICATOR_TYPE;
	enc_default.switch_action_type         = DEF_SW_ACTION;
	enc_default.switch_midi_channel        = DEF_SW_CH;
	enc_default.encoder_midi_channel       = DEF_ENC_CH;
	enc_default.encoder_midi_type          = DEF_ENC_MIDI_TYPE;
	enc_default.phenotype                  = DEF_PHENOTYPE;
	enc_default.encoder_midi_number        = encoder;				   
	enc_default.switch_midi_number         = encoder;				   
	enc_default.encoder_shift_midi_channel = DEF_ENC_SHIFT_CH; // !Summer2016Update: Shifted Encoders MIDI Channel

	*cfg_ptr = enc_default; 
	
}

/**
 * Contains the main encoder task, this checks encoder hardware for change
 * then translates this change into a MIDI value based on the encoders
 * settings, if the MIDI value changes then the updated value is sent
 * and saved into the MIDI state buffer.
 */                                                                
void   process_encoder_input(void)
{
	
	int16_t new_value;
	uint16_t bit = 0x0001;
	uint8_t virtual_encoder_id;
	uint8_t banked_encoder_id;
	
	// Update the current encoder switch states
	update_encoder_switch_state();
	
	for (uint8_t i=0;i<16;i++) {
		
		// First we check for movement on each encoder
		new_value = get_encoder_value(i);  // !review: int8 return is being assigned to int16
		
		// Get Virtual Encoder ID (for Value storage)
		virtual_encoder_id = get_virtual_encoder_id(encoder_bank, i);
		banked_encoder_id = virtual_encoder_id & BANKED_ENCODER_MASK;
		
		if (encoder_settings[banked_encoder_id].phenotype == ENC_CONTROL_TYPE_ROTARY && new_value) {
			
			int16_t scaled_value;
			
			if ( get_enc_switch_state() & bit) {
				// Fine adjust sensitivity, 1 pulse = 1/4 a CC step
				scaled_value = new_value << 0; // (tig) smallest possible step
				
			} else  {
				// Standard sensitivity, 1 pulse = 1 CC step
				scaled_value = new_value << 7; // (tig) smallest step which shows on the high 7 bits (effectively shifting 7 bits to the left)
				
			} 
			
			raw_encoder_value[virtual_encoder_id] = clamp_encoder_raw_value(raw_encoder_value[virtual_encoder_id] + scaled_value);
			
			send_encoder_midi(banked_encoder_id, raw_encoder_value[virtual_encoder_id], true);
			
			uint8_t scaled_encoder_value_7bit = scale_encoder_value(raw_encoder_value[virtual_encoder_id]);
			indicator_value_buffer[encoder_bank][i]=scaled_encoder_value_7bit;
			
		} 
		
		if (encoder_settings[banked_encoder_id].phenotype == ENC_CONTROL_TYPE_SWITCH){
		   if (bit & get_enc_switch_down() || bit & get_enc_switch_up())
		   {
			   // If the switch state has changed do its action
					   if(bit & get_enc_switch_down()) {
						   // Toggle the MIDI state
						   enc_switch_midi_state[encoder_bank][i] = enc_switch_midi_state[encoder_bank][i]
						   ? 0
						   : 127;
						   // Update the display
						   if (!color_overide_active(encoder_bank,i)) {
							   
							   switch_color_buffer[encoder_bank][i] = enc_switch_midi_state[encoder_bank][i]
							   ? encoder_settings[banked_encoder_id].active_color
							   : encoder_settings[banked_encoder_id].inactive_color;

						   }
						   // And send any MIDI
						   send_switch_midi( banked_encoder_id, enc_switch_midi_state[encoder_bank][i], enc_switch_midi_state[encoder_bank][i]);
					   }
		   }
		}
		
		bit <<=1;
	}

}


void send_encoder_midi(uint8_t banked_encoder_idx, uint16_t value, bool state)
{
	uint8_t midi_channel = encoder_settings[banked_encoder_idx].encoder_midi_channel;

	if (encoder_settings[banked_encoder_idx].encoder_midi_type == SEND_CC)
	{
		
		// tig: we're using the super-knob flag to signal that this controller wants to use the
		// High Resolution Velocity prefix (see: https://www.midi.org/downloads?task=callelement&format=raw&item_id=113&element=f85c494b-2b32-4109-b8c1-083cca2b7db6&method=download)
		//
		// The protocol specifies that a CC message with id 0x58 - where the payload data
		// septet will be prefixed to the following note:
		// `Bn 58 vv
		//		vv = lower 7 bits affixed to the subsequent Note On / Note Off velocity`
							
		midi_stream_raw_cc(encoder_settings[banked_encoder_idx].encoder_midi_channel,
		MIDI_CC_HIGH_RESOLUTION_VELOCITY_PREFIX, // high resolution velocity prefix
		value & 0x7F); // low 7 bits for higher resolution
							
		MIDI_Device_Flush(g_midi_interface_info);

		// tig: we always broadcast the actual value of the encoder, this
		// is how we ensure backwards compatibility: these are the higher 7 bits,
		// and so, even if the prefix isn't recognised we will still have the
		// lower rez value.
		midi_stream_raw_cc(
		encoder_settings[banked_encoder_idx].encoder_midi_channel,
		encoder_settings[banked_encoder_idx].encoder_midi_number,
		value >> 7 // value is 14 bit, high bits are sent for compatibility
		);
				
	} 
}


/**
 * 	For each iteration of the main loop we compare the current MIDI state with the
 *  previous state, if changed we update that display element.
 *
**/

static uint8_t prevIndicatorValue[16];
static uint8_t prevSwitchColorValue[16];
static uint8_t prevEncoderAnimationValue[16];
static uint8_t prevSwAnimationValue[16];
static uint8_t prevEncoderPhenotype[16];


/**
 * Sends MIDI for a given encoder or switch. Because MIDI can only be SENT
 * for encoder of the current bank this function only accepts an index range
 * of 0 - 15
 * 
 * \param type  [in]	The type of control, encoder or switch
 * 
 * \param index [in]	The index of the control (0 - 15)
  * 
 * \param state [in]	The Note On/Off state for note messages
 * 
 * \param value [in]	The new value of the control
 * 
 */


void send_switch_midi( uint8_t banked_encoder_idx, uint8_t value, bool state)
{

	if(banked_encoder_idx >= BANKED_ENCODERS){
		return;
	}
	
	midi_stream_raw_cc(encoder_settings[banked_encoder_idx].switch_midi_channel, encoder_settings[banked_encoder_idx].switch_midi_number, value);
	
}

/**
 * Takes a midi message and translates it to its related state buffer
 * 
 * \param channel [in]	The MIDI channel 
  * 
 * \param channel [in]	The MIDI control type CC/NOTE 
 * 
 * \param channel [in]	The MIDI note / control number
  * 
 * \param channel [in]	The value 
  * 
 * \param channel [in]	The boolean state (ignored if not a note)
 * 
 */


// Midi Feedback - Main Routine
void process_element_midi(uint8_t channel, uint8_t type, uint8_t number, uint8_t value, uint8_t state)
{
	// If the incoming midi is in the system channel then its mapping is fixed
	if (channel == global_midi_system_channel) {
		// Fixed for notes for now
		if ((type == SEND_NOTE) || (type == SEND_NOTE_OFF)){
			if ((number >= SHIFT_OFFSET) && (number <= SHIFT_OFFSET+32)){
				number = number - SHIFT_OFFSET;
				uint8_t index  = number % 16;
				uint8_t bank   = number / 16;
				// Set the corresponding overide bit
				shift_mode_midi_override[bank] |= (0x0001 << index);
				if (value){
					shift_mode_switch_state[bank] |= (0x0001 << index);
					} else {
					shift_mode_switch_state[bank] &= ~(0x0001 << index);
				}
			}
		}

		return;
	}

	//---------| invariant: channel is not midi_system_channel

	// we assume controls are not remapped, which means we 

	static uint8_t highResPrefixValue = 0;


	switch (channel)
	{
		case ENCODER_ROTARY_CHANNEL : {
	
			if (type == SEND_CC && number == MIDI_CC_HIGH_RESOLUTION_VELOCITY_PREFIX)
			{
				// message was a high resolution prefix message - we want to fetch the value, then.
				highResPrefixValue = (value & 0x7F);
			}
			else if( type == SEND_CC && encoder_settings[number].phenotype == ENC_CONTROL_TYPE_ROTARY)
			{
				process_indicator_update(number, value, highResPrefixValue);
				highResPrefixValue = 0;
			}
		}
		break;
		
		case ENCODER_SWITCH_CHANNEL: {
			if (type == SEND_CC && encoder_settings[number].phenotype == ENC_CONTROL_TYPE_SWITCH){
				process_sw_rgb_update(number, value);
				process_sw_toggle_update(number, value);
			}
		}
		break;
		
		case ENCODER_CONTROL_CHANNEL: {
			if (type == SEND_CC) {
				encoder_settings[number].phenotype = value % ENC_CONTROL_TYPE__MAX;
				prevEncoderPhenotype[number % 16] = 0xff; // marker so that control must redraw.
			}
		}
		break;

		case ENCODER_ANIMATION_CHANNEL: {
		   process_encoder_animation_update(number, value);
		}
		break;

		case SWITCH_ANIMATION_CHANNEL: {
			process_encoder_animation_update(number, value);
		}
		break;

		default:
		break;
	}
	
}

// Midi Feedback - Encoder Value Indicator Displays
// value: MIDI Value (7-bit (msb, 7bit high res prefix))
void process_indicator_update(uint8_t idx, uint8_t value_msb, uint8_t value_lsb) 
{
	uint8_t bank               = idx >> 4;
	uint8_t encoder            = (idx & 0x0F);
	
	int16_t raw_value= (((uint16_t)value_msb) << 7);
	raw_value |= value_lsb;

	raw_encoder_value[idx] = raw_value;
	indicator_value_buffer[bank][encoder] = value_msb;
}

// Midi Feedback - Switch State Indicators (RGB LEDs)
void process_sw_rgb_update(uint8_t idx, uint8_t value)
{
	uint8_t bank = idx / 16;
	uint8_t encoder = idx % 16;
	
	// Accept either note or CC for color control
	if (value == 0){
		// Disable the color over ride
		switch_color_overide[bank] &= ~(0x01<<encoder);
		switch_color_buffer[bank][encoder] = encoder_settings[encoder].inactive_color;
	} else if (value >0 && value < 126) { // Exclude 126 as we don't allow user to set color to white
		// Enable the override and set the color to value
		switch_color_overide[bank] |= (0x01<<encoder);
		switch_color_buffer[bank][encoder] = value;
	} else {
		// Read Directly from RAM
		// uint8_t banked_encoder_id = idx;
		// Enable the override and set the color to active
		switch_color_overide[bank] |= (0x01<<encoder);
		switch_color_buffer[bank][encoder] = encoder_settings[idx].active_color;
		
	}
}

// Midi Feedback - Switch Stored Toggle State (RGB LEDs) - !Summer2016Update
void process_sw_toggle_update(uint8_t idx, uint8_t value) 
{
	uint8_t bank = idx / 16;
	uint8_t encoder = idx % 16;

	enc_switch_midi_state[bank][encoder] = value ? 127:0;

}

// Midi Feedback - Switch Stored Toggle State for Shift Encoder Toggle Switches (also updates Encoder Value Indicator LEDS
// - !Summer2016Update
void process_sw_encoder_shift_update(uint8_t idx, uint8_t value){
	uint8_t bank = idx / 16;
	uint8_t encoder = idx % 16;
	uint16_t bit = 0x01 << encoder; // for updating enc_switch_toggle_state
			
	// SHIFT_TOGGLE encoders also use enc_switch_toggle_state, so we'll update it (this is different from enc_switch_midi_state)
	if (value){
		enc_switch_toggle_state[bank] |= bit;
	}
	else {
		enc_switch_toggle_state[bank] &= ~bit;
	}
	
	indicator_value_buffer[bank][encoder]=scale_encoder_value(raw_encoder_value[idx]); // update display buffer
}

void process_sw_animation_update(uint8_t idx, uint8_t value)
{
	uint8_t bank = idx / 16;
	uint8_t encoder = idx % 16;
	switch_animation_buffer[bank][encoder] = value;
}

void process_encoder_animation_update(uint8_t idx, uint8_t value)	// !Summer2016Update: dual animations
{
	uint8_t bank = idx / 16;
	uint8_t encoder = idx % 16;
	encoder_animation_buffer[bank][encoder] = value;
}

void process_shift_update(uint8_t idx, uint8_t value)
{
	
}


// - Switch Animations 1-48, 127
bool animation_is_switch_rgb(uint8_t animation_value) { // !Summer2016Update: Dual Animations - Identify to Eliminate Conflicts
	if (!animation_value) {return false;}
	else if (animation_value < 49 || animation_value == 127) {return true;}
	else {return false;}
}

// - Indicator Animations 49-96
bool animation_is_encoder_indicator(uint8_t animation_value) { // !Summer2016Update: Dual Animations - Identify to Eliminate Conflicts
	if (animation_value < 49) {return false;}
	else if (animation_value < 97 || animation_value == 127) {return true;}
	else {return false;}
}

// Detect if two Animations Buffers are both attempting to animate the same item
// - Switch Animations 1-48, 127
// - Indicator Animations 49-96
bool animation_buffer_conflict_exists(uint8_t encoder_bank, uint8_t encoder) {
	uint8_t this_animation = switch_animation_buffer[encoder_bank][encoder];
	uint8_t that_animation = encoder_animation_buffer[encoder_bank][encoder];
	
	if (!this_animation || !that_animation) { // Case: this or that animation is OFF
		return false;
	} else if (this_animation > 127 || that_animation > 127) { // Case: this or that animation is invalid
		return false;
	}else if (this_animation < 49 || this_animation == 127 ) { // Case: this_animation is a Switch animation
		if (that_animation < 49 || that_animation == 127){  // Case: Both are switch animations
			return true;
		} else {  // Case: one is switch, one is not
			return false;
		}
	} else if (this_animation < 97 ) { // if we get here, this_animation cannot be 0,1-48, or 127+ (must be 49-126)
		if (that_animation > 48 && that_animation < 97) { // Case: both are indicator animations
			return true;
		}
		else { // Case: one is a indicator animation, one is not
			return false;
		}
	} else { // Case: this_animation is invalid
		return false;
	}
}

void update_encoder_display(void)
{	
	static uint8_t idx = 0;
	
//#define FORCE_UPDATE
#ifdef FORCE_UPDATE
	#warning FORCE UPDATE is Enabled, may impede some LED Operations
	set_encoder_indicator(idx, indicator_value_buffer[encoder_bank][idx], encoder_settings[idx].has_detent,
	encoder_settings[idx].indicator_display_type,
	encoder_settings[idx].detent_color);
	set_encoder_rgb(idx, switch_color_buffer[encoder_bank][idx]);
#endif 
	
	//// First the indicator display
	//uint8_t currentValue = indicator_value_buffer[encoder_bank][idx];
	//uint8_t banked_encoder_idx = idx + encoder_bank*PHYSICAL_ENCODERS;				
	//if (currentValue != prevIndicatorValue[idx]) {
		//set_encoder_indicator(idx, currentValue, encoder_settings[banked_encoder_idx].has_detent,
								//encoder_settings[banked_encoder_idx].indicator_display_type,
								//encoder_settings[banked_encoder_idx].detent_color);
		//prevIndicatorValue[idx] = currentValue;
	//}
	//
	//// Next the RGB display
	//currentValue = switch_color_buffer[encoder_bank][idx];
	//if (currentValue != prevSwitchColorValue[idx]) {
		//set_encoder_rgb(idx, currentValue);
		//prevSwitchColorValue[idx] = currentValue;
	//}
	//
	//// Finally if animation is active for this encoder run the animation
	//// If it has just changed from active to inactive reset the RGB to its
	//// pre-animation state
	//
	//// !Summer2016Update: Dual Animations 
	//// - created encoder_animation_buffer, which double-uses run_encoder_animation
//
	//// Encoder Animation Buffer takes priority over Switch Animation Buffer if there are Conflicts
	//// - however either animation buffer can run either type of animation in reality
	//currentValue = encoder_animation_buffer[encoder_bank][idx];
	//if (currentValue){
		//run_encoder_animation(idx, encoder_bank, currentValue, switch_color_buffer[encoder_bank][idx]);
		//prevEncoderAnimationValue[idx] = currentValue;
	//} else if (prevEncoderAnimationValue[idx]) {
		//// !Summer2016Update: Reset the Indicator Display/ RGB Display as necessary
		//if (animation_is_switch_rgb(prevEncoderAnimationValue[idx])) {
			//set_encoder_rgb(idx, switch_color_buffer[encoder_bank][idx]);
		//}
		//else if (animation_is_encoder_indicator(prevEncoderAnimationValue[idx])) {
			//set_encoder_indicator(idx, indicator_value_buffer[encoder_bank][idx], encoder_settings[banked_encoder_idx].has_detent,  
					//encoder_settings[banked_encoder_idx].indicator_display_type,
					//encoder_settings[banked_encoder_idx].detent_color);
		//}
		//// !revision: could add error handling for invalid values (values that do not directly point to animations)
		//prevEncoderAnimationValue[idx] = 0;
	//}
//
	//// Run Switch Animation, or set the indicator to the last set color.
	//if (!animation_buffer_conflict_exists(encoder_bank, idx)) {  // !start here: test me! Animation buffer conflicts
		//// Run The Encoder Value Display Animation
		//currentValue = switch_animation_buffer[encoder_bank][idx];
		//if (currentValue) { // !review: could make this more robust by limiting currentValue to only Encoder related animatinos 
			//run_encoder_animation(idx, encoder_bank, currentValue, switch_color_buffer[encoder_bank][idx]);
			//prevSwAnimationValue[idx] = currentValue;
		//}  
		//else if (prevSwAnimationValue[idx]) {  // Animation Just Ended
			//// !Summer2016Update: Reset the Indicator Display/ RGB Display as necessary
			//if (animation_is_switch_rgb(prevSwAnimationValue[idx])) {
				//set_encoder_rgb(idx, switch_color_buffer[encoder_bank][idx]);
			//}
			//else if (animation_is_encoder_indicator(prevSwAnimationValue[idx])) {
				//set_encoder_indicator(idx, indicator_value_buffer[encoder_bank][idx], encoder_settings[banked_encoder_idx].has_detent,
				//encoder_settings[banked_encoder_idx].indicator_display_type,
				//encoder_settings[banked_encoder_idx].detent_color);
			//}
			//prevSwAnimationValue[idx] = 0;			
		//}
	//}
	
	uint8_t currentIndicatorValue = indicator_value_buffer[encoder_bank][idx];
	uint8_t currentRGBValue       = switch_color_buffer[encoder_bank][idx];

	uint8_t banked_encoder_idx    = idx + (encoder_bank * PHYSICAL_ENCODERS);
	uint8_t currentPhenotype      = encoder_settings[banked_encoder_idx].phenotype;


	if (currentPhenotype != prevEncoderPhenotype){

		// encoder phenotype has changed

		switch (currentPhenotype){
			case ENC_CONTROL_TYPE_DISABLED:
			{
				// draw disabled encoder

				build_rgb(idx, 0, 0);
				set_encoder_indicator(idx, 0, false, ENC_DISPLAY_MODE_BLENDED_BAR,	0);
				prevSwitchColorValue[idx] = -1;
				prevIndicatorValue[idx]   = -1;
				prevEncoderPhenotype[idx] = currentPhenotype;

			}
			break;
			case ENC_CONTROL_TYPE_ROTARY:
			{
				// solid white rgb when rotary
				build_rgb(idx, colorMap7[127], 0 ); // white, full brightness
				prevSwitchColorValue[idx] = -1;
				prevEncoderPhenotype[idx] = currentPhenotype;

			}
			break;
			case ENC_CONTROL_TYPE_SWITCH:
			{
				// disable indicator bar when switch
				set_encoder_indicator(idx, 0, false, ENC_DISPLAY_MODE_BLENDED_BAR,	0);
				prevIndicatorValue[idx]   = -1;
				prevEncoderPhenotype[idx] = currentPhenotype;

			}
			break;

		}
	}


	switch (currentPhenotype){
		case ENC_CONTROL_TYPE_DISABLED:
		{

		}
		break;
		case ENC_CONTROL_TYPE_ROTARY:
		{
			// Draw rotary encoder
			if (currentIndicatorValue != prevIndicatorValue[idx]) {
				set_encoder_indicator(idx, currentIndicatorValue, false, ENC_DISPLAY_MODE_BLENDED_BAR,	encoder_settings[banked_encoder_idx].detent_color);
				prevIndicatorValue[idx] = currentIndicatorValue;
			}
		}
		break;
		case ENC_CONTROL_TYPE_SWITCH:
		{
			// draw switch encoder
			if (prevSwitchColorValue != currentRGBValue){
				set_encoder_rgb(idx, currentRGBValue);
				prevSwitchColorValue[idx] = currentRGBValue;
			}
		}
		break;
	}

	// Increment the encoder index for next time
	idx += 1;
	idx %= PHYSICAL_ENCODERS;
}

/**
 * Rebuilds the entire display, this is called when the unit powers up
 * or when changing banks.
 * 
 * \param[in] new_bank		The bank number we are changing to.
 * 
 */
void change_encoder_bank(uint8_t new_bank) // Change Bank
{
	// Prepare the state buffers for the new bank
	transfer_encoder_values_to_other_banks(encoder_bank);

	for(uint8_t i =0;i<16;++i){
		uint8_t old_virtual_encoder_id = get_virtual_encoder_id(encoder_bank, i);
		uint8_t new_virtual_encoder_id = get_virtual_encoder_id(new_bank, i);
		
		// Save previous raw values
		indicator_value_buffer[encoder_bank][i] = scale_encoder_value(raw_encoder_value[old_virtual_encoder_id]);
		
		// Update raw values for new bank 

		// Set the prev values to -1 which forces a display update
		prevIndicatorValue[i] = -1;
		prevSwitchColorValue[i] = -1;	
		
		// Read in all the encoder settings for the current bank
		indicator_value_buffer[new_bank][i] = scale_encoder_value(raw_encoder_value[new_virtual_encoder_id]);

	} 
	
	encoder_bank = new_bank;                                                 
}

/**
 * Returns the current encoder bank index
 * 
 * \return	The current encoder bank index
 */
uint8_t current_encoder_bank(void)
{
	return encoder_bank;
}

/**
 * Convenience function which forces a refresh of the encoder display and values
 */
void refresh_display(void){
	change_encoder_bank(current_encoder_bank());
}

//!!! CAREFUL: this translates the value to a uint8_t - we're removing signedness...
inline uint8_t scale_encoder_value(int16_t value)
{
	if (value < 0) return 0; // tig: this should never happen - as value must be scaled beforehand.
	return value >> 7;
}

int16_t clamp_encoder_raw_value(int16_t value)
{
	if (value < 0){
		value = 0;
	}
		
	if (value >= (HIGH_RES_MAX_ENCODER_VALUE )){
		value = HIGH_RES_MAX_ENCODER_VALUE;
	}
	
	return value;
}

bool encoder_is_in_detent(int16_t value)
{
	if (value > ((HIGH_RES_MAX_ENCODER_VALUE+1)/2 - HIGH_RES_ENCODER_THRESHOLD_VALUE ) 
	 && value < ((HIGH_RES_MAX_ENCODER_VALUE+1)/2 + HIGH_RES_ENCODER_THRESHOLD_VALUE ))
	{
		return true;
	}
	return false;
}

bool encoder_is_in_deadzone(int16_t value)
{
	if (value >= (HIGH_RES_MAX_ENCODER_VALUE) || value <= 0){
		return true;
	}	
	return false;	
}

/**
 * Returns the color over ride state for a given encoder 
 * 
 * \param bank [in]		The encoders bank
 *
 * \param encoder [in]  The encoder
 * 
 * \return true if over ride is active
 */

bool color_overide_active(uint8_t bank, uint8_t encoder)
{
	uint16_t bit = 0x01 << encoder;
	bool active = false;
	active = (bit & switch_color_overide[bank]) ? true : false; 
	return active;
}

// This function assumes bank == current bank
bool encoder_midi_type_is_relative(uint8_t encoder) 
{
	uint8_t banked_encoder_idx = encoder + encoder_bank*PHYSICAL_ENCODERS;				
	if (encoder_settings[banked_encoder_idx].encoder_midi_type == SEND_REL_ENC){
		return true;
	}
	else{
		return false;
	}
}
