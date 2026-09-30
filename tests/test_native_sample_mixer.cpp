// Synthetic sample data only; no game ROM or extracted sample bytes.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "native_sample_mixer.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

namespace {
size_t allocations = 0;
}
void* operator new(size_t n) {
    ++allocations;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }

namespace {
using Bank = snd::NativeSampleBank;
using Mixer = snd::NativeSampleMixer;

void header(std::vector<uint8_t>& rom, unsigned sample, uint32_t start,
            uint32_t frames, uint32_t loop, bool packed = false) {
    assert(frames >= 1 && frames <= 65536 && loop <= 65535);
    auto* h = rom.data() + sample * 12;
    start |= packed ? 0x400000 : 0;
    h[0] = uint8_t(start >> 16); h[1] = uint8_t(start >> 8); h[2] = uint8_t(start);
    h[3] = uint8_t(loop >> 8); h[4] = uint8_t(loop);
    const uint32_t encoded_end = 65536 - frames;
    h[5] = uint8_t(encoded_end >> 8); h[6] = uint8_t(encoded_end);
}

std::vector<uint8_t> fixture(size_t bytes = 0x400000) {
    std::vector<uint8_t> rom(bytes, 0);
    for (unsigned s = 0; s < Bank::kSamples; ++s) header(rom, s, 0, 1, 1);
    return rom;
}

void near(float actual, float expected, float tolerance = 0.000001f) {
    assert(std::isfinite(actual));
    assert(std::fabs(actual - expected) <= tolerance);
}

Mixer::VoiceParams instant() {
    Mixer::VoiceParams p;
    p.envelope.attack_seconds = 0;
    p.envelope.release_seconds = 0;
    p.pan = -1;
    return p;
}

void decoder() {
    auto rom = fixture();
    header(rom, 0, 0x2000, 4, 1);
    rom[0x2000] = 0; rom[0x2001] = 64;
    rom[0x2002] = 128; rom[0x2003] = 255;
    header(rom, 1, 0x3000, 4, 0, true);
    // Four signed 12-bit values: -2048, -1, 0, +2047.
    const uint8_t packed[] = {0x80, 0xf0, 0xff, 0x00, 0xf0, 0x7f};
    std::memcpy(rom.data() + 0x3000, packed, sizeof packed);
    header(rom, 2, 0xfffff, 3, 0);
    rom[0xfffff] = 32; rom[0x200000] = 64; rom[0x200001] = 96;
    header(rom, 3, 0x200000, 1, 0); // logical unmapped area, not physical ROM
    header(rom, 4, 0x1fffff, 2, 0); // crosses the logical window end
    header(rom, 5, 0x2000, 4, 4); // invalid loop
    header(rom, 6, 0x4000, 65536, 0);
    Bank bank(rom.data(), rom.size(), 2);
    assert(bank.valid_sample_count() == 4);
    near(bank.value(0, 0), 0); near(bank.value(0, 1), 0.5f);
    near(bank.value(0, 2), -1); near(bank.value(0, 3), -1.0f / 128);
    near(bank.value(1, 0), -1); near(bank.value(1, 1), -1.0f / 2048);
    near(bank.value(1, 2), 0); near(bank.value(1, 3), 2047.0f / 2048);
    near(bank.value(2, 0), 0.25f); near(bank.value(2, 1), 0.5f);
    near(bank.value(2, 2), 0.75f);
    assert(bank.status(3) == Bank::Status::UnmappedData);
    assert(bank.status(4) == Bank::Status::UnmappedData);
    assert(bank.status(5) == Bank::Status::InvalidLoop);
    assert(bank.sample(6)->frames == 65536);
    near(bank.value(0, 4), 0); near(bank.value(512, 0), 0);
    assert(!bank.sample(512));
    for (unsigned selected = 0; selected < 4; ++selected) {
        header(rom, 7, 0x100010, 1, 0);
        rom[selected * 0x100000 + 0x10] = uint8_t(16 * selected);
        // This modifies an unrelated invalid header in the fixed page.
        Bank view(rom.data(), rom.size(), selected);
        near(view.value(7, 0), float(selected) / 8);
        near(view.value(0, 1), 0.5f); // the fixed range never changes bank
    }
    assert(!bank.load(nullptr, rom.size(), 0));
    assert(!bank.sample(0));
    assert(!bank.load(rom.data(), 6143, 0));
    assert(!bank.load(rom.data(), rom.size(), 4));

    auto odd = fixture(0x2005);
    header(odd, 0, 0x2000, 3, 0, true);
    odd[0x2000] = 0x80; odd[0x2001] = 0xf0; odd[0x2002] = 0xff;
    odd[0x2003] = 0x7f; odd[0x2004] = 0x0f;
    Bank final(odd.data(), odd.size(), 0);
    assert(final.sample(0));
    near(final.value(0, 2), 2047.0f / 2048);
    Bank truncated(odd.data(), odd.size() - 1, 0);
    assert(truncated.status(0) == Bank::Status::UnmappedData);
    header(odd, 1, 0x100000, 1, 0);
    Bank missing_selected(odd.data(), odd.size(), 3);
    assert(!missing_selected.sample(1));
}

void exhaustive_codes_and_malformed_tables() {
    auto rom = fixture(0x5000);
    header(rom, 0, 0x2000, 256, 0);
    for (unsigned i = 0; i < 256; ++i) rom[0x2000 + i] = uint8_t(i);
    header(rom, 1, 0x3000, 4096, 0, true);
    for (unsigned i = 0; i < 4096; i += 2) {
        const unsigned address = 0x3000 + (i / 2) * 3;
        rom[address] = uint8_t(i >> 4);
        rom[address + 1] = uint8_t((i & 15) | ((i + 1) & 15) << 4);
        rom[address + 2] = uint8_t((i + 1) >> 4);
    }
    Bank bank(rom.data(), rom.size(), 0);
    for (unsigned i = 0; i < 256; ++i)
        near(bank.value(0, i), float(i < 128 ? int(i) : int(i) - 256) / 128);
    for (unsigned i = 0; i < 4096; ++i)
        near(bank.value(1, i), float(i < 2048 ? int(i) : int(i) - 4096) / 2048);
    // Deterministically exercise malformed tables, each truncated at a distinct
    // byte. Valid samples are probed at both boundaries under the sanitizers.
    uint32_t state = 0xf0359721;
    for (unsigned trial = 0; trial < 400; ++trial) {
        for (unsigned i = 0; i < 512 * 12; ++i) {
            state ^= state << 13; state ^= state >> 17; state ^= state << 5;
            rom[i] = uint8_t(state);
        }
        assert(bank.load(rom.data(), rom.size() - trial, trial % 4));
        for (unsigned s = 0; s < 512; ++s) {
            const auto* sample = bank.sample(s);
            if (!sample) continue;
            for (uint32_t f : {uint32_t(0), sample->frames - 1}) {
                const float v = bank.value(s, f);
                assert(std::isfinite(v) && v >= -1 && v < 1);
            }
            near(bank.value(s, sample->frames), 0);
            near(bank.value(s, UINT32_MAX), 0);
        }
    }
}

void playback() {
    auto rom = fixture(0x2010);
    header(rom, 0, 0x2000, 4, 1);
    rom[0x2000] = 0; rom[0x2001] = 64;
    rom[0x2002] = 128; rom[0x2003] = 127;
    Bank bank(rom.data(), rom.size(), 0);
    Mixer mixer;
    assert(mixer.set_master_gain(1));
    auto p = instant(); p.loop = false; p.pitch = 0.5;
    assert(mixer.note_on(0, bank, 0, p));
    std::array<float, 20> out;
    out.fill(9);
    mixer.render(out.data(), 10);
    const float expected[] = {0, 0.25f, 0.5f, -0.25f, -1,
        -1.0f / 256, 127.0f / 128, 127.0f / 128, 0, 0};
    for (unsigned f = 0; f < 10; ++f) {
        near(out[f * 2], expected[f]); near(out[f * 2 + 1], 0);
    }
    assert(!mixer.active(0));
    p.loop = true; p.pitch = 5;
    assert(mixer.note_on(0, bank, 0, p));
    mixer.render(out.data(), 4);
    near(out[0], 0); near(out[2], -1); near(out[4], 0.5f); near(out[6], 127.0f / 128);
    p.pitch = 1; p.start_frame = 2; p.loop_start = 2; p.loop_end = 3;
    assert(mixer.note_on(0, bank, 0, p));
    mixer.render(out.data(), 4);
    for (unsigned f = 0; f < 4; ++f) near(out[f * 2], -1);
    assert(mixer.set_pan(0, 1)); assert(mixer.set_gain(0, 0.5f));
    mixer.render(out.data(), 1); near(out[0], 0); near(out[1], -0.5f);
    assert(mixer.set_pan(0, 0));
    mixer.render(out.data(), 1);
    near(out[0], -std::sqrt(0.5f) / 2); near(out[1], out[0]);
    mixer.note_off(0); assert(!mixer.active(0));
    out.fill(9); mixer.render(out.data(), 10);
    for (float f : out) near(f, 0);
}

void envelope_and_blocks() {
    auto rom = fixture(0x2001);
    header(rom, 0, 0x2000, 1, 0); rom[0x2000] = 64;
    Bank bank(rom.data(), rom.size(), 0);
    Mixer mixer; mixer.set_master_gain(1);
    auto p = instant();
    p.envelope.attack_seconds = 4.0f / 48000;
    p.envelope.decay_seconds = 2.0f / 48000;
    p.envelope.sustain = 0.5f;
    p.envelope.release_seconds = 4.0f / 48000;
    assert(mixer.note_on(0, bank, 0, p));
    std::array<float, 32> out{};
    mixer.render(out.data(), 8);
    const float expected[] = {0.125f, 0.25f, 0.375f, 0.5f, 0.375f, 0.25f, 0.25f, 0.25f};
    for (unsigned i = 0; i < 8; ++i) near(out[i * 2], expected[i]);
    mixer.note_off(0); mixer.render(out.data(), 1);
    near(out[0], 0.1875f);
    mixer.note_off(0); // repeated gate-off must not extend the release
    mixer.render(out.data(), 4);
    near(out[0], 0.125f); near(out[2], 0.0625f); near(out[4], 0); near(out[6], 0);
    assert(!mixer.active(0));

    Mixer whole, split;
    p.pitch = 0.738921; p.pan = 0.231f;
    assert(whole.note_on(0, bank, 0, p));
    assert(split.note_on(0, bank, 0, p));
    std::array<float, 2048> a{}, b{};
    whole.render(a.data(), 1024);
    size_t offset = 0;
    for (size_t n : {size_t(1), size_t(7), size_t(64), size_t(311), size_t(641)}) {
        split.render(b.data() + 2 * offset, n); offset += n;
    }
    assert(offset == 1024 && std::memcmp(a.data(), b.data(), sizeof a) == 0);
}

void validation_and_bounded_render() {
    auto rom = fixture(0x2004);
    header(rom, 0, 0x2000, 4, 0);
    for (unsigned i = 0; i < 4; ++i) rom[0x2000 + i] = 127;
    Bank bank(rom.data(), rom.size(), 0);
    Mixer mixer;
    const size_t before = allocations;
    auto p = instant();
    for (unsigned slot = 0; slot < Mixer::kVoices; ++slot)
        assert(mixer.note_on(slot, bank, 0, p));
    assert(mixer.active_voices() == 64);
    std::array<float, 2048> out{};
    mixer.render(out.data(), 1024);
    for (unsigned i = 0; i < 1024; ++i) { near(out[i * 2], 1); near(out[i * 2 + 1], 0); }
    assert(mixer.stats().clipped_samples == 1024);
    assert(allocations == before);
    const auto rejects = mixer.stats().rejected_commands;
    p.pitch = std::numeric_limits<double>::infinity(); assert(!mixer.note_on(0, bank, 0, p));
    p = instant(); p.envelope.sustain = std::numeric_limits<float>::quiet_NaN();
    assert(!mixer.note_on(0, bank, 0, p));
    p = instant(); p.start_frame = 4; assert(!mixer.note_on(0, bank, 0, p));
    p = instant(); p.loop_start = 4; assert(!mixer.note_on(0, bank, 0, p));
    p = instant(); p.loop_end = 5; assert(!mixer.note_on(0, bank, 0, p));
    p = instant(); assert(!mixer.note_on(64, bank, 0, p));
    assert(!mixer.note_on(0, bank, 512, p));
    assert(!mixer.set_pitch(0, 0)); assert(!mixer.set_pitch(0, -1));
    assert(!mixer.set_pitch(0, 257)); assert(!mixer.set_gain(0, -1));
    assert(!mixer.set_pan(0, 2)); assert(!mixer.set_master_gain(-1));
    assert(mixer.stats().rejected_commands == rejects + 13);
    assert(mixer.active_voices() == 64); // rejected replacement leaves voice intact
    for (unsigned i = 0; i < 100; ++i) {
        assert(mixer.set_pitch(0, 1.0 / 4294967296.0));
        mixer.render(out.data(), 1024);
    }
    assert(allocations == before);
    mixer.all_stop(); assert(mixer.active_voices() == 0);
    mixer.render(out.data(), 1024);
    for (float f : out) near(f, 0);
    mixer.render(nullptr, 0);
    mixer.render(out.data(), 0);
}
} // namespace

int main() {
    decoder();
    exhaustive_codes_and_malformed_tables();
    playback();
    envelope_and_blocks();
    validation_and_bounded_render();
    std::puts("native sample decoder/mixer: bounds, 8/12-bit, bank windows, loop/pitch, ADSR, pan, allocation-free render passed");
}
