// mac_shim.cpp: Monkey's Audio (.ape) for decode.c, behind three extern "C" calls (open, read, close), so decode.c
// grows the result and holds the size limit as it does for every decoder. mac/ holds the decoder side of the
// Monkey's Audio SDK 13.43 (BSD, see mac/License.txt and mac/VERSION.txt). Everything runs inside try/catch, so no
// C++ exception ever crosses into C (and from there into the host, which would crash it). The bytes are read from
// memory through our own reader, which never reads or seeks outside them, and the header is checked before the
// decoder is made, so a lying header can't make it allocate much (see header_sane).
#include <new>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "All.h"
#include "MACLib.h"
#include "APEInfo.h"
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

// An open .ape file: the decoder and the reader it reads through (which must outlive it)
struct ApeFile {
  MemReader io;
  IAPEDecompress *d;
  int chans, bytes, is_float, align;
  unsigned char *buf;                                   // one chunk of the decoder's bytes
  ApeFile(const unsigned char *b, size_t len) : io(b, (int64)len), d(APE_NULL), chans(0), bytes(0), is_float(0),
                                                align(0), buf(APE_NULL) {}
};
#define APE_CHUNK 4096

// The decoder sizes its buffers from the header: one frame of output (blocks per frame x block align) and one frame
// of input (the frame's bytes, from the seek table). Refuse a header whose numbers can't belong to a file of len
// bytes: a frame's output over 64 MB (real files have at most about 10 MB), more frames than bytes (every frame
// takes some), or a frame bigger than the file. Long silent files still pass: they are many small frames.
// Before anything is parsed: an APE tag footer at the end ("APETAGEX", its size at +12) must fit in the file. The SDK
// allocates what the footer says (up to 256 MB) before reading the tag.
static bool tag_sane(const unsigned char *b, size_t len) {
  for (int id3 = 0; id3 < 2; id3++) {                    // the footer may sit before a 128-byte ID3v1 tag
    size_t end = len - (id3 ? 128 : 0);
    if (len < (id3 ? 128u : 0u) + 32) continue;
    if (id3 && memcmp(b + len - 128, "TAG", 3)) continue;
    if (!memcmp(b + end - 32, "APETAGEX", 8)) {
      uint32_t size = (uint32_t)b[end - 20] | (uint32_t)b[end - 19] << 8 | (uint32_t)b[end - 18] << 16 | (uint32_t)b[end - 17] << 24;
      if (size > len) return false;
    }
  }
  return true;
}

static bool header_sane(CAPEInfo *info, size_t len) {
  int64 bpf = info->GetInfo(IAPEDecompress::APE_INFO_BLOCKS_PER_FRAME);
  int64 align = info->GetInfo(IAPEDecompress::APE_INFO_BLOCK_ALIGN);
  int64 frames = info->GetInfo(IAPEDecompress::APE_INFO_TOTAL_FRAMES);
  if (bpf < 1 || align < 1 || align > 1024 || bpf * align > ((int64)64 << 20)) return false;
  if (frames < 1 || frames > (int64)len) return false;
  for (int64 i = 0; i < frames; i++) {
    int64 fb = info->GetInfo(IAPEDecompress::APE_INFO_FRAME_BYTES, i);
    if (fb < 0 || fb > (int64)len) return false;
  }
  return true;
}

static void ape_free(ApeFile *f) {
  if (!f) return;
  try { delete f->d; } catch (...) {}
  free(f->buf);
  delete f;
}

// Opens the .ape file in b (which must stay put until smp_ape_close): a handle and its format, or NULL and a plain
// message. claimed is the length the header says, which a damaged file may get wrong.
extern "C" void *smp_ape_open(const unsigned char *b, size_t len, unsigned *ch, unsigned *sr,
                              unsigned long long *claimed, char *err, int cap) {
  ApeFile *f = APE_NULL;
  *ch = 0; *sr = 0; *claimed = 0;
  try {
    if (g_flags & 1) throw std::bad_alloc();
    if (!tag_sane(b, len)) {
      snprintf(err, (size_t)cap, "it's a Monkey's Audio file Samplosis can't read (it may be damaged)");
      return APE_NULL;
    }
    f = new ApeFile(b, len);
    int e = 0;
    CAPEInfo *info = new CAPEInfo(&e, &f->io);
    if (e != APE_ERROR_SUCCESS || !header_sane(info, len)) {
      delete info;
      ape_free(f);
      snprintf(err, (size_t)cap, "it's a Monkey's Audio file Samplosis can't read (it may be damaged)");
      return APE_NULL;
    }
    f->d = CreateIAPEDecompressEx2(info, -1, -1, &e);          // takes the info over
    if (!f->d || e != APE_ERROR_SUCCESS) {
      ape_free(f);
      snprintf(err, (size_t)cap, "it's a Monkey's Audio file Samplosis can't read (it may be damaged)");
      return APE_NULL;
    }
    int64 chans = f->d->GetInfo(IAPEDecompress::APE_INFO_CHANNELS);
    int64 rate = f->d->GetInfo(IAPEDecompress::APE_INFO_SAMPLE_RATE);
    int64 bits = f->d->GetInfo(IAPEDecompress::APE_INFO_BITS_PER_SAMPLE);
    int64 align = f->d->GetInfo(IAPEDecompress::APE_INFO_BLOCK_ALIGN);
    int64 total = f->d->GetInfo(IAPEDecompress::APE_DECOMPRESS_TOTAL_BLOCKS);
    int is_float = (f->d->GetInfo(IAPEDecompress::APE_INFO_FORMAT_FLAGS) & APE_FORMAT_FLAG_FLOATING_POINT) != 0;
    int bytes = (int)((bits + 7) / 8);
    if (chans < 1 || chans > 64 || rate < 1 || rate > 1000000 || bytes < 1 || bytes > 4 ||
        align != chans * bytes || (is_float && bytes != 4) || total < 1) {
      ape_free(f);
      snprintf(err, (size_t)cap, "it's a Monkey's Audio file with a format Samplosis can't read");
      return APE_NULL;
    }
    f->chans = (int)chans; f->bytes = bytes; f->is_float = is_float; f->align = (int)align;
    if (!(f->buf = (unsigned char *)malloc((size_t)APE_CHUNK * (size_t)align))) {
      ape_free(f);
      snprintf(err, (size_t)cap, "out of memory");
      return APE_NULL;
    }
    *ch = (unsigned)chans; *sr = (unsigned)rate; *claimed = (unsigned long long)total;
    return f;
  } catch (...) {
    ape_free(f);
    snprintf(err, (size_t)cap, "it's a Monkey's Audio file Samplosis can't read (the decoder failed)");
    return APE_NULL;
  }
}

// Up to frames (at most 4096) interleaved float frames into out: how many came, 0 at the end, -1 when the decoder
// failed (a damaged file stops there).
extern "C" int smp_ape_read(void *h, float *out, int frames) {
  ApeFile *f = (ApeFile *)h;
  try {
    if (g_flags & 1) throw std::bad_alloc();
    int64 got = 0;
    IAPEDecompress::APE_GET_DATA_PROCESSING proc = {true, false, false};   // float as float, 8-bit unsigned, little-endian
    if (frames > APE_CHUNK) frames = APE_CHUNK;
    if (frames <= 0) return 0;
    if (f->d->GetData(f->buf, frames, &got, &proc) != APE_ERROR_SUCCESS) return -1;
    if (got <= 0) return 0;
    if (got > frames) got = frames;
    for (int64 i = 0; i < got * f->chans; i++) out[i] = sample_of(f->buf + i * f->bytes, f->bytes, f->is_float);
    return (int)got;
  } catch (...) {
    return -1;
  }
}

extern "C" void smp_ape_close(void *h) {
  try { ape_free((ApeFile *)h); } catch (...) {}
}
