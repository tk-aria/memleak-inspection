// メモリリーク検出器。
// この .cpp をプロジェクトに含めるだけで有効になり、プログラム終了時にリーク領域の確保時のコールスタックをデバッグ出力に表示します。
// 
// oerator new / delete をオーバーライドすることで実現しています。
// そのため、malloc() などの global new を介さないメモリ確保は捕捉できませんし、
// 別モジュール (dll) のメモリ確保も捕捉できません。
// 
// 
// 環境を選ぶけどより強い検出力を備えたバージョン:
// https://github.com/i-saint/scribble/blob/master/MemoryLeakBuster2.cpp
// 
// Author i-saint (ishibashi) & T.K-Aria
// ___

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif // _CRT_SECURE_NO_WARNINGS

const size_t MinimumAlignment = 16;
const size_t MaxCallstackDepth = 32;
const size_t MaxStringBuffer = 512;

#pragma warning(disable: 4073) // init_seg(lib) は普通は使っちゃダメ的な warning。正当な理由があるので黙らせる
#pragma warning(disable: 4996) // _s じゃない CRT 関数使うとでるやつ
#pragma warning(disable: 4091)
#pragma init_seg(lib) // global オブジェクトの初期化の優先順位上げる

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "psapi.lib")

#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
#include <string>
#include <vector>
#include <map>
//#include <unordered_map> // なぜかエラーが出る
#include <algorithm>
namespace stl = std;

// windows.h の悪さ対策
#ifdef max
#undef  max
#undef  min
#endif // max


template<size_t N>
inline int istsprintf(char (&buf)[N], const char *format, ...)
{
    va_list vl;
    va_start(vl, format);
    int r = _vsnprintf(buf, N, format, vl);
    va_end(vl);
    return r;
}

template<size_t N>
inline int istvsprintf(char (&buf)[N], const char *format, va_list vl)
{
    return _vsnprintf(buf, N, format, vl);
}

#define istPrint(...) DebugPrint(__FILE__, __LINE__, __VA_ARGS__)

static const int DPRINTF_MES_LENGTH  = 4096;
void DebugPrintV(const char* /*file*/, int /*line*/, const char* fmt, va_list vl)
{
    char buf[DPRINTF_MES_LENGTH];
    //istsprintf(buf, "%s:%d - ", file, line);
    //::OutputDebugStringA(buf);
    //WriteLogFile(buf);
    istvsprintf(buf, fmt, vl);
    ::OutputDebugStringA(buf);
}

void DebugPrint(const char* file, int line, const char* fmt, ...)
{
    va_list vl;
    va_start(vl, fmt);
    DebugPrintV(file, line, fmt, vl);
    va_end(vl);
}



bool InitializeDebugSymbol()
{
    if(!::SymInitialize(::GetCurrentProcess(), NULL, TRUE)) {
        return false;
    }
    ::SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);

    return true;
}

void FinalizeDebugSymbol()
{
    ::SymCleanup(::GetCurrentProcess());
}


int GetCallstack(void **callstack, int callstack_size, int skip_size)
{
    return CaptureStackBackTrace(skip_size, callstack_size, callstack, NULL);
}

stl::string AddressToSymbolName(void *address)
{
#ifdef _WIN64
    typedef DWORD64 DWORDX;
    typedef PDWORD64 PDWORDX;
#else
    typedef DWORD DWORDX;
    typedef PDWORD PDWORDX;
#endif

    char buf[1024];
    HANDLE process = ::GetCurrentProcess();
    IMAGEHLP_MODULE imageModule = { sizeof(IMAGEHLP_MODULE) };
    IMAGEHLP_LINE line ={sizeof(IMAGEHLP_LINE)};
    DWORDX dispSym = 0;
    DWORD dispLine = 0;

    char symbolBuffer[sizeof(IMAGEHLP_SYMBOL) + MAX_PATH] = {0};
    IMAGEHLP_SYMBOL * imageSymbol = (IMAGEHLP_SYMBOL*)symbolBuffer;
    imageSymbol->SizeOfStruct = sizeof(IMAGEHLP_SYMBOL);
    imageSymbol->MaxNameLength = MAX_PATH;

    if(!::SymGetModuleInfo(process, (DWORDX)address, &imageModule)) {
        istsprintf(buf, "[0x%p]\n", address);
    }
    else if(!::SymGetSymFromAddr(process, (DWORDX)address, &dispSym, imageSymbol)) {
        istsprintf(buf, "%s + 0x%x [0x%p]\n", imageModule.ModuleName, ((size_t)address-(size_t)imageModule.BaseOfImage), address);
    }
    else if(!::SymGetLineFromAddr(process, (DWORDX)address, &dispLine, &line)) {
        istsprintf(buf, "%s!%s + 0x%x [0x%p]\n", imageModule.ModuleName, imageSymbol->Name, ((size_t)address-(size_t)imageSymbol->Address), address);
    }
    else {
        istsprintf(buf, "%s(%d): %s!%s + 0x%x [0x%p]\n", line.FileName, line.LineNumber,
            imageModule.ModuleName, imageSymbol->Name, ((size_t)address-(size_t)imageSymbol->Address), address);
    }
    return buf;
}

stl::string CallstackToSymbolNames(void **callstack, int callstack_size, int clamp_head=0, int clamp_tail=0, const char *indent="")
{
    stl::string tmp;
    int begin = stl::max<int>(0, clamp_head);
    int end = stl::max<int>(0, callstack_size-clamp_tail);
    for(int i=begin; i<end; ++i) {
        tmp += indent;
        tmp += AddressToSymbolName(callstack[i]);
    }
    return tmp;
}




template<class T>
class ScopedLock
{
public:
    ScopedLock(T &m) : m_mutex(m) { m_mutex.lock(); }

    template<class F>
    ScopedLock(T &m, const F &f) : m_mutex(m)
    {
        while(!m_mutex.tryLock()) { f(); }
    }

    ~ScopedLock() { m_mutex.unlock(); }

private:
    T &m_mutex;

    ScopedLock& operator=(const ScopedLock&);
};
class Mutex
{
public:
    typedef ScopedLock<Mutex> ScopedLock;
    typedef CRITICAL_SECTION Handle;

    Mutex()          { InitializeCriticalSection(&m_lockobj); }
    ~Mutex()         { DeleteCriticalSection(&m_lockobj); }
    void lock()      { EnterCriticalSection(&m_lockobj); }
    bool tryLock()   { return TryEnterCriticalSection(&m_lockobj)==TRUE; }
    void unlock()    { LeaveCriticalSection(&m_lockobj); }

    Handle getHandle() const { return m_lockobj; }

private:
    Handle m_lockobj;
};


// アロケーション情報を格納するコンテナのアロケータが new / delete を使うと永久再起するので、
// malloc()/free() を呼ぶだけのアロケータを用意
template<typename T>
class malloc_allocator {
public : 
    //    typedefs
    typedef T value_type;
    typedef value_type* pointer;
    typedef const value_type* const_pointer;
    typedef value_type& reference;
    typedef const value_type& const_reference;
    typedef std::size_t size_type;
    typedef std::ptrdiff_t difference_type;

public : 
    //    convert an allocator<T> to allocator<U>
    template<typename U>
    struct rebind {
        typedef malloc_allocator<U> other;
    };

public : 
    malloc_allocator() {}
    malloc_allocator(const malloc_allocator&) {}
    template<typename U> malloc_allocator(const malloc_allocator<U>&) {}
    ~malloc_allocator() {}

    pointer address(reference r) { return &r; }
    const_pointer address(const_reference r) { return &r; }

    pointer allocate(size_type cnt, const void *p=NULL) { p; return (pointer)::malloc(cnt * sizeof(T)); }
    void deallocate(pointer p, size_type) { ::free(p); }

    size_type max_size() const { return std::numeric_limits<size_type>::max() / sizeof(T); }

    void construct(pointer p, const T& t) { new(p) T(t); }
    void destroy(pointer p) { p; p->~T(); }

    bool operator==(malloc_allocator const&) { return true; }
    bool operator!=(malloc_allocator const& a) { return !operator==(a); }
};
template<class T, typename Alloc> inline bool operator==(const malloc_allocator<T>& l, const malloc_allocator<T>& r) { return (l.equals(r)); }
template<class T, typename Alloc> inline bool operator!=(const malloc_allocator<T>& l, const malloc_allocator<T>& r) { return (!(l == r)); }


class MemoryLeakBusterCustom
{
public:

	typedef stl::basic_string<char, std::char_traits<char>, malloc_allocator<char> > TempString;

	// アロケート時の callstack を保持
    struct AllocInfo
    {
        void *location;
        size_t size;
        void *stack[MaxCallstackDepth];
        int depth;

		bool customInfo;
		int blockUsage;
		char filename[MaxStringBuffer];
		int lineNumber;
    };

	typedef stl::map<void*, AllocInfo, stl::less<void*>, malloc_allocator<stl::pair<const void*, AllocInfo> > > DataTable;
	typedef stl::vector<AllocInfo, malloc_allocator<AllocInfo>> AllocDataList;

	MemoryLeakBusterCustom() 
		: m_enabled(true)
		, allAllocSize(0), allMemoryleakSize(0)
		, allocDataList()
	{
        InitializeDebugSymbol();
    }

    ~MemoryLeakBusterCustom()
	{
        printLeakInfo();
        FinalizeDebugSymbol();
    }

    void enableLeakCheck(bool v) { m_enabled=v; }

    void addAllocationInfo(void *p, size_t size )
    {
        if(!m_enabled) { return; }

		AllocInfo cs = {};
		cs.customInfo = false;
        cs.location = p;
        cs.size = size;
        cs.depth = GetCallstack(cs.stack, _countof(cs.stack), 3);
        {
            Mutex::ScopedLock l(m_mutex);
            m_leakinfo[p] = cs;
        }

		allAllocSize += size;
		allMemoryleakSize += size;
		allocDataList.push_back(cs);

    }

	void addCustomAllocationInfo(void *p, size_t size, int blockUsage,const char* filename, int line)
	{
		if (!m_enabled) { return; }

		AllocInfo cs = {};
		cs.customInfo = true;
		cs.location = p;
		cs.size = size;
		cs.blockUsage = blockUsage;
		sprintf(cs.filename, "%s", filename);
		cs.lineNumber = line;
		cs.depth = GetCallstack(cs.stack, _countof(cs.stack), 3);
		{
			Mutex::ScopedLock l(m_mutex);
			m_leakinfo[p] = cs;
		}

		allAllocSize += size;
		allMemoryleakSize += size;
		allocDataList.push_back(cs);

	}

    void eraseAllocationInfo(void *p)
    {
        Mutex::ScopedLock l(m_mutex);

		auto itr = m_leakinfo.find(p);
		allMemoryleakSize -= itr->second.size;

        m_leakinfo.erase(p);
    }

    void printLeakInfo()
    {

		auto PrintAllocInfo = [&](AllocInfo data,int index) {

			if (data.customInfo) {

				char sourcefilename[MaxStringBuffer] = {};
				char hyperLink[MaxStringBuffer] = {};
				char filename[MaxStringBuffer] = {};
				char extension[MaxStringBuffer] = {};
				_splitpath(data.filename, nullptr, nullptr, filename, extension);
				sprintf(sourcefilename, "%s%s", filename, extension);
				sprintf(hyperLink, "file://%s", data.filename);
				istPrint("→→→ memory alloc data[%d] : address->0x%p (%llu byte) code : %s (line : %d) blockUsage : %d (%s:%d) ←←←  \n",
					index, data.location, (unsigned long long)data.size, sourcefilename, data.lineNumber, data.blockUsage, hyperLink,data.lineNumber);

			}
			else {
				istPrint("→→→ memory alloc data[%d] : address->0x%p (%llu byte) ←←← \n", index, data.location, (unsigned long long)data.size);
			}
		};

		istPrint("--- version info : %s --- \n↓↓\n", label);
		istPrint("--- Memory Alloc Info !! --- \n↓↓\n");
		istPrint("--- all alloc size : %d byte --- \n",(int)allAllocSize);
		istPrint("--- all memory leak size : %d/%d byte --- \n↓↓\n", (int)allMemoryleakSize,(int)allAllocSize);
        
        istPrint("--- All MemoryAlloc Info !! --- \n↓↓\n");
		int index = 0;
		for (auto data : allocDataList) {
			
			PrintAllocInfo(data,index);
			index++;
		}
		istPrint("\n\n");
		
        istPrint("--- Memory Leak Info !! --- \n↓↓\n");
        Mutex::ScopedLock l(m_mutex);
        for(DataTable::iterator i=m_leakinfo.begin(); i!=m_leakinfo.end(); ++i) {
            stl::string text = CallstackToSymbolNames(i->second.stack, i->second.depth);
			PrintAllocInfo(i->second,(int)i->first);
			istPrint(text.c_str());
			OutputDebugStringA(text.c_str());
            istPrint("\n");
        }
    }


private:

	const char* label = "Memory Leak Buster Custom ver1.01";

	unsigned __int64 allAllocSize;
	unsigned __int64 allMemoryleakSize;
    AllocDataList allocDataList;
	//stl::map<void*, size_t,> usedMemoryPointsOfSizeMap;
	//std::unordered_map<void*, size_t> memoryleakPointsOfSizeMap;

    DataTable m_leakinfo;
    Mutex m_mutex;
    bool m_enabled;
};

// global 変数にすることで main 開始前に初期化、main 抜けた後に終了処理をさせる。
// entry point を乗っ取ってもっとスマートにやりたかったが、
// WinMainCRTStartup() は main を呼んだ後 exit() してしまい、main の後にリーク箇所を出力することができないため断念
MemoryLeakBusterCustom g_memory_leak_buster;


// 以下 operator new & delete overload

void* operator new(size_t size)
{
    void *p = _aligned_malloc(size, MinimumAlignment);
    g_memory_leak_buster.addAllocationInfo(p,size);
    return p;
}

void* operator new[](size_t size)
{
    void *p = _aligned_malloc(size, MinimumAlignment);
    g_memory_leak_buster.addAllocationInfo(p,size);
    return p;
}

void* operator new(size_t  size, int _BlockUse, char const* _FileName, int _LineNumber)
{
	void *p = _aligned_malloc(size, MinimumAlignment);
	g_memory_leak_buster.addCustomAllocationInfo(p,size,_BlockUse,_FileName,_LineNumber);
	return p;
}

void* operator new[](size_t  size, int _BlockUse, char const* _FileName, int _LineNumber)
{
	void *p = _aligned_malloc(size, MinimumAlignment);
	g_memory_leak_buster.addCustomAllocationInfo(p, size, _BlockUse, _FileName, _LineNumber);
	return p;
}

void operator delete(void* p)
{
    g_memory_leak_buster.eraseAllocationInfo(p);
    _aligned_free(p);
}

void operator delete[](void* p)
{
    g_memory_leak_buster.eraseAllocationInfo(p);
    _aligned_free(p);
}

// Not Compatible overload funtion @ 多分使う人がいないので未対応
/*
void operator delete(
	void*       _Block,
	int         _BlockUse,
	char const* _FileName,
	int         _LineNumber )
{

}

void operator delete[](
	void*       _Block,
	int         _BlockUse,
	char const* _FileName,
	int         _LineNumber )
{

}
*/


