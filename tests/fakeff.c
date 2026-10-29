/* fakeff.c: a stand-in for ffmpeg.exe and ffprobe.exe, for the tests of the FFmpeg stage. Copy it into a folder as
   ffmpeg.exe and ffprobe.exe; the folder's name (or the environment variable SAMPLOSIS_FAKEFF) says how it behaves:
     hang       ffmpeg never writes anything and never ends
     garbage    ffmpeg writes 100000 random bytes, says why on stderr and exits 1
     fail       ffmpeg writes nothing, says why on stderr and exits 1
     orphan     ffmpeg starts a copy of itself that hangs holding its stdout, then writes 1 s of silence (44100
                stereo float frames) and exits 0: the pipe stays open after ffmpeg has gone
     probehang  ffprobe never writes anything and never ends
   Otherwise, as ffprobe.exe it says: one audio stream, 44100 Hz, 2 channels, 1 second. Every run adds its process id
   to pids.txt beside the exe, so a test can check nothing is left running. Built by "build.bat test". */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <io.h>

int main(void) {
  wchar_t exe[1100], dir[1100], *slash, *name, pids[1200], mode[64] = L"";
  FILE *f;
  GetModuleFileNameW(NULL, exe, 1100);
  wcscpy(dir, exe);
  slash = wcsrchr(dir, L'\\');
  name = slash ? slash + 1 : dir;
  swprintf(pids, 1200, L"%.*s\\pids.txt", (int)(slash ? slash - dir : 0), dir);
  if ((f = _wfopen(pids, L"a"))) { fprintf(f, "%lu\n", GetCurrentProcessId()); fclose(f); }
  if (!GetEnvironmentVariableW(L"SAMPLOSIS_FAKEFF", mode, 64)) {   /* else the folder's name */
    wchar_t folder[1100], *up;
    wcscpy(folder, dir);
    if (slash) folder[slash - dir] = 0;
    up = wcsrchr(folder, L'\\');
    wcsncpy(mode, up ? up + 1 : folder, 63);
  }
  _wcslwr(mode);
  if (wcsstr(_wcslwr(name), L"ffprobe")) {
    if (!wcscmp(mode, L"probehang")) { for (;;) Sleep(1000); }
    printf("{\n    \"streams\": [\n        {\n            \"sample_rate\": \"44100\",\n            \"channels\": 2\n"
           "        }\n    ],\n    \"format\": {\n        \"duration\": \"1.000000\"\n    }\n}\n");
    return 0;
  }
  _setmode(_fileno(stdout), _O_BINARY);
  if (!wcscmp(mode, L"hang")) { for (;;) Sleep(1000); }
  if (!wcscmp(mode, L"orphan")) {
    static float z[44100 * 2];
    STARTUPINFOW si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si); si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE); si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    SetEnvironmentVariableW(L"SAMPLOSIS_FAKEFF", L"hang");
    if (CreateProcessW(exe, NULL, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
      CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
    fwrite(z, sizeof z, 1, stdout); fflush(stdout);
    return 0;
  }
  if (!wcscmp(mode, L"garbage")) {
    static unsigned char b[100000]; unsigned s = GetTickCount(); int i;
    for (i = 0; i < (int)sizeof b; i++) { s = s * 1103515245u + 12345u; b[i] = (unsigned char)(s >> 16); }
    fwrite(b, 1, sizeof b, stdout); fflush(stdout);
    fprintf(stderr, "[aist#0:0/tta @ 000001d2c0ffee00] Error while decoding stream: Invalid data found when processing input\r\n");
    return 1;
  }
  fprintf(stderr, "Error opening input files: Invalid data found when processing input\r\n");
  return 1;
}
