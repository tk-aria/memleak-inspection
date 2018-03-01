#include <Windows.h>
#include <new>

#ifdef _DEBUG
#define _configuration "d"
#else
#define _configuration ""
#endif // _DEBUG

#pragma comment(lib, _configuration "MemoryLeakBuster.lib")

void main(int argc, char* argv[])
{
	
	auto i = new int;

	auto k = new bool;

	auto j = new float;

	delete i;
	//delete k;
	//delete j;

	return;
}