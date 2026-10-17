// Boot ROM: the CD audio player, the visualizers and the audio panels.
//
// CD audio: .WAV files (16-bit PCM, mono or stereo, 8-48 kHz) in the disc's
// root directory. A track streams through a 256 KB ring in two halves: while
// Tyke voices 16 (left) and 17 (right) play one half, the drive refills the
// other through a staging area (the copy also turns little-endian samples
// into the big-endian words Tyke reads).
//
// Streamed themes: LADP (IMA ADPCM, tools/ima_adpcm.h) in the ROM, decoded by
// Tuffy a block at a time into the same ring and voices, kept half a ring ahead.
//
// Visualizers read the mixer's scope buffer (the last 256 output frames):
// a 16-band spectrum from Goertzel filters and an oscilloscope trace.
#include "rom/builder.h"

namespace lion::romgen {

void RomBuilder::audio_routines() {
  constexpr u32 kV = kMusicVoice;

  // ---- music_voices(): volumes of the two voices for the track's channel count.
  a_.label("music_vols");
  ram_ld(3, kMChans);
  a_ << cmpwi(0, 3, 2);
  a_.beq("mv_stereo");
  a_.li32(4, 0x0100'0100);
  a_ << stw(4, voice_reg(kV, 4), rTyke) << li(4, 0) << stw(4, voice_reg(kV + 1, 4), rTyke) << blr();
  a_.label("mv_stereo");
  a_.li32(4, 0x0100'0000);
  a_ << stw(4, voice_reg(kV, 4), rTyke) << li(4, 0x100) << stw(4, voice_reg(kV + 1, 4), rTyke) << blr();

  // ---- music_voices_on: key on the ring for the current track.
  a_.label("music_voices_on");
  prologue();
  a_.li32(3, kRing);
  a_.li32(4, 2 * kRingHalf / 2);  // ring length in samples
  ram_ld(5, kMPitch);
  for (u32 v = kV; v <= kV + 1; ++v) {
    a_ << stw(3, voice_reg(v, 0), rTyke) << stw(4, voice_reg(v, 1), rTyke) << li(0, 0) << stw(0, voice_reg(v, 2), rTyke);
    a_ << stw(5, voice_reg(v, 3), rTyke);
    if (v == kV) a_ << addi(3, 3, 2);  // the right channel is the odd sample
  }
  a_.bl("music_vols");
  a_ << li(0, 3) << stw(0, voice_reg(kV, 5), rTyke);
  ram_ld(3, kMChans);
  a_ << cmpwi(0, 3, 2);
  a_.bne("mvo_mono");
  a_ << stw(0, voice_reg(kV + 1, 5), rTyke);
  a_.label("mvo_mono");
  epilogue();

  // ---- music_cancel: abandons a read in flight.
  a_.label("music_cancel");
  ram_ld(3, kMReading);
  a_ << cmpwi(0, 3, 0);
  a_.beq("mc_done");
  a_ << li(0, Cdrom::kStop) << stw(0, 4, rCd) << li(0, Cdrom::kAck) << stw(0, 4, rCd);
  ram_set(kCdOwner, kOwnerNone);
  ram_set(kMReading, 0);
  a_.label("mc_done");
  a_ << blr();

  // ---- stream_start(r3 = LADP blob) / stream_stop: the theme's music stream.
  a_.label("stream_start");
  ram_st(3, kTsBase);
  a_ << lwz(4, 8, 3);
  ram_st(4, kTsBlocks);
  a_ << lwz(4, 12, 3);
  ram_st(4, kTsRate);
  for (u32 off : {kTsBlock, kTsW, kTsStarted}) ram_set(off, 0);
  ram_set(kTsActive, 1);
  a_ << li(0, 0) << stw(0, voice_reg(kV, 5), rTyke) << stw(0, voice_reg(kV + 1, 5), rTyke) << blr();
  a_.label("stream_stop");
  ram_ld(3, kTsActive);
  a_ << cmpwi(0, 3, 0);
  a_.beq("ss_done");
  ram_set(kTsActive, 0);
  a_ << li(0, 0) << stw(0, voice_reg(kV, 5), rTyke) << stw(0, voice_reg(kV + 1, 5), rTyke);
  a_.label("ss_done");
  a_ << blr();

  // ---- stream_block: decodes the next LADP block into the ring at kTsW.
  a_.label("stream_block");
  ram_ld(3, kTsBase);
  ram_ld(4, kTsBlock);
  a_ << mulli(4, 4, int16_t(ima::kBlockBytes)) << add(3, 3, 4) << addi(3, 3, int16_t(ima::kHeaderBytes));
  ram_ld(8, kTsW);
  a_.li32(0, kRing);
  a_ << slwi(8, 8, 2) << add(8, 8, 0);
  a_.la(9, "ima_step");
  a_.la(10, "ima_index");
  a_ << lha(4, 0, 3) << lbz(5, 2, 3) << lha(6, 4, 3) << lbz(7, 6, 3) << addi(3, 3, 8);
  a_ << li(0, int16_t(ima::kBlockFrames)) << mtctr(0);
  // One IMA step: nibble in r13, predictor and index in (pred, idx); r0, r2, r12 scratch.
  auto decode = [&](u32 pred, u32 idx) {
    const std::string b4 = L("ima4"), b2 = L("ima2"), b1 = L("ima1"), b8 = L("ima8"), hi = L("imahi"), lo = L("imalo"),
                      i0 = L("imai0"), i88 = L("imai88");
    a_ << slwi(2, idx, 1) << lhzx(12, 9, 2) << srwi(0, 12, 3);
    a_ << andi_(2, 13, 4);
    a_.beq(b4);
    a_ << add(0, 0, 12);
    a_.label(b4);
    a_ << andi_(2, 13, 2);
    a_.beq(b2);
    a_ << srwi(2, 12, 1) << add(0, 0, 2);
    a_.label(b2);
    a_ << andi_(2, 13, 1);
    a_.beq(b1);
    a_ << srwi(2, 12, 2) << add(0, 0, 2);
    a_.label(b1);
    a_ << andi_(2, 13, 8);
    a_.beq(b8);
    a_ << neg(0, 0);
    a_.label(b8);
    a_ << add(pred, pred, 0) << cmpwi(0, pred, 32767);
    a_.ble(hi);
    a_ << li(pred, 32767);
    a_.label(hi);
    a_ << cmpwi(0, pred, -32768);
    a_.bge(lo);
    a_ << li(pred, -32768);
    a_.label(lo);
    a_ << slwi(2, 13, 2) << lwzx(2, 10, 2) << add(idx, idx, 2) << cmpwi(0, idx, 0);
    a_.bge(i0);
    a_ << li(idx, 0);
    a_.label(i0);
    a_ << cmpwi(0, idx, 88);
    a_.ble(i88);
    a_ << li(idx, 88);
    a_.label(i88);
  };
  a_.label("sb_frame");
  a_ << lbz(11, 0, 3) << addi(3, 3, 1) << srwi(13, 11, 4);
  decode(4, 5);
  a_ << sth(4, 0, 8) << clrlwi(13, 11, 28);
  decode(6, 7);
  a_ << sth(6, 2, 8) << addi(8, 8, 4);
  a_.bdnz("sb_frame");
  ram_ld(4, kTsW);
  a_ << addi(4, 4, int16_t(ima::kBlockFrames)) << clrlwi(4, 4, 16);
  ram_st(4, kTsW);
  ram_ld(4, kTsBlock);
  ram_ld(5, kTsBlocks);
  a_ << addi(4, 4, 1) << cmplw(0, 4, 5);
  a_.blt("sb_keep");
  a_ << li(4, 0);  // loop the theme
  a_.label("sb_keep");
  ram_st(4, kTsBlock);
  a_ << blr();

  // ---- stream_update: keeps the ring half a ring ahead of the voices; starts them once full.
  a_.label("stream_update");
  prologue();
  ram_ld(3, kTsActive);
  a_ << cmpwi(0, 3, 0);
  a_.beq("su_ret");
  a_.label("su_more");
  a_ << li(5, 0);
  ram_ld(3, kTsStarted);
  a_ << cmpwi(0, 3, 0);
  a_.beq("su_pos");
  a_ << lwz(5, voice_reg(kV, 6), rTyke) << srwi(5, 5, 1);  // frame being played
  a_.label("su_pos");
  ram_ld(4, kTsW);
  a_ << subf(6, 5, 4) << clrlwi(6, 6, 16) << cmplwi(0, 6, 32768);
  a_.bge("su_full");
  a_.bl("stream_block");
  a_.b("su_more");
  a_.label("su_full");
  ram_ld(3, kTsStarted);
  a_ << cmpwi(0, 3, 0);
  a_.bne("su_ret");
  ram_set(kTsStarted, 1);
  ram_ld(3, kTsRate);
  a_.li32(4, u32(kAudioRate));
  a_ << slwi(3, 3, 16) << divwu(3, 3, 4) << slwi(3, 3, 1);  // stereo: twice the rate ratio
  a_.li32(4, kRing);
  a_.li32(5, 2 * kRingHalf / 2);
  for (u32 v = kV; v <= kV + 1; ++v) {
    a_ << stw(4, voice_reg(v, 0), rTyke) << stw(5, voice_reg(v, 1), rTyke) << li(0, 0) << stw(0, voice_reg(v, 2), rTyke);
    a_ << stw(3, voice_reg(v, 3), rTyke) << addi(4, 4, 2);
  }
  a_.li32(0, 0x0100'0000);
  a_ << stw(0, voice_reg(kV, 4), rTyke) << li(0, 0x100) << stw(0, voice_reg(kV + 1, 4), rTyke);
  a_ << li(0, 3) << stw(0, voice_reg(kV, 5), rTyke) << stw(0, voice_reg(kV + 1, 5), rTyke);
  a_.label("su_ret");
  epilogue();

  // ---- music_play(r3 = track): starts streaming a track; the theme is muted.
  a_.label("music_play");
  prologue();
  ram_st(3, kMusicTrack);
  ram_set(kTsActive, 0);
  a_.bl("music_cancel");
  ram_set(kSeqPtr, 0);
  a_ << li(0, 0) << addi(4, rTyke, voice_reg(0, 5)) << li(5, int16_t(kV + 2)) << mtctr(5);
  a_.label("mp_off");
  a_ << stw(0, 0, 4) << addi(4, 4, 32);
  a_.bdnz("mp_off");
  ram_ld(3, kMusicTrack);
  a_ << mulli(3, 3, int16_t(kTrackSize)) << add(3, 3, rRam) << lwz(4, int16_t(kTracks), 3) << lwz(5, int16_t(kTracks + 4), 3);
  ram_st(4, kMLba);
  ram_st(5, kMFsize);
  for (u32 off : {kMPhase, kMEof, kMusicError, kMusicElapsed}) ram_set(off, 0);
  ram_set(kMusicState, kMusicStarting);
  epilogue();

  // ---- music_stop: silences the player (the caller restarts the theme if it wants).
  a_.label("music_stop");
  prologue();
  a_.bl("music_cancel");
  a_ << li(0, 0) << stw(0, voice_reg(kV, 5), rTyke) << stw(0, voice_reg(kV + 1, 5), rTyke);
  ram_set(kMusicState, kMusicStopped);
  epilogue();

  // ---- music_toggle: pause / resume (pitch 0 freezes the voices).
  a_.label("music_toggle");
  prologue();
  ram_ld(3, kMusicState);
  a_ << cmpwi(0, 3, kMusicPlaying);
  a_.bne("mt_resume");
  a_ << li(0, 0);
  for (u32 v = kV; v <= kV + 1; ++v) a_ << stw(0, voice_reg(v, 3), rTyke) << stw(0, voice_reg(v, 4), rTyke);
  ram_set(kMusicState, kMusicPaused);
  a_.b("mt_done");
  a_.label("mt_resume");
  a_ << cmpwi(0, 3, kMusicPaused);
  a_.bne("mt_done");
  ram_ld(0, kMPitch);
  a_ << stw(0, voice_reg(kV, 3), rTyke) << stw(0, voice_reg(kV + 1, 3), rTyke);
  a_.bl("music_vols");
  ram_set(kMusicState, kMusicPlaying);
  a_.label("mt_done");
  epilogue();

  // ---- cd_start(r6 = LBA, r7 = sectors, r8 = destination): a read owned by the player.
  a_.label("cd_start");
  a_ << stw(6, 8, rCd) << stw(7, 12, rCd) << stw(8, 16, rCd) << li(0, Cdrom::kRead) << stw(0, 4, rCd);
  ram_set(kCdOwner, kOwnerMusic);
  ram_set(kMReading, 1);
  a_ << blr();

  // ---- chunk_read: the next 128 KB of sample data (whole sectors, into the staging area).
  a_.label("chunk_read");
  ram_ld(3, kMDoff);
  ram_ld(4, kMFcur);
  a_ << add(3, 3, 4) << srwi(5, 3, 11) << clrlwi(3, 3, 21);
  ram_st(3, kMSkip);
  ram_ld(4, kMFsize);
  a_ << addi(4, 4, 2047) << srwi(4, 4, 11) << subf(7, 5, 4) << cmpwi(0, 7, int16_t(kChunkSectors));
  a_.ble("cr_count");
  a_ << li(7, int16_t(kChunkSectors));
  a_.label("cr_count");
  ram_ld(6, kMLba);
  a_ << add(6, 6, 5);
  a_.li32(8, kStage);
  a_.b("cd_start");

  // ---- chunk_copy(r3 = half): staging -> ring, byte-swapping each sample; zero past the end.
  a_.label("chunk_copy");
  ram_ld(4, kMDlen);
  ram_ld(5, kMFcur);
  a_ << subf(6, 5, 4);  // bytes left
  a_.li32(7, kRingHalf);
  a_ << cmplw(0, 6, 7);
  a_.ble("cc_n");
  a_ << mr(6, 7);
  a_.label("cc_n");
  a_ << add(5, 5, 6);
  ram_st(5, kMFcur);
  a_ << cmplw(0, 5, 4);
  a_.blt("cc_more");
  ram_set(kMEof, 1);
  a_.label("cc_more");
  a_.li32(8, kStage);
  ram_ld(9, kMSkip);
  a_ << add(8, 8, 9);
  a_.li32(9, kRing);
  a_ << slwi(3, 3, 17) << add(9, 9, 3);
  a_ << srwi(10, 6, 2) << cmpwi(0, 10, 0);
  a_.beq("cc_zero");
  a_ << mtctr(10);
  a_.label("cc_copy");
  a_ << lwbrx(0, 0, 8) << rlwinm(0, 0, 16, 0, 31) << stw(0, 0, 9) << addi(8, 8, 4) << addi(9, 9, 4);
  a_.bdnz("cc_copy");
  a_.label("cc_zero");
  a_ << subf(10, 6, 7) << srwi(10, 10, 2) << cmpwi(0, 10, 0);
  a_.beq("cc_done");
  a_ << mtctr(10) << li(0, 0);
  a_.label("cc_clear");
  a_ << stw(0, 0, 9) << addi(9, 9, 4);
  a_.bdnz("cc_clear");
  a_.label("cc_done");
  a_ << blr();

  // ---- wav_parse: RIFF header in the staging sector -> r3 = 0 (ok) or 1.
  a_.label("wav_parse");
  a_.li32(6, kStage);
  a_ << lwz(7, 0, 6);
  a_.li32(8, 0x5249'4646);  // "RIFF"
  a_ << cmpw(0, 7, 8);
  a_.bne("wp_fail");
  a_ << lwz(7, 8, 6);
  a_.li32(8, 0x5741'5645);  // "WAVE"
  a_ << cmpw(0, 7, 8);
  a_.bne("wp_fail");
  a_ << li(9, 12) << li(10, 0);
  a_.label("wp_chunk");
  a_ << cmpwi(0, 9, 2040);
  a_.bgt("wp_fail");
  a_ << lwzx(7, 6, 9) << addi(11, 9, 4) << lwbrx(12, 6, 11);
  a_.li32(8, 0x666D'7420);  // "fmt "
  a_ << cmpw(0, 7, 8);
  a_.bne("wp_not_fmt");
  a_ << addi(11, 9, 8) << lhbrx(0, 6, 11) << cmpwi(0, 0, 1);  // PCM
  a_.bne("wp_fail");
  a_ << addi(11, 9, 10) << lhbrx(0, 6, 11);
  ram_st(0, kMChans);
  a_ << addi(11, 9, 12) << lwbrx(0, 6, 11);
  ram_st(0, kMRate);
  a_ << addi(11, 9, 22) << lhbrx(0, 6, 11) << cmpwi(0, 0, 16);
  a_.bne("wp_fail");
  a_ << li(10, 1);
  a_.label("wp_not_fmt");
  a_.li32(8, 0x6461'7461);  // "data"
  a_ << cmpw(0, 7, 8);
  a_.beq("wp_data");
  a_ << add(9, 9, 12) << addi(9, 9, 8) << andi_(0, 12, 1) << add(9, 9, 0);
  a_.b("wp_chunk");
  a_.label("wp_data");
  a_ << cmpwi(0, 10, 0);
  a_.beq("wp_fail");
  a_ << addi(0, 9, 8);
  ram_st(0, kMDoff);
  // Validate, and clip the data to the file.
  ram_ld(4, kMChans);
  a_ << addi(0, 4, -1) << cmplwi(0, 0, 1);
  a_.bgt("wp_fail");
  ram_ld(5, kMRate);
  a_ << cmplwi(0, 5, 8000);
  a_.blt("wp_fail");
  a_.li32(0, 48000);
  a_ << cmplw(0, 5, 0);
  a_.bgt("wp_fail");
  ram_ld(0, kMFsize);
  ram_ld(7, kMDoff);
  a_ << subf(0, 7, 0) << cmplw(0, 12, 0);
  a_.ble("wp_len");
  a_ << mr(12, 0);
  a_.label("wp_len");
  ram_st(12, kMDlen);
  // PITCH = channels * rate / 44100 (16.16): stereo voices skip every other sample.
  a_ << slwi(6, 5, 16);
  a_.li32(7, u32(kAudioRate));
  a_ << divwu(6, 6, 7) << mullw(6, 6, 4);
  ram_st(6, kMPitch);
  // Output frames = input frames * 44100 / rate.
  a_ << slwi(7, 4, 1) << divwu(8, 12, 7);
  a_.li32(9, u32(kAudioRate) << 16);
  a_ << divwu(9, 9, 5) << mulhwu(10, 8, 9) << mullw(11, 8, 9) << slwi(10, 10, 16) << srwi(11, 11, 16) << or_(10, 10, 11);
  ram_st(10, kMTotal);
  ram_set(kMFcur, 0);
  ram_set(kMEof, 0);
  a_ << li(3, 0) << blr();
  a_.label("wp_fail");
  a_ << li(3, 1) << blr();

  // ---- music_update: called once per dashboard frame.
  a_.label("music_update");
  prologue();
  ram_ld(3, kMusicState);
  a_ << cmpwi(0, 3, kMusicStopped);
  a_.beq("mu_ret");
  a_ << lwz(4, 0, rCd) << andi_(0, 4, Cdrom::kPresent);
  a_.bne("mu_present");
  a_.bl("music_stop");  // disc gone
  a_.bl("theme_start");
  a_.b("mu_ret");
  a_.label("mu_present");
  ram_ld(5, kMReading);
  a_ << cmpwi(0, 5, 0);
  a_.beq("mu_issue");
  a_ << andi_(0, 4, Cdrom::kDone);
  a_.beq("mu_tick");
  a_ << li(0, Cdrom::kAck) << stw(0, 4, rCd);
  ram_set(kCdOwner, kOwnerNone);
  ram_set(kMReading, 0);
  a_ << andi_(0, 4, Cdrom::kError);
  a_.bne("mu_fail");
  ram_ld(5, kMPhase);
  a_ << cmpwi(0, 5, kPhaseHeader);
  a_.bne("mu_chunk_done");
  a_.bl("wav_parse");
  a_ << cmpwi(0, 3, 0);
  a_.bne("mu_fail");
  ram_set(kMPhase, kPhaseHalf0);
  a_.b("mu_issue");
  a_.label("mu_chunk_done");
  a_ << cmpwi(0, 5, kPhaseRun);
  a_.beq("mu_half_run");
  a_ << addi(3, 5, -1);
  a_.b("mu_copy");
  a_.label("mu_half_run");
  ram_ld(3, kMHalf);
  a_.label("mu_copy");
  a_.bl("chunk_copy");
  ram_ld(5, kMPhase);
  a_ << cmpwi(0, 5, kPhaseRun);
  a_.bne("mu_fill_next");
  ram_ld(3, kMHalf);
  a_ << xori(3, 3, 1);
  ram_st(3, kMHalf);
  a_.b("mu_issue");
  a_.label("mu_fill_next");
  a_ << addi(5, 5, 1);
  ram_st(5, kMPhase);
  a_ << cmpwi(0, 5, kPhaseRun);
  a_.bne("mu_issue");
  a_.bl("music_voices_on");
  ram_set(kMHalf, 0);
  ram_set(kMusicState, kMusicPlaying);
  a_.label("mu_issue");
  ram_ld(0, kCdOwner);
  a_ << cmpwi(0, 0, kOwnerNone);
  a_.bne("mu_tick");
  ram_ld(5, kMPhase);
  a_ << cmpwi(0, 5, kPhaseHeader);
  a_.bne("mu_issue_chunk");
  ram_ld(6, kMLba);
  a_ << li(7, 1);
  a_.li32(8, kStage);
  a_.bl("cd_start");
  a_.b("mu_tick");
  a_.label("mu_issue_chunk");
  ram_ld(0, kMEof);
  a_ << cmpwi(0, 0, 0);
  a_.beq("mu_more_data");
  // A track shorter than half the ring: silence the second half and start.
  a_ << cmpwi(0, 5, kPhaseHalf1);
  a_.bne("mu_tick");
  a_ << li(3, 1);
  a_.bl("chunk_copy");
  ram_ld(5, kMPhase);
  a_.b("mu_fill_next");
  a_.label("mu_more_data");
  a_ << cmpwi(0, 5, kPhaseRun);
  a_.bne("mu_do_chunk");
  // Streaming: refill a half only once the voice has moved on to the other one.
  a_ << lwz(6, voice_reg(kV, 6), rTyke) << srwi(6, 6, 16);
  ram_ld(7, kMHalf);
  a_ << cmpw(0, 6, 7);
  a_.beq("mu_tick");
  a_.label("mu_do_chunk");
  a_.bl("chunk_read");
  a_.label("mu_tick");
  ram_ld(3, kMusicState);
  a_ << cmpwi(0, 3, kMusicPlaying);
  a_.bne("mu_ret");
  ram_ld(4, kMusicElapsed);
  a_ << addi(4, 4, int16_t(kAudioRate / kFrameRate));
  ram_st(4, kMusicElapsed);
  ram_ld(5, kMTotal);
  a_ << cmplw(0, 4, 5);
  a_.blt("mu_ret");
  // Track over: the next one, or back to the theme after the last.
  ram_ld(3, kMusicTrack);
  ram_ld(4, kTrackCount);
  a_ << addi(3, 3, 1) << cmplw(0, 3, 4);
  a_.bge("mu_end");
  a_.bl("music_play");
  a_.b("mu_ret");
  a_.label("mu_end");
  a_.bl("music_stop");
  a_.bl("theme_start");
  a_.b("mu_ret");
  a_.label("mu_fail");
  a_.bl("music_stop");
  ram_set(kMusicError, 1);
  a_.bl("theme_start");
  a_.label("mu_ret");
  epilogue();

  // ---- time_string: "MM:SS / MM:SS" (elapsed / total) into kTimeStr.
  a_.label("time_string");
  prologue();
  ram_ld(3, kMusicElapsed);
  a_.li32(5, u32(kAudioRate));
  a_ << divwu(3, 3, 5) << addi(4, rRam, int16_t(kTimeStr));
  a_.bl("fmt_time");
  a_.la(3, "s_slash");
  a_.bl("strcpy");
  ram_ld(3, kMTotal);
  a_.li32(5, u32(kAudioRate));
  a_ << divwu(3, 3, 5);
  a_.bl("fmt_time");
  epilogue();

  // ---- viz_update: scope buffer -> window -> 16 Goertzel bands, smoothed into kVizBars.
  a_.label("viz_update");
  a_ << lwz(5, int16_t(kScopePos * 4), rTyke) << li(6, 0);  // oldest frame first
  lf(20, 1.0f / 65536.0f);
  a_.label("vu_read");
  a_ << add(7, 5, 6) << clrlwi(7, 7, 24) << slwi(7, 7, 3) << add(7, 7, rTyke);
  a_ << lwz(8, int16_t(kScopeBase * 4), 7) << lwz(9, int16_t(kScopeBase * 4 + 4), 7) << add(8, 8, 9);
  i2f(5, 8);
  a_ << fmuls(5, 5, 20);  // -1..1
  a_ << slwi(7, 6, 2) << add(9, 7, rPool) << lfs(6, int16_t(hann_), 9) << fmuls(6, 5, 6);
  a_ << add(9, 7, rRam) << stfs(6, int16_t(kVizF), 9);
  a_ << andi_(0, 6, 1);
  a_.bne("vu_odd");
  // Every other sample, three times louder and clipped to the trace's box: 128 points.
  lf(6, 3.0f);
  a_ << fmuls(5, 5, 6) << fsubs(6, 5, 30) << fsel(5, 6, 30, 5) << fadds(6, 5, 30) << fneg(7, 30) << fsel(5, 6, 5, 7);
  a_ << slwi(7, 6, 1) << add(9, 7, rRam) << stfs(5, int16_t(kScopeY), 9);
  a_.label("vu_odd");
  a_ << addi(6, 6, 1) << cmpwi(0, 6, 256);
  a_.blt("vu_read");
  a_ << li(5, 0);
  lf(21, 1.0f / 544.0f), lf(22, 0.035f), lf(23, 160.0f);
  a_.label("vu_band");
  a_ << slwi(7, 5, 2) << add(7, 7, rPool) << lfs(10, int16_t(goertzel_), 7);
  a_ << fmr(11, 31) << fmr(12, 31) << addi(6, rRam, int16_t(kVizF)) << li(0, 256) << mtctr(0);
  a_.label("vu_goertzel");
  a_ << lfs(9, 0, 6) << addi(6, 6, 4) << fmadds(13, 10, 11, 9) << fsubs(13, 13, 12) << fmr(12, 11) << fmr(11, 13);
  a_.bdnz("vu_goertzel");
  // Power = s1^2 + s2^2 - c s1 s2; 32 log2(power) from its float bits.
  a_ << fmuls(1, 11, 11) << fmadds(1, 12, 12, 1) << fmuls(2, 11, 12) << fmuls(2, 2, 10) << fsubs(1, 1, 2);
  a_ << stfs(1, kScratch, rRam) << lwz(8, kScratch, rRam) << srawi(8, 8, 18) << addi(8, 8, -127 * 32);
  i2f(1, 8);
  a_ << fadds(1, 1, 23) << fmuls(1, 1, 21) << fsel(1, 1, 1, 31);  // 0.. (clamped below)
  a_ << fsubs(2, 1, 30) << fsel(1, 2, 30, 1);                      // ..1
  // Bars fall slowly.
  a_ << slwi(7, 5, 2) << add(7, 7, rRam) << lfs(3, int16_t(kVizBars), 7) << fsubs(3, 3, 22);
  a_ << fsubs(4, 1, 3) << fsel(1, 4, 1, 3) << fsel(1, 1, 1, 31) << stfs(1, int16_t(kVizBars), 7);
  a_ << addi(5, 5, 1) << cmpwi(0, 5, kVizBands);
  a_.blt("vu_band");
  a_ << blr();

  // ---- viz_spectrum(f1 x, f2 bottom, f3 width, f4 height).
  a_.label("viz_spectrum");
  prologue();
  a_ << fmr(20, 1) << fmr(21, 2) << fmr(22, 4);
  lf(5, 1.0f / kVizBands);
  a_ << fmuls(23, 3, 5);  // band pitch
  lf(5, 0.72f);
  a_ << fmuls(24, 23, 5) << li(13, 0);  // bar width
  a_.label("vs_bar");
  a_ << slwi(7, 13, 2) << add(7, 7, rRam) << lfs(5, int16_t(kVizBars), 7) << fmuls(5, 5, 22);
  lf(6, 2.0f);
  a_ << fadds(5, 5, 6);  // at least a sliver
  i2f(1, 13);
  a_ << fmadds(1, 1, 23, 20) << fadds(3, 1, 24) << fsubs(2, 21, 5) << fmr(4, 21);
  load(3, 0xFFFFC040), load(4, 0xFFC01818);
  a_.bl("quad");
  a_ << addi(13, 13, 1) << cmpwi(0, 13, kVizBands);
  a_.blt("vs_bar");
  epilogue();

  // ---- viz_scope(f1 x, f2 centre, f3 width, f4 half height): trace triggered on a rising edge.
  a_.label("viz_scope");
  a_ << li(5, 1);
  a_.label("vsc_trig");
  a_ << slwi(7, 5, 2) << add(7, 7, rRam) << lfs(5, int16_t(kScopeY - 4), 7) << lfs(6, int16_t(kScopeY), 7);
  a_ << fcmpu(0, 5, 31);
  a_.bge("vsc_next");
  a_ << fcmpu(0, 6, 31);
  a_.bge("vsc_found");
  a_.label("vsc_next");
  a_ << addi(5, 5, 1) << cmpwi(0, 5, 32);
  a_.blt("vsc_trig");
  a_ << li(5, 0);
  a_.label("vsc_found");
  lf(7, 1.0f / 95.0f);
  a_ << fmuls(7, 3, 7);  // x step
  lf(8, 1.3f);           // half thickness
  a_.li32(9, 0xFFFFB040);
  a_ << slwi(7, 5, 2) << add(7, 7, rRam) << addi(7, 7, int16_t(kScopeY)) << li(0, 95) << mtctr(0);
  a_ << lfs(9, 0, 7) << fnmsubs(9, 9, 4, 2) << fmr(10, 1);  // y0 = centre - s * h
  a_.label("vsc_seg");
  a_ << lfs(11, 4, 7) << addi(7, 7, 4) << fnmsubs(11, 11, 4, 2) << fadds(12, 10, 7);
  a_ << fsubs(13, 9, 8) << fadds(14, 9, 8) << fsubs(15, 11, 8) << fadds(16, 11, 8);
  tri_header();
  vtx(10, 13, 9), vtx(10, 14, 9), vtx(12, 16, 9);
  tri_header();
  vtx(10, 13, 9), vtx(12, 16, 9), vtx(12, 15, 9);
  a_ << fmr(9, 11) << fmr(10, 12);
  a_.bdnz("vsc_seg");
  a_ << blr();
}

// ------------------------------------------------------------------ panels

namespace {
constexpr u32 kTextLight = 0xFFE6DCD2, kTextGrey = 0xFF988880, kTextAmber = 0xFFFFB040, kTextTitle = 0xFFFF5A3A;
}  // namespace

void RomBuilder::panel_system() {
  mode(kMText);
  text("s_sysinfo", 320, 112, 18, kTextTitle, true);
  for (int i = 0; i < 7; ++i) {
    const float y = 160.0f + 32.0f * float(i);
    text("s_sys_k" + std::to_string(i), 80, y, 13, 0xFFFF9A40, false);
    text("s_sys_v" + std::to_string(i), 216, y, 13, kTextLight, false);
  }
}

void RomBuilder::panel_theme() {
  mode(kMText);
  text("s_theme_title", 320, 112, 18, kTextTitle, true);
  for (u32 t = 0; t < rom::kThemeCount; ++t) {
    const float y = 150.0f + 43.0f * float(t);
    const std::string no_cursor = L("pt_nocur"), no_mark = L("pt_nomark");
    ram_ld(0, kThemeCursor);
    a_ << cmpwi(0, 0, int16_t(t));
    a_.bne(no_cursor);
    mode(kMAdd);
    rect(70, y - 5, 570, y + 37, 0xFF5A1414, 0xFF1C0606);
    mode(kMText);
    a_.label(no_cursor);
    ram_ld(0, kTheme);
    a_ << cmpwi(0, 0, int16_t(t));
    a_.bne(no_mark);
    text("s_active", 86, y, 16, kTextAmber, false);
    a_.label(no_mark);
    text("s_theme_name" + std::to_string(t), 112, y, 15, kTextLight, false);
    text("s_theme_blurb" + std::to_string(t), 112, y + 19, 10, kTextGrey, false);
  }
}

void RomBuilder::panel_tyke() {
  mode(kMText);
  text("s_tyke_title", 320, 112, 18, kTextTitle, true);
  for (u32 k = 0; k < 4; ++k) {
    const float y = 160.0f + 40.0f * float(k);
    const std::string dim = L("pk_dim"), lit = L("pk_lit");
    ram_ld(0, kTykeRow);
    a_ << cmpwi(0, 0, int16_t(k));
    a_.bne(dim);
    load(4, kTextAmber);
    a_.b(lit);
    a_.label(dim);
    load(4, kTextLight);
    a_.label(lit);
    a_.la(3, "s_row" + std::to_string(k));
    lf(1, 64), lf(2, y), lf(3, 13);
    a_.bl("text");
    mode(kM2D);
    rect(150, y + 3, 270, y + 9, 0xFF301010, 0xFF301010);
    // Fill proportional to the Tyke register.
    a_ << lwz(17, int16_t(4 * k), rTyke);
    i2f(3, 17);
    lf(5, 120.0f / 256.0f), lf(1, 150);
    a_ << fmadds(3, 3, 5, 1);
    lf(2, y + 2), lf(4, y + 10);
    load(3, 0xFFFFC040), load(4, 0xFFE02020);
    a_.bl("quad");
    mode(kMText);
    a_ << mr(3, 17) << addi(4, rRam, int16_t(kPctStr + 8 * k));
    a_.bl("fmt_pct");
    a_ << addi(3, rRam, int16_t(kPctStr + 8 * k));
    text_r(278, y, 12, kTextLight, false);
  }
  // Voice meters.
  text("s_voices", 64, 312, 10, kTextGrey, false);
  text("s_spectrum", 330, 150, 10, kTextGrey, false);
  text("s_scope", 330, 304, 10, kTextGrey, false);
  mode(kM2D);
  a_ << li(17, 0);
  lf(20, 64.0f);
  a_.label("pk_bar");
  a_ << slwi(3, 17, 5) << add(3, 3, rTyke) << lwz(5, 0x100 + 20, 3) << lwz(6, 0x100 + 16, 3) << andi_(0, 5, 1);
  a_.beq("pk_bar_off");
  a_ << srwi(7, 6, 16) << clrlwi(6, 6, 16) << add(6, 6, 7);
  i2f(5, 6);
  lf(6, 0.13f), lf(7, 6.0f);
  a_ << fmadds(5, 5, 6, 7);
  load(3, 0xFFFFC848), load(4, 0xFFE02020);
  a_.b("pk_bar_draw");
  a_.label("pk_bar_off");
  lf(5, 4.0f);
  load(3, 0xFF4A1010), load(4, 0xFF4A1010);
  a_.label("pk_bar_draw");
  lf(4, 396.0f);
  a_ << fsubs(2, 4, 5) << fmr(1, 20);
  lf(6, 7.0f);
  a_ << fadds(3, 20, 6);
  a_.bl("quad");
  lf(6, 9.75f);
  a_ << fadds(20, 20, 6) << addi(17, 17, 1) << cmpwi(0, 17, Tyke::kChannels);
  a_.blt("pk_bar");
  // Spectrum and scope.
  a_.bl("viz_update");
  mode(kMAdd);
  lf(1, 330), lf(2, 288), lf(3, 250), lf(4, 120);
  a_.bl("viz_spectrum");
  lf(1, 330), lf(2, 354), lf(3, 250), lf(4, 34);
  a_.bl("viz_scope");
}

void RomBuilder::panel_music() {
  mode(kMText);
  text("s_cdaudio", 320, 112, 18, kTextTitle, true);
  ram_ld(0, kTrackCount);
  a_ << cmpwi(0, 0, 0);
  a_.bne("pm_list");
  ram_ld(0, kDiscState);
  a_ << cmpwi(0, 0, kReady);
  a_.beq("pm_notracks");
  text("s_noaudio", 320, 236, 14, kTextLight, true);
  a_.b("pm_viz");
  a_.label("pm_notracks");
  text("s_notracks", 320, 236, 14, kTextLight, true);
  a_.b("pm_viz");

  // Track list: eight rows around the cursor.
  a_.label("pm_list");
  ram_ld(17, kMusicCursor);
  a_ << addi(17, 17, -7) << cmpwi(0, 17, 0);
  a_.bge("pm_first");
  a_ << li(17, 0);
  a_.label("pm_first");
  a_ << li(18, 0);
  a_.label("pm_row");
  a_ << add(21, 17, 18);
  ram_ld(0, kTrackCount);
  a_ << cmplw(0, 21, 0);
  a_.bge("pm_rows_done");
  i2f(20, 18);
  lf(5, 24.0f), lf(6, 152.0f);
  a_ << fmadds(20, 20, 5, 6);  // y
  ram_ld(0, kMusicCursor);
  a_ << cmpw(0, 21, 0);
  a_.bne("pm_no_cursor");
  mode(kMAdd);
  lf(5, 4.0f), lf(6, 18.0f);
  a_ << fsubs(2, 20, 5) << fadds(4, 20, 6);
  lf(1, 56), lf(3, 316);
  load(3, 0xFF5A1414), load(4, 0xFF1C0606);
  a_.bl("quad");
  mode(kMText);
  a_.label("pm_no_cursor");
  a_ << addi(3, 21, 1) << addi(4, rRam, int16_t(kNumStr));
  a_.bl("fmt2");
  a_ << addi(3, rRam, int16_t(kNumStr));
  load(4, kTextGrey);
  lf(1, 66), lf(3, 13);
  a_ << fmr(2, 20);
  a_.bl("text");
  // The track playing is amber.
  load(4, kTextLight);
  ram_ld(0, kMusicState);
  a_ << cmpwi(0, 0, kMusicStopped);
  a_.beq("pm_plain");
  ram_ld(0, kMusicTrack);
  a_ << cmpw(0, 21, 0);
  a_.bne("pm_plain");
  load(4, kTextAmber);
  a_.label("pm_plain");
  a_ << mulli(3, 21, int16_t(kTrackSize)) << add(3, 3, rRam) << addi(3, 3, int16_t(kTracks + 8));
  lf(1, 102), lf(3, 13);
  a_ << fmr(2, 20);
  a_.bl("text");
  a_ << addi(18, 18, 1) << cmpwi(0, 18, 8);
  a_.blt("pm_row");
  a_.label("pm_rows_done");

  // Now playing: state, name and time.
  ram_ld(3, kMusicState);
  a_.la(4, "state_names");
  a_ << slwi(3, 3, 2) << lwzx(3, 4, 3);
  text_r(460, 150, 14, kTextLight, true);
  ram_ld(0, kMusicError);
  a_ << cmpwi(0, 0, 0);
  a_.beq("pm_no_error");
  text("s_badwav", 460, 178, 12, kTextTitle, true);
  a_.b("pm_viz");
  a_.label("pm_no_error");
  ram_ld(0, kMusicState);
  a_ << cmpwi(0, 0, kMusicStopped);
  a_.beq("pm_viz");
  ram_ld(3, kMusicTrack);
  a_ << mulli(3, 3, int16_t(kTrackSize)) << add(3, 3, rRam) << addi(3, 3, int16_t(kTracks + 8));
  text_r(460, 178, 12, kTextAmber, true);
  a_.bl("time_string");
  a_ << addi(3, rRam, int16_t(kTimeStr));
  text_r(460, 204, 14, kTextLight, true);

  a_.label("pm_viz");
  a_.bl("viz_update");
  mode(kMAdd);
  lf(1, 334), lf(2, 318), lf(3, 252), lf(4, 92);
  a_.bl("viz_spectrum");
  lf(1, 334), lf(2, 364), lf(3, 252), lf(4, 28);
  a_.bl("viz_scope");
}

}  // namespace lion::romgen
