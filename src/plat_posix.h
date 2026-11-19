/* The Win32 subset Samplosis uses, on POSIX (macOS). Included through plat.h, never on Windows.

   - Types and constants as Windows defines them (LONG and DWORD are 32-bit).
   - Handles: one tagged object for events, threads, files and folder searches, so WaitForSingleObject,
     WaitForMultipleObjects and CloseHandle work on all of them.
   - Wide-character paths are UTF-32 here (wchar_t is 4 bytes); every call that reaches the file system converts to
     UTF-8 and turns backslashes into slashes, so paths built the Windows way ("dir\\name") work unchanged.
   - Overlapped reads complete at once (pread), which Win32 allows: ReadFile returns TRUE, the event is set and
     GetOverlappedResult reports the count.
   - SRW locks are mutexes (a shared acquire takes it exclusively), so condition variables can wait on them. */
#ifndef SMP_PLAT_POSIX_H
#define SMP_PLAT_POSIX_H
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wchar.h>
#include <wctype.h>
#include <pthread.h>
#include <sched.h>
#ifdef __cplusplus
extern "C" {
#endif

/* ---- types */
/* LONG is long, as Windows' LONG and long are one type there (prototypes mix them); DWORD stays 32-bit */
typedef long LONG;
typedef unsigned long ULONG;
typedef uint32_t DWORD, UINT;
typedef int64_t LONG64, LONGLONG, INT64;
typedef uint64_t ULONG64, ULONGLONG, DWORD64, UINT64, DWORDLONG;
#ifndef __OBJC__
typedef int BOOL;              /* (Objective-C has its own BOOL; the window code passes none to these calls) */
#endif
typedef uint8_t BYTE, BOOLEAN;
typedef uint16_t WORD;
typedef int16_t SHORT;
typedef char CHAR;
typedef wchar_t WCHAR;
typedef size_t SIZE_T;
typedef ptrdiff_t SSIZE_T;
typedef intptr_t INT_PTR, LONG_PTR;
typedef uintptr_t UINT_PTR, ULONG_PTR, DWORD_PTR;
typedef void VOID, *PVOID, *LPVOID;
typedef const void *LPCVOID;
typedef DWORD *LPDWORD, *PDWORD;
typedef LONG *PLONG;
typedef wchar_t *LPWSTR, *PWSTR;
typedef const wchar_t *LPCWSTR, *PCWSTR;
typedef char *LPSTR;
typedef const char *LPCSTR;
typedef BOOL *LPBOOL;
typedef struct SmpHandle *HANDLE;
typedef HANDLE *PHANDLE;
typedef void *HMODULE, *HINSTANCE;
typedef union { struct { DWORD LowPart; LONG HighPart; }; struct { DWORD LowPart; LONG HighPart; } u; LONGLONG QuadPart; } LARGE_INTEGER, *PLARGE_INTEGER;
typedef union { struct { DWORD LowPart; DWORD HighPart; }; struct { DWORD LowPart; DWORD HighPart; } u; ULONGLONG QuadPart; } ULARGE_INTEGER, *PULARGE_INTEGER;
typedef struct { DWORD dwLowDateTime, dwHighDateTime; } FILETIME, *PFILETIME, *LPFILETIME;
typedef struct { DWORD nLength; LPVOID lpSecurityDescriptor; BOOL bInheritHandle; } SECURITY_ATTRIBUTES, *LPSECURITY_ATTRIBUTES;
typedef struct { ULONG_PTR Internal, InternalHigh; union { struct { DWORD Offset, OffsetHigh; }; PVOID Pointer; }; HANDLE hEvent; } OVERLAPPED, *LPOVERLAPPED;
typedef DWORD (*LPTHREAD_START_ROUTINE)(LPVOID);

#define WINAPI
#define CALLBACK
#define APIENTRY
#define TRUE 1
#define FALSE 0
#ifndef MAX_PATH
#define MAX_PATH 1024
#endif
#define INFINITE 0xFFFFFFFFu
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)

/* ---- errors (GetLastError) */
#define ERROR_SUCCESS 0
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_PATH_NOT_FOUND 3
#define ERROR_TOO_MANY_OPEN_FILES 4
#define ERROR_ACCESS_DENIED 5
#define ERROR_INVALID_HANDLE 6
#define ERROR_NOT_ENOUGH_MEMORY 8
#define ERROR_OUTOFMEMORY 14
#define ERROR_INVALID_DRIVE 15
#define ERROR_NO_MORE_FILES 18
#define ERROR_NOT_READY 21
#define ERROR_SHARING_VIOLATION 32
#define ERROR_LOCK_VIOLATION 33
#define ERROR_HANDLE_EOF 38
#define ERROR_HANDLE_DISK_FULL 39
#define ERROR_NOT_SUPPORTED 50
#define ERROR_BAD_NETPATH 53
#define ERROR_NETNAME_DELETED 64
#define ERROR_UNEXP_NET_ERR 59
#define ERROR_BAD_NET_NAME 67
#define ERROR_FILE_EXISTS 80
#define ERROR_INVALID_PARAMETER 87
#define ERROR_BROKEN_PIPE 109
#define ERROR_DISK_FULL 112
#define ERROR_SEM_TIMEOUT 121
#define ERROR_INSUFFICIENT_BUFFER 122
#define ERROR_INVALID_NAME 123
#define ERROR_DIR_NOT_EMPTY 145
#define ERROR_ALREADY_EXISTS 183
#define ERROR_FILENAME_EXCED_RANGE 206
#define ERROR_NO_UNICODE_TRANSLATION 1113
#define ERROR_OPERATION_ABORTED 995
#define ERROR_IO_INCOMPLETE 996
#define ERROR_IO_PENDING 997
#define ERROR_NETWORK_UNREACHABLE 1231
#define ERROR_HOST_UNREACHABLE 1232
#define ERROR_TIMEOUT 1460
DWORD GetLastError(void);
void SetLastError(DWORD e);

/* ---- strings */
#define CP_ACP 0
#define CP_OEMCP 1          /* the DOS code page 437 (old zip file names) */
#define CP_UTF8 65001
#define MB_ERR_INVALID_CHARS 0x8
#define WC_ERR_INVALID_CHARS 0x80
#define CSTR_LESS_THAN 1
#define CSTR_EQUAL 2
#define CSTR_GREATER_THAN 3
int MultiByteToWideChar(UINT cp, DWORD flags, const char *s, int n, wchar_t *w, int cap);
int WideCharToMultiByte(UINT cp, DWORD flags, const wchar_t *w, int n, char *s, int cap, const char *def, BOOL *used);
int CompareStringOrdinal(const wchar_t *a, int na, const wchar_t *b, int nb, BOOL ignore_case);
LPWSTR CharLowerW(LPWSTR s);
DWORD CharLowerBuffW(LPWSTR s, DWORD n);
#define _snwprintf swprintf
#define _TRUNCATE ((size_t)-1)
/* the _TRUNCATE form: copies what fits and always ends the string */
static inline int wcsncpy_s(wchar_t *d, size_t cap, const wchar_t *s, size_t count) {
  size_t n = 0;
  if (!d || !cap) return 22;
  while (n + 1 < cap && n < count && s[n]) { d[n] = s[n]; n++; }
  d[n] = 0;
  return (s[n] && n < count) ? 80 /* STRUNCATE */ : 0;
}
#define _wcsicmp wcscasecmp
#define _wcsnicmp wcsncasecmp
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define _wcsdup wcsdup
#define _strdup strdup
#define lstrcmpiW wcscasecmp
DWORD GetEnvironmentVariableW(LPCWSTR name, LPWSTR buf, DWORD cap);

/* ---- atomics (full barriers, as Windows' Interlocked functions are) */
#define InterlockedIncrement(p) __atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST)
#define InterlockedDecrement(p) __atomic_sub_fetch((p), 1, __ATOMIC_SEQ_CST)
#define InterlockedIncrement64(p) __atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST)
#define InterlockedDecrement64(p) __atomic_sub_fetch((p), 1, __ATOMIC_SEQ_CST)
#define InterlockedExchange(p, v) __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedExchange8(p, v) __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedExchange64(p, v) __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedExchangePointer(p, v) __atomic_exchange_n((void **)(p), (void *)(v), __ATOMIC_SEQ_CST)
#define InterlockedExchangeAdd(p, v) __atomic_fetch_add((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedExchangeAdd64(p, v) __atomic_fetch_add((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedAdd(p, v) __atomic_add_fetch((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedAdd64(p, v) __atomic_add_fetch((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedOr(p, v) __atomic_fetch_or((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedAnd(p, v) __atomic_fetch_and((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedOr64(p, v) __atomic_fetch_or((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedAnd64(p, v) __atomic_fetch_and((p), (v), __ATOMIC_SEQ_CST)
static inline LONG smp_cas32(volatile LONG *p, LONG x, LONG c) { __atomic_compare_exchange_n(p, &c, x, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return c; }
static inline LONG64 smp_cas64(volatile LONG64 *p, LONG64 x, LONG64 c) { __atomic_compare_exchange_n(p, &c, x, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return c; }
static inline void *smp_casp(void *volatile *p, void *x, void *c) { __atomic_compare_exchange_n(p, &c, x, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return c; }
#define InterlockedCompareExchange(p, x, c) smp_cas32((volatile LONG *)(p), (LONG)(x), (LONG)(c))
#define InterlockedCompareExchange64(p, x, c) smp_cas64((volatile LONG64 *)(p), (LONG64)(x), (LONG64)(c))
#define InterlockedCompareExchangePointer(p, x, c) smp_casp((void *volatile *)(p), (void *)(x), (void *)(c))
#define MemoryBarrier() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define _ReadWriteBarrier() __asm__ __volatile__("" ::: "memory")
#define YieldProcessor() sched_yield()

/* ---- time */
ULONGLONG GetTickCount64(void);
DWORD GetTickCount(void);
BOOL QueryPerformanceCounter(LARGE_INTEGER *c);
BOOL QueryPerformanceFrequency(LARGE_INTEGER *f);
void GetSystemTimeAsFileTime(FILETIME *ft);
void Sleep(DWORD ms);
BOOL SwitchToThread(void);

/* ---- system */
typedef struct { WORD wProcessorArchitecture, wReserved; DWORD dwPageSize; LPVOID lpMinimumApplicationAddress, lpMaximumApplicationAddress;
                 DWORD_PTR dwActiveProcessorMask; DWORD dwNumberOfProcessors, dwProcessorType, dwAllocationGranularity;
                 WORD wProcessorLevel, wProcessorRevision; } SYSTEM_INFO;
void GetSystemInfo(SYSTEM_INFO *si);
#define ALL_PROCESSOR_GROUPS 0xffff
DWORD GetActiveProcessorCount(WORD group);
DWORD GetCurrentProcessId(void);
DWORD GetCurrentThreadId(void);
#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define MEM_DECOMMIT 0x4000
#define MEM_RELEASE 0x8000
#define PAGE_NOACCESS 0x01
#define PAGE_READWRITE 0x04
LPVOID VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD protect);
BOOL VirtualFree(LPVOID addr, SIZE_T size, DWORD type);
/* modules: Samplosis only takes a reference on itself to keep its code loaded while a worker runs. A macOS bundle
   with Objective-C classes in it is never unloaded, so the reference is a stand-in and freeing it does nothing. */
#define GET_MODULE_HANDLE_EX_FLAG_PIN 0x1
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 0x2
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 0x4
BOOL GetModuleHandleExW(DWORD flags, LPCWSTR addr, HMODULE *out);
BOOL FreeLibrary(HMODULE m);
__attribute__((noreturn)) void FreeLibraryAndExitThread(HMODULE m, DWORD code);

/* ---- handles: events and threads */
#define WAIT_OBJECT_0 0
#define WAIT_ABANDONED 0x80
#define WAIT_TIMEOUT 258
#define WAIT_FAILED 0xFFFFFFFFu
#define MAXIMUM_WAIT_OBJECTS 64
HANDLE CreateEventW(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL initial, LPCWSTR name);
#define CreateEvent CreateEventW
BOOL SetEvent(HANDLE h);
BOOL ResetEvent(HANDLE h);
DWORD WaitForSingleObject(HANDLE h, DWORD ms);
DWORD WaitForMultipleObjects(DWORD n, const HANDLE *hs, BOOL all, DWORD ms);
BOOL CloseHandle(HANDLE h);
#define CREATE_SUSPENDED 0x4
#define STACK_SIZE_PARAM_IS_A_RESERVATION 0x10000
HANDLE CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stack, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD flags, LPDWORD id);
DWORD ResumeThread(HANDLE h);
BOOL GetExitCodeThread(HANDLE h, LPDWORD code);
#define GetCurrentThread smp_GetCurrentThread   /* the Carbon headers have a GetCurrentThread of their own */
HANDLE GetCurrentThread(void);
#define THREAD_PRIORITY_LOWEST -2
#define THREAD_PRIORITY_BELOW_NORMAL -1
#define THREAD_PRIORITY_NORMAL 0
#define THREAD_PRIORITY_ABOVE_NORMAL 1
#define THREAD_PRIORITY_HIGHEST 2
#define THREAD_PRIORITY_TIME_CRITICAL 15
BOOL SetThreadPriority(HANDLE h, int prio);
BOOL CancelSynchronousIo(HANDLE h);

/* ---- locks */
typedef struct { pthread_mutex_t m; } CRITICAL_SECTION, *LPCRITICAL_SECTION;
void InitializeCriticalSection(CRITICAL_SECTION *cs);
BOOL InitializeCriticalSectionAndSpinCount(CRITICAL_SECTION *cs, DWORD spin);
void DeleteCriticalSection(CRITICAL_SECTION *cs);
#define EnterCriticalSection(cs) pthread_mutex_lock(&(cs)->m)
#define LeaveCriticalSection(cs) pthread_mutex_unlock(&(cs)->m)
#define TryEnterCriticalSection(cs) (pthread_mutex_trylock(&(cs)->m) == 0)
typedef struct { pthread_mutex_t m; } SRWLOCK, *PSRWLOCK;
#define SRWLOCK_INIT { PTHREAD_MUTEX_INITIALIZER }
#define InitializeSRWLock(l) pthread_mutex_init(&(l)->m, NULL)
#define AcquireSRWLockExclusive(l) pthread_mutex_lock(&(l)->m)
#define ReleaseSRWLockExclusive(l) pthread_mutex_unlock(&(l)->m)
#define AcquireSRWLockShared(l) pthread_mutex_lock(&(l)->m)
#define ReleaseSRWLockShared(l) pthread_mutex_unlock(&(l)->m)
#define TryAcquireSRWLockExclusive(l) (pthread_mutex_trylock(&(l)->m) == 0)
#define TryAcquireSRWLockShared(l) (pthread_mutex_trylock(&(l)->m) == 0)
typedef struct { pthread_cond_t c; } CONDITION_VARIABLE, *PCONDITION_VARIABLE;
#define CONDITION_VARIABLE_INIT { PTHREAD_COND_INITIALIZER }
#define CONDITION_VARIABLE_LOCKMODE_SHARED 0x1
#define InitializeConditionVariable(v) pthread_cond_init(&(v)->c, NULL)
#define WakeConditionVariable(v) pthread_cond_signal(&(v)->c)
#define WakeAllConditionVariable(v) pthread_cond_broadcast(&(v)->c)
BOOL SleepConditionVariableSRW(CONDITION_VARIABLE *v, SRWLOCK *l, DWORD ms, ULONG flags);
BOOL SleepConditionVariableCS(CONDITION_VARIABLE *v, CRITICAL_SECTION *cs, DWORD ms);
typedef struct { pthread_mutex_t m; volatile int done; } INIT_ONCE, *PINIT_ONCE;
#define INIT_ONCE_STATIC_INIT { PTHREAD_MUTEX_INITIALIZER, 0 }
typedef BOOL (*PINIT_ONCE_FN)(PINIT_ONCE, PVOID, PVOID *);
BOOL InitOnceExecuteOnce(INIT_ONCE *o, PINIT_ONCE_FN fn, PVOID p, PVOID *ctx);

/* ---- thread pool (render.c): a fixed-size pool of plain threads */
typedef struct SmpTpPool TP_POOL, *PTP_POOL;
typedef struct SmpTpWork TP_WORK, *PTP_WORK;
typedef struct SmpTpInst *PTP_CALLBACK_INSTANCE;
typedef struct { PTP_POOL Pool; } TP_CALLBACK_ENVIRON, *PTP_CALLBACK_ENVIRON;
typedef void (*PTP_WORK_CALLBACK)(PTP_CALLBACK_INSTANCE, PVOID, PTP_WORK);
PTP_POOL CreateThreadpool(PVOID reserved);
void CloseThreadpool(PTP_POOL p);
void SetThreadpoolThreadMaximum(PTP_POOL p, DWORD n);
BOOL SetThreadpoolThreadMinimum(PTP_POOL p, DWORD n);
#define InitializeThreadpoolEnvironment(e) memset((e), 0, sizeof *(e))
#define DestroyThreadpoolEnvironment(e) ((void)(e))
#define SetThreadpoolCallbackPool(e, p) ((e)->Pool = (p))
#define SetThreadpoolCallbackLibrary(e, m) ((void)(e), (void)(m))
PTP_WORK CreateThreadpoolWork(PTP_WORK_CALLBACK fn, PVOID arg, PTP_CALLBACK_ENVIRON env);
void SubmitThreadpoolWork(PTP_WORK w);
void WaitForThreadpoolWorkCallbacks(PTP_WORK w, BOOL cancel_pending);
void CloseThreadpoolWork(PTP_WORK w);

/* ---- files */
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_READ_ATTRIBUTES 0x80
#define FILE_WRITE_ATTRIBUTES 0x100
#define FILE_SHARE_READ 1
#define FILE_SHARE_WRITE 2
#define FILE_SHARE_DELETE 4
#define CREATE_NEW 1
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define TRUNCATE_EXISTING 5
#define FILE_ATTRIBUTE_READONLY 0x1
#define FILE_ATTRIBUTE_HIDDEN 0x2
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define FILE_ATTRIBUTE_ARCHIVE 0x20
#define FILE_ATTRIBUTE_NORMAL 0x80
#define FILE_ATTRIBUTE_TEMPORARY 0x100
#define FILE_ATTRIBUTE_REPARSE_POINT 0x400
#define INVALID_FILE_ATTRIBUTES 0xFFFFFFFFu
#define INVALID_FILE_SIZE 0xFFFFFFFFu
#define FILE_FLAG_WRITE_THROUGH 0x80000000u
#define FILE_FLAG_OVERLAPPED 0x40000000u
#define FILE_FLAG_NO_BUFFERING 0x20000000u
#define FILE_FLAG_RANDOM_ACCESS 0x10000000u
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000u
#define FILE_FLAG_DELETE_ON_CLOSE 0x04000000u
#define FILE_FLAG_BACKUP_SEMANTICS 0x02000000u
#define FILE_BEGIN 0
#define FILE_CURRENT 1
#define FILE_END 2
HANDLE CreateFileW(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl);
BOOL ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD got, LPOVERLAPPED o);
BOOL WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD put, LPOVERLAPPED o);
#define STATUS_PENDING 0x103
#define HasOverlappedIoCompleted(o) ((o)->Internal != STATUS_PENDING)
BOOL GetOverlappedResult(HANDLE h, LPOVERLAPPED o, LPDWORD got, BOOL wait);
BOOL CancelIoEx(HANDLE h, LPOVERLAPPED o);
BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER to, PLARGE_INTEGER now, DWORD how);
BOOL SetEndOfFile(HANDLE h);
BOOL FlushFileBuffers(HANDLE h);
BOOL GetFileSizeEx(HANDLE h, PLARGE_INTEGER size);
BOOL GetFileTime(HANDLE h, LPFILETIME created, LPFILETIME accessed, LPFILETIME written);
BOOL SetFileTime(HANDLE h, const FILETIME *created, const FILETIME *accessed, const FILETIME *written);
typedef enum { GetFileExInfoStandard } GET_FILEEX_INFO_LEVELS;
typedef struct { DWORD dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime; DWORD nFileSizeHigh, nFileSizeLow; } WIN32_FILE_ATTRIBUTE_DATA;
DWORD GetFileAttributesW(LPCWSTR path);
BOOL SetFileAttributesW(LPCWSTR path, DWORD attrs);
BOOL GetFileAttributesExW(LPCWSTR path, GET_FILEEX_INFO_LEVELS lvl, LPVOID out);
typedef struct { DWORD dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime; DWORD nFileSizeHigh, nFileSizeLow;
                 DWORD dwReserved0, dwReserved1; WCHAR cFileName[MAX_PATH]; WCHAR cAlternateFileName[14]; } WIN32_FIND_DATAW, *LPWIN32_FIND_DATAW;
typedef enum { FindExInfoStandard, FindExInfoBasic } FINDEX_INFO_LEVELS;
typedef enum { FindExSearchNameMatch, FindExSearchLimitToDirectories } FINDEX_SEARCH_OPS;
#define FIND_FIRST_EX_CASE_SENSITIVE 1
#define FIND_FIRST_EX_LARGE_FETCH 2
HANDLE FindFirstFileW(LPCWSTR pattern, LPWIN32_FIND_DATAW fd);
HANDLE FindFirstFileExW(LPCWSTR pattern, FINDEX_INFO_LEVELS lvl, LPVOID fd, FINDEX_SEARCH_OPS op, LPVOID filter, DWORD flags);
BOOL FindNextFileW(HANDLE h, LPWIN32_FIND_DATAW fd);
BOOL FindClose(HANDLE h);
BOOL CreateDirectoryW(LPCWSTR path, LPSECURITY_ATTRIBUTES sa);
BOOL RemoveDirectoryW(LPCWSTR path);
BOOL DeleteFileW(LPCWSTR path);
#define MOVEFILE_REPLACE_EXISTING 1
#define MOVEFILE_COPY_ALLOWED 2
#define MOVEFILE_WRITE_THROUGH 8
BOOL MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags);
DWORD GetFullPathNameW(LPCWSTR path, DWORD cap, LPWSTR out, LPWSTR *file_part);
#define DRIVE_UNKNOWN 0
#define DRIVE_NO_ROOT_DIR 1
#define DRIVE_REMOVABLE 2
#define DRIVE_FIXED 3
#define DRIVE_REMOTE 4
#define DRIVE_CDROM 5
#define DRIVE_RAMDISK 6
UINT GetDriveTypeW(LPCWSTR root);
BOOL GetVolumePathNameW(LPCWSTR path, LPWSTR out, DWORD cap);
BOOL GetDiskFreeSpaceExW(LPCWSTR dir, PULARGE_INTEGER avail, PULARGE_INTEGER total, PULARGE_INTEGER free_);
#include <stdio.h>
FILE *_wfopen(const wchar_t *path, const wchar_t *mode);
#define _fseeki64 fseeko
#define _ftelli64 ftello
/* UTF-8 path for the file system: backslashes become slashes. 0 on failure. */
int smp_path_utf8(const wchar_t *w, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
