// Mini-shim Arduino pour compiler/tester la logique du sketch sur PC (-DHOST_TEST)
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

extern uint32_t g_millis;                       // horloge virtuelle (avance avec l'audio écrit)
static inline uint32_t millis() { return g_millis; }
static inline void delay(uint32_t ms) { g_millis += ms; }
static inline long random(long n) { return rand() % n; }
static inline long random(long a, long b) { return a + rand() % (b - a); }
static inline void randomSeed(unsigned long s) { srand(s); }

struct SerialShim {
  void printf(const char *f, ...) { va_list a; va_start(a, f); vprintf(f, a); va_end(a); }
  void println(const char *s) { puts(s); }
};
static SerialShim Serial;
