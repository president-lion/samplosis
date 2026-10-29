/* gigmake.cpp: writes a GigaStudio file with libgig itself, for the GigaStudio tests (built by "build.bat test").

   gigmake DESC OUT.gig

   DESC is a text file, one item per line, as name=value words (the first word says what the line is):
     sample name=N rate=R channels=1|2 bits=16|24 data=RAWFILE [unity=K] [loop=FIRST,LAST] [compressed=1]
       RAWFILE holds the frames, interleaved little-endian PCM. compressed=1 (16-bit only) stores the wave the way
       GigaStudio compresses it (an ewav chunk, then frames of 2048 points: mode 1, 8-bit second differences, where
       they fit, else mode 0, plain 16-bit), which libgig can read but not write.
     instrument name=N [program=P] [bank=MSB] [banklsb=LSB] [drum=1] [bend=SEMITONES] [fine=CENTS] [swlo=K swhi=K]
     region lo=K hi=K [keygroup=G]
     dim type=velocity|layer|samplechannel|releasetrigger|keyboard|roundrobin|random|modwheel|smartmidi bits=B zones=Z
     dimreg i=INDEX sample=S [unity=K] [fine=CENTS] [att=DB] [pan=-64..63] [attack=S] [decay1=S] [decay2=S]
            [sustain=PERMILLE] [release=S] [infinite=0|1] [rtdecay=0..8] [upper=DIM:VALUE] [loop=START,LENGTH]
            [pitchtrack=0|1] [eg3=CENTS] [lfo3=HZ:CENTS] [vcf=CUTOFF:RESONANCE] [start=FRAMES]
   Exit code 0 when written; otherwise a message on stderr. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <string>
#include <vector>
#include "gig.h"

/* a wave's protected list chunk and data chunk, through pointers to member named by a derived class */
struct WaveAccess : gig::Sample {
  static RIFF::List *list(gig::Sample *s) { return s->*(&WaveAccess::pWaveList); }
  static RIFF::Chunk *data(gig::Sample *s) { return s->*(&WaveAccess::pCkData); }
};

typedef std::map<std::string, std::string> Kv;

static Kv words(const std::string &line, std::string &kind) {
  Kv kv;
  size_t i = 0, n = line.size();
  kind.clear();
  while (i < n) {
    while (i < n && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) i++;
    size_t j = i;
    while (j < n && line[j] != ' ' && line[j] != '\t' && line[j] != '\r') j++;
    if (j > i) {
      std::string w = line.substr(i, j - i);
      size_t eq = w.find('=');
      if (kind.empty() && eq == std::string::npos) kind = w;
      else if (eq != std::string::npos) kv[w.substr(0, eq)] = w.substr(eq + 1);
    }
    i = j;
  }
  return kv;
}

static double num(const Kv &kv, const char *k, double d) {
  auto it = kv.find(k);
  return it == kv.end() ? d : atof(it->second.c_str());
}
static bool has(const Kv &kv, const char *k) { return kv.find(k) != kv.end(); }
static void pair2(const Kv &kv, const char *k, double &a, double &b) {
  auto it = kv.find(k);
  if (it == kv.end()) return;
  sscanf(it->second.c_str(), "%lf%*[,:]%lf", &a, &b);
}

/* GigaStudio's compression of 16-bit mono: frames of 2048 points; mode 1 keeps the first point and the first
   difference, then 8-bit second differences (the decoder does dy -= d; y -= dy), mode 0 is plain 16-bit */
static std::vector<uint8_t> compress16(const int16_t *x, size_t n) {
  std::vector<uint8_t> out;
  for (size_t at = 0; at < n; at += 2048) {
    size_t m = n - at < 2048 ? n - at : 2048;
    std::vector<int8_t> d(m);
    /* the state before the first point: the point before it extrapolated, and the first slope, so that the
       second differences that follow are small for smooth waves */
    int x1 = m > 1 ? x[at + 1] : x[at];
    int y = 2 * x[at] - x1, dy = x[at] - x1;
    bool fits = y >= -32768 && y <= 32767 && dy >= -32768 && dy <= 32767;
    const int y0 = y, dy0 = dy;
    for (size_t i = 0; i < m && fits; i++) {
      int ndy = y - x[at + i];                                   /* y - ndy is the point wanted */
      int dd = dy - ndy;                                         /* dy -= dd makes dy ndy */
      if (dd < -128 || dd > 127) { fits = false; break; }
      d[i] = (int8_t)dd;
      dy = ndy; y = x[at + i];
    }
    if (fits) {
      out.push_back(1);
      out.push_back((uint8_t)(y0 & 255)); out.push_back((uint8_t)((y0 >> 8) & 255));
      out.push_back((uint8_t)(dy0 & 255)); out.push_back((uint8_t)((dy0 >> 8) & 255));
      for (size_t i = 0; i < m; i++) out.push_back((uint8_t)d[i]);
    } else {
      out.push_back(0);
      for (size_t i = 0; i < m; i++) { out.push_back((uint8_t)(x[at + i] & 255)); out.push_back((uint8_t)((x[at + i] >> 8) & 255)); }
    }
  }
  return out;
}

static gig::dimension_t dim_type(const std::string &t) {
  if (t == "velocity") return gig::dimension_velocity;
  if (t == "layer") return gig::dimension_layer;
  if (t == "samplechannel") return gig::dimension_samplechannel;
  if (t == "releasetrigger") return gig::dimension_releasetrigger;
  if (t == "keyboard") return gig::dimension_keyboard;
  if (t == "roundrobin") return gig::dimension_roundrobin;
  if (t == "random") return gig::dimension_random;
  if (t == "modwheel") return gig::dimension_modwheel;
  if (t == "smartmidi") return gig::dimension_smartmidi;
  throw gig::Exception("unknown dimension type " + t);
}

struct Pending { gig::Sample *s; std::vector<uint8_t> bytes; size_t frames; bool raw; };

int main(int argc, char **argv) {
  if (argc != 3) { fprintf(stderr, "usage: gigmake DESC OUT.gig\n"); return 2; }
  try {
    FILE *fd = fopen(argv[1], "rb");
    if (!fd) { fprintf(stderr, "can't read %s\n", argv[1]); return 2; }
    std::string text;
    char buf[4096];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, fd)) > 0) text.append(buf, got);
    fclose(fd);

    gig::File file;
    file.pInfo->Name = "gigmake";
    std::vector<Pending> pend;
    gig::Instrument *ins = nullptr;
    gig::Region *rg = nullptr;
    size_t pos = 0;
    while (pos < text.size()) {
      size_t e = text.find('\n', pos);
      if (e == std::string::npos) e = text.size();
      std::string kind;
      Kv kv = words(text.substr(pos, e - pos), kind);
      pos = e + 1;
      if (kind.empty()) continue;
      if (kind == "sample") {
        gig::Sample *s = file.AddSample();
        int ch = (int)num(kv, "channels", 1), bits = (int)num(kv, "bits", 16);
        s->pInfo->Name = kv["name"];
        s->Channels = (uint16_t)ch; s->BitDepth = (uint16_t)bits; s->FrameSize = (uint)(ch * bits / 8);
        s->BlockAlign = (uint16_t)s->FrameSize;
        s->SamplesPerSecond = (uint32_t)num(kv, "rate", 44100);
        s->AverageBytesPerSecond = s->SamplesPerSecond * s->FrameSize;
        s->MIDIUnityNote = (uint32_t)num(kv, "unity", 60);
        if (has(kv, "loop")) {
          double a = 0, b = 0;
          pair2(kv, "loop", a, b);
          s->Loops = 1; s->LoopStart = (uint32_t)a; s->LoopEnd = (uint32_t)b; s->LoopSize = s->LoopEnd - s->LoopStart + 1;
        }
        FILE *fr = fopen(kv["data"].c_str(), "rb");
        if (!fr) { fprintf(stderr, "can't read %s\n", kv["data"].c_str()); return 2; }
        Pending p;
        p.s = s;
        while ((got = fread(buf, 1, sizeof buf, fr)) > 0) p.bytes.insert(p.bytes.end(), buf, buf + got);
        fclose(fr);
        p.frames = p.bytes.size() / s->FrameSize;
        p.raw = num(kv, "compressed", 0) != 0;
        if (p.raw) {
          if (bits != 16 || ch != 1) throw gig::Exception("compressed=1 is for 16-bit mono");
          std::vector<uint8_t> c = compress16((const int16_t *)p.bytes.data(), p.frames);
          p.bytes.swap(c);
          /* the data chunk exactly as long as the compressed stream (sized in 1-byte points for the moment) */
          s->FrameSize = 1;
          s->Resize(p.bytes.size());
          s->FrameSize = 2;
          RIFF::Chunk *ew = WaveAccess::list(s)->AddSubChunk(CHUNK_ID_EWAV, 4);
          memset(ew->LoadChunkData(), 0, 4);
          s->Compressed = true;                             /* or saving deletes the ewav chunk again */
        } else {
          s->Resize(p.frames);
        }
        pend.push_back(p);
      } else if (kind == "instrument") {
        ins = file.AddInstrument();
        ins->pInfo->Name = kv["name"];
        ins->MIDIProgram = (uint32_t)num(kv, "program", 0);
        ins->MIDIBankCoarse = (uint8_t)num(kv, "bank", 0);
        ins->MIDIBankFine = (uint8_t)num(kv, "banklsb", 0);
        ins->MIDIBank = ins->MIDIBankCoarse * 128 + ins->MIDIBankFine;
        ins->IsDrum = num(kv, "drum", 0) != 0;
        ins->PitchbendRange = (uint16_t)num(kv, "bend", 2);
        ins->FineTune = (int16_t)num(kv, "fine", 0);
        if (has(kv, "swlo")) {
          ins->DimensionKeyRange.low = (uint8_t)num(kv, "swlo", 0);
          ins->DimensionKeyRange.high = (uint8_t)num(kv, "swhi", 0);
        }
        rg = nullptr;
      } else if (kind == "region") {
        if (!ins) throw gig::Exception("region before an instrument");
        rg = ins->AddRegion();
        rg->SetKeyRange((uint16_t)num(kv, "lo", 0), (uint16_t)num(kv, "hi", 127));
        rg->KeyGroup = (uint16_t)num(kv, "keygroup", 0);
        if (!pend.empty()) rg->SetSample(pend[0].s);
      } else if (kind == "dim") {
        if (!rg) throw gig::Exception("dim before a region");
        gig::dimension_def_t d;
        memset(&d, 0, sizeof d);
        d.dimension = dim_type(kv["type"]);
        d.bits = (uint8_t)num(kv, "bits", 1);
        d.zones = (uint8_t)num(kv, "zones", 2);
        rg->AddDimension(&d);
      } else if (kind == "dimreg") {
        if (!rg) throw gig::Exception("dimreg before a region");
        int i = (int)num(kv, "i", 0);
        gig::DimensionRegion *dr = i >= 0 && i < 256 ? rg->pDimensionRegions[i] : nullptr;
        if (!dr) throw gig::Exception("no dimension region " + std::to_string(i));
        int si = (int)num(kv, "sample", 0);
        if (si < 0 || si >= (int)pend.size()) throw gig::Exception("no sample " + std::to_string(si));
        dr->pSample = pend[si].s;
        if (i == 0) rg->SetSample(pend[si].s);
        dr->UnityNote = (uint8_t)num(kv, "unity", pend[si].s->MIDIUnityNote);
        dr->FineTune = (int16_t)num(kv, "fine", 0);
        if (has(kv, "att")) dr->SetGain((int32_t)(num(kv, "att", 0) * 655360.0));
        dr->Pan = (int8_t)num(kv, "pan", 0);
        if (has(kv, "attack")) dr->EG1Attack = num(kv, "attack", 0);
        if (has(kv, "decay1")) dr->EG1Decay1 = num(kv, "decay1", 0);
        if (has(kv, "decay2")) dr->EG1Decay2 = num(kv, "decay2", 0);
        if (has(kv, "sustain")) dr->EG1Sustain = (uint16_t)num(kv, "sustain", 1000);
        if (has(kv, "release")) dr->EG1Release = num(kv, "release", 0);
        if (has(kv, "infinite")) dr->EG1InfiniteSustain = num(kv, "infinite", 1) != 0;
        if (has(kv, "rtdecay")) dr->ReleaseTriggerDecay = (uint8_t)num(kv, "rtdecay", 0);
        if (has(kv, "pitchtrack")) dr->PitchTrack = num(kv, "pitchtrack", 1) != 0;
        if (has(kv, "eg3")) dr->EG3Depth = (int16_t)num(kv, "eg3", 0);
        if (has(kv, "start")) dr->SampleStartOffset = (uint16_t)num(kv, "start", 0);
        if (has(kv, "upper")) {
          double d = 0, v = 0;
          pair2(kv, "upper", d, v);
          dr->DimensionUpperLimits[(int)d] = (uint8_t)v;
        }
        if (has(kv, "loop")) {
          double a = 0, b = 0;
          pair2(kv, "loop", a, b);
          DLS::sample_loop_t L;
          L.Size = sizeof L; L.LoopType = gig::loop_type_normal; L.LoopStart = (uint32_t)a; L.LoopLength = (uint32_t)b;
          dr->AddSampleLoop(&L);
        }
        if (has(kv, "lfo3")) {
          double hz = 0, c = 0;
          pair2(kv, "lfo3", hz, c);
          dr->LFO3Frequency = hz; dr->LFO3InternalDepth = (int16_t)c; dr->LFO3Controller = gig::lfo3_ctrl_internal;
        }
        if (has(kv, "vcf")) {
          double cut = 127, res = 0;
          pair2(kv, "vcf", cut, res);
          dr->VCFEnabled = true; dr->VCFType = gig::vcf_type_lowpass; dr->VCFCutoff = (uint8_t)cut;
          dr->VCFResonance = (uint8_t)res; dr->EG2Attack = 0; dr->EG2Sustain = 1000; dr->EG2InfiniteSustain = true;
          dr->SetVCFVelocityScale(0);
          dr->SetVCFCutoffController(gig::vcf_cutoff_ctrl_none);
        }
      } else {
        throw gig::Exception("unknown line " + kind);
      }
    }
    file.Save(argv[2]);
    /* the wave data, straight into the saved file */
    for (auto &p : pend) {
      if (p.raw) {
        RIFF::Chunk *d = WaveAccess::data(p.s);
        d->SetPos(0);
        d->Write(p.bytes.data(), p.bytes.size(), 1);
      } else {
        p.s->Write(p.bytes.data(), p.frames);
      }
    }
    return 0;
  } catch (RIFF::Exception &e) {
    fprintf(stderr, "gigmake: %s\n", e.Message.c_str());
  } catch (std::exception &e) {
    fprintf(stderr, "gigmake: %s\n", e.what());
  }
  return 1;
}
