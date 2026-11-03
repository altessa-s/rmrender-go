/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* Per-face, per-size width calibration, the same table as rmrender-swift's
 * Calibration.swift; no device font assets. */

#include "rmr_font_calibration.h"

const rmr_stretch_entry rmr_stretches[] = {
{"reMarkableSans-Regular",32,0.9687},
{"reMarkableSans-Bold",32,0.9528},
{"reMarkableSans-Bold",36,0.9711},
{"reMarkableSans-RegularItalic",32,1.0364},
{"reMarkableSans-RegularItalic",36,1.0333},
{"reMarkableSans-Medium",36,0.9764},
{"reMarkableSerifSmall-Regular",46,1.1445},
{"reMarkableSerifSmall-Regular",64,1.1425},
{"reMarkableSerifSmall-Bold",46,1.0967},
{"reMarkableSerifSmall-Bold",64,1.0966},
{"reMarkableSerifSmall-RegularItalic",46,1.1172},
{"reMarkableSerifSmall-RegularItalic",64,1.1177},
{"NotoSans-Regular",32,0.9983},
{"NotoSans-Regular",46,0.9983},
{"NotoSans-Regular",64,0.9983},
{"NotoSans-Bold",32,1.0},
{"NotoSans-Bold",36,1.0123},
{"NotoSans-Bold",46,1.0},
{"NotoSans-Bold",64,1.0},
{"NotoSans-Medium",36,0.9983},
{"NotoSans-Light",64,0.9983},
{"NotoSansKR-Regular",32,1.0},
{"NotoSansKR-Regular",46,1.0},
{"NotoSansKR-Regular",64,1.0},
{"NotoSansKR-Medium",36,1.0},
{"NotoSansKR-Bold",32,1.0},
{"NotoSansKR-Light",64,1.0},
{"NotoSansJP-Regular",32,0.9986},
{"NotoSansJP-Regular",46,0.9986},
{"NotoSansJP-Regular",64,0.9986},
{"NotoSansJP-Medium",36,0.9986},
{"NotoSansJP-Bold",32,0.9986},
{"NotoSansJP-Light",64,0.9986},
{"NotoSansThaiLooped-Regular",32,1.0},
{"NotoSansThaiLooped-Regular",46,1.0},
{"NotoSansThaiLooped-Regular",64,1.0},
{"NotoSansThaiLooped-Medium",36,1.0},
{"NotoSansThaiLooped-Bold",32,1.0},
{"NotoSansThaiLooped-Bold",36,1.0139},
{"NotoSansThaiLooped-Bold",46,1.0},
{"NotoSansThaiLooped-Bold",64,1.0},
{"NotoSansThaiLooped-Light",64,1.0},
{"NotoSansHebrew-Regular",32,1.0},
{"NotoSansHebrew-Regular",46,1.0},
{"NotoSansHebrew-Regular",64,1.0},
{"NotoSansHebrew-Medium",36,1.0},
{"NotoSansHebrew-Bold",32,1.0},
{"NotoSansHebrew-Bold",36,1.0194},
{"NotoSansHebrew-Bold",46,1.0},
{"NotoSansHebrew-Bold",64,1.0},
{"NotoSansHebrew-Light",64,1.0},
{"NotoSansArabic-Regular",32,1.0},
{"NotoSansArabic-Medium",36,1.0},
{"NotoSansArabic-Bold",32,0.9950},
{"NotoSansArabic-Bold",36,1.0237},
{"NotoSansArabic-Bold",46,0.9950},
{"NotoSansArabic-Bold",64,0.9950},
{"NotoSansArabic-Light",64,1.0},
{"NotoSansDevanagari-Regular",32,1.0},
{"NotoSansDevanagari-Regular",46,1.0},
{"NotoSansDevanagari-Regular",64,1.0},
{"NotoSansDevanagari-Medium",36,1.0},
{"NotoSansDevanagari-Bold",32,1.0},
{"NotoSansDevanagari-Bold",36,1.0230},
{"NotoSansDevanagari-Bold",46,1.0},
{"NotoSansDevanagari-Bold",64,1.0},
{"NotoSansDevanagari-Light",64,1.0},
};
const size_t rmr_stretch_count = sizeof rmr_stretches / sizeof *rmr_stretches;
double rmr_noto_glyph_share(uint16_t c) { switch(c) {
case 0x20: return -0.0234;
default: return 0; } }
double rmr_sans_glyph_share(uint16_t c) { switch(c) {
case 0x31: return -0.1313;
case 0x20ac: return 0.1217;
case 0x43: return 0.0890;
case 0xdf: return 0.0785;
case 0x63: return 0.0732;
case 0x10d: return 0.0731;
case 0x30: return 0.0567;
case 0x2c: return -0.0470;
case 0x67: return -0.0468;
case 0x72: return -0.0453;
case 0x2e: return -0.0447;
case 0x38: return 0.0426;
case 0x2014: return -0.0417;
case 0x6d: return -0.0391;
case 0x29: return 0.0376;
case 0x161: return 0.0364;
case 0x73: return 0.0354;
case 0x2d: return 0.0343;
case 0x36: return 0.0325;
case 0x50: return 0.0321;
case 0x28: return 0.0320;
case 0x2f: return 0.0295;
case 0xf3: return -0.0255;
case 0xf6: return -0.0242;
case 0x34: return 0.0235;
case 0x32: return 0.0229;
case 0x35: return 0.0216;
case 0x6c: return 0.0187;
case 0x33: return 0.0186;
case 0x6f: return -0.0181;
case 0xbb: return 0.0173;
case 0x76: return 0.0171;
case 0x68: return -0.0163;
case 0x79: return 0.0162;
case 0x75: return -0.0159;
case 0xfc: return -0.0159;
case 0xfa: return -0.0159;
case 0x20: return -0.0153;
case 0x6e: return -0.0144;
default: return 0; } }
double rmr_serif_glyph_share(uint16_t c) { switch(c) {
case 0xbb: return -0.2912;
case 0x54: return -0.2255;
case 0x201d: return -0.1635;
case 0x201c: return -0.1616;
case 0x2014: return -0.1590;
case 0x56: return -0.1403;
case 0x47: return -0.1224;
case 0x4c: return -0.1217;
case 0xe6: return 0.1206;
case 0x4b: return -0.1171;
case 0x201e: return -0.1038;
case 0x51: return -0.1003;
case 0x15f: return 0.0862;
case 0x15b: return 0.0842;
case 0x5a: return -0.0842;
case 0xdf: return 0.0831;
case 0x161: return 0.0828;
case 0x73: return 0.0821;
case 0x2e: return -0.0800;
case 0x2c: return -0.0670;
case 0xe0: return 0.0648;
case 0x103: return 0.0648;
case 0xe2: return 0.0648;
case 0xe5: return 0.0647;
case 0x101: return 0.0645;
case 0xe1: return 0.0641;
case 0x28: return -0.0640;
case 0x105: return 0.0639;
case 0x61: return 0.0632;
case 0xe4: return 0.0621;
case 0x119: return 0.0609;
case 0x27: return -0.0603;
case 0x17a: return 0.0598;
case 0x65: return 0.0587;
case 0x17e: return 0.0586;
case 0x17c: return 0.0565;
case 0xeb: return 0.0548;
case 0x7a: return 0.0548;
case 0xe9: return 0.0544;
case 0x142: return -0.0532;
case 0x50: return -0.0524;
case 0xea: return 0.0488;
case 0x77: return -0.0445;
case 0x66: return -0.0435;
case 0x29: return -0.0432;
case 0x171: return -0.0379;
case 0xfc: return -0.0364;
case 0x75: return -0.0361;
case 0xfa: return -0.0355;
case 0x16b: return -0.0355;
case 0x69: return -0.0333;
case 0xed: return -0.0332;
case 0xef: return -0.0331;
case 0xee: return -0.0331;
case 0x12b: return -0.0331;
case 0x131: return -0.0331;
case 0x3a: return -0.0293;
case 0x67: return 0.0277;
case 0x6c: return -0.0275;
case 0x111: return 0.0266;
case 0x64: return 0.0265;
case 0x2d: return 0.0259;
case 0x10f: return 0.0257;
case 0xfd: return -0.0245;
case 0x74: return -0.0242;
case 0x72: return -0.0239;
case 0x6e: return -0.0219;
case 0x107: return 0.0197;
case 0x144: return -0.0197;
case 0x148: return -0.0196;
case 0x10d: return 0.0192;
case 0xe7: return 0.0186;
case 0x62: return 0.0174;
case 0x63: return 0.0172;
case 0x165: return -0.0164;
case 0x76: return -0.0126;
case 0x6d: return -0.0125;
case 0x6a: return -0.0115;
case 0x6f: return 0.0110;
case 0xf8: return 0.0105;
default: return 0; } }
double rmr_sans_bold_glyph_share(uint16_t c) { switch(c) {
case 0x30: return 0.0835;
case 0x10d: return 0.0735;
case 0x63: return 0.0735;
case 0x50: return 0.0649;
case 0x161: return 0.0507;
case 0x73: return 0.0500;
case 0x32: return 0.0483;
case 0x67: return -0.0386;
case 0x72: return -0.0245;
case 0x2c: return -0.0245;
case 0x6d: return -0.0232;
case 0x2e: return -0.0218;
case 0x6f: return -0.0213;
case 0x6b: return -0.0211;
case 0x20: return -0.0197;
case 0x79: return 0.0194;
case 0x6c: return 0.0166;
case 0x75: return -0.0160;
case 0x54: return 0.0156;
case 0x68: return -0.0151;
case 0x6e: return -0.0131;
case 0x6a: return -0.0120;
case 0x74: return -0.0117;
case 0x69: return -0.0115;
case 0x17e: return 0.0112;
case 0xf3: return -0.0104;
case 0xf6: return -0.0100;
case 0x76: return 0.0078;
case 0x70: return -0.0051;
case 0x62: return 0.0017;
case 0x77: return 0.0012;
case 0x64: return -0.0006;
default: return 0; } }
double rmr_sans_medium_glyph_share(uint16_t c) { switch(c) {
case 0x72: return -0.0317;
case 0x6f: return -0.0271;
case 0x20: return -0.0245;
default: return 0; } }
double rmr_sans_italic_glyph_share(uint16_t c) { switch(c) {
case 0x63: return 0.0674;
case 0x67: return -0.0534;
case 0x72: return -0.0511;
case 0x73: return 0.0495;
case 0x6d: return -0.0419;
case 0x20: return -0.0324;
case 0x65: return 0.0323;
case 0x6f: return -0.0220;
case 0x69: return -0.0171;
case 0x75: return -0.0167;
case 0x6e: return -0.0162;
case 0x68: return -0.0158;
case 0x74: return 0.0130;
default: return 0; } }
double rmr_serif_bold_glyph_share(uint16_t c) { switch(c) {
case 0x73: return 0.0703;
case 0x72: return -0.0638;
case 0x65: return 0.0596;
case 0x61: return 0.0563;
case 0x66: return -0.0509;
case 0x69: return -0.0472;
case 0x74: return -0.0458;
case 0x63: return 0.0435;
case 0x6f: return 0.0412;
case 0x20: return -0.0410;
case 0x2c: return -0.0404;
case 0x6c: return -0.0362;
case 0x70: return 0.0239;
case 0x71: return 0.0237;
case 0x6b: return -0.0189;
case 0x6d: return 0.0144;
case 0x6e: return -0.0129;
default: return 0; } }
double rmr_serif_italic_glyph_share(uint16_t c) { switch(c) {
case 0xbb: return -0.3413;
case 0x4b: return -0.1838;
case 0x54: return -0.1778;
case 0xe6: return 0.1750;
case 0x56: return -0.1654;
case 0x4c: return -0.1312;
case 0x5a: return -0.1290;
case 0x6f: return 0.1022;
case 0x2014: return -0.1010;
case 0xf3: return 0.1004;
case 0xf6: return 0.1003;
case 0xf8: return 0.1003;
case 0x151: return 0.1001;
case 0xf4: return 0.0997;
case 0x201d: return -0.0992;
case 0xf5: return 0.0991;
case 0x201c: return -0.0935;
case 0x47: return -0.0910;
case 0x201e: return -0.0835;
case 0x15f: return 0.0822;
case 0x161: return 0.0796;
case 0x15b: return 0.0772;
case 0x70: return 0.0726;
case 0x142: return -0.0721;
case 0x66: return -0.0709;
case 0x73: return 0.0702;
case 0x6c: return -0.0696;
case 0xe7: return 0.0693;
case 0x107: return 0.0679;
case 0x10d: return 0.0675;
case 0x63: return 0.0624;
case 0x105: return 0.0613;
case 0x2e: return -0.0612;
case 0x2c: return -0.0593;
case 0x6b: return -0.0587;
case 0x131: return -0.0576;
case 0x69: return -0.0569;
case 0x72: return -0.0569;
case 0x119: return 0.0551;
case 0xea: return 0.0517;
case 0xef: return -0.0509;
case 0xee: return -0.0509;
case 0x12b: return -0.0509;
case 0x28: return -0.0504;
case 0xed: return -0.0503;
case 0xeb: return 0.0496;
case 0xe9: return 0.0486;
case 0x65: return 0.0485;
case 0x50: return -0.0484;
case 0x137: return -0.0457;
case 0x62: return 0.0421;
case 0x13e: return -0.0386;
case 0x27: return -0.0372;
case 0x6a: return -0.0365;
case 0x51: return -0.0318;
case 0x6d: return -0.0318;
case 0x17c: return -0.0302;
case 0x7a: return -0.0293;
case 0x17e: return -0.0288;
case 0x17a: return -0.0286;
case 0x74: return -0.0269;
case 0x78: return -0.0262;
case 0x3b: return 0.0256;
case 0x29: return -0.0251;
case 0x67: return 0.0197;
case 0xe0: return 0.0189;
case 0x2d: return 0.0187;
case 0x6e: return -0.0168;
case 0xe2: return 0.0168;
case 0x71: return 0.0153;
case 0x103: return 0.0152;
case 0x64: return -0.0150;
case 0x21: return -0.0147;
case 0xe5: return 0.0146;
case 0xe1: return 0.0139;
case 0xe4: return 0.0131;
case 0xfa: return 0.0115;
case 0x16b: return 0.0115;
case 0x101: return 0.0112;
case 0x68: return 0.0107;
case 0x20: return -0.0106;
default: return 0; } }
