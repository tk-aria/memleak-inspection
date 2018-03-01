#pragma once
#ifndef __MemoryLeakBusterInclude__
#define __MemoryLeakBusterInclude__

#define _CRTDBG_MAP_ALLOC
#include <cstdlib>
#include <crtdbg.h>
#define new new(_NORMAL_BLOCK, __FILE__, __LINE__)

#endif // __MemoryLeakBusterInclude__