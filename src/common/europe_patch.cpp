// Embedded europe.patch for Linux
#include <cstdint>

#ifndef _WIN32
__asm__(
    ".section .rodata\n"
    ".global kEuropePatchStart\n"
    ".global kEuropePatchEnd\n"
    ".type kEuropePatchStart, @object\n"
    ".type kEuropePatchEnd, @object\n"
    "kEuropePatchStart:\n"
    ".incbin \"europe.patch\"\n"
    "kEuropePatchEnd:\n"
);
#endif
