-- scripts/bouncing_balls.lua
-- Live Bouncing-Ball Polyrhythmic Sequencer Co-Processor for serialosc & MIDI.
--
-- Press any pad (x, y) to launch/toggle a bouncing particle in column x.
-- Balls bounce between the top (y=0) and bottom (y=7) walls AND bounce off
-- any pad lit by serialosc/Ableton (host_led(x, y) > 0), firing real
-- serialosc mext grid_key(x, y, 1/0) and MIDI notes on every bounce!
--Hold SHIFT + press a pad to clear that column's ball.

function init()
    for x = 0, 7 do
        mem[x] = -1
        mem[x + 8] = 1
    end
    set_tempo(75)
end

function on_grid(x, y, z)
    if z == 1 then
        if shift() == 1 or mem[x] == y then
            mem[x] = -1
        else
            mem[x] = y
            if y < 4 then
                mem[x + 8] = 1
            else
                mem[x + 8] = -1
            end
        end
        return 1
    end
    return 0
end

function on_fader(idx, val)
    if idx == 8 then
        set_tempo(30 + (127 - val))
    end
end

function on_tick(step)
    led_all(0)
    for x = 0, 7 do
        local y = mem[x]
        if y >= 0 then
            local dy = mem[x + 8]
            local ny = y + dy
            if ny < 0 or ny > 7 or host_led(x, ny) > 0 then
                dy = -dy
                mem[x + 8] = dy
                ny = y + dy
                if ny < 0 then ny = 0 end
                if ny > 7 then ny = 7 end
                grid_key(x, y, 1)
                grid_key(x, y, 0)
                led(x, y, 15)
            else
                led(x, y, 4)
            end
            mem[x] = ny
            led(x, ny, 12)
        end
    end
end
