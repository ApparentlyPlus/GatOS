#define HH_KERNEL_OFFSET 0xFFFFFF8000000000

#ifdef __ASSEMBLER__
#define V2P(a) ((a) - HH_KERNEL_OFFSET)
#define P2V(a) ((a) + HH_KERNEL_OFFSET)
#else
#include <stdint.h>
#define V2P(a) ((uintptr_t)(a) & ~HH_KERNEL_OFFSET)
#define P2V(a) ((uintptr_t)(a) | HH_KERNEL_OFFSET)
#endif