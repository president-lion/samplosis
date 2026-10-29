// mac_shim.cpp: Monkey's Audio (.ape) for decode.c, behind one extern "C" call. mac/ holds the decoder side of the
// Monkey's Audio SDK 13.43 (BSD, see mac/License.txt and mac/VERSION.txt). Everything runs inside try/catch, so no
// C++ exception ever crosses into C (and from there into the host, which would crash it). The bytes are read from
// memory through our own reader, which never reads or seeks outside them.
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "All.h"
#include "MACLib.h"
#include "IAPEIO.h"

using namespace APE;

static volatile long g_flags;   // tests: 1 = throw inside the guarded region

extern "C" void smp_ape_set_debug_flags(long f) { g_flags = f; }

// a read-only file in memory
class MemReader : public IAPEIO {
public:
  MemReader(const unsigned char *b, int64 n) : m_b(b), m_n(n), m_at(0) {}
  int Open(const str_utfn *, bool) APE_OVERRIDE { return APE_ERROR_UNDEFINED; }
  int Close() APE_OVERRIDE { return APE_ERROR_SUCCESS; }
  int Read(void *out, int64 want, int64 *got) APE_OVERRIDE {
    int64 left = m_at < m_n ? m_n - m_at : 0, k = want < 0 ? 0 : want < left ? want : left;
    if (k) memcpy(out, m_b + m_at, (size_t)k);
    m_at += k;
    if (got) *got = k;
    return APE_ERROR_SUCCESS;
  }
  int Write(const void *, int64, int64 *) APE_OVERRIDE { return APE_ERROR_IO_WRITE; }
  int Seek(int64 pos, SeekMethod how) APE_OVERRIDE {
    int64 to = how == SeekFileBegin ? pos : how == SeekFileCurrent ? m_at + pos : m_n + pos;
    if (to < 0 || to > m_n) return APE_ERROR_IO_READ;
    m_at = to;
    return APE_ERROR_SUCCESS;
  }
  int Create(const str_utfn *) APE_OVERRIDE { return APE_ERROR_UNDEFINED; }
  int Delete() APE_OVERRIDE { return APE_ERROR_UNDEFINED; }
  int SetEOF() APE_OVERRIDE { return APE_ERROR_UNDEFINED; }
  unsigned char *GetBuffer(int *n) APE_OVERRIDE { if (n) *n = 0; return APE_NULL; }
  int64 GetPosition() APE_OVERRIDE { return m_at; }
  int64 GetSize() APE_OVERRIDE { return m_n; }
private:
  const unsigned char *m_b;
  int64 m_n, m_at;
};

// one sample of bytes (little-endian, as a WAV holds it) to float
static float sample_of(const unsigned char *s, int bytes, int is_float) {
  if (is_float) { float f; memcpy(&f, s, 4); return f; }
  if (bytes == 1) return ((float)s[0] - 128.0f) / 128.0f;               // 8-bit WAV samples are unsigned
  {
    uint32_t u = 0;
    for (int k = 0; k < bytes; k++) u |= (uint32_t)s[k] << (8 * k);
    int32_t v = (int32_t)(u << (32 - 8 * bytes)) >> (32 - 8 * bytes);   // sign-extend
    return (float)((double)v / (double)(1u << (8 * bytes - 1)));
  }
}

// The .ape file in b: interleaved float (malloc'd), or NULL and a plain message.
extern "C" float *smp_ape_decode(const unsigned char *b, size_t len, unsigned *ch, unsigned *sr,
                                 unsigned long long *n, char *err, int cap) {
  IAPEDecompress *d = NULL;
  float *out = NULL;
  unsigned char *buf = NULL;
  MemReader io(b, (int64)len);                       // outlives the decoder, which reads through it
  *ch = 0; *sr = 0; *n = 0;
  try {
    if (g_flags & 1) throw std::bad_alloc();
    int e = 0;
    d = CreateIAPEDecompressEx(&io, &e);
    if (!d || e != APE_ERROR_SUCCESS) {
      delete d;
      snprintf(err, (size_t)cap, "it's a Monkey's Audio file Samplosis can't read (it may be damaged)");
      return NULL;
    }
    int64 chans = d->GetInfo(IAPEDecompress::APE_INFO_CHANNELS);
    int64 rate = d->GetInfo(IAPEDecompress::APE_INFO_SAMPLE_RATE);
    int64 bits = d->GetInfo(IAPEDecompress::APE_INFO_BITS_PER_SAMPLE);
    int64 align = d->GetInfo(IAPEDecompress::APE_INFO_BLOCK_ALIGN);
    int64 total = d->GetInfo(IAPEDecompress::APE_DECOMPRESS_TOTAL_BLOCKS);
    int is_float = (d->GetInfo(IAPEDecompress::APE_INFO_FORMAT_FLAGS) & APE_FORMAT_FLAG_FLOATING_POINT) != 0;
    int bytes = (int)((bits + 7) / 8);
    if (chans < 1 || chans > 64 || rate < 1 || rate > 1000000 || bytes < 1 || bytes > 4 ||
        align != chans * bytes || (is_float && bytes != 4) || total < 1 || total > 0x7FFFFFFF) {
      delete d;
      snprintf(err, (size_t)cap, "it's a Monkey's Audio file with a format Samplosis can't read");
      return NULL;
    }
    // the file can't decode to more than a few hundred times its size: refuse an absurd block count
    if ((unsigned long long)total * (unsigned long long)align > (unsigned long long)len * 1024 + (1u << 20)) {
      delete d;
      snprintf(err, (size_t)cap, "it's a damaged Monkey's Audio file");
      return NULL;
    }
    const int64 chunk = 4096;
    out = (float *)malloc(sizeof(float) * (size_t)total * (size_t)chans);
    buf = (unsigned char *)malloc((size_t)(chunk * align));
    if (!out || !buf) {
      free(out); free(buf); delete d;
      snprintf(err, (size_t)cap, "out of memory");
      return NULL;
    }
    IAPEDecompress::APE_GET_DATA_PROCESSING proc = {true, false, false};   // float as float, 8-bit unsigned, little-endian
    int64 got_all = 0;
    while (got_all < total) {
      int64 want = total - got_all < chunk ? total - got_all : chunk, got = 0;
      if (d->GetData(buf, want, &got, &proc) != APE_ERROR_SUCCESS || got <= 0) break;
      if (got > want) got = want;
      for (int64 i = 0; i < got * chans; i++)
        out[(size_t)(got_all * chans + i)] = sample_of(buf + i * bytes, bytes, is_float);
      got_all += got;
    }
    free(buf); buf = NULL;
    delete d; d = NULL;
    if (!got_all) {
      free(out);
      snprintf(err, (size_t)cap, "it's a damaged Monkey's Audio file");
      return NULL;
    }
    *ch = (unsigned)chans; *sr = (unsigned)rate; *n = (unsigned long long)got_all;
    return out;
  } catch (...) {
    free(out); free(buf);
    try { delete d; } catch (...) {}
    snprintf(err, (size_t)cap, "it's a Monkey's Audio file Samplosis can't read (the decoder failed)");
    return NULL;
  }
}
