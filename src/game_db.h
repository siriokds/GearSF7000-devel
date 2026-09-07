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

#ifndef GAME_DB_H
#define	GAME_DB_H

enum GC_GameDBMode
{
	GC_GameDBMode_None			= 0,
	GC_GameDBMode_SRAM			= 1,
	GC_GameDBMode_SG1000		= 2,
	GC_GameDBMode_SG1000_16K	= 4,
	GC_GameDBMode_SC3000		= 8,
	GC_GameDBMode_SC3000_32K	= 16,
	GC_GameDBMode_SF7000		= 32
};

struct GC_GameDBEntry
{
	u32 crc;
	const char* title;
	GC_GameDBMode mode;
};

const GC_GameDBEntry kGameDatabase[] =
{
	// SF-7000 BIOS
	{0xd8f49994, "SF-7000 IPL (modified)", GC_GameDBMode_SF7000 },
	{0xD76810B8, "SF-7000 IPL"           , GC_GameDBMode_SF7000 },

	// CART SG-1000 16K
	{ 0x2768487C, "MIKE MECH"													, GC_GameDBMode_SG1000_16K },

	{ 0x6370BB6D, "SV-1001 - BurgerTime"                                        , GC_GameDBMode_SG1000_16K },
	{ 0x9C7C85C6, "SV-1002 - DonkeyKong"                                        , GC_GameDBMode_SG1000_16K },
	{ 0x79EC38F5, "SV-1003 - UpNDown"                                           , GC_GameDBMode_SG1000_16K },
	{ 0xA914D095, "SV-1004 - Turbo"                                             , GC_GameDBMode_SG1000_16K },
	{ 0xD0E606A0, "SV-1005 - TimePilot"                                         , GC_GameDBMode_SG1000_16K },
	{ 0x386AE8AD, "SV-1006 - Antarctic Adventure"                               , GC_GameDBMode_SG1000_16K },
	{ 0x9B17A746, "SV-1007 - LunarBall"                                         , GC_GameDBMode_SG1000_16K },
	{ 0x444CCDD5, "SV-1008 - Bosconian"                                         , GC_GameDBMode_SG1000_16K },
	{ 0x294B1797, "SV-1009 - KonamiSoccer"                                      , GC_GameDBMode_SG1000_16K },
	{ 0xC006B6C7, "SV-1010 - TheGoonies"                                        , GC_GameDBMode_SG1000_16K },
	{ 0xAEBA2CFA, "SV-1011 - GuruLogic"                                         , GC_GameDBMode_SG1000_16K },
	{ 0x7141E8B6, "SV-1012 - ZanacAI"                                           , GC_GameDBMode_SG1000_16K },
	{ 0x9400FF27, "SV-1013 - ExoideZArea5"                                      , GC_GameDBMode_SG1000_16K },
	{ 0xF16FBA66, "SV-1014 - GommyMedievalDefender"                             , GC_GameDBMode_SG1000_16K },
	{ 0x430210DB, "SV-1015 - MagicalTree"                                       , GC_GameDBMode_SG1000_16K },
	{ 0x62B8FB04, "SV-1016 - Pippols"                                           , GC_GameDBMode_SG1000_16K },
	{ 0xBAE09273, "SV-1017 - SHMUP!"                                            , GC_GameDBMode_SG1000_16K },
	{ 0xAC7ACE50, "SV-1018 - InqAndSuq"                                         , GC_GameDBMode_SG1000_16K },
	{ 0x049C3D07, "SV-1019 - BombermanSpecialDeluxe"                            , GC_GameDBMode_SG1000_16K },
	{ 0x5F4020E7, "SV-1020 - Twinbee"                                           , GC_GameDBMode_SG1000_16K },
	{ 0x2401C704, "SV-1021 - Knightmare"                                        , GC_GameDBMode_SG1000_16K },
	{ 0x84669EB7, "SV-1022 - Yie Ar Kung-Fu (2 Players)"                        , GC_GameDBMode_SG1000_16K },
	{ 0x2C555D36, "SV-1023 - Yie Ar Kung-Fu II - The Emperor Yie-Gah"           , GC_GameDBMode_SG1000_16K },
	{ 0x37749E51, "SV-1024 - KonamiTennis"                                      , GC_GameDBMode_SG1000_16K },
	{ 0x98133EF7, "SV-1025 - GojiraKun"                                         , GC_GameDBMode_SG1000_16K },
	{ 0xA2D8362D, "SV-1026 - InspecteurZ"                                       , GC_GameDBMode_SG1000_16K },
	{ 0x1628A29D, "SV-1027 - HoleInOne"                                         , GC_GameDBMode_SG1000_16K },
	{ 0x644D7A2B, "SV-1028 - KonamiGolf"                                        , GC_GameDBMode_SG1000_16K },
	{ 0xB6B4544D, "SV-1029 - StarSoldier"                                       , GC_GameDBMode_SG1000_16K },
	{ 0xE094CC70, "SV-1030 - IgaNinpouten2"                                     , GC_GameDBMode_SG1000_16K },
	{ 0x72631F13, "SV-1031 - FinalJustice"                                      , GC_GameDBMode_SG1000_16K },
	{ 0xE89AA951, "SV-1032 - MobilePlanetSuthirus"                              , GC_GameDBMode_SG1000_16K },
	{ 0xE968C8F3, "SV-1033 - Caos Begins"                                       , GC_GameDBMode_SG1000_16K },
	{ 0xEED52D4E, "SV-1034 - HadesuNoMonsho"                                    , GC_GameDBMode_SG1000_16K },
	{ 0x7C3859B8, "SV-1035 - Malaika (Relevo)"                                  , GC_GameDBMode_SG1000_16K },
	{ 0x479D69B3, "SV-1036 - Mecha 8"                                           , GC_GameDBMode_SG1000_16K },
	{ 0xE88DC126, "SV-1037 - Subacuatic"                                        , GC_GameDBMode_SG1000_16K },
	{ 0x2C66C07C, "SV-1038 - Ninja Savior"                                      , GC_GameDBMode_SG1000_16K },
	{ 0x0354EBE8, "SV-1039 - Arkanoid"                                          , GC_GameDBMode_SG1000_16K },
	{ 0x50F3E178, "SV-1040 - Shouganai"                                         , GC_GameDBMode_SG1000_16K },
	{ 0xDE6A5EBC, "SV-1041 - TheStoneOfWisdom"                                  , GC_GameDBMode_SG1000_16K },
	{ 0x7FE3DBA8, "SV-1042 - Dinj Belmonte's revenge"                           , GC_GameDBMode_SG1000_16K },
	{ 0xB1BA5E95, "SV-1043 - Tetris (Uttum)"                                    , GC_GameDBMode_SG1000_16K },
	{ 0x8F5157A5, "SV-1044 - Youkai Yashiki - Ghost House"                      , GC_GameDBMode_SG1000_16K },
	{ 0x1DCE5BC3, "SV-1045 - Mr.Do!"                                            , GC_GameDBMode_SG1000_16K },
	{ 0x4CE7A04D, "SV-1046 - Mopiranger - Konami (1985)"                        , GC_GameDBMode_SG1000_16K },
	{ 0x1BEC5D7F, "SV-1047 - NightKnight"                                       , GC_GameDBMode_SG1000_16K },
	{ 0x8ED95E65, "SV-1048 - Duckstroma pt2 (r2)"                               , GC_GameDBMode_SG1000_16K },
	{ 0x6B63EA20, "SV-1049 - Guardic"                                           , GC_GameDBMode_SG1000_16K },
	{ 0x6D67B192, "SV-1050 - Menace"                                            , GC_GameDBMode_SG1000_16K },
	{ 0xCAC1990A, "SV-1051 - Pipi"                                              , GC_GameDBMode_SG1000_16K },
	{ 0xB0713A56, "SV-1052 - Stray Cat"                                         , GC_GameDBMode_SG1000_16K },
	{ 0x30270070, "SV-1053 - Heroes Arena"                                      , GC_GameDBMode_SG1000_16K },
	{ 0xFD4BE274, "SV-1054 - Malaika - Prehistoric Quest"                       , GC_GameDBMode_SG1000_16K },
	{ 0x0E1C6772, "SV-1055 - Dig Dug"                                           , GC_GameDBMode_SG1000_16K },
	{ 0x019F3319, "SV-1056 - Rally-X"                                           , GC_GameDBMode_SG1000_16K },
	{ 0x806699C5, "SV-1057 - King & Balloon"                                    , GC_GameDBMode_SG1000_16K },
	{ 0xEC297EB0, "SV-1058 - Pyramid Warp (Enhanced Plus)"                      , GC_GameDBMode_SG1000_16K },
	{ 0x020F0E6A, "SV-1059 - Invasion of the Zombie Monsters (r2)"              , GC_GameDBMode_SG1000_16K },
	{ 0xB54D3663, "SV-1060 - Draconic Throne"                                   , GC_GameDBMode_SG1000_16K },
	{ 0xF56A21C7, "SV-1061 - Comic Bakery"                                      , GC_GameDBMode_SG1000_16K },
	{ 0x96DA6E58, "SV-1062 - Griel's Quest For The Sangraal"                    , GC_GameDBMode_SG1000_16K },
	{ 0x38AB1EE3, "SV-1063 - Buddhagillie"                                      , GC_GameDBMode_SG1000_16K },
	{ 0x5569C3AC, "SV-1064 - Super Tennis"                                      , GC_GameDBMode_SG1000_16K },
	{ 0xCEA1D84D, "SV-1065 - Gun.Smoke"                                         , GC_GameDBMode_SG1000_16K },
	{ 0x18CAB455, "SV-1066 - Teodoro no sabe volar"                             , GC_GameDBMode_SG1000_16K },
	{ 0xC5D09DD1, "SV-1067 - Super Columns"                                     , GC_GameDBMode_SG1000_16K },
	{ 0xF1626130, "SV-1068 - Pretty kingdom"                                    , GC_GameDBMode_SG1000_16K },
	{ 0x4475C252, "SV-1069 - Mars II"                                           , GC_GameDBMode_SG1000_16K },

	// MyCard SG
	{ 0xD8A87095, "Bank Panic (Japan) (1985) (C-53)"                            , GC_GameDBMode_SG1000 },
	{ 0x26ECD094, "Black Onyx, The (Japan) (1987) (C-72)"                       , GC_GameDBMode_SG1000 },
	{ 0xEA0F2691, "Bomb Jack (Japan) (1985) (C-61)"                             , GC_GameDBMode_SG1000 },
	{ 0xD37BDA49, "Chack'n Pop (Japan) (1985) (C-52)"                           , GC_GameDBMode_SG1000 },
	{ 0x62B21E31, "Champion Billiards (Japan) (Compile 1986) (C-71)"            , GC_GameDBMode_SG1000 },
	{ 0xBDC05652, "Champion Ice Hockey (Japan) (1985) (C-59)"                   , GC_GameDBMode_SG1000 },
	{ 0x10CDEBCE, "Champion Kendo (Japan) (1986) (C-67)"                        , GC_GameDBMode_SG1000 },
	{ 0x11DB4B1D, "Championship Lode Runner (Japan) (Compile 1985) (C-57)"      , GC_GameDBMode_SG1000 },
	{ 0x732B7180, "Choplifter (Japan) (Compile 1985) (C-48)"                    , GC_GameDBMode_SG1000 },
	{ 0xBE7ED0EB, "C_So! (Japan) (Compile 1985) (C-64)"                         , GC_GameDBMode_SG1000 },
	{ 0x346556B9, "Doki Doki Penguin Land (Japan) (1985) (C-50)"                , GC_GameDBMode_SG1000 },
	{ 0x99C3DE21, "Dragon Wang (Japan) (1985) (C-46)"                           , GC_GameDBMode_SG1000 },
	{ 0x288940CB, "Drol (Japan) (1985) (C-51)"                                  , GC_GameDBMode_SG1000 },
	{ 0x5AF8F69D, "Elevator Action (Japan) (1985) (C-55)"                       , GC_GameDBMode_SG1000 },
	{ 0x15A754A3, "Gulkave (Japan) (Compile 1986) (C-63)"                       , GC_GameDBMode_SG1000 },
	{ 0x11090ABC, "Gulkave (Japan) (Compile 1986) (C-63).sg"                    , GC_GameDBMode_SG1000 },
	{ 0x7133FBDD, "Gulkave (Japan) (Compile 1986) (C-63)_logoblue"              , GC_GameDBMode_SG1000 },
	{ 0x15A754A3, "Gulkave (Japan) (Compile 1986) (C-63)_logoblue.sg"           , GC_GameDBMode_SG1000 },
	{ 0x4587DE6E, "H.E.R.O. (Japan) (1985) (C-58)"                              , GC_GameDBMode_SG1000 },
	{ 0x9BE3C6BD, "Hang-On II (Japan) (1985) (C-60)"                            , GC_GameDBMode_SG1000 },
	{ 0x02E5D66A, "Monaco GP (Japan) (Rev 2) (1985) (C-17)"                     , GC_GameDBMode_SG1000 },
	{ 0x3B912408, "Ninja Princess (Japan) (1986) (C-65)"                        , GC_GameDBMode_SG1000 },
	{ 0x37FCA2EB, "Pitfall II - The Lost Caverns (Japan) (1985) (C-49)"         , GC_GameDBMode_SG1000 },
	{ 0x0FFDD03D, "Rock n' Bolt (Japan) (1985) (C-54)"                          , GC_GameDBMode_SG1000 },
	{ 0x922C5468, "Sokoban (Japan) (1985) (C-56)"                               , GC_GameDBMode_SG1000 },
	{ 0x084CC13E, "Super Tank (Japan) (1986) (C-66)"                            , GC_GameDBMode_SG1000 },
	{ 0x160535C5, "Wonder Boy (Japan) (1986) (C-69)"                            , GC_GameDBMode_SG1000 },
	{ 0xBC5D20DF, "Zippy Race (Japan) (1985) (G-1026)"                          , GC_GameDBMode_SG1000 },
	{ 0x093830D8, "Zoom 909 (Japan) (1985) (C-46)"                              , GC_GameDBMode_SG1000 },

	// CART SG
	{ 0x4916112D, "Astrododge"                                                  , GC_GameDBMode_SG1000 },
	{ 0x0B4BCA74, "Borderline (Japan, Europe) (Compile 1984) (G-1001)"          , GC_GameDBMode_SG1000 },
	{ 0x092F29D6, "Castle, The (Japan) (1986) (G-1046)"                         , GC_GameDBMode_SG1000 },
	{ 0x5970A12B, "Champion Baseball (Japan) (16kB) (1983) (G-1011)"            , GC_GameDBMode_SG1000 },
	{ 0x26F947D1, "Champion Boxing (Japan) (1984) (G-1033)"                     , GC_GameDBMode_SG1000 },
	{ 0x868419B5, "Champion Golf (Japan) (1984) (G-1005)"                       , GC_GameDBMode_SG1000 },
	{ 0x372FE6BC, "Champion Pro Wrestling (Japan) (1985) (G-1039)"              , GC_GameDBMode_SG1000 },
	{ 0x6F39719E, "Champion Soccer (Japan) (1984) (G-1034)"                     , GC_GameDBMode_SG1000 },
	{ 0x7C663316, "Champion Tennis (Japan) (1983) (G-1009)"                     , GC_GameDBMode_SG1000 },
	{ 0x5EB48A20, "Congo Bongo (Japan) (1983) (G-1007)"                         , GC_GameDBMode_SG1000 },
	{ 0xA2C45B61, "Exerion (Japan, Europe) (1984) (G-1028)"                     , GC_GameDBMode_SG1000 },
	{ 0xBD24D27B, "Flicky (Japan) (1984) (G-1036)"                              , GC_GameDBMode_SG1000 },
	{ 0x1898F274, "Girl's Garden (Japan) (1985) (G-1037)"                       , GC_GameDBMode_SG1000 },
	{ 0x0D159ED0, "Golgo 13 (Japan) (1984) (G-1014)"                            , GC_GameDBMode_SG1000 },
	{ 0x942ADF84, "GP World (Japan) (1985) (G-1040)"                            , GC_GameDBMode_SG1000 },
	{ 0xC9D1AE7D, "Home Mahjong (Japan) (1984) (G-1030)"                        , GC_GameDBMode_SG1000 },
	{ 0xA627D440, "Hustle Chumy (Japan) (Compile 1984) (G-1035)"                , GC_GameDBMode_SG1000 },
	{ 0xBA09A0FD, "Hyper Sports (Japan) (1985) (G-1042)"                        , GC_GameDBMode_SG1000 },
	{ 0x00ED3970, "Lode Runner (Japan, Europe) (1984) (G-1031)"                 , GC_GameDBMode_SG1000 },
	{ 0x6D909857, "Mahjong (Japan) (1983) (G-1004)"                             , GC_GameDBMode_SG1000 },
	{ 0x8572D73A, "Monaco GP (Japan) (1983) (G-1017)"                           , GC_GameDBMode_SG1000 },
	{ 0x09196FC5, "N-Sub (Europe) (Compile 1983) (G-1003)"                      , GC_GameDBMode_SG1000 },
	{ 0x652BBD1E, "N-Sub (Japan) (Compile 1983) (G-1003)"                       , GC_GameDBMode_SG1000 },
	{ 0x3E371769, "N-Sub (Taiwan) (Compile 1983) (G-1003)"                      , GC_GameDBMode_SG1000 },
	{ 0xF4F78B76, "Orguss (Japan, Europe) (1984) (G-1015)"                      , GC_GameDBMode_SG1000 },
	{ 0xAF4F14BC, "Othello (Japan) (1985) (G-1044)"                             , GC_GameDBMode_SG1000 },
	{ 0x19949375, "Pacar (Japan) (40kB) (1983) (G-1020)"                        , GC_GameDBMode_SG1000 },
	{ 0x326587E1, "Pachinko (Japan) (1983) (G-1027)"                            , GC_GameDBMode_SG1000 },
	{ 0xFD7CB50A, "Pachinko II (Japan) (1984) (G-1029)"                         , GC_GameDBMode_SG1000 },
	{ 0xDB6404BA, "Pop Flamer (Japan, Europe) (1983) (G-1019)"                  , GC_GameDBMode_SG1000 },
	{ 0x49E9718B, "Safari Hunting (Japan) (Compile 1983) (G-1002)"              , GC_GameDBMode_SG1000 },
	{ 0x619DD066, "Safari Race (Europe) (1984) (G-1032)"                        , GC_GameDBMode_SG1000 },
	{ 0xFD76AD99, "Sega Flipper (Japan) (40kB) (1983) (G-1018)"                 , GC_GameDBMode_SG1000 },
	{ 0x981E36C1, "Sega-Galaga (Japan) (16kB) (1983) (G-1022)"                  , GC_GameDBMode_SG1000 },
	{ 0x545FC9BB, "Serizawa Hachidan no Tsume Shogi (Japan) (1983) (G-1006)"    , GC_GameDBMode_SG1000 },
	{ 0x207E7E99, "SG-1000 M2 Test Cartridge (Japan)"                           , GC_GameDBMode_SG1000 },
	{ 0x5A917E06, "Shinnyu Shain Toru-kun (Japan) (1985) (G-1041)"              , GC_GameDBMode_SG1000 },
	{ 0x01932DF9, "Sindbad Mystery (Japan, Europe) (1984) (G-1012)"             , GC_GameDBMode_SG1000 },
	{ 0x6AD5CB3D, "Space Invaders (Japan) (1985) (G-1045)"                      , GC_GameDBMode_SG1000 },
	{ 0xB8B58B30, "Space Slalom (Japan) (1983) (G-1023)"                        , GC_GameDBMode_SG1000 },
	{ 0xB846B52A, "Star Force (Japan) (1985) (G-1043)"                          , GC_GameDBMode_SG1000 },
	{ 0x1AE94122, "Star Jacker (Japan) (1983) (G-1010)"                         , GC_GameDBMode_SG1000 },
	{ 0xDD4A661B, "Terebi Oekaki (Japan) (GB-800)"                              , GC_GameDBMode_SG1000 },
	{ 0x9C7497FF, "Yamato (Japan) (40kB) (1983) (G-1008)"                       , GC_GameDBMode_SG1000 },
	{ 0x905467E4, "Zaxxon (Japan) (1985) (G-1038)"                              , GC_GameDBMode_SG1000 },

	// CART TAIWAN
	{ 0xCE5648C3, "Bomberman Special (Taiwan) (DahJee)"                         , GC_GameDBMode_SG1000 },
	{ 0x042C36BA, "Flipper (Taiwan)"                                            , GC_GameDBMode_SG1000 },
	{ 0x845BBB22, "Galaga (Taiwan)"                                             , GC_GameDBMode_SG1000 },
	{ 0x223397A1, "King's Valley (Taiwan)"                                      , GC_GameDBMode_SG1000 },
	{ 0x281D2888, "Knightmare (Taiwan)"                                         , GC_GameDBMode_SG1000 },
	{ 0x2E7166D5, "Legend of Kage, The (Taiwan)"                                , GC_GameDBMode_SG1000 },
	{ 0xFFC4EE3F, "Magical Kid Wiz (Taiwan)"                                    , GC_GameDBMode_SG1000 },
	{ 0x4573F5BC, "Orguss (Taiwan)"                                             , GC_GameDBMode_SG1000 },
	{ 0xDF7CBFA5, "Pippols (Taiwan)"                                            , GC_GameDBMode_SG1000 },
	{ 0x476A079B, "Pitfall II (Taiwan) (Chinese Logo)"                          , GC_GameDBMode_SG1000 },
	{ 0x306D5F78, "Rally-X (Taiwan) (DahJee)"                                   , GC_GameDBMode_SG1000 },
	{ 0x29E047CC, "Road Fighter (Taiwan) (Jumbo)"                               , GC_GameDBMode_SG1000 },
	{ 0xE0816BB7, "Star Soldier (Taiwan) (GA006)"                               , GC_GameDBMode_SG1000 },
	{ 0x5CBD1163, "Tank Battalion (Taiwan) (GR-014)"                            , GC_GameDBMode_SG1000 },
	{ 0xC550B4F0, "TwinBee (Taiwan) (GB-003)"                                   , GC_GameDBMode_SG1000 },
	{ 0xFC87463C, "Yie Ar Kung-Fu II (Taiwan) (GA008)"                          , GC_GameDBMode_SG1000 },

	// CART OTHELLO MULTIVISION
	{ 0xA8B5B57F, "007 James Bond (Japan) (v2.6) (Tsukuda Original 1984) (OM-G008)"                 , GC_GameDBMode_SG1000 },
	{ 0xC91551DA, "Challenge Derby (Japan) (vA) (16kB) (Tsukuda Original 1984) (OM-G005)"           , GC_GameDBMode_SG1000 },
	{ 0x61FA9EA0, "Guzzler (Japan) (Tsukuda Original 1983) (OM-G002)"                               , GC_GameDBMode_SG1000 },
	{ 0x547DD7FD, "Okamoto Ayako no Match Play Golf (Japan) (Tsukuda Original 1984) (OM-G006)"      , GC_GameDBMode_SG1000 },
	{ 0x77DB4704, "Q-bert (Japan) (Tsukuda Original 1983) (OM-G001)"                                , GC_GameDBMode_SG1000 },
	{ 0x885FA64D, "San-nin Mahjong (Japan) (Tsukuda Original 1984) (OM-G004)"                       , GC_GameDBMode_SG1000 },
	{ 0xD23B0E3E, "Space Armor (Japan) (v1.0) (Tsukuda Original 1984) (OM-G007)"                    , GC_GameDBMode_SG1000 },
	{ 0xBBD87D8F, "Space Mountain (Japan) (Tsukuda Original 1984) (OM-G003)"                        , GC_GameDBMode_SG1000 },
	{ 0xC5A67B95, "[BIOS] Othello Multivision (Japan)"                                              , GC_GameDBMode_SG1000 },


	// CART SC-3000
	{ 0xF691F9C7, "Sega BASIC Level II B (Japan) (SC-3000) (B-20)"                                  , GC_GameDBMode_SC3000 },
	{ 0x5D9F11CA, "Sega BASIC Level III B (Export) (SC-3000) (B-40)"                                , GC_GameDBMode_SC3000_32K },
	{ 0x3EE9E3B1, "Sega BASIC Level III B (Italian) (SC-3000) (B-40)"                               , GC_GameDBMode_SC3000_32K },
	{ 0x345C8BC8, "Chuugaku Hisshuu Eibunpou (Chuugaku 1-Nen) (Japan) (SC-3000) (E-105)"            , GC_GameDBMode_SC3000 },
	{ 0x9BAFD9E9, "Chuugaku Hisshuu Eisakubun (Chuugaku 1-Nen) (Japan) (16kB) (SC-3000) (E-105)"    , GC_GameDBMode_SC3000 },
	{ 0x6CCB297A, "Chuugaku Hisshuu Eitango (Chuugaku 1-Nen) (Japan) (SC-3000) (E-103)"             , GC_GameDBMode_SC3000 },
	{ 0x78A37CBC, "Home Basic (Japan) (SC-3000) (B-50)"                                             , GC_GameDBMode_SC3000 },
	{ 0xF9C81FE1, "Kagaku (Gensokigou Master) (Japan) (SC-3000) (E-107)"                            , GC_GameDBMode_SC3000 },
	{ 0xD5E919C1, "LinkWord (A) (SC-3000)"                                                          , GC_GameDBMode_SC3000 },
	{ 0x2EC28526, "Music (Japan) (SC-3000) (E-101)"                                                 , GC_GameDBMode_SC3000 },
	{ 0x129D6359, "Nihonshi Nenpyou (Japan) (SC-3000) (E-108)"                                      , GC_GameDBMode_SC3000 },
	{ 0x3274EE48, "Sekaishi Nenpyou (Japan) (SC-3000) (E-109)"                                      , GC_GameDBMode_SC3000 },
	{ 0x5FDE25BB, "Tanoshii Sansuu (Shougaku 4-Nen Ge) (Japan) (SC-3000) (E-113)"                   , GC_GameDBMode_SC3000 },
	{ 0x7C400E3B, "Tanoshii Sansuu (Shougaku 4-Nen Jou) (J) (SC-3000) (40K) (E-106)"                , GC_GameDBMode_SC3000 },
	{ 0x6A96978D, "Tanoshii Sansuu (Shougaku 5-Nen Ge) (SC-3000) (E-114)"                           , GC_GameDBMode_SC3000 },
	{ 0x419D75D5, "Tanoshii Sansuu (Shougaku 5-Nen Jou) (J) (SC-3000) (E-116)"                      , GC_GameDBMode_SC3000 },
	{ 0xD0D18E70, "Tanoshii Sansuu (Shougaku 6-Nen Ge) (J) (SC-3000) (E-115)"                       , GC_GameDBMode_SC3000 },
	{ 0x8B5E6E1B, "Tanoshii Sansuu (Shougaku 6-Nen Jou) (J) (SC-3000) (E-117)"                      , GC_GameDBMode_SC3000 },
	{ 0xAE4F92CF, "Uranai Angel Cutie (Japan) (SC-3000) (E-119)"                                    , GC_GameDBMode_SC3000 },

	{0, 0, GC_GameDBMode_None}
};

// “CRC-32/ISO-HDLC” o “CRC-32/ADCCP”
const uint32_t kCRC32_tab[] = 
{
	0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f,
	0xe963a535, 0x9e6495a3,	0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988,
	0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91, 0x1db71064, 0x6ab020f2,
	0xf3b97148, 0x84be41de,	0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
	0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec,	0x14015c4f, 0x63066cd9,
	0xfa0f3d63, 0x8d080df5,	0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172,
	0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b,	0x35b5a8fa, 0x42b2986c,
	0xdbbbc9d6, 0xacbcf940,	0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
	0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116, 0x21b4f4b5, 0x56b3c423,
	0xcfba9599, 0xb8bda50f, 0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924,
	0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d,	0x76dc4190, 0x01db7106,
	0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
	0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818, 0x7f6a0dbb, 0x086d3d2d,
	0x91646c97, 0xe6635c01, 0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e,
	0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457, 0x65b0d9c6, 0x12b7e950,
	0x8bbeb8ea, 0xfcb9887c, 0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
	0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2, 0x4adfa541, 0x3dd895d7,
	0xa4d1c46d, 0xd3d6f4fb, 0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0,
	0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9, 0x5005713c, 0x270241aa,
	0xbe0b1010, 0xc90c2086, 0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
	0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4, 0x59b33d17, 0x2eb40d81,
	0xb7bd5c3b, 0xc0ba6cad, 0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a,
	0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683, 0xe3630b12, 0x94643b84,
	0x0d6d6a3e, 0x7a6a5aa8, 0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
	0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe, 0xf762575d, 0x806567cb,
	0x196c3671, 0x6e6b06e7, 0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc,
	0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5, 0xd6d6a3e8, 0xa1d1937e,
	0x38d8c2c4, 0x4fdff252, 0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
	0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60, 0xdf60efc3, 0xa867df55,
	0x316e8eef, 0x4669be79, 0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236,
	0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f, 0xc5ba3bbe, 0xb2bd0b28,
	0x2bb45a92, 0x5cb36a04, 0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
	0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a, 0x9c0906a9, 0xeb0e363f,
	0x72076785, 0x05005713, 0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38,
	0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21, 0x86d3d2d4, 0xf1d4e242,
	0x68ddb3f8, 0x1fda836e, 0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
	0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c, 0x8f659eff, 0xf862ae69,
	0x616bffd3, 0x166ccf45, 0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2,
	0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db, 0xaed16a4a, 0xd9d65adc,
	0x40df0b66, 0x37d83bf0, 0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
	0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6, 0xbad03605, 0xcdd70693,
	0x54de5729, 0x23d967bf, 0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94,
	0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d
};

u32 CalculateCRC32(u32 crc, const u8 *buf, int size)
{
	const u8 *p;

	p = buf;
	crc = crc ^ ~0U;

	while (size--)
		crc = kCRC32_tab[(crc ^ *p++) & 0xFF] ^ (crc >> 8);

	return crc ^ ~0U;
}

#endif /* GAME_DB_H */


