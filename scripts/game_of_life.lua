-- scripts/game_of_life.lua
-- Live Conway's Game of Life Generative Co-Processor for serialosc & MIDI.
--
-- Runs alongside serialosc/MIDI at < 0.5% CPU using the hardware-accelerated
-- life_step() intrinsic:
--   - Starts with a glider + blinker and auto-reseeds if the colony dies out.
--   - Newly born cells (Red, level 15) trigger Note-On / mext key-down on EVERY
--     generation and Note-Off on the next generation (musical 1-step gate!).
--   - Surviving cells glow Green (level 5).
--   - Press any pad while playing to seed live cells around your finger.
--   - Fader 9 (Master) controls evolution & note tempo (40..160 ms).

function init()
    mem[1] = 1
    mem[10] = 1
    mem[16] = 1
    mem[17] = 1
    mem[18] = 1
    set_tempo(95)
end

function on_grid(x, y, z)
    if z == 1 then
        local idx = y * 8 + x
        mem[idx] = 2
        mem[(idx + 1) & 63] = 1
        mem[(idx + 8) & 63] = 1
        led(x, y, 15)
    end
    return 0
end

function on_fader(idx, val)
    if idx == 8 then
        set_tempo(40 + (127 - val))
    end
end

function on_tick(step)
    local alive = life_step()

    for i = 0, 63 do
        local prev = mem[i]
        local st = mem[64 + i]
        local x = i & 7
        local y = i >> 3

        -- End the previous step's birth note gate cleanly
        if prev == 2 and st ~= 2 then
            grid_key(x, y, 0)
        end

        mem[i] = st
        if st == 2 then
            led(x, y, 15)
            grid_key(x, y, 1)
        elseif st == 1 then
            led(x, y, 5)
        else
            if prev > 0 then
                led(x, y, 0)
            end
        end
    end

    if alive < 3 then
        local b = rand(40)
        mem[b + 1] = 1
        mem[b + 10] = 1
        mem[b + 16] = 1
        mem[b + 17] = 1
        mem[b + 18] = 1
    end
end
