// Shim: only errors are printed.
#pragma once
#include <cstdio>
#define ERROR_LOG(type, ...) (std::fprintf(stderr, __VA_ARGS__), std::fputc('\n', stderr))
#define WARN_LOG(type, ...) ((void)0)
#define INFO_LOG(type, ...) ((void)0)
#define DEBUG_LOG(type, ...) ((void)0)
