-- scripts/bouncy_echo.lua
-- Built-in default On-Chip Lua Co-Processor script for Open APC Mini.
-- Runs alongside Monome serialosc (0403:6001) and Akai USB-MIDI (09E8:0028).
--
-- Features while playing:
--   1. Every physical pad press passes through to serialosc / MIDI (return 0)
--      and spawns an expanding varibright diamond ripple (15 -> 9 -> 4 -> 0).
--   2. Non-destructive overlay: setting led(x, y, 0) automatically restores
--      whatever LED state serialosc or Ableton had on that pad underneath!
--   3. Fader 8 (fader(7)) controls generative echo repeats (triggers real
--      serialosc mext key / MIDI Note echoes on adjacent pads!).
--   4. Fader 9 (fader(8), Master) controls ripple & echo speed (tempo).

function init()
    slot = 0
    set_tempo(55)
end

function on_grid(x, y, z)
    if z == 1 then
        local base = slot * 4
        mem[base] = x
        mem[base + 1] = y
        mem[base + 2] = 1
        -- Echo count from Fader 8 (0..3 repeats)
        mem[base + 3] = fader(7) >> 5
        slot = (slot + 1) & 3
        led(x, y, 15)
    end
    return 0
end

function on_fader(idx, val)
    if idx == 8 then
        set_tempo(25 + ((127 - val) >> 1))
    end
end

function on_tick(step)
    led_all(0)
    for i = 0, 3 do
        local base = i * 4
        local r = mem[base + 2]
        if r > 0 then
            local cx = mem[base]
            local cy = mem[base + 1]
            local echoes = mem[base + 3]
            local lvl = 15 - (r * 4)
            if lvl > 0 then
                if cx - r >= 0 then led(cx - r, cy, lvl) end
                if cx + r <= 7 then led(cx + r, cy, lvl) end
                if cy - r >= 0 then led(cx, cy - r, lvl) end
                if cy + r <= 7 then led(cx, cy + r, lvl) end
                if echoes > 0 and r <= echoes then
                    local ex = (cx + r) & 7
                    grid_key(ex, cy, 1)
                    grid_key(ex, cy, 0)
                end
                mem[base + 2] = r + 1
            else
                mem[base + 2] = 0
            end
        end
    end
end
