/** @file common.h
 *  @brief General purpose macros.
 */

#ifndef __COMMON_H__
#define __COMMON_H__

#include <stdio.h>
#include <unistd.h>
#include <setjmp.h>
#include <chrono>
#include <string>
#include <vector>
#include "config.h"
#include "printf.h"
#include "stl-model.h"

extern int model_out;

extern std::chrono::time_point<std::chrono::system_clock> start_time;

extern jmp_buf test_jmpbuf;

// define a new struct containing size and strings for stack trace
struct stack_trace_struct {
	int sz;
	char** strings;
};


#define model_print(fmt, ...) do { \
		char mprintbuf[2048];                                                \
		int printbuflen=snprintf_(mprintbuf, 2048, fmt, ## __VA_ARGS__);     \
		int lenleft = printbuflen < 2048 ? printbuflen : 2048;                   \
		int totalwritten = 0; \
		while(lenleft) {                                                    \
			int byteswritten=write(model_out, &mprintbuf[totalwritten], lenleft); \
			lenleft-=byteswritten;                                            \
			totalwritten+=byteswritten;                                       \
		}                                                                   \
} while (0)

#ifdef CONFIG_DEBUG
#define DEBUG(fmt, ...) do { model_print("*** %15s:%-4d %25s() *** " fmt, __FILE__, __LINE__, __func__, ## __VA_ARGS__); } while (0)
#define DBG() DEBUG("\n")
#define DBG_ENABLED() (1)
#else
#define DEBUG(fmt, ...)
#define DBG()
#define DBG_ENABLED() (0)
#endif

void assert_hook(void);

#ifdef CONFIG_ASSERT
#define ASSERT(expr) \
	do { \
		if (!(expr)) { \
			fprintf(stderr, "### Assertion error in %s at line %d\n", __FILE__, __LINE__); \
			/* print_trace(); // Trace printing may cause dynamic memory allocation */ \
			assert_hook();                           \
			model_print("Or attach gdb to process with id # %u\n", getpid());               \
			model_print("Model checker internal assertion triggered, continue execution... \n");      \
			siglongjmp(test_jmpbuf, 1); \
		} \
	} while (0)
#else
#define ASSERT(expr) \
	do { } while (0)
#endif	/* CONFIG_ASSERT */

#define error_msg(...) fprintf(stderr, "Error: " __VA_ARGS__)

stack_trace_struct get_trace(void);
void print_trace(void);
// std::shared_ptr<std::vector<void *>> save_stack_trace();
#endif	/* __COMMON_H__ */
