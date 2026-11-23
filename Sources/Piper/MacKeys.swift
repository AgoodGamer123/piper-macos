// macOS virtual key codes (kVK_*, NSEvent.keyCode) -> Linux key codes,
// which is what libratbag stores in key and macro actions.

let macToEvdev: [UInt16: UInt32] = [
    0x00: 30,  // KEY_A
    0x01: 31,  // KEY_S
    0x02: 32,  // KEY_D
    0x03: 33,  // KEY_F
    0x04: 35,  // KEY_H
    0x05: 34,  // KEY_G
    0x06: 44,  // KEY_Z
    0x07: 45,  // KEY_X
    0x08: 46,  // KEY_C
    0x09: 47,  // KEY_V
    0x0A: 86,  // KEY_102ND (ISO section key)
    0x0B: 48,  // KEY_B
    0x0C: 16,  // KEY_Q
    0x0D: 17,  // KEY_W
    0x0E: 18,  // KEY_E
    0x0F: 19,  // KEY_R
    0x10: 21,  // KEY_Y
    0x11: 20,  // KEY_T
    0x12: 2,  // KEY_1
    0x13: 3,  // KEY_2
    0x14: 4,  // KEY_3
    0x15: 5,  // KEY_4
    0x16: 7,  // KEY_6
    0x17: 6,  // KEY_5
    0x18: 13,  // KEY_EQUAL
    0x19: 10,  // KEY_9
    0x1A: 8,  // KEY_7
    0x1B: 12,  // KEY_MINUS
    0x1C: 9,  // KEY_8
    0x1D: 11,  // KEY_0
    0x1E: 27,  // KEY_RIGHTBRACE
    0x1F: 24,  // KEY_O
    0x20: 22,  // KEY_U
    0x21: 26,  // KEY_LEFTBRACE
    0x22: 23,  // KEY_I
    0x23: 25,  // KEY_P
    0x24: 28,  // KEY_ENTER
    0x25: 38,  // KEY_L
    0x26: 36,  // KEY_J
    0x27: 40,  // KEY_APOSTROPHE
    0x28: 37,  // KEY_K
    0x29: 39,  // KEY_SEMICOLON
    0x2A: 43,  // KEY_BACKSLASH
    0x2B: 51,  // KEY_COMMA
    0x2C: 53,  // KEY_SLASH
    0x2D: 49,  // KEY_N
    0x2E: 50,  // KEY_M
    0x2F: 52,  // KEY_DOT
    0x30: 15,  // KEY_TAB
    0x31: 57,  // KEY_SPACE
    0x32: 41,  // KEY_GRAVE
    0x33: 14,  // KEY_BACKSPACE
    0x35: 1,  // KEY_ESC
    0x36: 126,  // KEY_RIGHTMETA (right command)
    0x37: 125,  // KEY_LEFTMETA (command)
    0x38: 42,  // KEY_LEFTSHIFT
    0x39: 58,  // KEY_CAPSLOCK
    0x3A: 56,  // KEY_LEFTALT (option)
    0x3B: 29,  // KEY_LEFTCTRL
    0x3C: 54,  // KEY_RIGHTSHIFT
    0x3D: 100,  // KEY_RIGHTALT (right option)
    0x3E: 97,  // KEY_RIGHTCTRL
    0x40: 187,  // KEY_F17
    0x41: 83,  // KEY_KPDOT
    0x43: 55,  // KEY_KPASTERISK
    0x45: 78,  // KEY_KPPLUS
    0x47: 69,  // KEY_NUMLOCK (keypad clear)
    0x48: 115,  // KEY_VOLUMEUP
    0x49: 114,  // KEY_VOLUMEDOWN
    0x4A: 113,  // KEY_MUTE
    0x4B: 98,  // KEY_KPSLASH
    0x4C: 96,  // KEY_KPENTER
    0x4E: 74,  // KEY_KPMINUS
    0x4F: 188,  // KEY_F18
    0x50: 189,  // KEY_F19
    0x51: 117,  // KEY_KPEQUAL
    0x52: 82,  // KEY_KP0
    0x53: 79,  // KEY_KP1
    0x54: 80,  // KEY_KP2
    0x55: 81,  // KEY_KP3
    0x56: 75,  // KEY_KP4
    0x57: 76,  // KEY_KP5
    0x58: 77,  // KEY_KP6
    0x59: 71,  // KEY_KP7
    0x5A: 190,  // KEY_F20
    0x5B: 72,  // KEY_KP8
    0x5C: 73,  // KEY_KP9
    0x5D: 124,  // KEY_YEN
    0x5E: 89,  // KEY_RO
    0x5F: 95,  // KEY_KPJPCOMMA
    0x60: 63,  // KEY_F5
    0x61: 64,  // KEY_F6
    0x62: 65,  // KEY_F7
    0x63: 61,  // KEY_F3
    0x64: 66,  // KEY_F8
    0x65: 67,  // KEY_F9
    0x66: 94,  // KEY_MUHENKAN (eisu)
    0x67: 87,  // KEY_F11
    0x68: 93,  // KEY_KATAKANAHIRAGANA (kana)
    0x69: 183,  // KEY_F13 (also Print Screen on PC keyboards)
    0x6A: 186,  // KEY_F16
    0x6B: 184,  // KEY_F14 (also Scroll Lock on PC keyboards)
    0x6D: 68,  // KEY_F10
    0x6E: 127,  // KEY_COMPOSE (context menu)
    0x6F: 88,  // KEY_F12
    0x71: 185,  // KEY_F15 (also Pause on PC keyboards)
    0x72: 110,  // KEY_INSERT ("help" on Apple keyboards, Insert on PC ones)
    0x73: 102,  // KEY_HOME
    0x74: 104,  // KEY_PAGEUP
    0x75: 111,  // KEY_DELETE
    0x76: 62,  // KEY_F4
    0x77: 107,  // KEY_END
    0x78: 60,  // KEY_F2
    0x79: 109,  // KEY_PAGEDOWN
    0x7A: 59,  // KEY_F1
    0x7B: 105,  // KEY_LEFT
    0x7C: 106,  // KEY_RIGHT
    0x7D: 108,  // KEY_DOWN
    0x7E: 103,  // KEY_UP
]
