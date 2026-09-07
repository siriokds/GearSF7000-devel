/*
 * GearSF7000 - SC-3000/SF-7000 Emulator
 * Copyright (C) 2026  Saverio Russo

 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/
 *
 */

#ifndef GUI_DEBUG_CONSTANTS_H
#define	GUI_DEBUG_CONSTANTS_H

static const int gui_debug_symbols_count = 119; // 9;

static const char* gui_debug_symbols[gui_debug_symbols_count] = {
 "00:0008 RST_00"
,"00:0008 API_08"
,"00:0010 API_10"
,"00:0018 API_18"
,"00:0020 API_20"
,"00:0028 API_28_UTILITY"
,"00:0030 API_30"
,"00:0066 nmi_handler"
,"00:0069 self_test_redirector"
,"00:006c boot_system_redirector"
,"00:006f self_test_passed_redirector"
,"00:0072 do_nothing_redirector"
,"00:0075 self_test_error_beep_1"
,"00:0079 self_test_error_beep_2"
,"00:007d self_test_error_beep_3"
,"00:007f self_test_error_beep"
,"00:00b6 self_test"
,"00:0160 check_ram"
,"00:0191 install_ram_code"
,"00:01bd ram_code_1"
,"00:01c1 ram_code_1_end"
,"00:01c1 ram_code_2"
,"00:01c8 ram_code_2_end"
,"00:01c8 self_test_passed"
,"00:01cd boot_system"
,"00:0222 msg_ipl_loading"
,"00:0247 show_msg_disk_error"
,"00:0257 show_msg_disk_not_ready"
,"00:0260 msg_cannot_read_disk"
,"00:029e msg_disk_not_ready"
,"00:02cf show_msg_not_sys_disk"
,"00:02d8 msg_not_sys_disk"
,"00:031f wait_for_space_then_boot"
,"00:0329 do_nothing"
,"00:032a sc3k_utility"
,"00:0375 sc3k_utility_functions"
,"00:037b msg_sc3k_utility"
,"00:0398 sc3k_utility_disk_format"
,"00:0402 format_exit_error"
,"00:040c msg_disk_formatting"
,"00:0428 msg_new_disk"
,"00:044c msg_format_done"
,"00:0462 msg_format_error"
,"00:047c sc3k_utility_disk_copy"
,"00:0484 sc3k_utility_disk_copy_copy_chunk"
,"00:04f9 sc3k_utility_disk_copy_error"
,"00:0503 msg_disk_copy"
,"00:0519 msg_source_disk"
,"00:054a msg_dest_disk"
,"00:0580 msg_copy_done"
,"00:0594 msg_copy_error"
,"00:05ac read_line"
,"00:05f5 read_line_bs"
,"00:0605 read_line_cr"
,"00:0612 read_key"
,"00:065d read_key_end"
,"00:0661 table_shifted_chars"
,"00:06a1 table_unshifted_chars"
,"00:06e1 write_text"
,"00:06f1 write_char"
,"00:0744 write_newline"
,"00:0765 write_newline_scroll_screen"
,"00:07b3 write_formfeed"
,"00:07d2 write_backspace"
,"00:07dd write_backspace_wrap_to_previous_line"
,"00:07ed write_backspace_reached_top_left"
,"00:07f9 delete_char_from_tilemap"
,"00:081d write_char_exit"
,"00:0824 long_pause"
,"00:0832 setup_vdp_registers"
,"00:08a6 read_vdp_status"
,"00:08a9 set_vdp_register"
,"00:08b3 set_vram_read_address_to_hl"
,"00:08be wait_and_read_from_vdp"
,"00:08c6 set_vram_write_address_to_hl"
,"00:08d3 wait_and_output_a_to_vdp"
,"00:08da at_key_just_pressed"
,"00:08ff read_key_buffer"
,"00:0939 raw_read_from_keyboard"
,"00:0947 pause_for_ppi"
,"00:094a beep_a_times"
,"00:0968 wait_51ms"
,"00:0974 pause_loop_over_c"
,"00:097c font_data"
,"00:107c font_data_end"
,"00:107c disk_initialise"
,"00:109f disk_initialise_loop"
,"00:10f2 disk_initialise_error"
,"00:10f5 fdc_specification_data"
,"00:10f8 fdc_specification_data_end"
,"00:10f8 read_from_disk"
,"00:1122 read_from_disk_error"
,"00:1127 read_track_c_sector_b_to_de"
,"00:1143 try_raw_read_sector_from_disk"
,"00:115e write_to_disk"
,"00:1189 write_sector_from_de_to_fdc_track_c_sector_b"
,"00:11a5 write_256_bytes_from_hl_to_fdc_when_ready"
,"00:11c0 test_read_sector_from_fdc_track_c_sector_b"
,"00:11d9 test_read_256_bytes_from_fdc_when_ready"
,"00:11f2 format_disk"
,"00:11fc format_track_c"
,"00:1242 format_disk_error"
,"00:1245 do_disk_format"
,"00:125d output_64_bytes_from_hl_to_fdc"
,"00:1278 seek_track_c"
,"00:1289 output_no_data_fdc_command_and_get_result"
,"00:129f return_carry_set"
,"00:12a3 calibrate_and_seek_to_track_0"
,"00:12b4 flush_fdc_status"
,"00:12bd write_cmd_to_fdc"
,"00:12c2 wcmdfdc_1"
,"00:12c8 wcmdfdc_2"
,"00:12d8 write_cmd_to_fdc_error"
,"00:12de fill_fdc_command_data_buffer"
,"00:12fc fill_fdc_command_result_buffer"
,"00:1321 stop_disk"
,"00:1340 msg_disk_not_ready2"
,"00:1396 wait_for_space_and_u_then_boot"
,"00:13aa do_nothing2"

    //,
    //"00:1F61 PLAY_SONGS",
    //"00:1F64 ACTIVATEP",
    //"00:1F67 PUTOBJP",
    //"00:1F6A REFLECT_VERTICAL",
    //"00:1F6D REFLECT_HORIZONTAL",
    //"00:1F70 ROTATE_90",
    //"00:1F73 ENLARGE",
    //"00:1F76 CONTROLLER_SCAN",
    //"00:1F79 DECODER",
    //"00:1F7C GAME_OPT",
    //"00:1F7F LOAD_ASCII",
    //"00:1F82 FILL_VRAM",
    //"00:1F85 MODE_1",
    //"00:1F88 UPDATE_SPINNER",
    //"00:1F8B INIT_TABLEP",
    //"00:1F8E GET_VRAMP",
    //"00:1F91 PUT_VRAMP",
    //"00:1F94 INIT_SPR_ORDERP",
    //"00:1F97 WR_SPR_NM_TBLP",
    //"00:1F9A INIT_TIMERP",
    //"00:1F9D FREE_SIGNALP",
    //"00:1FA0 REQUEST_SIGNALP",
    //"00:1FA3 TEST_SIGNALP",
    //"00:1FA6 WRITE_REGISTERP",
    //"00:1FA9 WRITE_VRAMP",
    //"00:1FAC READ_VRAMP",
    //"00:1FAF INIT_WRITERP",
    //"00:1FB2 SOUND_INITP",
    //"00:1FB5 PLAY_ITP",
    //"00:1FB8 INIT_TABLE",
    //"00:1FBB GET_VRAM",
    //"00:1FBE PUT_VRAM",
    //"00:1FC1 INIT_SPR_ORDER",
    //"00:1FC4 WR_SPR_NM_TBL",
    //"00:1FC7 INIT_TIMER",
    //"00:1FCA FREE_SIGNAL",
    //"00:1FCD REQUEST_SIGNAL",
    //"00:1FD0 TEST_SIGNAL",
    //"00:1FD3 TIME_MGR",
    //"00:1FD6 TURN_OFF_SOUND",
    //"00:1FD9 WRITE_REGISTER",
    //"00:1FDC READ_REGISTER",
    //"00:1FDF WRITE_VRAM",
    //"00:1FE2 READ_VRAM",
    //"00:1FE5 INIT_WRITER",
    //"00:1FE8 WRITER",
    //"00:1FEB POLLER",
    //"00:1FEE SOUND_INIT",
    //"00:1FF1 PLAY_IT",
    //"00:1FF4 SOUND_MAN",
    //"00:1FF7 ACTIVATE",
    //"00:1FFA PUTOBJ",
    //"00:1FFD RAND_GEN",
    //"00:8021 NMI_INT_VECTOR"
};

#endif	/* GUI_DEBUG_CONSTANTS_H */