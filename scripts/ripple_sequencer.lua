-- scripts/ripple_sequencer.lua
-- Reflection-Free 2D Ripple Musical Sequencer (Tear-Free Framebuffer)
-- for Akai APC Mini On-Chip Lua Co-Processor.
--
-- Controls:
--   Tap Pad (x, y)             : Toggle Note resonator (or fire click wave on Emitter)
--   SHIFT + Tap Pad            : Toggle BPM-synced Emitter (fires immediate wave)
--   Hold Note Pad + Fader      : Dial per-note Max Velocity (16..127)
--   Hold Emitter Pad + Fader   : Dial per-emitter Pulse Period (56..8 frames)
--   Scene Buttons 1..8         : Select Scale (Minor Pent, Dorian, Major Pent, Nat Minor,
--                                Major, Phrygian, Lydian, Hirajoshi)
--   Track Button 1             : Play / Pause steady emitter pulses
--   Track Button 8             : Clear Ripples (+SHIFT: Clear All Notes & Emitters)
--   Fader 1 (idx 0)            : BPM / Global Pulse Rate (56..8 ticks, UP=faster)
--   Fader 2 (idx 1)            : Wave Speed (3..8 Q4 units/frame, UP=faster)
--   Fader 3 (idx 2)            : Root Note (C2..C5, MIDI 36..72)
--   Fader 4 (idx 3)            : Distance Decay (0..100%, UP=faster falloff with distance)
--   Fader 5 (idx 4)            : Note Probability (0..100%, default 100%, dial down to thin)
--   Fader 8 (idx 7)            : Gate / Cooldown Length (4..16 ticks = 100..400 ms)
--   Master Fader 9 (idx 8)     : Frame Clock Fine-Tune (35..15 ms, UP=faster)
--
-- LED Palette (Default Palette 0):
--   Expanding Waves            : Green (3)
--   Note Cells                 : Yellow (8) idle, Red (15) on trigger
--   Emitter Cells              : Red (15) steady beacon, Yellow (8) flash on pulse
--
-- Memory Map mem[0..511]:
--   0..63     : Cell type (0=empty, 1=note, 2=emitter)
--   64..127   : Per-cell param (note max_vel 16..127 | emitter custom period 0 or 8..56)
--   128..191  : Cooldown / flash timer (gate_len frames, default 6 = 150 ms)
--   192..255  : Sounding MIDI note + 1 (for notes) | pulse countdown (for emitters)
--   256..271  : 8 scales x 2 base-12 packed words (notes 0..3; notes 4..6 + len*1728)
--   272..280  : Resting 7-bit CC value for faders 0..8 (for p-lock deadband latch)
--   288..431  : 24 Wave slots x 6 words { wx, wy, radius, velocity, 0, 0 }
--   448..511  : 64-pad atomic LED framebuffer

function init()
    -- Base-12 packed scales: w0 = n0 + 12*n1 + 144*n2 + 1728*n3; w1 = n4 + 12*n5 + 144*n6 (+ 1728 if len=7)
    mem[256] = 12852; mem[257] = 10   -- 0: Minor Pentatonic [0,3,5,7,10]
    mem[258] = 9096;  mem[259] = 3283 -- 1: Dorian [0,2,3,5,7,9,10]
    mem[260] = 12696; mem[261] = 9    -- 2: Major Pentatonic [0,2,4,7,9]
    mem[262] = 9096;  mem[263] = 3271 -- 3: Natural Minor [0,2,3,5,7,8,10]
    mem[264] = 9240;  mem[265] = 3427 -- 4: Major / Ionian [0,2,4,5,7,9,11]
    mem[266] = 9084;  mem[267] = 3271 -- 5: Phrygian [0,1,3,5,7,8,10]
    mem[268] = 10968; mem[269] = 3427 -- 6: Lydian [0,2,4,6,7,9,11]
    mem[270] = 12552; mem[271] = 8    -- 7: Hirajoshi [0,2,3,7,8]

    for i = 0, 8 do
        mem[272 + i] = fader(i)
    end

    pulse_div = 28
    wave_spd = 4
    scale_idx = 0
    root_note = 48
    dist_decay = 80
    note_prob = 127
    gate_len = 6
    paused = 0
    held_cell = -1
    held_fader = -1
    held_new = 0
    held_mod = 0
    held_shift = 0

    led_all(0)
    led(8, 0, 1)
    led(0, 8, 1)
    set_tempo(25)
end

function on_button(group, idx, z)
    if group == 2 then
        if z == 1 then
            led(8, scale_idx, 0)
            scale_idx = idx & 7
            led(8, scale_idx, 1)
        end
        return 1
    end
    if group == 1 and z == 1 then
        if idx == 0 then
            paused = 1 - paused
            led(0, 8, 1 + paused)
        elseif idx == 7 then
            local sh = shift()
            for c = 0, 63 do
                if mem[c] ~= 2 then
                    if mem[192 + c] > 0 then
                        note_off(0, mem[192 + c] - 1)
                        mem[192 + c] = 0
                    end
                    mem[128 + c] = 0
                end
                if sh == 1 then
                    mem[c] = 0
                    mem[64 + c] = 0
                    mem[128 + c] = 0
                    mem[192 + c] = 0
                end
            end
            for i = 288, 431 do
                mem[i] = 0
            end
            led_all(0)
        end
        return 1
    end
    if group == 1 or group == 3 then
        return 1
    end
    return 0
end

function on_fader(idx, val, hires)
    if held_cell >= 0 then
        if held_fader < 0 then
            if abs(val - mem[272 + idx]) < 3 then
                return 0
            end
            held_fader = idx
        end
        if idx == held_fader then
            held_mod = 1
            mem[272 + idx] = val
            local ct = mem[held_cell]
            if ct == 1 then
                mem[64 + held_cell] = 16 + ((val * 111) / 127)
            elseif ct == 2 then
                local p = 56 - ((val * 48) / 127)
                mem[64 + held_cell] = p
                if mem[192 + held_cell] > p then
                    mem[192 + held_cell] = p
                end
            end
        end
        return 0
    end
    if idx < 8 and val == mem[272 + idx] then
        return 0
    end

    if idx == 0 then
        mem[272] = val
        pulse_div = 56 - ((val * 48) / 127)
    elseif idx == 1 then
        mem[273] = val
        wave_spd = 3 + ((val * 5) / 127)
    elseif idx == 2 then
        mem[274] = val
        root_note = 36 + ((val * 36) / 127)
    elseif idx == 3 then
        mem[275] = val
        dist_decay = (val * 180) / 127
    elseif idx == 4 then
        mem[276] = val
        note_prob = val
    elseif idx == 7 then
        mem[279] = val
        gate_len = 4 + ((val * 12) / 127)
    elseif idx == 8 then
        if abs(val - mem[280]) >= 2 then
            mem[280] = val
            set_tempo(35 - ((hires * 20) / 16383))
        end
    else
        mem[272 + idx] = val
    end
end

function on_grid(x, y, z)
    local c = y * 8 + x
    local spawn = 0
    local clr = 0
    if z == 1 then
        held_cell = c
        held_fader = -1
        held_mod = 0
        held_shift = shift()
        local ct = mem[c]
        if held_shift == 1 then
            if ct == 2 then
                held_new = 0
            else
                if mem[192 + c] > 0 then
                    note_off(0, mem[192 + c] - 1)
                end
                mem[c] = 2
                mem[64 + c] = 0
                mem[128 + c] = 2
                mem[192 + c] = pulse_div
                held_new = 1
                spawn = 1
            end
        elseif ct == 0 then
            mem[c] = 1
            mem[64 + c] = 127
            mem[128 + c] = 0
            mem[192 + c] = 0
            held_new = 1
        else
            held_new = 0
        end
    elseif c == held_cell then
        if held_new == 0 and held_mod == 0 then
            local ct = mem[c]
            if ct == 1 then
                if mem[192 + c] > 0 then
                    note_off(0, mem[192 + c] - 1)
                end
                clr = 1
            elseif ct == 2 then
                if held_shift == 1 or shift() == 1 then
                    clr = 1
                else
                    mem[128 + c] = 2
                    spawn = 1
                end
            end
        end
        held_cell = -1
        held_fader = -1
    end

    if clr == 1 then
        mem[c] = 0
        mem[64 + c] = 0
        mem[128 + c] = 0
        mem[192 + c] = 0
    end

    if spawn == 1 then
        local s = 288
        local min_v = 200
        local best_b = 288
        while s < 432 and min_v >= 15 do
            local sv = mem[s + 3]
            if sv < min_v then
                min_v = sv
                best_b = s
            end
            s = s + 6
        end
        mem[best_b] = x * 16 + 8
        mem[best_b + 1] = y * 16 + 8
        mem[best_b + 2] = 0
        mem[best_b + 3] = 127
        mem[best_b + 4] = 0
        mem[best_b + 5] = 0
    end
    return 1
end

function on_tick(step)
    local b, wx, wy, r, vel, eff_vel, s, min_v, best_b, sv
    local rlo, rhi, rlo2, rhi2, x0, x1, y0, y1, dy, dy2, dx, d2
    local ct, cd, deg, slen, oct, rem, pitch, qb

    -- 1. Clear atomic 64-pad framebuffer (never call led_all(0) inside on_tick)
    for c = 0, 63 do
        mem[448 + c] = 0
    end

    -- 2. Cooldowns, automatic Note-Off gating, Emitter timers & static cell LEDs
    for c = 0, 63 do
        local x = c & 7
        local y = c >> 3
        ct = mem[c]
        cd = mem[128 + c]
        if cd > 0 then
            cd = cd - 1
            mem[128 + c] = cd
        end
        if ct == 2 then
            if paused == 0 then
                b = mem[192 + c] - 1
                if b <= 0 then
                    if mem[64 + c] > 0 then
                        mem[192 + c] = mem[64 + c]
                    else
                        mem[192 + c] = pulse_div
                    end
                    cd = 2
                    mem[128 + c] = 2
                    s = 288; min_v = 200; best_b = 288
                    while s < 432 and min_v >= 15 do
                        sv = mem[s + 3]
                        if sv < min_v then
                            min_v = sv
                            best_b = s
                        end
                        s = s + 6
                    end
                    mem[best_b] = x * 16 + 8
                    mem[best_b + 1] = y * 16 + 8
                    mem[best_b + 2] = 0
                    mem[best_b + 3] = 127
                    mem[best_b + 4] = 0
                    mem[best_b + 5] = 0
                else
                    mem[192 + c] = b
                end
            end
            if cd > 0 then
                mem[448 + c] = 8
            else
                mem[448 + c] = 15
            end
        else
            if cd == 0 and mem[192 + c] > 0 then
                note_off(0, mem[192 + c] - 1)
                mem[192 + c] = 0
            end
            if ct == 1 then
                if mem[192 + c] > 0 then
                    mem[448 + c] = 15
                else
                    mem[448 + c] = 8
                end
            end
        end
    end

    -- 3. Expand primary wave rings (clipped to [0..7] x [0..7], no reflections)
    for b = 288, 426, 6 do
        vel = mem[b + 3]
        if vel >= 15 then
            wx = mem[b]
            wy = mem[b + 1]
            r = mem[b + 2] + wave_spd
            mem[b + 2] = r
            eff_vel = vel - ((vel * r * dist_decay) >> 14)

            if eff_vel < 15 or (r - 6) > (max(wx, 128 - wx) + max(wy, 128 - wy)) then
                mem[b + 3] = 0
            else
                rlo = max(0, r - 5)
                rlo2 = rlo * rlo
                rhi = r + 5
                rhi2 = rhi * rhi
                y0 = max(0, (wy - rhi) >> 4)
                y1 = min(7, (wy + rhi) >> 4)
                x0 = max(0, (wx - rhi) >> 4)
                x1 = min(7, (wx + rhi) >> 4)

                if x0 <= x1 and y0 <= y1 then
                    for y = y0, y1 do
                        dy = (y * 16 + 8) - wy
                        dy2 = dy * dy
                        if dy2 <= rhi2 then
                            for x = x0, x1 do
                                dx = (x * 16 + 8) - wx
                                d2 = dx * dx + dy2
                                if d2 >= rlo2 and d2 <= rhi2 then
                                    local c = y * 8 + x
                                    ct = mem[c]
                                    if ct == 0 then
                                        mem[448 + c] = 3
                                    elseif ct == 1 and mem[128 + c] == 0 and d2 > 0 then
                                        mem[128 + c] = gate_len
                                        if rand(127) < note_prob then
                                            deg = (7 - y) * 3 + x
                                            qb = mem[257 + scale_idx * 2]
                                            if qb >= 1728 then
                                                slen = 7; qb = qb - 1728
                                            else
                                                slen = 5
                                            end
                                            oct = deg / slen
                                            rem = deg % slen
                                            if rem < 4 then
                                                pitch = mem[256 + scale_idx * 2]
                                            else
                                                pitch = qb; rem = rem - 4
                                            end
                                            while rem > 0 do
                                                pitch = pitch / 12
                                                rem = rem - 1
                                            end
                                            pitch = min(127, root_note + oct * 12 + (pitch % 12))
                                            note_on(0, pitch, max(1, (eff_vel * mem[64 + c]) / 127))
                                            mem[192 + c] = pitch + 1
                                            mem[448 + c] = 15
                                        end
                                    end
                                end
                            end
                        end
                    end
                end
            end
        end
    end

    -- 4. Flush completed framebuffer atomically
    for c = 0, 63 do
        led(c & 7, c >> 3, mem[448 + c])
    end
end
