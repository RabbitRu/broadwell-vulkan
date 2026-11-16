#pragma once
/* Microsoft C++ enums are signed even when every enumerator is nonnegative.
 * ELK's narrow enum bitfields require the unsigned semantics used on Linux.
 * Clang supports fixed underlying enums in C and C++; keep their size at 32
 * bits so both languages retain the same Microsoft bitfield layout. */
#if defined(_WIN32) && defined(__clang__)
#define ELK_ENUM_BASE : unsigned int
#define ELK_COMPARE_FUNC_BITS 4
#else
#define ELK_ENUM_BASE
#define ELK_COMPARE_FUNC_BITS 3
#endif
